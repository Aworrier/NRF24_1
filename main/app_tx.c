#include "app_tx.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "app_proto.h"
#include "app_stats.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nrf24.h"
#include "sdkconfig.h"

/*
 * app_tx.c — 发送端应用实现
 *
 * 模块职责:
 *   1. Burst 调度：按帧间隔将连续发送划分为离散时隙。
 *   2. MAC 门控（gate）：每时隙决定是否发送（ALOHA 概率 / CSMA 侦听）。
 *   3. 协议帧构建：将用户载荷包装为带 CRC 的协议帧。
 *   4. NRF24 发送与 ACK 等待。
 *   5. 统计更新与 GUI_STAT 日志。
 *
 * 核心概念: 时隙化发送
 *
 *   时隙（slot）是 TX 任务的最小时间单位（默认 20ms）。
 *   TX 任务按 slot_ms 的节奏周期性唤醒，在每个时隙中：
 *     1. 判断当前时隙是否应该发送帧（根据 interval）。
 *     2. 调用 MAC 门控决定（gate_decide）。
 *     3. 如果允许发送：构建协议帧 → nrf24_send_payload → 更新统计。
 *     4. 如果禁止发送：跳过（输出 GUI_STAT 日志）。
 *
 *   时隙化的好处:
 *     - 精确控制帧间间隔（避免 FreeRTOS vTaskDelay 的误差累积）。
 *     - 支持 MAC 协议的时隙模型（CSMA 退避以时隙为单位）。
 *     - 支持 GUI_STAT 统计（每个时隙一个数据点）。
 *
 * CSMA 载波侦听说明:
 *   NRF24 芯片本身不提供完整的 CCA（空闲信道评估），
 *   但提供了 RPD（接收功率检测）位。本模块使用 nrf24_carrier_sense
 *   来临时切换到 RX 模式进行一段时间的能量检测。
 *   当检测到能量时认为信道"繁忙"，跳过当前时隙并启动退避窗口。
 *
 *   注意：这不是标准的 802.11 CSMA/CA，而是一个简化的载波侦听策略。
 *   RPD 检测的是"任意信号能量"，无法区分是本网络还是其他网络的信号。
 *
 * 编译条件:
 *   本文件中标记有 #if defined(CONFIG_NRF24_ROLE_TX) 的代码只在 TX 角色下编译。
 *   非 TX 角色提供空桩（stub）函数，所有 API 调用不产生实际效果。
 */

static const char *TAG = "nrf24_app";

#if defined(CONFIG_NRF24_ROLE_TX)

/*
 * Burst 命令队列元素。
 *
 * 每个 burst 请求包含: 帧数、帧间隔、载荷数据和长度。
 * 由控制台（UART/TCP）通过 app_tx_submit_burst 投递到队列。
 */
typedef struct {
    uint32_t count;         /* 发送帧数 */
    uint32_t interval_ms;   /* 帧间间隔（毫秒） */
    uint8_t data[32];       /* 载荷数据副本 */
    size_t len;             /* 载荷长度（字节） */
} app_tx_burst_cmd_t;

/*
 * GUI_STAT 单时隙统计记录。
 *
 * 在 burst 开始时动态分配数组，每时隙记录一个条目的发送结果。
 * 通过 ESP_LOGI("GUI_STAT: ...") 输出，供上位机（如 Python 脚本）解析。
 */
typedef struct {
    uint32_t slot_num;      /* 时隙序号（从 0 递增） */
    bool attempted;         /* 是否尝试发送了帧 */
    bool success;           /* 是否发送成功（收到 ACK） */
    uint8_t reason;         /* 未发送/失败原因码 */
} slot_stat_t;

/*
 * GUI_STAT 上下文。
 *
 * 管理 burst 级别的统计元数据。
 * stats 数组在 burst 开始时 calloc 分配，burst 结束后释放。
 * （当前实现中未在 burst 结束时释放，而是被下一个 burst 覆盖）
 */
typedef struct {
    uint32_t target_count;  /* burst 的目标帧数 */
    uint32_t sent_count;    /* 成功发送的帧数 */
    uint32_t total_slots;   /* 已消耗的时隙总数 */
    slot_stat_t *stats;     /* 时隙统计数组（动态分配） */
    size_t max_slots;       /* stats 数组的容量 */
} tx_stat_context_t;

/* 全局 GUI_STAT 上下文 */
static tx_stat_context_t s_gui_stat_ctx = {0};

/* TX 命令队列句柄（控制台 → TX 任务） */
static QueueHandle_t s_tx_cmd_queue;

/* TX 启用标志（volatile，由其他任务写入，TX 任务读取） */
static volatile bool s_tx_enabled = true;

/* TX 中止标志（volatile，由控制台任务设置，TX 任务检查并清除） */
static volatile bool s_tx_abort = false;

/*
 * MAC 协议运行时状态。
 *
 * s_mac_mode:        当前 MAC 模式（ALOHA 或 CSMA）。
 * s_mac_q_percent:   每时隙的发送概率（0=从不, 100=总是）。
 * s_slot_ms:         时隙长度（毫秒）。
 * s_csma_window_slots:CSMA 信道繁忙退避窗口（时隙数）。
 * s_task_slot_limit: 单个 burst 的最大时隙数限制（0=不限制）。
 * s_slot_seq:        全局时隙序列号（单调递增）。
 * s_csma_grant_until:CSMA 退避有效期（在此序列号之前禁止重新侦听）。
 */
