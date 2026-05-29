#pragma once

#include "nrf24.h"

const char *app_role_name(void);

nrf24_data_rate_t app_cfg_data_rate(void);
nrf24_pa_level_t app_cfg_pa_level(void);
const char *app_cfg_data_rate_name(void);
const char *app_cfg_pa_level_name(void);

void app_build_nrf24_config(nrf24_config_t *cfg);
esp_err_t app_nrf24_setup_addresses(void);
void app_log_startup_config(const nrf24_config_t *cfg);
