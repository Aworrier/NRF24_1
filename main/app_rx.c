#include "app_rx.h"

#include <ctype.h>
#include <string.h>

#include "app_proto.h"
#include "app_stats.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nrf24.h"
#include "sdkconfig.h"

/*
 * app_rx.c — 接收端应用实现
 *
 * 模块职责:
 *   1. IRQ 事件队列管理（接收硬件中断通知）。
 *   2. RX FIFO 轮询与排空（将原始数据搬到软件队列）。
 *   3. 帧解析与统计更新（协议校验 + 序列号追踪）。
 *   4. 接收日志输出（hex + 可打印 ASCII）。
 *
 * 双任务架构:
 *
 *   app_irq_task (优先级 9，栈 4KB):
 *     负责从 IRQ 队列接收事件，然后排空 NRF24 RX FIFO 中的全部数据。
 *     设计要点：
 *       - IRQ 队列阻塞等待（100ms 超时），超时后也会尝试轮询 FIFO。
 *       - 用内层 while(1) 循环一次性排空 FIFO（因为 IRQ 信号可能在排空中
 *         被新的接收数据触发，但芯片不会再次产生 IRQ 边沿，所以需要主动排空）。
 *       - 如果 IRQ 信号指示的不是 RX_DR（如 TX_DS/MAX_RT），也会清中断避免积压。
 *
 *   app_rx_consumer_task (优先级 7，栈 4KB):
 *     负责从载荷队列读取数据，执行帧解析、统计更新和日志输出。
 *     设计要点：
 *       - 阻塞等待载荷（portMAX_DELAY），无数据时休眠。
 *       - 解析失败也输出日志（含警告级别），便于检测空口干扰。
 *       - 成功解析的帧打印 hex 和 ASCII 两种形式的载荷内容。
 *
 * 优先级选择理由:
 *   - IRQ 任务（9）高于消费者任务（7），确保 FIFO 尽快排空，避免溢出。
 *   - 消费者任务（7）高于 IDLE（0），但低于关键系统任务。
 */

static const char *TAG = "nrf24_app";

#if defined(CONFIG_NRF24_ROLE_RX)

/* IRQ 事件队列句柄：ISR 写，IRQ 任务读 */
static QueueHandle_t s_irq_evt_queue;

/* 载荷队列句柄：IRQ 任务写，消费者任务读 */
static QueueHandle_t s_rx_payload_queue;

/*
 * IRQ 事件处理任务。
 *
 * 这是 NRF24 接收链路的第一级软件处理。
 * 收到 IRQ 事件后，一次性排空 RX FIFO 中的所有载荷。
 *
 * 为什么需要内层 while(1) 循环排空 FIFO？
 *   - NRF24 的 RX FIFO 有 3 级深度。
 *   - 一批数据到达时可能只产生一次 IRQ 边沿。
 *   - 如果不循环排空，后续两帧可能滞留在 FIFO 中直到下次 IRQ 才被读出。
 *
 * 调试模式（CONFIG_NRF24_MODE_TUTORIAL_DEBUG）:
 *   每 2 秒输出一次状态日志，帮助确认 RX 任务仍在运行。
 */
static void app_irq_task(void *arg)
{
    (void)arg;
    uint32_t evt = 0;
    nrf24_irq_status_t irq = {0};
#if CONFIG_NRF24_MODE_TUTORIAL_DEBUG
    TickType_t last_diag_tick = xTaskGetTickCount();
#endif

    while (1) {
        /*
         * 等待 IRQ 事件，超时 100ms。
         * 使用超时而非 portMAX_DELAY 的原因：
         *   即使没有 IRQ，也可能有遗留数据在 FIFO 中（例如之前的排空
         *   循环在读取过程中被中断），所以周期性地检查 FIFO。
         */
        bool got_irq_event = (xQueueReceive(s_irq_evt_queue, &evt, pdMS_TO_TICKS(100)) == pdTRUE);

        if (got_irq_event) {
            /* 读取 IRQ 状态，用于日志和诊断 */
            if (nrf24_get_irq_status(&irq) != ESP_OK) {
                continue;
            }

#if CONFIG_NRF24_LOG_LEVEL_VERBOSE
            ESP_LOGI(TAG, "IRQ status=0x%02X rx=%d tx_ok=%d max_rt=%d",
                     irq.status, irq.rx_ready, irq.tx_success, irq.tx_failed);
#endif
        }

        /*
         * 排空 RX FIFO：循环读取直到 FIFO 为空。
         * nrf24_read_rx_payload 在 FIFO 为空时返回 ESP_ERR_NOT_FOUND 来退出循环。
         * 每次读取后立即投递到载荷队列（非阻塞，队列满时丢弃）。
         */
        while (1) {
            nrf24_rx_payload_t payload = {0};
            esp_err_t err = nrf24_read_rx_payload(&payload);
            if (err != ESP_OK) {
                break;
            }
            xQueueSend(s_rx_payload_queue, &payload, 0);
        }

        /*
         * 如果收到了 IRQ 事件但不是 RX_DR 中断（可能是 TX_DS 或 MAX_RT 的残余），
         * 手动清除所有中断标志位，防止中断线一直保持低电平阻塞后续 IRQ。
         */
        if (got_irq_event && !irq.rx_ready) {
            nrf24_clear_irq_flags();
        }

#if CONFIG_NRF24_MODE_TUTORIAL_DEBUG
        /* 调试模式：每 2 秒输出存活日志 */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_diag_tick) >= pdMS_TO_TICKS(2000)) {
            last_diag_tick = now;
            uint8_t status = nrf24_get_status();
            ESP_LOGI(TAG, "RX alive, polling FIFO... status=0x%02X", status);
        }