static app_mac_mode_t s_mac_mode            = APP_MAC_ALOHA;
static uint8_t       s_mac_q_percent        = 100;
static uint32_t      s_slot_ms              = 20;
static uint32_t      s_csma_window_slots    = 1;
static uint32_t      s_task_slot_limit      = 0;
static uint64_t      s_slot_seq             = 0;
static uint64_t      s_csma_grant_until     = 0;

/*
 * 初始化 GUI_STAT 统计缓冲区。
 *
 * 在每次新 burst 开始时调用。
 * 为时隙统计数组分配内存，大小 = max(target_count * (csma_window + 2),
 * task_slot_limit)（取较大者）。
 *
 * 参数:
 *   target_count: burst 目标帧数。
 *   max_slots:    预分配的时隙统计数组容量。
 */
static void app_gui_stat_init(uint32_t target_count, size_t max_slots)
{
    /* 释放旧数组 */
    if (s_gui_stat_ctx.stats) {
        free(s_gui_stat_ctx.stats);
        memset(&s_gui_stat_ctx, 0, sizeof(s_gui_stat_ctx));
    }

    if (max_slots == 0) {
        max_slots = 1;
    }
    s_gui_stat_ctx.target_count = target_count;
    s_gui_stat_ctx.sent_count = 0;
    s_gui_stat_ctx.total_slots = 0;
    s_gui_stat_ctx.max_slots = max_slots;
    s_gui_stat_ctx.stats = calloc(max_slots, sizeof(slot_stat_t));
    assert(s_gui_stat_ctx.stats);
}

/*
 * 返回 MAC 模式的可读名称。
 */
const char *app_tx_mac_mode_name(app_mac_mode_t mode)
{
    return mode == APP_MAC_CSMA ? "CSMA" : "ALOHA";
}

/*
 * 设置 MAC 模式与概率门限。
 */
void app_tx_set_mac_config(app_mac_mode_t mode, uint8_t q_percent)
{
    s_mac_mode = mode;
    s_mac_q_percent = q_percent;
}

/*
 * 查询 MAC 模式与概率门限。
 */
void app_tx_get_mac_config(app_mac_mode_t *mode, uint8_t *q_percent)
{
    if (mode != NULL) {
        *mode = s_mac_mode;
    }
    if (q_percent != NULL) {
        *q_percent = s_mac_q_percent;
    }
}

/*
 * 设置时隙参数。
 *
 * 修改时隙参数会重置 CSMA 退避计数器和时隙序列号，
 * 因为新的时隙长度下旧的退避窗口不再有意义。
 */
void app_tx_set_slot_params(uint32_t slot_ms, uint32_t csma_window_slots)
{
    if (slot_ms > 0) {
        s_slot_ms = slot_ms;
    }
    if (csma_window_slots > 0) {
        s_csma_window_slots = csma_window_slots;
    }
    s_csma_grant_until = 0;
    s_slot_seq = 0;
}

/*
 * 查询时隙参数。
 */
void app_tx_get_slot_params(uint32_t *slot_ms, uint32_t *csma_window_slots)
{
    if (slot_ms != NULL) {
        *slot_ms = s_slot_ms;
    }
    if (csma_window_slots != NULL) {
        *csma_window_slots = s_csma_window_slots;
    }
}

/*
 * 设置 burst 最大时隙数限制。
 */
void app_tx_set_slot_limit(uint32_t limit)
{
    s_task_slot_limit = limit;
}

/*
 * 查询 burst 最大时隙数限制。
 */
uint32_t app_tx_get_slot_limit(void)
{
    return s_task_slot_limit;
}

/*
 * 启用/禁用 TX。
 *
 * 禁用时自动设置中止标志，TX 任务会在下一个时隙检测到并停止发送。
 */
void app_tx_set_enabled(bool enabled)
{
    s_tx_enabled = enabled;
    if (!enabled) {
        s_tx_abort = true;
    }
}

/*
 * 查询 TX 启用状态。
 */
bool app_tx_is_enabled(void)
{
    return s_tx_enabled;
}

/*
 * 中止当前 burst。
 */
void app_tx_abort(void)
{
    s_tx_abort = true;
}

/*
 * 生成 0-99 的随机百分比值。
 *
 * 使用 ESP32 硬件随机数发生器（esp_random），
 * 基于射频噪声产生真随机数，比软件 PRNG 更适合 MAC 随机退避。
 */
static uint32_t app_rand_percent(void)
{
    return esp_random() % 100U;
}

/*
 * 将时隙长度转为 FreeRTOS ticks。
 *
 * 最小返回 1 tick，防止 vTaskDelayUntil 收到 0 ticks 导致未定义行为。
 */
static uint32_t app_slot_ticks(void)
{
    uint32_t ms = s_slot_ms > 0 ? s_slot_ms : 1;
    uint32_t ticks = (uint32_t)pdMS_TO_TICKS(ms);
    return ticks > 0 ? ticks : 1;
}

