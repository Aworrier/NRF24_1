#include "app_tx.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "app_proto.h"
#include "app_stats.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nrf24.h"
#include "sdkconfig.h"

/* TX runtime: burst scheduler + ALOHA/CSMA gate + GUI_STAT logging. */

static const char *TAG = "nrf24_app";

#if defined(CONFIG_NRF24_ROLE_TX)
typedef struct {
    uint32_t count;
    uint32_t interval_ms;
    uint8_t data[32];
    size_t len;
} app_tx_burst_cmd_t;

typedef struct {
    uint32_t slot_num;
    bool attempted;
    bool success;
    uint8_t reason;
} slot_stat_t;

typedef struct {
    uint32_t target_count;
    uint32_t sent_count;
    uint32_t total_slots;
    slot_stat_t *stats;
    size_t max_slots;
} tx_stat_context_t;

static tx_stat_context_t s_gui_stat_ctx = {0};

static QueueHandle_t s_tx_cmd_queue;
static volatile bool s_tx_enabled = true;
static volatile bool s_tx_abort = false;

static app_mac_mode_t s_mac_mode = APP_MAC_ALOHA;
static uint8_t s_mac_q_percent = 100;
static uint32_t s_slot_ms = 20;
static uint32_t s_csma_window_slots = 1;
static uint32_t s_task_slot_limit = 0;
static uint64_t s_slot_seq = 0;
static uint64_t s_csma_grant_until = 0;

