#include "app_config.h"

#include <ctype.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

/* Menuconfig mapping + address parsing + startup log. */

static const char *TAG = "nrf24_app";

#ifndef CONFIG_NRF24_PIPE1_ADDR
#define CONFIG_NRF24_PIPE1_ADDR "C2C2C2C2C2"
#endif

#ifndef CONFIG_NRF24_AUTO_RETR_DELAY_US
#define CONFIG_NRF24_AUTO_RETR_DELAY_US 750
#endif

#ifndef CONFIG_NRF24_AUTO_RETR_COUNT
#define CONFIG_NRF24_AUTO_RETR_COUNT 10
#endif

/* Return role label based on build-time config. */
const char *app_role_name(void)
{
#if defined(CONFIG_NRF24_ROLE_TX)
    return "TX";
#else
    return "RX";
#endif
}

/* Parse hex string into raw address bytes. */
static bool app_hex_to_addr(const char *hex, uint8_t *out, size_t width)
{
    if (hex == NULL || out == NULL) {
        return false;
    }

    size_t hex_len = strlen(hex);
    if (hex_len != width * 2) {
        return false;
    }

    for (size_t i = 0; i < width; ++i) {
        char hi = (char)toupper((int)hex[i * 2]);
        char lo = (char)toupper((int)hex[i * 2 + 1]);

        if (!isxdigit((int)hi) || !isxdigit((int)lo)) {
            return false;
        }

        uint8_t val_hi = (uint8_t)(hi <= '9' ? hi - '0' : hi - 'A' + 10);
        uint8_t val_lo = (uint8_t)(lo <= '9' ? lo - '0' : lo - 'A' + 10);
        out[i] = (uint8_t)((val_hi << 4) | val_lo);
    }
    return true;
}

/* Map menuconfig data rate to driver enum. */
nrf24_data_rate_t app_cfg_data_rate(void)
{
#if CONFIG_NRF24_DATA_RATE_2M
    return NRF24_DR_2MBPS;
#elif CONFIG_NRF24_DATA_RATE_250K
    return NRF24_DR_250KBPS;
#else
    return NRF24_DR_1MBPS;
#endif
}

/* Map menuconfig PA level to driver enum. */
nrf24_pa_level_t app_cfg_pa_level(void)
{
#if CONFIG_NRF24_PA_18DBM
    return NRF24_PA_NEG18DBM;
#elif CONFIG_NRF24_PA_12DBM
    return NRF24_PA_NEG12DBM;
#elif CONFIG_NRF24_PA_6DBM
    return NRF24_PA_NEG6DBM;
#else
    return NRF24_PA_0DBM;
#endif
}

/* Return readable data rate string for logs. */
const char *app_cfg_data_rate_name(void)
{
#if CONFIG_NRF24_DATA_RATE_2M
    return "2Mbps";
#elif CONFIG_NRF24_DATA_RATE_250K
    return "250Kbps";
#else
    return "1Mbps";
#endif
}

/* Return readable PA level string for logs. */
const char *app_cfg_pa_level_name(void)
{
#if CONFIG_NRF24_PA_18DBM
    return "-18dBm";
#elif CONFIG_NRF24_PA_12DBM
    return "-12dBm";
#elif CONFIG_NRF24_PA_6DBM
    return "-6dBm";
#else
    return "0dBm";
#endif
}