/*
 * 将帧间间隔（毫秒）转换为时隙数。
 *
 * 向上取整，确保间隔 >= 要求的毫秒数。
 * 例如 interval_ms=100, slot_ms=20 → 返回 5。
 *       interval_ms=90,  slot_ms=20 → 返回 5（ceil(90/20) = 5）。
 *       interval_ms=10,  slot_ms=20 → 返回 1（最少 1 个时隙）。
 *
 * 最小返回 1，即至少间隔 1 个时隙。
 */
static uint32_t app_interval_to_slots(uint32_t interval_ms)
{
    uint32_t slot = s_slot_ms > 0 ? s_slot_ms : 1;
    if (interval_ms == 0) {
        return 1;
    }
    uint32_t slots = interval_ms / slot;
    if ((interval_ms % slot) != 0) {
        ++slots;
    }
    return slots == 0 ? 1 : slots;
}

/*
 * 等待指定数量的时隙。
 *
 * 使用 vTaskDelayUntil 实现精确的周期性唤醒。
 * vTaskDelayUntil 的好处:
 *   - 补偿执行时间，避免误差累积。
 *   - 与 vTaskDelay 不同，它保证稳定的周期性，不受任务执行时间波动影响。
 *
 * 每等待一个时隙递增全局时隙序列号 s_slot_seq。
 *
 * 参数:
 *   slots:      等待的时隙数。
 *   last_wake:  上次唤醒的 tick 值（既是输入也是输出）。
 *   slot_ticks: 单个时隙对应的 FreeRTOS ticks。
 */
static void app_wait_slots(uint32_t slots, TickType_t *last_wake, TickType_t slot_ticks)
{
    if (slots == 0) {
        slots = 1;
    }
    for (uint32_t i = 0; i < slots; ++i) {
        vTaskDelayUntil(last_wake, slot_ticks);
        ++s_slot_seq;
    }
}

/*
 * MAC 门控决策结果枚举。
 */
typedef enum {
    APP_TX_GATE_ALLOW = 0,       /* 允许发送 */
    APP_TX_GATE_BUSY,            /* 信道繁忙（CSMA 检测到能量）*/
    APP_TX_GATE_BACKOFF,         /* CSMA 退避窗口中 */
    APP_TX_GATE_PROB_REJECT,     /* 概率门限拒绝（q% 测试未通过）*/
} app_tx_gate_result_t;

/*
 * MAC 门控决策：判断当前时隙是否允许发送。
 *
 * 这是 MAC 层的发送准入控制。
 *
 * 决策流程:
 *
 *   如果 MAC 模式是 CSMA:
 *     a. 检查是否在退避窗口中。
 *        如果在，直接返回 BACKOFF（不做侦听，节省功耗和时间）。
 *     b. 执行载波侦听（nrf24_carrier_sense, 200us 侦听窗口）。
 *        如果检测到能量（RPD=1）：设置退避窗口，返回 BUSY。
 *        如果未检测到能量（RPD=0）：进入概率测试。
 *
 *   概率测试:
 *     - 生成 0-99 随机数。
 *     - 如果 rand < q_percent：返回 ALLOW。
 *     - 否则：返回 PROB_REJECT。
 *
 *   ALOHA 模式没有载波侦听步骤，直接进入概率测试。
 *
 * 参数:
 *   prob_ok:  输出参数，指示概率测试是否通过（可为 NULL）。
 *   rpd_out:  输出参数，载波侦听的 RPD 原始结果（仅 CSMA 模式有效，可为 NULL）。
 *             true=检测到载波能量(信道繁忙), false=信道空闲。
 *             在 ALOHA 模式或退避窗口内返回时，此值不会被写入。
 *   rand_out: 输出参数，本次概率测试的随机数值 0-99（可为 NULL）。
 *
 * 返回值:
 *   APP_TX_GATE_ALLOW:       允许发送。
 *   APP_TX_GATE_BUSY:        信道繁忙，跳过。
 *   APP_TX_GATE_BACKOFF:     CSMA 退避中，跳过。
 *   APP_TX_GATE_PROB_REJECT: 概率拒绝，跳过。
 */
static app_tx_gate_result_t app_tx_gate_decide(bool *prob_ok, bool *rpd_out, uint32_t *rand_out)
{
    /* CSMA 模式专用：载波侦听 + 退避窗口管理 */
    if (s_mac_mode == APP_MAC_CSMA) {
        /* 检查退避窗口：全局时隙序列号 < 退避有效期 → 仍在退避中 */
        if (s_slot_seq < s_csma_grant_until) {
            return APP_TX_GATE_BACKOFF;
        }

        /* 执行载波侦听（200us 侦听窗口） */
        bool rpd = false;
        if (nrf24_carrier_sense(200, &rpd) == ESP_OK) {
            if (rpd_out != NULL) {
                *rpd_out = rpd;    /* 传出 RPD 原始结果供上层日志使用 */
            }
            if (rpd) {
                /* 信道繁忙：设置退避窗口 = 当前时隙 + csma_window + 1 */
                s_csma_grant_until = s_slot_seq + s_csma_window_slots + 1U;
                return APP_TX_GATE_BUSY;
            }
        } else {
            /* 载波侦听失败（SPI 异常），保守处理：视为信道空闲，继续概率测试 */
            if (rpd_out != NULL) {
                *rpd_out = false;
            }
        }
    }

    /* 概率门限测试（ALOHA 和 CSMA 共用） */
    const uint32_t rand_percent = app_rand_percent();
    const bool pass_prob_test = (rand_percent < s_mac_q_percent);

    if (prob_ok) {
        *prob_ok = pass_prob_test;
    }
    if (rand_out != NULL) {
        *rand_out = rand_percent;
    }

    return pass_prob_test ? APP_TX_GATE_ALLOW : APP_TX_GATE_PROB_REJECT;
}