/* Allocate or reset GUI slot statistics buffer. */
static void app_gui_stat_init(uint32_t target_count, size_t max_slots)
{
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

/* Return readable MAC mode name. */
const char *app_tx_mac_mode_name(app_mac_mode_t mode)
{
    return mode == APP_MAC_CSMA ? "CSMA" : "ALOHA";
}

/* Set MAC mode and probability gate. */
void app_tx_set_mac_config(app_mac_mode_t mode, uint8_t q_percent)
{
    s_mac_mode = mode;
    s_mac_q_percent = q_percent;
}

/* Read MAC mode and probability gate. */
void app_tx_get_mac_config(app_mac_mode_t *mode, uint8_t *q_percent)
{
    if (mode != NULL) {
        *mode = s_mac_mode;
    }
    if (q_percent != NULL) {
        *q_percent = s_mac_q_percent;
    }
}

/* Update slot duration and CSMA window. */
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

/* Read slot duration and CSMA window. */
void app_tx_get_slot_params(uint32_t *slot_ms, uint32_t *csma_window_slots)
{
    if (slot_ms != NULL) {
        *slot_ms = s_slot_ms;
    }
    if (csma_window_slots != NULL) {
        *csma_window_slots = s_csma_window_slots;
    }
}

/* Set max slots allowed per burst task. */
void app_tx_set_slot_limit(uint32_t limit)
{
    s_task_slot_limit = limit;
}

/* Get max slots allowed per burst task. */
uint32_t app_tx_get_slot_limit(void)
{
    return s_task_slot_limit;
}

/* Enable or disable TX; disabling aborts current burst. */
void app_tx_set_enabled(bool enabled)
{
    s_tx_enabled = enabled;
    if (!enabled) {
        s_tx_abort = true;
    }
}

/* Read current TX enable flag. */
bool app_tx_is_enabled(void)
{
    return s_tx_enabled;
}

/* Abort current burst as soon as possible. */
void app_tx_abort(void)
{
    s_tx_abort = true;
}

/* Return a random percent value in [0, 99]. */
static uint32_t app_rand_percent(void)
{
    return esp_random() % 100U;
}

/* Convert slot duration to RTOS ticks (min 1 tick). */
static uint32_t app_slot_ticks(void)
{
    uint32_t ms = s_slot_ms > 0 ? s_slot_ms : 1;
    uint32_t ticks = (uint32_t)pdMS_TO_TICKS(ms);
    return ticks > 0 ? ticks : 1;
}

/* Convert interval milliseconds to slot count (min 1). */
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

/* Delay for a number of slots while tracking slot sequence. */
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

typedef enum {
    APP_TX_GATE_ALLOW = 0,
    APP_TX_GATE_BUSY,
    APP_TX_GATE_BACKOFF,
    APP_TX_GATE_PROB_REJECT,
} app_tx_gate_result_t;

/* Decide whether to transmit this slot (CSMA + probability gate). */
static app_tx_gate_result_t app_tx_gate_decide(bool *prob_ok)
{
    if (s_mac_mode == APP_MAC_CSMA) {
        if (s_slot_seq < s_csma_grant_until) {
            return APP_TX_GATE_BACKOFF;
        }

        bool rpd = false;
        if (nrf24_carrier_sense(200, &rpd) == ESP_OK) {
            if (rpd) {
                s_csma_grant_until = s_slot_seq + s_csma_window_slots + 1U;
                return APP_TX_GATE_BUSY;
            }
        }
    }

    const uint32_t rand_percent = app_rand_percent();
    const bool pass_prob_test = (rand_percent < s_mac_q_percent);

    if (prob_ok) {
        *prob_ok = pass_prob_test;
    }

    return pass_prob_test ? APP_TX_GATE_ALLOW : APP_TX_GATE_PROB_REJECT;
}

/* Queue a burst send request for the TX task. */
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

    if (xQueueSend(s_tx_cmd_queue, &req, pdMS_TO_TICKS(30)) != pdTRUE) {
        return false;
    }

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

/* TX task: schedule slots, build frames, and send payloads. */
static void app_tx_task(void *arg)
{
    (void)arg;
    app_tx_burst_cmd_t burst = {0};
    TickType_t last_wake = xTaskGetTickCount();
    app_tx_stats_t *stats = app_stats_tx();

    while (1) {
        if (xQueueReceive(s_tx_cmd_queue, &burst, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        last_wake = xTaskGetTickCount();

        if (!s_tx_enabled) {
            ESP_LOGW(TAG, "TX disabled, drop burst(count=%lu)", (unsigned long)burst.count);
            continue;
        }

        s_tx_abort = false;

        uint32_t burst_index = 0;
        uint16_t current_seq = stats->next_seq;

        while (burst_index < burst.count) {
            if (!s_tx_enabled || s_tx_abort) {
                ESP_LOGW(TAG, "TX burst aborted at %lu/%lu", (unsigned long)burst_index, (unsigned long)burst.count);
                break;
            }

            TickType_t slot_ticks = app_slot_ticks();
            uint32_t slots = app_interval_to_slots(burst.interval_ms);
            app_wait_slots(slots, &last_wake, slot_ticks);

            s_gui_stat_ctx.total_slots += slots;
            if (s_task_slot_limit > 0 && s_gui_stat_ctx.total_slots > s_task_slot_limit) {
                ESP_LOGW(TAG, "GUI_STAT: Sent %u/%u packets in %u slots (timeout)",
                         s_gui_stat_ctx.sent_count,
                         s_gui_stat_ctx.target_count,
                         (unsigned)s_task_slot_limit);
                break;
            }

            const uint32_t current_slot = s_gui_stat_ctx.total_slots - 1U;
            slot_stat_t *stat = NULL;
            if (s_gui_stat_ctx.stats != NULL && current_slot < s_gui_stat_ctx.max_slots) {
                stat = &s_gui_stat_ctx.stats[current_slot];
                stat->slot_num = current_slot;
                stat->attempted = false;
                stat->success = false;
                stat->reason = 0;
            }

            {
                bool prob_ok = true;
                app_tx_gate_result_t gate = app_tx_gate_decide(&prob_ok);
                if (gate != APP_TX_GATE_ALLOW) {
                    if (stat != NULL) {
                        if (gate == APP_TX_GATE_PROB_REJECT) {
                            stat->reason = 2;
                        } else {
                            stat->reason = 0;
                        }
                    }
                    if (stat != NULL) {
                        ESP_LOGI(TAG, "GUI_STAT: slot=%u attempted=%u success=%u reason=%u",
                                 (unsigned)stat->slot_num,
                                 stat->attempted ? 1U : 0U,
                                 stat->success ? 1U : 0U,
                                 (unsigned)stat->reason);
                    }
#if CONFIG_NRF24_LOG_LEVEL_VERBOSE
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
            }

            if (stat != NULL) {
                stat->attempted = true;
            }

#if CONFIG_NRF24_TX_POWER_SAVE
            nrf24_power_up();
#endif
            uint8_t payload[CONFIG_NRF24_PAYLOAD_SIZE] = {0};
            app_proto_frame_t frame = {0};
            frame.seq = current_seq;
            frame.flags = 0;
            frame.payload_len = burst.len > APP_PROTO_MAX_USER_PAYLOAD ? APP_PROTO_MAX_USER_PAYLOAD : (uint8_t)burst.len;
            if (frame.payload_len > 0) {
                memcpy(frame.payload, burst.data, frame.payload_len);
            }

            size_t packed = app_proto_build_frame(payload, sizeof(payload), &frame);
            if (packed == 0) {
                ESP_LOGE(TAG, "TX frame build failed, payload cfg too small");
                stats->tx_fail++;
                break;
            }

            esp_err_t err = nrf24_send_payload(payload, sizeof(payload), pdMS_TO_TICKS(120));

            if (stat != NULL) {
                stat->success = (err == ESP_OK);
                if (!stat->success) {
                    stat->reason = (err == ESP_ERR_TIMEOUT) ? 1 : 3;
                }
                ESP_LOGI(TAG, "GUI_STAT: slot=%u attempted=%u success=%u reason=%u",
                         (unsigned)stat->slot_num,
                         stat->attempted ? 1U : 0U,
                         stat->success ? 1U : 0U,
                         (unsigned)stat->reason);
            }

            if (err == ESP_OK && s_gui_stat_ctx.target_count > 0) {
                if (++s_gui_stat_ctx.sent_count >= s_gui_stat_ctx.target_count) {
                    ESP_LOGI(TAG, "GUI_STAT: Sent %u/%u packets in %u slots",
                             s_gui_stat_ctx.sent_count,
                             s_gui_stat_ctx.target_count,
                             s_gui_stat_ctx.total_slots);
                }
            }
            stats->frame_sent++;
            if (err == ESP_OK) {
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
                stats->next_seq++;
                current_seq = stats->next_seq;
                ++burst_index;
            } else {
                uint8_t status = nrf24_get_status();
                uint8_t lost = 0;
                uint8_t retries = 0;
                nrf24_get_lost_and_retries(&lost, &retries);
                stats->tx_fail++;
                ESP_LOGW(TAG, "TX failed err=%s status=0x%02X retries=%u lost=%u", esp_err_to_name(err), status, retries, lost);
            }

#if CONFIG_NRF24_TX_POWER_SAVE
            nrf24_power_down();
#endif
        }
    }
}

/* Initialize TX queue and task. */
void app_tx_init(void)
{
    s_tx_cmd_queue = xQueueCreate(8, sizeof(app_tx_burst_cmd_t));
    ESP_ERROR_CHECK(s_tx_cmd_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    xTaskCreate(app_tx_task, "nrf24_tx", 4096, NULL, 8, NULL);
}

#else
/* No-op stub for non-TX role. */
const char *app_tx_mac_mode_name(app_mac_mode_t mode)
{
    (void)mode;
    return "N/A";
}

/* No-op stub for non-TX role. */
void app_tx_set_mac_config(app_mac_mode_t mode, uint8_t q_percent)
{
    (void)mode;
    (void)q_percent;
}

/* No-op stub for non-TX role. */
void app_tx_get_mac_config(app_mac_mode_t *mode, uint8_t *q_percent)
{
    if (mode != NULL) {
        *mode = APP_MAC_ALOHA;
    }
    if (q_percent != NULL) {
        *q_percent = 0;
    }
}

/* No-op stub for non-TX role. */
void app_tx_set_slot_params(uint32_t slot_ms, uint32_t csma_window_slots)
{
    (void)slot_ms;
    (void)csma_window_slots;
}

/* No-op stub for non-TX role. */
void app_tx_get_slot_params(uint32_t *slot_ms, uint32_t *csma_window_slots)
{
    if (slot_ms != NULL) {
        *slot_ms = 0;
    }
    if (csma_window_slots != NULL) {
        *csma_window_slots = 0;
    }
}

/* No-op stub for non-TX role. */
void app_tx_set_slot_limit(uint32_t limit)
{
    (void)limit;
}

/* No-op stub for non-TX role. */
uint32_t app_tx_get_slot_limit(void)
{
    return 0;
}

/* No-op stub for non-TX role. */
void app_tx_set_enabled(bool enabled)
{
    (void)enabled;
}

/* No-op stub for non-TX role. */
bool app_tx_is_enabled(void)
{
    return false;
}

/* No-op stub for non-TX role. */
void app_tx_abort(void)
{
}

/* No-op stub for non-TX role. */
bool app_tx_submit_burst(uint32_t count, uint32_t interval_ms, const uint8_t *data, size_t len)
{
    (void)count;
    (void)interval_ms;
    (void)data;
    (void)len;
    return false;
}

/* No-op stub for non-TX role. */
void app_tx_init(void)
{
}
#endif
