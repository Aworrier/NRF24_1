#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * app_tx.h — 发送端（TX）应用层接口
 *
 * 模块职责：
 *   管理 NRF24 发送端的 burst 调度、MAC 协议选择和时隙控制。
 *
 * 架构概览（TX 角色）:
 *
 *   UART/TCP 控制台命令
 *       |
 *       | app_tx_submit_burst() → 投递 burst 请求到命令队列
 *       |
 *   app_tx_task (FreeRTOS 任务，优先级 8)
 *       |   → 从命令队列取出 burst 请求
 *       |   → 按 interval_ms 划分时隙
 *       |   → 每时隙调用 app_tx_gate_decide() 决定是否发送
 *       |   → 构建协议帧 (app_proto_build_frame)
 *       |   → 调用 nrf24_send_payload() 发送
 *       |   → 更新统计 (app_stats_tx)
 *       |
 *   NRF24 硬件 → 空口发送 → 等待 ACK
 *
 * MAC 协议支持:
 *   1. ALOHA（纯随机接入）:
 *      - 每个时隙以概率 q% 发送。
 *      - q=100 意味着每个时隙都发送（无退避）。
 *      - q<100 意味着跳过 (100-q)% 的时隙（概率退避）。
 *
 *   2. CSMA（载波侦听多址）:
 *      - 发送前先执行载波侦听（nrf24_carrier_sense）。
 *      - 信道繁忙（RPD=1）：跳过当前时隙 + csma_window 个时隙的退避窗口。
 *      - 信道空闲（RPD=0）：与 ALOHA 一样以概率 q% 发送。
 *
 * 时隙调度:
 *   slot_ms: 时隙长度（毫秒），发送循环的最小时间单元。
 *   interval_ms: burst 中相邻帧之间的间隔（毫秒）。
 *     间隔按时隙数计算: interval_slots = ceil(interval_ms / slot_ms)。
 *     例如 slot_ms=20, interval_ms=100 → 每 5 个时隙发送一帧。
 *
 *   slot_limit: burst 的总时隙上限。
 *     如果 burst 在规定时隙数内未完成所有帧，提前终止。
 *     设 0 表示不限制。
 *
 * GUI_STAT 日志:
 *   每个时隙输出一行 GUI_STAT 日志（无论是否发送），
 *   格式: "GUI_STAT: slot=<n> attempted=<0|1> success=<0|1> reason=<code>"
 *   reason 含义:
 *     0: 跳过（信道忙 / CSMA 退避）。
 *     1: 发送失败（超时 / MAX_RT）。
 *     2: 概率拒绝（q% 测试未通过）。
 *     3: 其他错误。
 */

/*
 * MAC 协议模式枚举。
 */
typedef enum {
    APP_MAC_ALOHA = 0,  /* 纯 ALOHA：概率发送，无载波侦听 */
    APP_MAC_CSMA,        /* CSMA：发送前侦听信道，忙则退避 */
} app_mac_mode_t;

/*
 * 初始化 TX 模块。
 *
 * 创建命令队列和 TX 发送任务。
 * 必须在 nrf24_init 之后、控制台启动之前调用。
 */
void app_tx_init(void);

/*
 * 提交一个 burst（批量发送）请求到 TX 任务队列。
 *
 * burst 是一次"批量发送任务"：在指定的帧间间隔下，
 * 连续发送 count 帧相同内容的载荷数据。
 *
 * 参数:
 *   count:       发送帧数。
 *   interval_ms: 帧间间隔（毫秒）。
 *   data:        载荷数据指针（可为 NULL，当 len=0 时）。
 *   len:         载荷长度（字节），必须 <= 32。
 *
 * 返回:
 *   true:  请求已入队，TX 任务将异步执行。
 *   false: 队列已满或参数无效（count=0 或 len>32）。
 *
 * 注意: 此函数非阻塞，只是把请求放入队列，
 *       实际的发送在 TX 任务中异步执行。
 */
bool app_tx_submit_burst(uint32_t count, uint32_t interval_ms, const uint8_t *data, size_t len);

/*
 * 启用或禁用 TX 发送。
 *
 * 禁用时，TX 任务会丢弃所有待处理的 burst 请求，
 * 并中止正在执行的 burst。
 *
 * 参数:
 *   enabled: true=启用, false=禁用（并中止当前发送）。
 */
void app_tx_set_enabled(bool enabled);

/*
 * 查询 TX 是否处于启用状态。
 * 返回: true=启用, false=禁用。
 */
bool app_tx_is_enabled(void);

/*
 * 立即中止当前正在执行的 burst。
 * 与 set_enabled(false) 不同，此函数不会阻止后续 burst。
 */
void app_tx_abort(void);

/*
 * 设置 MAC 协议模式和概率门限 q。
 *
 * 参数:
 *   mode:      MAC 协议模式（ALOHA 或 CSMA）。
 *   q_percent: 每时隙的发送概率（0-100）。
 *              100 = 每个时隙都发送（无概率退避）。
 *              50  = 一半时隙发送（50% 概率）。
 *              0   = 从不发送（可配合 ENABLE 用于暂停）。
 */
void app_tx_set_mac_config(app_mac_mode_t mode, uint8_t q_percent);

/*
 * 查询当前 MAC 协议模式和概率门限 q。
 *
 * 参数:
 *   mode:      输出 MAC 模式（可为 NULL 表示不关心）。
 *   q_percent: 输出概率门限（可为 NULL 表示不关心）。
 */
void app_tx_get_mac_config(app_mac_mode_t *mode, uint8_t *q_percent);

/*
 * 返回 MAC 模式的可读名称。
 * 返回: "ALOHA" 或 "CSMA"。
 */
const char *app_tx_mac_mode_name(app_mac_mode_t mode);

/*
 * 设置时隙参数。
 *
 * 参数:
 *   slot_ms:           时隙长度（毫秒），>= 1。
 *   csma_window_slots: CSMA 模式下信道繁忙后的退避时隙数，>= 1。
 *
 * 修改时隙参数会重置内部的时隙序列号和 CSMA 退避计数器。
 */
void app_tx_set_slot_params(uint32_t slot_ms, uint32_t csma_window_slots);

/*
 * 查询当前时隙参数。
 *
 * 参数:
 *   slot_ms:             输出时隙长度（可为 NULL）。
 *   csma_window_slots:   输出 CSMA 退避窗口（可为 NULL）。
 */
void app_tx_get_slot_params(uint32_t *slot_ms, uint32_t *csma_window_slots);

/*
 * 设置 burst 最大时隙数限制。
 *
 * 参数:
 *   limit: 最大时隙数（0 = 不限制）。
 *
 * 当 burst 在 limit 个时隙内未完成所有帧时，TX 任务会自动终止
 * 该 burst 并输出 GUI_STAT 超时日志。
 */
void app_tx_set_slot_limit(uint32_t limit);

/*
 * 查询 burst 最大时隙数限制。
 * 返回: 当前限制值（0 = 不限制）。
 */
uint32_t app_tx_get_slot_limit(void);
