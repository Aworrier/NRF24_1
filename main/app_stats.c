#include "app_stats.h"

#include <string.h>

/*
 * app_stats.c — 通信统计模块实现
 *
 * 模块职责：
 *   1. 维护全局 TX/RX 统计结构体的存储和访问。
 *   2. 提供 RESETSTATS 命令支持（清空所有计数器）。
 *   3. 提供 RX 帧解析结果到错误计数器的映射（app_rx_stats_on_parse_result）。
 *   4. 提供 RX 序列号连续性追踪逻辑（app_rx_stats_on_frame_ok）。
 *
 * 设计决策：
 *   - 使用全局静态变量存储统计数据，而非动态分配，避免内存碎片。
 *   - 统计数据由单一任务写入，控制台任务读取，无需加锁。
 *   - TX 侧字段由 app_tx.c 的 TX 任务直接通过 app_stats_tx() 修改。
 *   - RX 侧字段由 app_rx.c 的消费者任务通过辅助函数修改。
 */

/* 全局 TX 统计实例，静态初始化全零 */
static app_tx_stats_t s_tx_stats = {0};

/* 全局 RX 统计实例，静态初始化全零 */
static app_rx_stats_t s_rx_stats = {0};

/*
 * 重置所有统计计数器。
 *
 * 由 UART/TCP 控制台的 RESETSTATS 命令触发。
 * 将 TX 和 RX 两个方向的全部字段归零，
 * 注意：RX 的 has_last_seq 也会被清除为 false，
 * 这意味着复位后首帧会被视为新的序列号基线。
 */
void app_stats_reset(void)
{
    memset(&s_tx_stats, 0, sizeof(s_tx_stats));
    memset(&s_rx_stats, 0, sizeof(s_rx_stats));
}

/*
 * 获取 TX 统计数据的可写指针。
 *
 * TX 任务在发送循环中直接通过此指针修改字段，
 * 包括 tx_ok/fail/retries 等实时计数。
 */
app_tx_stats_t *app_stats_tx(void)
{
    return &s_tx_stats;
}

/*
 * 获取 RX 统计数据的可写指针。
 *
 * RX 消费者任务在收到有效帧后直接更新 rx_packets/frame_ok 等字段。
 */
app_rx_stats_t *app_stats_rx(void)
{
    return &s_rx_stats;
}

/*
 * 获取 TX 统计数据的只读指针。
 *
 * 控制台 STATUS 命令使用此函数安全读取统计信息。
 */
const app_tx_stats_t *app_stats_tx_get(void)
{
    return &s_tx_stats;
}

/*
 * 获取 RX 统计数据的只读指针。
 *
 * 控制台 STATUS 命令使用此函数安全读取统计信息。
 */
const app_rx_stats_t *app_stats_rx_get(void)
{
    return &s_rx_stats;
}

/*
 * 帧解析错误分类统计。
 *
 * 在 RX 消费者任务中，当 app_proto_parse_frame 返回非 OK 时调用此函数，
 * 将解析结果映射到对应的错误计数器上。
 *
 * 映射关系:
 *   APP_PROTO_PARSE_ERR_LEN   -> len_fail++   （缓冲区不足或载荷长度字段异常）
 *   APP_PROTO_PARSE_ERR_MAGIC -> magic_fail++ （魔数/版本不匹配）
 *   APP_PROTO_PARSE_ERR_CRC   -> crc_fail++   （数据校验失败）
 *
 * 注意：APP_PROTO_PARSE_OK 不会递增任何计数器（调用者应在 OK 时调用
 *       app_rx_stats_on_frame_ok 来更新 frame_ok 和序列号追踪）。
 */
void app_rx_stats_on_parse_result(app_rx_stats_t *stats, app_proto_parse_result_t result)
{
    if (stats == NULL) {
        return;
    }

    switch (result) {
        case APP_PROTO_PARSE_ERR_LEN:
            stats->len_fail++;
            break;
        case APP_PROTO_PARSE_ERR_MAGIC:
            stats->magic_fail++;
            break;
        case APP_PROTO_PARSE_ERR_CRC:
            stats->crc_fail++;
            break;
        default:
            /* APP_PROTO_PARSE_OK 不会走到这里，忽略 */
            break;
    }
}

/*
 * RX 序列号连续性追踪。
 *
 * 这是接收端判断链路质量的关键函数。每收到一个合法帧时调用，
 * 通过与上一个序列号的对比来判断是否发生了丢包、重复或乱序。
 *
 * 状态机逻辑:
 *   1. 首个帧（has_last_seq == false）:
 *      - 设置 has_last_seq = true，记录 last_seq 为当前 seq。
 *      - 此帧不作为任何连续性异常的来源（冷启动容忍）。
 *
 *   2. seq == last_seq（重复帧）:
 *      - 可能原因：接收端 ACK 丢失，发送端重传导致对端收到重复帧。
 *      - 递增 seq_dup。
 *
 *   3. seq == last_seq + 1（顺序连续）:
 *      - 理想的正常情况，不做额外计数。
 *
 *   4. seq > last_seq + 1（序列号跳跃）:
 *      - 可能原因：中间有帧在空口中丢失。
 *      - seq_gap += (seq - expected)，累计丢失的序列号跨度。
 *
 *   5. seq < last_seq（乱序）:
 *      - 可能原因：多路径传输延迟差异（本系统不适用），
 *        或发送端在 burst 中途异常重启导致序列号回绕。
 *      - 递增 seq_out_of_order。
 *
 * 最后更新 last_seq 为当前序列号。
 */
void app_rx_stats_on_frame_ok(app_rx_stats_t *stats, uint16_t seq)
{
    if (stats == NULL) {
        return;
    }

    stats->frame_ok++;

    /* 首个帧：建立序列号基线 */
    if (!stats->has_last_seq) {
        stats->has_last_seq = true;
        stats->last_seq = seq;
        return;
    }

    uint16_t expected = (uint16_t)(stats->last_seq + 1U);

    if (seq == stats->last_seq) {
        /* 重复帧：当前序列号等于上一帧序列号 */
        stats->seq_dup++;
    } else if (seq == expected) {
        /* 连续帧：当前序列号恰好是 expected，正常情况 */
    } else if (seq > expected) {
        /* 跳跃：中间帧丢失，累积丢失的序列号跨度 */
        stats->seq_gap += (uint32_t)(seq - expected);
    } else {
        /* 倒序：当前序列号小于上一帧 */
        stats->seq_out_of_order++;
    }

    /* 记录当前序列号作为下一次比对的基线 */
    stats->last_seq = seq;
}