/* Build nrf24_config_t from menuconfig values. */
void app_build_nrf24_config(nrf24_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    *cfg = (nrf24_config_t){
        .spi_host = CONFIG_NRF24_SPI_HOST == 3 ? SPI3_HOST : SPI2_HOST,
        .pin_mosi = CONFIG_NRF24_PIN_MOSI,
        .pin_miso = CONFIG_NRF24_PIN_MISO,
        .pin_sck = CONFIG_NRF24_PIN_SCK,
        .pin_csn = CONFIG_NRF24_PIN_CSN,
        .pin_ce = CONFIG_NRF24_PIN_CE,
        .pin_irq = CONFIG_NRF24_PIN_IRQ,
        .spi_clock_hz = 8 * 1000 * 1000,
        .channel = CONFIG_NRF24_CHANNEL,
        .payload_size = CONFIG_NRF24_PAYLOAD_SIZE,
        .address_width = CONFIG_NRF24_ADDR_WIDTH,
        .data_rate = app_cfg_data_rate(),
        .pa_level = app_cfg_pa_level(),
        .crc_2bytes = true,
        .enable_dyn_payload = false,
        .retr_delay_us = (uint8_t)((CONFIG_NRF24_AUTO_RETR_DELAY_US + 249) / 250),
        .retr_count = (uint8_t)CONFIG_NRF24_AUTO_RETR_COUNT,
    };
}

/* Apply pipe/TX addresses and enable RX pipes. */
esp_err_t app_nrf24_setup_addresses(void)
{
    uint8_t pipe0[5] = {0};
    uint8_t pipe1[5] = {0};
    uint8_t tx[5] = {0};
    const size_t aw = CONFIG_NRF24_ADDR_WIDTH;

    ESP_RETURN_ON_FALSE(app_hex_to_addr(CONFIG_NRF24_PIPE0_ADDR, pipe0, aw), ESP_ERR_INVALID_ARG, TAG, "invalid PIPE0 address");
    ESP_RETURN_ON_FALSE(app_hex_to_addr(CONFIG_NRF24_PIPE1_ADDR, pipe1, aw), ESP_ERR_INVALID_ARG, TAG, "invalid PIPE1 address");
    ESP_RETURN_ON_FALSE(app_hex_to_addr(CONFIG_NRF24_TX_ADDR, tx, aw), ESP_ERR_INVALID_ARG, TAG, "invalid TX address");

    ESP_RETURN_ON_ERROR(nrf24_set_tx_address(tx, aw), TAG, "set tx address failed");
    ESP_RETURN_ON_ERROR(nrf24_set_rx_address(0, pipe0, aw), TAG, "set pipe0 addr failed");

    ESP_LOGI(TAG, "ADDR cfg: aw=%u pipe0=%s tx=%s", (unsigned)aw,
             CONFIG_NRF24_PIPE0_ADDR, CONFIG_NRF24_TX_ADDR);

#if CONFIG_NRF24_ENABLE_PIPE1
    ESP_LOGI(TAG, "ADDR cfg: pipe1=%s", CONFIG_NRF24_PIPE1_ADDR);
    ESP_RETURN_ON_ERROR(nrf24_set_rx_address(1, pipe1, aw), TAG, "set pipe1 addr failed");
    ESP_RETURN_ON_ERROR(nrf24_enable_rx_pipes(0x03), TAG, "enable pipes failed");
#else
    ESP_RETURN_ON_ERROR(nrf24_enable_rx_pipes(0x01), TAG, "enable pipes failed");
#endif

    return ESP_OK;
}

/* Print startup config for diagnostics. */
void app_log_startup_config(const nrf24_config_t *cfg)
{
#if CONFIG_NRF24_MODE_TUTORIAL_DEBUG
    ESP_LOGI(TAG, "PIN cfg: MOSI=%d MISO=%d SCK=%d CSN=%d CE=%d IRQ=%d",
             cfg->pin_mosi, cfg->pin_miso, cfg->pin_sck, cfg->pin_csn, cfg->pin_ce, cfg->pin_irq);
    ESP_LOGI(TAG, "RADIO cfg: ch=%u payload=%u dr=%s pa=%s retr=%u/%u",
             cfg->channel,
             cfg->payload_size,
             app_cfg_data_rate_name(),
             app_cfg_pa_level_name(),
             (unsigned)CONFIG_NRF24_AUTO_RETR_DELAY_US,
             (unsigned)CONFIG_NRF24_AUTO_RETR_COUNT);
#else
    ESP_LOGI(TAG, "RADIO cfg: ch=%u dr=%s", cfg->channel, app_cfg_data_rate_name());
#endif
}