/*
 * 提交 burst 请求到 TX 任务队列。
 *
 * 请求会被拷贝到队列中，因此调用者在函数返回后可以立即释放 data 缓冲区。
 *
 * 返回 false 的常见原因:
 *   - TX 队列尚未初始化（app_tx_init 未调用或失败）。
 *   - count == 0（无效请求）。
 *   - len > 32（超过 NRF24 最大载荷）。
 *   - 队列已满（上次 burst 尚未完成，队列深度 = 8）。
 *
 * 提交成功时:
 *   - 初始化 GUI_STAT 统计缓冲区（分配时隙统计数组）。
 *   - 递增 burst_queued 计数器。
 */
bool app_tx_submit_burst(uint32_t count, uint32_t interval_ms, const uint8_t *data, size_t len)
{
    if (s_tx_cmd_queue == NULL || count == 0 || len > 32) {
        return false;
    }

    app_tx_burst_cmd_t req = {0};
    req.count = count;
    req.interval_ms = interval_ms;
    req.len = len;
    if (len > 0 && data != NULL) {
        memcpy(req.data, data, len);
    }

    /* 非阻塞发送到队列（超时 30ms） */
    if (xQueueSend(s_tx_cmd_queue, &req, pdMS_TO_TICKS(30)) != pdTRUE) {
        return false;
    }

    /* 初始化 GUI_STAT 统计缓冲区 */
    {
        size_t max_slots = (size_t)count * (size_t)(s_csma_window_slots + 2U);
        if (s_task_slot_limit > 0 && (size_t)s_task_slot_limit > max_slots) {
            max_slots = (size_t)s_task_slot_limit;
        }
        app_gui_stat_init(count, max_slots);
    }

    app_stats_tx()->burst_queued++;
    return true;
}

/*
 * TX 发送任务主函数。
 *
 * 这是 NRF24 发送端的核心调度循环。
 *
 * 任务生命周期:
 *   1. 从命令队列接收 burst 请求（阻塞等待）。
 *   2. 按时隙循环发送 burst 中的每一帧。
 *   3. burst 完成后回到步骤 1，等待下一个请求。
 *
 * 内部循环（每时隙）处理:
 *   1. 检查启用/中止标志。
 *   2. 等待帧间间隔的时隙数（vTaskDelayUntil）。
 *   3. 检查 slot_limit 是否超限。
 *   4. 调用 MAC 门控决策。
 *   5. 如果允许发送:
 *      a. 构建协议帧（填充 seq/flags/payload + 计算 CRC）。
 *      b. 调用 nrf24_send_payload 发送并等待 ACK。
 *      c. 更新统计（frame_sent/tx_ok/tx_fail/retries）。
 *      d. 输出 GUI_STAT 和详细日志。
 *   6. 如果禁止发送:
 *      a. 输出 GUI_STAT 跳过日志。
 *
 * 详细日志（TX ok / TX failed）:
 *   成功: "TX ok burst=<当前>/<总数> seq=<序列号> retries=<重传次数> lost=<丢包数> payload=<载荷长度>"
 *   失败: "TX failed err=<错误名> status=<STATUS寄存器> retries=<重传次数> lost=<丢包数>"
 *
 * GUI_STAT 日志:
 *   每时隙输出: "GUI_STAT: slot=<时隙号> attempted=<0|1> success=<0|1> reason=<原因码>"
 */
