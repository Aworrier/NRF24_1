#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    APP_MAC_ALOHA = 0,
    APP_MAC_CSMA,
} app_mac_mode_t;

void app_tx_init(void);
bool app_tx_submit_burst(uint32_t count, uint32_t interval_ms, const uint8_t *data, size_t len);
void app_tx_set_enabled(bool enabled);
bool app_tx_is_enabled(void);
void app_tx_abort(void);

void app_tx_set_mac_config(app_mac_mode_t mode, uint8_t q_percent);
void app_tx_get_mac_config(app_mac_mode_t *mode, uint8_t *q_percent);
const char *app_tx_mac_mode_name(app_mac_mode_t mode);

void app_tx_set_slot_params(uint32_t slot_ms, uint32_t csma_window_slots);
void app_tx_get_slot_params(uint32_t *slot_ms, uint32_t *csma_window_slots);

void app_tx_set_slot_limit(uint32_t limit);
uint32_t app_tx_get_slot_limit(void);
