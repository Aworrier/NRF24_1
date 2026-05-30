#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_proto.h"

/*
 * app_stats.h — 应用层通信统计数据结构与接口
 *
 * 模块职责：
 *   提供发送（TX）和接收（RX）两个方向的运行统计计数器，
 *   供 UART/TCP 控制台的 STATUS 命令查询，也用于 GUI_STAT 日志输出。
 *
 * 统计维度：
 *   TX 侧：队列、发送、ACK 成功率、重传次数。
 *   RX 侧：原始包数、解析成功/失败分类、序列号连续性。
 *
 * 线程安全说明：
 *   本模块的统计数据由单个任务写入（TX 任务或 RX 消费者任务），
 *   由控制台任务只读查询，因此不需要互斥锁保护。
 */

/*
 * TX（发送端）统计数据结构。
 *
 * 各字段在 TX 任务中实时更新，由 STATUS 命令输出。
 *
 * burst_queued:  已入队的 burst 请求总数。一个 burst 包含多帧，由 UART/WiFi 命令触发。
 * frame_sent:    实际通过 nrf24_send_payload 发出的帧总数（含发送失败的帧）。
 * tx_ok:         发送成功次数（收到 ACK，TX_DS 中断置位）。
 * tx_fail:       发送失败次数（MAX_RT 中断或超时）。
 * retries_sum:   所有成功发送的帧的自动重发次数之和。
 * retries_max:   单帧重发次数的最大值。
 * next_seq:      下一帧将使用的序列号（从 0 递增，溢出回绕）。
 */
typedef struct {
    uint32_t burst_queued;   /* 入队的 burst 请求总数 */
    uint32_t frame_sent;     /* 已发出的帧总数 */
    uint32_t tx_ok;          /* 发送成功次数（收到 ACK） */
    uint32_t tx_fail;        /* 发送失败次数（MAX_RT 或超时） */
    uint32_t retries_sum;    /* 重传次数累计和 */
    uint32_t retries_max;    /* 单帧最大重传次数 */
    uint16_t next_seq;       /* 下一个序列号 */
} app_tx_stats_t;

/*
 * RX（接收端）统计数据结构。
 *
 * rx_packets:      从 NRF24 RX FIFO 读取到的原始数据包总数。
 * frame_ok:        通过协议帧校验的合法帧数量。
 * crc_fail:        CRC 校验失败次数（数据损坏）。
 * magic_fail:      魔数或版本不匹配次数（收到非本协议数据）。
 * len_fail:        帧长度字段异常次数。
 * seq_dup:         序列号重复次数（收到重复帧，通常由 ACK 丢失导致）。
 * seq_out_of_order:序列号倒序次数（先收到大号再收到小号，路由延迟变化）。
 * seq_gap:         序列号跳跃缺口累计值（用于评估丢帧严重程度）。
 * has_last_seq:    是否已记录首个序列号（冷启动标记）。
 * last_seq:        最后收到的合法帧序列号，用于计算下一次的连续性。
 */
typedef struct {
    uint32_t rx_packets;        /* 原始接收包总数 */
    uint32_t frame_ok;          /* 合法帧数 */
    uint32_t crc_fail;          /* CRC 校验失败次数 */
    uint32_t magic_fail;        /* 魔数/版本不匹配次数 */
    uint32_t len_fail;          /* 长度错误次数 */
    uint32_t seq_dup;           /* 重复序列号次数 */
    uint32_t seq_out_of_order;  /* 乱序帧次数 */
    uint32_t seq_gap;           /* 序列号跳跃缺口累计 */
    bool has_last_seq;          /* 是否已初始化序列号基线 */
    uint16_t last_seq;          /* 上一帧的序列号 */
} app_rx_stats_t;

/*
 * 重置所有 TX/RX 统计数据为零。
 * 由 RESETSTATS 命令触发，用于重新开始一轮测试统计。
 */
void app_stats_reset(void);

/*
 * 获取 TX 统计数据的可写指针。
 * 返回: 指向全局 TX 统计结构体的指针（调用者可直接修改字段）。
 */
app_tx_stats_t *app_stats_tx(void);

/*
 * 获取 RX 统计数据的可写指针。
 * 返回: 指向全局 RX 统计结构体的指针（调用者可直接修改字段）。
 */
app_rx_stats_t *app_stats_rx(void);

/*
 * 获取 TX 统计数据的只读指针。
 * 返回: 指向全局 TX 统计结构体的 const 指针（用于 STATUS 查询）。
 */
const app_tx_stats_t *app_stats_tx_get(void);

/*
 * 获取 RX 统计数据的只读指针。
 * 返回: 指向全局 RX 统计结构体的 const 指针（用于 STATUS 查询）。
 */
const app_rx_stats_t *app_stats_rx_get(void);

/*
 * 根据帧解析结果更新 RX 错误计数器。
 *
 * 参数:
 *   stats:  RX 统计结构体指针。
 *   result: 帧解析结果枚举值。
 *
 * 内部根据 result 的不同值递增对应的 crc_fail / magic_fail / len_fail。
 */
void app_rx_stats_on_parse_result(app_rx_stats_t *stats, app_proto_parse_result_t result);

/*
 * 收到合法帧后更新序列号连续性统计。
 *
 * 参数:
 *   stats: RX 统计结构体指针。
 *   seq:   当前帧的序列号。
 *
 * 内部逻辑:
 *   - 首个帧：记录为基线。
 *   - seq == last_seq:       重复帧，递增 seq_dup。
 *   - seq == last_seq + 1:   连续帧，正常。
 *   - seq > last_seq + 1:    跳跃，累计 seq_gap。
 *   - seq < last_seq:        倒序，递增 seq_out_of_order。
 */
void app_rx_stats_on_frame_ok(app_rx_stats_t *stats, uint16_t seq);