static void app_tx_task(void *arg)
{
    (void)arg;
    app_tx_burst_cmd_t burst = {0};
    TickType_t last_wake = xTaskGetTickCount();
    app_tx_stats_t *stats = app_stats_tx();

    while (1) {
        /*
         * 步骤1: 等待下一个 burst 请求。
         *
         * CSMA 空闲 RPD 轮询策略:
         *   每次空闲周期用 X 队列接收 Burst 命令（超时=1 tick，不阻塞）。
         *   若无命令，则进入 RX 模式持续侦听 5ms——NRF24 的 RPD 是锁存位，
         *   只要侦听窗口内任意时刻信号超过 -64dBm 就会被置位，读取后清零。
         *   单次 5ms 侦听可覆盖约 50% 的空闲时间，远优于短窗口采样。
         *
         * 其他模式: 阻塞等待（无超时）。
         */
        TickType_t recv_timeout;
        if (s_mac_mode == APP_MAC_CSMA && s_tx_enabled) {
            recv_timeout = 0;  /* 非阻塞，立即检查队列 */
        } else {
            recv_timeout = portMAX_DELAY;
        }

        if (xQueueReceive(s_tx_cmd_queue, &burst, recv_timeout) != pdTRUE) {
            if (s_mac_mode == APP_MAC_CSMA && s_tx_enabled && !app_tx_jam_is_active()) {
                /*
                 * 空闲 CSMA 周期: 进入 RX 模式持续侦听 5ms。
                 * 每 10 次空闲周期输出一次诊断信息，便于确认硬件状态。
                 */
                static uint32_t idle_cycle = 0;
                idle_cycle++;

                /* 侦听前直接读一次 RPD（此时应为 PTX 模式，RPD 不可信） */
                bool rpd_before = false;
                nrf24_read_rpd(&rpd_before);  /* 忽略返回值，仅用于对比 */

                bool rpd = false;
                esp_err_t err = nrf24_carrier_sense(5000, &rpd);

                /* 侦听后立即再读一次 RPD */
                bool rpd_after = false;
                nrf24_read_rpd(&rpd_after);

                if (err == ESP_OK) {
                    if (rpd) {
                        ESP_LOGI(TAG, "CSMA idle poll: RPD=1 (CHANNEL BUSY)");
                    }
                }

                /* 每 10 次（约 500ms）输出一次诊断，无论 RPD 值 */
                if (idle_cycle % 10 == 1) {
                    uint8_t status = nrf24_get_status();
                    ESP_LOGI(TAG, "CSMA diag: cycle=%lu RPD(before=%d, sense=%d, after=%d) status=0x%02X err=%s",
                             (unsigned long)idle_cycle,
                             rpd_before ? 1 : 0,
                             rpd ? 1 : 0,
                             rpd_after ? 1 : 0,
                             status,
                             err == ESP_OK ? "OK" : "FAIL");
                }

                vTaskDelay(pdMS_TO_TICKS(45));
            }
            continue;
        }

        /* 记录开始时间戳 */
        last_wake = xTaskGetTickCount();

        /* 检查是否被禁用 */
        if (!s_tx_enabled) {
            ESP_LOGW(TAG, "TX disabled, drop burst(count=%lu)", (unsigned long)burst.count);
            continue;
        }

        /* 清除中止标志 */
        s_tx_abort = false;

        uint32_t burst_index = 0;
        uint16_t current_seq = stats->next_seq;

        /*
         * 步骤2: 按时隙循环发送
         *
         * 每个迭代:
         *   - 等待 interval_slots 个时隙（帧间间隔）。
         *   - 执行 MAC 门控决策。
         *   - 如果允许且帧未发完：发送一帧。
         */
        while (burst_index < burst.count) {
            /* 检查启用/中止标志 */
            if (!s_tx_enabled || s_tx_abort) {
                ESP_LOGW(TAG, "TX burst aborted at %lu/%lu",
                         (unsigned long)burst_index, (unsigned long)burst.count);
                break;
            }

            TickType_t slot_ticks = app_slot_ticks();
            uint32_t slots = app_interval_to_slots(burst.interval_ms);

            /* 等待帧间间隔的时隙数 */
            app_wait_slots(slots, &last_wake, slot_ticks);

            /* 更新总时隙计数 */
            s_gui_stat_ctx.total_slots += slots;

            /* 检查时隙限制 */
            if (s_task_slot_limit > 0 && s_gui_stat_ctx.total_slots > s_task_slot_limit) {
                ESP_LOGW(TAG, "GUI_STAT: Sent %u/%u packets in %u slots (timeout)",
                         s_gui_stat_ctx.sent_count,
                         s_gui_stat_ctx.target_count,
                         (unsigned)s_task_slot_limit);
                break;
            }

            /* 获取当前时隙的统计条目 */
            const uint32_t current_slot = s_gui_stat_ctx.total_slots - 1U;
            slot_stat_t *stat = NULL;
            if (s_gui_stat_ctx.stats != NULL && current_slot < s_gui_stat_ctx.max_slots) {
                stat = &s_gui_stat_ctx.stats[current_slot];
                stat->slot_num = current_slot;
                stat->attempted = false;
                stat->success = false;
                stat->reason = 0;
            }

            /* MAC 门控决策 */
            {
                bool prob_ok = true;
                bool rpd_result = false;
                uint32_t rand_val = 0;
                app_tx_gate_result_t gate = app_tx_gate_decide(&prob_ok, &rpd_result, &rand_val);

                if (gate != APP_TX_GATE_ALLOW) {
                    /* 禁止发送：记录原因并跳过 */
                    if (stat != NULL) {
                        if (gate == APP_TX_GATE_PROB_REJECT) {
                            stat->reason = 2;  /* 概率拒绝 */
                        } else {
                            stat->reason = 0;  /* 信道忙或退避（不计为错误） */
                        }
                    }
                    if (stat != NULL) {
                        ESP_LOGI(TAG, "GUI_STAT: slot=%u attempted=%u success=%u reason=%u",
                                 (unsigned)stat->slot_num,
                                 stat->attempted ? 1U : 0U,
                                 stat->success ? 1U : 0U,
                                 (unsigned)stat->reason);
                    }

                    /*
                     * CSMA 模式：输出载波侦听详细日志（INFO 级别，终端可见）。
                     *
                     * 日志内容说明:
                     *   - RPD (Received Power Detector):
                     *       1=检测到较强的射频能量（信道被占用），0=信道空闲。
                     *       这是 NRF24 芯片内置的简单能量检测，不区分是
                     *       本网络还是其他网络（WiFi/蓝牙/微波炉等）的信号。
                     *   - backoff_remain: CSMA 退避窗口中剩余的时隙数。
                     *   - rand/q: 概率测试的随机值 vs 门限值（q%）。
                     *       当 rand < q 时允许发送。
                     */
                    if (s_mac_mode == APP_MAC_CSMA) {
                        if (gate == APP_TX_GATE_BUSY) {
                            uint64_t backoff_slots = s_csma_grant_until - s_slot_seq;
                            ESP_LOGI(TAG,
                                     "CSMA carrier sense: RPD=%d (CHANNEL BUSY) backoff=%llu slots",
                                     rpd_result ? 1 : 0,
                                     (unsigned long long)backoff_slots);
                        } else if (gate == APP_TX_GATE_BACKOFF) {
                            uint64_t remain = s_csma_grant_until - s_slot_seq;
                            ESP_LOGI(TAG,
                                     "CSMA carrier sense: BACKOFF window, remain=%llu/%lu slots",
                                     (unsigned long long)remain,
                                     (unsigned long)s_csma_window_slots);
                        } else if (gate == APP_TX_GATE_PROB_REJECT) {
                            ESP_LOGI(TAG,
                                     "CSMA carrier sense: RPD=%d (channel free) but prob reject rand=%lu >= q=%u",
                                     rpd_result ? 1 : 0,
                                     (unsigned long)rand_val,
                                     (unsigned)s_mac_q_percent);
                        }
                    }
#if CONFIG_NRF24_LOG_LEVEL_VERBOSE
                    /* 详细日志：输出跳过原因 */
                    if (gate == APP_TX_GATE_BUSY) {
                        ESP_LOGI(TAG, "TX gate: channel busy, skip");
                    } else if (gate == APP_TX_GATE_BACKOFF) {
                        ESP_LOGI(TAG, "TX gate: csma backoff window, skip");
                    } else if (gate == APP_TX_GATE_PROB_REJECT && !prob_ok) {
                        ESP_LOGI(TAG, "TX gate: prob reject, skip");
                    }
#endif
                    continue;
                }

                /*
                 * CSMA 模式 + 允许发送：输出载波侦听通过日志。
                 * 显示 RPD=0（信道空闲）且概率测试通过。
                 */
                if (s_mac_mode == APP_MAC_CSMA) {
                    ESP_LOGI(TAG,
                             "CSMA carrier sense: RPD=%d (channel free) prob pass rand=%lu < q=%u → ALLOW",
                             rpd_result ? 1 : 0,
                             (unsigned long)rand_val,
                             (unsigned)s_mac_q_percent);
                }
            }

            /* 标记本时隙尝试发送 */
            if (stat != NULL) {
                stat->attempted = true;
            }

#if CONFIG_NRF24_TX_POWER_SAVE
            /* 省电模式：发送前上电（如果之前处于 power_down 状态） */
            nrf24_power_up();
#endif

            /*
             * 构建协议帧
             *
             * 帧格式见 app_proto.h 中的详细定义。
             * 注意: 载荷长度不能超过 APP_PROTO_MAX_USER_PAYLOAD（22 字节），
             * 因为总帧长 = 8(帧头) + pl + 2(CRC) 必须 <= 32。
             */
            uint8_t payload[CONFIG_NRF24_PAYLOAD_SIZE] = {0};
            app_proto_frame_t frame = {0};
            frame.seq = current_seq;
            frame.flags = 0;
            frame.payload_len = burst.len > APP_PROTO_MAX_USER_PAYLOAD
                                    ? APP_PROTO_MAX_USER_PAYLOAD
                                    : (uint8_t)burst.len;
            if (frame.payload_len > 0) {
                memcpy(frame.payload, burst.data, frame.payload_len);
            }

            size_t packed = app_proto_build_frame(payload, sizeof(payload), &frame);
            if (packed == 0) {
                ESP_LOGE(TAG, "TX frame build failed, payload cfg too small");
                stats->tx_fail++;
                break;
            }

            /*
             * 发送并等待结果
             *
             * nrf24_send_payload 内部流程:
             *   1. 停止 RX 监听 → 进入 PTX 模式。
             *   2. 清中断标志。
             *   3. 写入 TX payload。
             *   4. CE 脉冲触发发送（15us 脉冲）。
             *   5. 轮询 IRQ 状态: TX_DS=成功, MAX_RT=失败, 超时=无响应。
             *
             * 超时时间: 120ms。
             *   在 1Mbps 速率、32 字节载荷、10 次重发（每次 1000us 间隔）下，
             *   最坏情况约 10ms。120ms 提供了充足的余量。
             */
            esp_err_t err = nrf24_send_payload(payload, sizeof(payload), pdMS_TO_TICKS(120));

            /* 更新 GUI_STAT 统计 */
            if (stat != NULL) {
                stat->success = (err == ESP_OK);
                if (!stat->success) {
                    stat->reason = (err == ESP_ERR_TIMEOUT) ? 1 : 3;
                    /* reason=1: 超时/MAX_RT, reason=3: 其他错误 */
                }
                ESP_LOGI(TAG, "GUI_STAT: slot=%u attempted=%u success=%u reason=%u",
                         (unsigned)stat->slot_num,
                         stat->attempted ? 1U : 0U,
                         stat->success ? 1U : 0U,
                         (unsigned)stat->reason);
            }

            /* 检查目标帧数是否达成 */
            if (err == ESP_OK && s_gui_stat_ctx.target_count > 0) {
                if (++s_gui_stat_ctx.sent_count >= s_gui_stat_ctx.target_count) {
                    ESP_LOGI(TAG, "GUI_STAT: Sent %u/%u packets in %u slots",
                             s_gui_stat_ctx.sent_count,
                             s_gui_stat_ctx.target_count,
                             s_gui_stat_ctx.total_slots);
                }
            }

            /* 更新 TX 统计计数器 */
            stats->frame_sent++;

            if (err == ESP_OK) {
                /* 发送成功：更新成功计数和重传统计 */
                uint8_t lost = 0;
                uint8_t retries = 0;
                nrf24_get_lost_and_retries(&lost, &retries);

                stats->tx_ok++;
                stats->retries_sum += retries;
                if (retries > stats->retries_max) {
                    stats->retries_max = retries;
                }

                ESP_LOGI(TAG,
                         "TX ok burst=%lu/%lu seq=%u retries=%u lost=%u payload=%u",
                         (unsigned long)(burst_index + 1),
                         (unsigned long)burst.count,
                         (unsigned)frame.seq,
                         retries,
                         lost,
                         (unsigned)frame.payload_len);

                /* 推进序列号并进入下一帧 */
                stats->next_seq++;
                current_seq = stats->next_seq;
                ++burst_index;
            } else {
                /* 发送失败：输出详细诊断信息 */
                uint8_t status = nrf24_get_status();
                uint8_t lost = 0;
                uint8_t retries = 0;
                nrf24_get_lost_and_retries(&lost, &retries);

                stats->tx_fail++;
                ESP_LOGW(TAG, "TX failed err=%s status=0x%02X retries=%u lost=%u",
                         esp_err_to_name(err), status, retries, lost);
            }

#if CONFIG_NRF24_TX_POWER_SAVE
            /* 省电模式：发送完毕后进入低功耗状态 */
            nrf24_power_down();
#endif
        }
    }
}

