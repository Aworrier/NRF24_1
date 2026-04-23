#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ble_ctrl_command_cb_t)(const char *line, void *ctx);

esp_err_t ble_ctrl_init(const char *device_name, ble_ctrl_command_cb_t command_cb, void *ctx);
esp_err_t ble_ctrl_send_line(const char *line);
bool ble_ctrl_is_connected(void);

#ifdef __cplusplus
}
#endif
