#include "app_config.h"
#include "app_control.h"
#include "app_pin_test.h"
#include "app_rx.h"
#include "app_tx.h"
#include "app_wifi_control.h"
#include "nrf24.h"

#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

/* 应用入口：构建配置，初始化驱动，启动控制与 RX/TX 任务。 */

static const char *TAG = "nrf24_app";

#ifndef CONFIG_NRF24_AUTO_RETR_DELAY_US
#define CONFIG_NRF24_AUTO_RETR_DELAY_US 750
#endif

#ifndef CONFIG_NRF24_AUTO_RETR_COUNT
#define CONFIG_NRF24_AUTO_RETR_COUNT 10
#endif

/* Build config, init driver, and start runtime tasks. */
void app_main(void)
{
    nrf24_config_t cfg = {0};
    app_build_nrf24_config(&cfg);

#if CONFIG_NRF24_PIN_TEST_MODE
    app_pin_test_start(&cfg);
    return;
#endif

    ESP_ERROR_CHECK(nrf24_init(&cfg));

    app_log_startup_config(&cfg);

    ESP_ERROR_CHECK(app_nrf24_setup_addresses());
    ESP_ERROR_CHECK(nrf24_config_retransmit(CONFIG_NRF24_AUTO_RETR_DELAY_US, CONFIG_NRF24_AUTO_RETR_COUNT));

    app_control_start_uart();
    app_wifi_control_start();
    app_rx_start();
    app_tx_init();

    ESP_LOGI(TAG, "NRF24 app started. role=%s", app_role_name());
}
