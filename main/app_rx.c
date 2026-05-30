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

/* RX runtime: IRQ queue + FIFO polling + frame parsing + stats. */

static const char *TAG = "nrf24_app";

#if defined(CONFIG_NRF24_ROLE_RX)
static QueueHandle_t s_irq_evt_queue;
static QueueHandle_t s_rx_payload_queue;

/* Handle IRQ events and drain RX FIFO into a queue. */
static void app_irq_task(void *arg)
{
    (void)arg;
    uint32_t evt = 0;
    nrf24_irq_status_t irq = {0};
#if CONFIG_NRF24_MODE_TUTORIAL_DEBUG
    TickType_t last_diag_tick = xTaskGetTickCount();
#endif

    while (1) {
        bool got_irq_event = (xQueueReceive(s_irq_evt_queue, &evt, pdMS_TO_TICKS(100)) == pdTRUE);

        if (got_irq_event) {
            if (nrf24_get_irq_status(&irq) != ESP_OK) {
                continue;
            }

#if CONFIG_NRF24_LOG_LEVEL_VERBOSE
            ESP_LOGI(TAG, "IRQ status=0x%02X rx=%d tx_ok=%d max_rt=%d", irq.status, irq.rx_ready, irq.tx_success, irq.tx_failed);
#endif
        }

        while (1) {
            nrf24_rx_payload_t payload = {0};
            esp_err_t err = nrf24_read_rx_payload(&payload);
            if (err != ESP_OK) {
                break;
            }
            xQueueSend(s_rx_payload_queue, &payload, 0);
        }

        if (got_irq_event && !irq.rx_ready) {
            nrf24_clear_irq_flags();
        }

#if CONFIG_NRF24_MODE_TUTORIAL_DEBUG
        TickType_t now = xTaskGetTickCount();
        if ((now - last_diag_tick) >= pdMS_TO_TICKS(2000)) {
            last_diag_tick = now;
            uint8_t status = nrf24_get_status();
            ESP_LOGI(TAG, "RX alive, polling FIFO... status=0x%02X", status);
        }
#endif
    }
}

/* Consume payloads, parse frames, and update stats. */
static void app_rx_consumer_task(void *arg)
{
    (void)arg;
    nrf24_rx_payload_t payload = {0};
    app_rx_stats_t *stats = app_stats_rx();

    while (1) {
        if (xQueueReceive(s_rx_payload_queue, &payload, portMAX_DELAY) == pdTRUE) {
            app_proto_frame_t frame = {0};
            stats->rx_packets++;

            app_proto_parse_result_t parse = app_proto_parse_frame(payload.data, payload.len, &frame);
            if (parse != APP_PROTO_PARSE_OK) {
                app_rx_stats_on_parse_result(stats, parse);
                ESP_LOGW(TAG, "RX invalid frame pipe=%u len=%u", payload.pipe, payload.len);
                continue;
            }

            app_rx_stats_on_frame_ok(stats, frame.seq);

            char txt[APP_PROTO_MAX_USER_PAYLOAD + 1] = {0};
            char hex[APP_PROTO_MAX_USER_PAYLOAD * 2 + 1] = {0};
            size_t cp = frame.payload_len < sizeof(txt) - 1 ? frame.payload_len : sizeof(txt) - 1;
            if (cp > 0) {
                memcpy(txt, frame.payload, cp);
                for (size_t i = 0; i < cp; ++i) {
                    if (!isprint((int)(unsigned char)txt[i])) {
                        txt[i] = '.';
                    }
                }
            }
            app_proto_bytes_to_hex(frame.payload, frame.payload_len, hex, sizeof(hex));

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

/* Initialize RX queues, IRQ hook, and RX tasks. */
void app_rx_start(void)
{
#if defined(CONFIG_NRF24_ROLE_RX)
    s_irq_evt_queue = xQueueCreate(16, sizeof(uint32_t));
    s_rx_payload_queue = xQueueCreate(16, sizeof(nrf24_rx_payload_t));
    ESP_ERROR_CHECK(s_irq_evt_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(s_rx_payload_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(nrf24_irq_queue_install(s_irq_evt_queue));
    ESP_ERROR_CHECK(nrf24_start_listening());
    xTaskCreate(app_irq_task, "nrf24_irq", 4096, NULL, 9, NULL);
    xTaskCreate(app_rx_consumer_task, "nrf24_rx", 4096, NULL, 7, NULL);
#else
    ESP_ERROR_CHECK(nrf24_stop_listening());
#endif
}