/*
 * 初始化 TX 模块。
 *
 * 创建命令队列（深度 8，即最多 8 个 burst 请求等待处理）
 * 和 TX 发送任务（优先级 8，栈 4KB）。
 */
void app_tx_init(void)
{
    s_tx_cmd_queue = xQueueCreate(8, sizeof(app_tx_burst_cmd_t));
    ESP_ERROR_CHECK(s_tx_cmd_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    xTaskCreate(app_tx_task, "nrf24_tx", 4096, NULL, 8, NULL);
}

/*
 * =========================================================================
 * 信号发生器 (Jammer) — 用于产生信道干扰，辅助验证 CSMA/RPD。
 *
 * 特点:
 *   - 独立任务 "nrf24_jam"，与 TX burst 任务完全隔离。
 *   - 自动禁用 auto-ack，最大化空口占空比。
 *   - 发送全 0xFF 满载载荷（32 字节），产生最大空口能量。
 *   - 紧循环发送，包间仅留 50µs PLL 稳定间隙。
 * =========================================================================
 */

static volatile bool s_jam_active = false;
static TaskHandle_t  s_jam_task    = NULL;

/*
 * Jammer 任务 — 紧循环发送。
 *
 * 每轮:
 *   1. 上电并停止监听（确保 PTX 模式）。
 *   2. 写入 32 字节全满载荷。
 *   3. CE 脉冲触发发送（等待 120ms 超时——无 ACK 时通常瞬间完成）。
 *   4. 极小间隙后立即进入下一轮。
 *
 * 不使用 auto-ack：无 ACK 时 NRF24 跳过重发等待，直接完成 TX_DS。
 */
static void app_tx_jammer_task(void *arg)
{
    (void)arg;

    uint8_t payload[32];
    memset(payload, 0xFF, sizeof(payload));

    /* 暂存原有 auto-ack 配置，jammer 期间禁用以最大化速率 */
    uint8_t saved_aa_mask = 0;
    nrf24_get_auto_ack_mask(&saved_aa_mask);
    nrf24_set_auto_ack_mask(0);   /* 全部管道禁用 ACK */

    /* 一次性进入 PTX 模式并上电，不在循环内重复调用（省去每轮 ~3ms delay） */
    nrf24_power_up();
    nrf24_stop_listening();

    ESP_LOGI(TAG, "Jammer started (no ACK, 32B payload, max rate)");

    uint32_t pkt_count = 0;
    while (s_jam_active) {
        /*
         * 紧循环发送，最小化包间间隔:
         *   1. 清中断 + 写 TX FIFO（无 ACK 时忽略 ack_payload 参数）。
         *   2. CE 脉冲触发空口发射（≥10µs 即可）。
         *   3. 自旋等待 IRQ（无 ACK 时 TX_DS/MAX_RT 通常 <200µs）。
         *   4. 每 1000 包喂一次狗，避免看门狗复位。
         */
        nrf24_irq_status_t irq = {0};

        nrf24_clear_irq_flags();
        nrf24_write_payload(payload, sizeof(payload), false);

        nrf24_pulse_ce();  /* CE 脉冲触发一次发射（≥10µs） */

        /* 自旋等待 IRQ（无 ACK 时 TX_DS 通常在 ~130µs 内触发） */
        bool done = false;
        for (int i = 0; i < 50 && !done; i++) {
            if (nrf24_get_irq_status(&irq) == ESP_OK) {
                if (irq.tx_success || irq.tx_failed) {
                    done = true;
                }
            }
            if (!done) {
                esp_rom_delay_us(5);  /* 每次自旋 5µs，总共最多 250µs */
            }
        }
        nrf24_clear_irq_flags();

        if (irq.tx_failed) {
            nrf24_flush_tx();
        }

        /* 每 1000 包喂狗 + 让出 CPU 1 tick（确保低优先级任务不被饿死） */
        if (++pkt_count % 1000 == 0) {
            vTaskDelay(1);
        }
    }

    /* 恢复 auto-ack 配置并断电 */
    nrf24_set_auto_ack_mask(saved_aa_mask);
    nrf24_power_down();
    s_jam_task = NULL;
    ESP_LOGI(TAG, "Jammer stopped (%lu packets sent)", (unsigned long)pkt_count);
    vTaskDelete(NULL);
}

void app_tx_jam_start(uint8_t channel_override)
{
    if (s_jam_active) {
        ESP_LOGW(TAG, "Jammer already running");
        return;
    }

    s_jam_active = true;
    xTaskCreate(app_tx_jammer_task, "nrf24_jam", 2048, NULL, 10, &s_jam_task);
    ESP_LOGI(TAG, "Jammer ON (continuous TX, no ACK)");
}

void app_tx_jam_stop(void)
{
    if (!s_jam_active) {
        return;
    }
    s_jam_active = false;
    /* 等待任务退出（最多 200ms） */
    for (int i = 0; i < 20 && s_jam_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_jam_task != NULL) {
        vTaskDelete(s_jam_task);
        s_jam_task = NULL;
    }
    nrf24_power_down();
    ESP_LOGI(TAG, "Jammer OFF");
}

bool app_tx_jam_is_active(void)
{
    return s_jam_active;
}

#else
/*
 * =========================================================================
 * 非 TX 角色的空桩（stub）实现。
 *
 * 当编译为 RX 角色时，这些函数不产生任何实际效果。
 * 这样做的目的是让控制台代码可以在 RX 和 TX 角色下都能编译通过，
 * 而不需要使用 #ifdef 在每个控制台命令处做条件编译。
 *
 * RX 角色下调用这些函数的场景:
 *   - STATUS 命令在 RX 角色下访问 tx stats（返回全零）。
 *   - 控制台在 RX 角色下收到 BURST 命令会先被 handle_line 中的
 *     条件编译拦截（回复 "ERR RX role"），所以这些 stub 通常不会被调用。
 * =========================================================================
 */

const char *app_tx_mac_mode_name(app_mac_mode_t mode)
{
    (void)mode;
    return "N/A";
}

void app_tx_set_mac_config(app_mac_mode_t mode, uint8_t q_percent)
{
    (void)mode;
    (void)q_percent;
}

void app_tx_get_mac_config(app_mac_mode_t *mode, uint8_t *q_percent)
{
    if (mode != NULL) {
        *mode = APP_MAC_ALOHA;
    }
    if (q_percent != NULL) {
        *q_percent = 0;
    }
}

void app_tx_set_slot_params(uint32_t slot_ms, uint32_t csma_window_slots)
{
    (void)slot_ms;
    (void)csma_window_slots;
}

void app_tx_get_slot_params(uint32_t *slot_ms, uint32_t *csma_window_slots)
{
    if (slot_ms != NULL) {
        *slot_ms = 0;
    }
    if (csma_window_slots != NULL) {
        *csma_window_slots = 0;
    }
}

void app_tx_set_slot_limit(uint32_t limit)
{
    (void)limit;
}

uint32_t app_tx_get_slot_limit(void)
{
    return 0;
}

void app_tx_set_enabled(bool enabled)
{
    (void)enabled;
}

bool app_tx_is_enabled(void)
{
    return false;
}

void app_tx_abort(void)
{
}

bool app_tx_submit_burst(uint32_t count, uint32_t interval_ms, const uint8_t *data, size_t len)
{
    (void)count;
    (void)interval_ms;
    (void)data;
    (void)len;
    return false;
}

void app_tx_jam_start(uint8_t channel_override)
{
    (void)channel_override;
}

void app_tx_jam_stop(void)
{
}

bool app_tx_jam_is_active(void)
{
    return false;
}

void app_tx_init(void)
{
}
#endif