#endif
    }
}

/*
 * RX 载荷消费者任务。
 *
 * 这是 NRF24 接收链路的第二级软件处理。
 * 负责：
 *   1. 从载荷队列取出原始数据包。
 *   2. 调用协议层解析帧（魔数/版本/CRC 校验）。
 *   3. 更新错误统计或序列号连续性统计。
 *   4. 打印带格式的接收日志。
 *
 * 日志输出格式:
 *   RX ok pipe=<管道号> seq=<序列号> pl=<载荷长度> hex=<hex字符串> txt='<可打印文本>' crc_ok=<累计合法帧数>
 *
 * 不可打印字符处理:
 *   在 txt 字段中，所有不可打印字符被替换为 '.'，
 *   确保日志始终是纯 ASCII 可显示文本。
 */
static void app_rx_consumer_task(void *arg)
{
    (void)arg;
    nrf24_rx_payload_t payload = {0};
    app_rx_stats_t *stats = app_stats_rx();

    while (1) {
        /* 阻塞等待载荷数据 */
        if (xQueueReceive(s_rx_payload_queue, &payload, portMAX_DELAY) == pdTRUE) {
            app_proto_frame_t frame = {0};

            /* 原始包计数 +1（每个从 FIFO 读出的包都计入） */
            stats->rx_packets++;

            /* 协议帧解析 */
            app_proto_parse_result_t parse = app_proto_parse_frame(payload.data, payload.len, &frame);
            if (parse != APP_PROTO_PARSE_OK) {
                /* 解析失败：分类更新错误统计 */
                app_rx_stats_on_parse_result(stats, parse);
                ESP_LOGW(TAG, "RX invalid frame pipe=%u len=%u", payload.pipe, payload.len);
                continue;
            }

            /* 解析成功：更新序列号连续性统计 */
            app_rx_stats_on_frame_ok(stats, frame.seq);

            /* 构建 ASCII 文本视图（不可打印字符替换为 '.'） */
            char txt[APP_PROTO_MAX_USER_PAYLOAD + 1] = {0};
            char hex[APP_PROTO_MAX_USER_PAYLOAD * 2 + 1] = {0};
            size_t cp = frame.payload_len < sizeof(txt) - 1 ? frame.payload_len : sizeof(txt) - 1;
            if (cp > 0) {
                memcpy(txt, frame.payload, cp);
                for (size_t i = 0; i < cp; ++i) {
                    if (!isprint((int)(unsigned char)txt[i])) {
                        txt[i] = '.';  /* 不可打印字符替换为 '.' */
                    }
                }
            }

            /* 构建 hex 视图 */
            app_proto_bytes_to_hex(frame.payload, frame.payload_len, hex, sizeof(hex));

            /* 打印接收日志 */
            ESP_LOGI(TAG,
                     "RX ok pipe=%u seq=%u pl=%u hex=%s txt='%s' crc_ok=%lu",
                     payload.pipe,
                     (unsigned)frame.seq,
                     (unsigned)frame.payload_len,
                     hex,
                     txt,
                     (unsigned long)stats->frame_ok);
        }
    }
}
#endif

/*
 * 启动 RX 接收流程（对外接口）。
 *
 * RX 角色:
 *   创建两个队列 + 安装 IRQ 回调 + 进入监听模式 + 创建两个任务。
 *
 * TX 角色:
 *   调用 nrf24_stop_listening 确保芯片不处于接收模式。
 *   因为 TX 端在发送时用到 Enhanced ShockBurst 的 ACK 机制，
 *   需要 PIPE0 RX 地址匹配才能收到 ACK，但芯片保持在 PTX 模式，
 *   不在持续的 PRX 监听状态。
 */
void app_rx_start(void)
{
#if defined(CONFIG_NRF24_ROLE_RX)
    /* 创建队列 */
    s_irq_evt_queue = xQueueCreate(16, sizeof(uint32_t));
    s_rx_payload_queue = xQueueCreate(16, sizeof(nrf24_rx_payload_t));
    ESP_ERROR_CHECK(s_irq_evt_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(s_rx_payload_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    /* 安装 IRQ 回调（ISR 中投递事件到队列） */
    ESP_ERROR_CHECK(nrf24_irq_queue_install(s_irq_evt_queue));

    /* 进入 PRX 接收监听模式（PRIM_RX=1, CE=1） */
    ESP_ERROR_CHECK(nrf24_start_listening());

    /* 创建 IRQ 处理任务（高优先级，确保 FIFO 及时排空） */
    xTaskCreate(app_irq_task, "nrf24_irq", 4096, NULL, 9, NULL);

    /* 创建载荷消费者任务（解析 + 日志 + 统计） */
    xTaskCreate(app_rx_consumer_task, "nrf24_rx", 4096, NULL, 7, NULL);
#else
    /* TX 角色：停止监听，保持 PTX 模式 */
    ESP_ERROR_CHECK(nrf24_stop_listening());
#endif
}
