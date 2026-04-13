#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nrf24.h"

/*
 * app_main.c
 *
 * 这个文件负责“应用层流程编排”，不直接操作底层寄存器。
 * 阅读建议：
 * 1) 看 app_main：初始化总流程。
 * 2) 看 app_start_rx_mode / app_start_tx_mode：角色逻辑。
 * 3) 看 app_irq_task / app_tx_task：运行期数据路径。
 */

static const char *TAG = "nrf24_app";

static QueueHandle_t s_irq_evt_queue;
static QueueHandle_t s_rx_payload_queue;

#ifndef CONFIG_NRF24_PIPE1_ADDR
#define CONFIG_NRF24_PIPE1_ADDR "C2C2C2C2C2"
#endif

#ifndef CONFIG_NRF24_AUTO_RETR_DELAY_US
#define CONFIG_NRF24_AUTO_RETR_DELAY_US 750
#endif

#ifndef CONFIG_NRF24_AUTO_RETR_COUNT
#define CONFIG_NRF24_AUTO_RETR_COUNT 10
#endif

#if CONFIG_NRF24_PIN_TEST_MODE
/* 打印当前引脚配置，辅助排查接线问题。 */
static void app_dump_nrf_pins(const nrf24_config_t *cfg)
{
    uint64_t mask = (1ULL << cfg->pin_mosi) |
                    (1ULL << cfg->pin_miso) |
                    (1ULL << cfg->pin_sck) |
                    (1ULL << cfg->pin_csn) |
                    (1ULL << cfg->pin_ce) |
                    (1ULL << cfg->pin_irq);
    gpio_dump_io_configuration(stdout, mask);
}
#endif

#if CONFIG_NRF24_PIN_TEST_MODE
typedef struct {
    /* 下面是 PIN TEST 任务会用到的实际 GPIO。 */
    gpio_num_t pin_ce;
    gpio_num_t pin_csn;
    gpio_num_t pin_mosi;
    gpio_num_t pin_miso;
    gpio_num_t pin_sck;
    gpio_num_t pin_irq;
} nrf24_pin_test_ctx_t;

static void app_pin_test_task(void *arg)
{
    nrf24_pin_test_ctx_t *ctx = (nrf24_pin_test_ctx_t *)arg;
    int level = 0;
    size_t pattern_idx = 0;
    static const uint8_t patterns[] = {0xA5, 0x5A, 0x3C, 0xC3};

    while (1) {
        /* 每个周期先翻转 CE/CSN，模拟控制脚工作状态。 */
        level = !level;
        int ce_expected = level;
        int csn_expected = !level;
        uint8_t tx_pattern = patterns[pattern_idx];
        uint8_t rx_sample = 0;
        int miso_probe_low = 0;
        int miso_probe_high = 0;

        gpio_set_level(ctx->pin_ce, ce_expected);
        gpio_set_level(ctx->pin_csn, csn_expected);

        /* 在 SCK 空闲时做 MISO 探测，用于判断是否存在串线/短接。 */
        gpio_set_level(ctx->pin_sck, 0);
        gpio_set_level(ctx->pin_mosi, 0);
        esp_rom_delay_us(20);
        miso_probe_low = gpio_get_level(ctx->pin_miso);
        gpio_set_level(ctx->pin_mosi, 1);
        esp_rom_delay_us(20);
        miso_probe_high = gpio_get_level(ctx->pin_miso);

        /* 模拟 1 字节 SPI 传输，验证 SCK/MOSI/MISO 动态行为。 */
        gpio_set_level(ctx->pin_csn, 0);
        for (int bit = 7; bit >= 0; --bit) {
            int mosi_bit = (tx_pattern >> bit) & 0x01;
            gpio_set_level(ctx->pin_mosi, mosi_bit);
            gpio_set_level(ctx->pin_sck, 0);
            esp_rom_delay_us(20);
            gpio_set_level(ctx->pin_sck, 1);
            esp_rom_delay_us(20);
            rx_sample = (uint8_t)((rx_sample << 1) | (gpio_get_level(ctx->pin_miso) & 0x01));
        }
        gpio_set_level(ctx->pin_sck, 0);
        gpio_set_level(ctx->pin_csn, csn_expected);

        ESP_LOGI(TAG,
             "PIN TEST: CE(exp=%d,act=%d) CSN(exp=%d,act=%d) SPI(tx=0x%02X,rx=0x%02X) MISOprobe(0->%d,1->%d) MOSI=%d MISO=%d SCK=%d IRQ=%d",
             ce_expected,
                 gpio_get_level(ctx->pin_ce),
             csn_expected,
                 gpio_get_level(ctx->pin_csn),
                 tx_pattern,
                 rx_sample,
                 miso_probe_low,
                 miso_probe_high,
                 gpio_get_level(ctx->pin_mosi),
                 gpio_get_level(ctx->pin_miso),
                 gpio_get_level(ctx->pin_sck),
                 gpio_get_level(ctx->pin_irq));

        pattern_idx = (pattern_idx + 1) % (sizeof(patterns) / sizeof(patterns[0]));

        vTaskDelay(pdMS_TO_TICKS(CONFIG_NRF24_PIN_TEST_PERIOD_MS));
    }
}
#endif

static const char *app_role_name(void)
{
#if defined(CONFIG_NRF24_ROLE_TX)
    return "TX";
#else
    return "RX";
#endif
}

static bool hex_to_addr(const char *hex, uint8_t *out, size_t width)
{
    /* 把 "E7E7E7E7E7" 这种字符串地址转换为字节数组。 */
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

static nrf24_data_rate_t app_cfg_data_rate(void)
{
    /* 将 menuconfig 选项映射到驱动枚举。 */
#if CONFIG_NRF24_DATA_RATE_2M
    return NRF24_DR_2MBPS;
#elif CONFIG_NRF24_DATA_RATE_250K
    return NRF24_DR_250KBPS;
#else
    return NRF24_DR_1MBPS;
#endif
}

static nrf24_pa_level_t app_cfg_pa_level(void)
{
    /* 将 menuconfig 功率档位映射到驱动枚举。 */
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

static const char *app_cfg_data_rate_name(void)
{
#if CONFIG_NRF24_DATA_RATE_2M
    return "2Mbps";
#elif CONFIG_NRF24_DATA_RATE_250K
    return "250Kbps";
#else
    return "1Mbps";
#endif
}

#if CONFIG_NRF24_MODE_TUTORIAL_DEBUG
static const char *app_cfg_pa_level_name(void)
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
#endif

static esp_err_t app_nrf24_setup_addresses(void)
{
    /* 地址配置是两板互通的关键步骤。 */
    uint8_t pipe0[5] = {0};
    uint8_t pipe1[5] = {0};
    uint8_t tx[5] = {0};
    const size_t aw = CONFIG_NRF24_ADDR_WIDTH;

    ESP_RETURN_ON_FALSE(hex_to_addr(CONFIG_NRF24_PIPE0_ADDR, pipe0, aw), ESP_ERR_INVALID_ARG, TAG, "invalid PIPE0 address");
    ESP_RETURN_ON_FALSE(hex_to_addr(CONFIG_NRF24_PIPE1_ADDR, pipe1, aw), ESP_ERR_INVALID_ARG, TAG, "invalid PIPE1 address");
    ESP_RETURN_ON_FALSE(hex_to_addr(CONFIG_NRF24_TX_ADDR, tx, aw), ESP_ERR_INVALID_ARG, TAG, "invalid TX address");

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

static void app_log_startup_config(const nrf24_config_t *cfg)
{
    /* Tutorial 模式打印完整配置，Release 模式只保留关键字段。 */
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

#if defined(CONFIG_NRF24_ROLE_RX)
static void app_irq_task(void *arg)
{
    (void)arg;
    uint32_t evt = 0;
    nrf24_irq_status_t irq = {0};
#if CONFIG_NRF24_MODE_TUTORIAL_DEBUG
    TickType_t last_diag_tick = xTaskGetTickCount();
#endif

    while (1) {
        /* 优先处理 IRQ 事件；如果没有 IRQ，也继续轮询 FIFO 兜底。 */
        bool got_irq_event = (xQueueReceive(s_irq_evt_queue, &evt, pdMS_TO_TICKS(100)) == pdTRUE);

        if (got_irq_event) {
            if (nrf24_get_irq_status(&irq) != ESP_OK) {
                continue;
            }

#if CONFIG_NRF24_LOG_LEVEL_VERBOSE
            ESP_LOGI(TAG, "IRQ status=0x%02X rx=%d tx_ok=%d max_rt=%d", irq.status, irq.rx_ready, irq.tx_success, irq.tx_failed);
#endif
        }

        /* RX mode always polls FIFO as fallback even when IRQ is quiet. */
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

static void app_rx_consumer_task(void *arg)
{
    (void)arg;
    nrf24_rx_payload_t payload = {0};

    while (1) {
        /* 从 RX 队列中取包并转成可读日志。 */
        if (xQueueReceive(s_rx_payload_queue, &payload, portMAX_DELAY) == pdTRUE) {
            char txt[33] = {0};
            size_t cp = payload.len < sizeof(txt) - 1 ? payload.len : sizeof(txt) - 1;
            memcpy(txt, payload.data, cp);
            ESP_LOGI(TAG, "RX pipe=%u len=%u data='%s'", payload.pipe, payload.len, txt);
        }
    }
}
#endif

#if defined(CONFIG_NRF24_ROLE_TX)
static void app_tx_task(void *arg)
{
    (void)arg;
    uint32_t seq = 0;

    while (1) {
        /* 周期发送固定长度载荷，便于与固定 payload 配置对齐。 */
#if CONFIG_NRF24_TX_POWER_SAVE
        nrf24_power_up();
#endif
        uint8_t payload[CONFIG_NRF24_PAYLOAD_SIZE] = {0};
        int len = snprintf((char *)payload, sizeof(payload), "hello nrf24 #%lu", (unsigned long)seq++);
        if (len < 0) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_NRF24_TX_PERIOD_MS));
            continue;
        }

        esp_err_t err = nrf24_send_payload(payload, sizeof(payload), pdMS_TO_TICKS(120));
        if (err == ESP_OK) {
            uint8_t lost = 0;
            uint8_t retries = 0;
            nrf24_get_lost_and_retries(&lost, &retries);
            ESP_LOGI(TAG, "TX ok msg='%s' len=%u retries=%u lost=%u", (char *)payload, (unsigned)sizeof(payload), retries, lost);
        } else {
            uint8_t status = nrf24_get_status();
            uint8_t lost = 0;
            uint8_t retries = 0;
            nrf24_get_lost_and_retries(&lost, &retries);
            ESP_LOGW(TAG, "TX failed err=%s status=0x%02X retries=%u lost=%u", esp_err_to_name(err), status, retries, lost);
        }

    #if CONFIG_NRF24_TX_POWER_SAVE
        nrf24_power_down();
    #endif
        vTaskDelay(pdMS_TO_TICKS(CONFIG_NRF24_TX_PERIOD_MS));
    }
}
#endif

#if CONFIG_NRF24_PIN_TEST_MODE
static void app_run_pin_test_mode(const nrf24_config_t *cfg)
{
    /* 教学诊断路径：只做 GPIO/PIN 动态测试，不进入正常无线收发。 */
    ESP_LOGW(TAG, "PIN self-test mode enabled. Normal NRF24 TX/RX is disabled.");

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << cfg->pin_ce) | (1ULL << cfg->pin_csn) |
                        (1ULL << cfg->pin_mosi) | (1ULL << cfg->pin_sck),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));

    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << cfg->pin_miso) | (1ULL << cfg->pin_irq),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));

    app_dump_nrf_pins(cfg);

    static nrf24_pin_test_ctx_t pin_test_ctx;
    pin_test_ctx.pin_ce = cfg->pin_ce;
    pin_test_ctx.pin_csn = cfg->pin_csn;
    pin_test_ctx.pin_mosi = cfg->pin_mosi;
    pin_test_ctx.pin_miso = cfg->pin_miso;
    pin_test_ctx.pin_sck = cfg->pin_sck;
    pin_test_ctx.pin_irq = cfg->pin_irq;

    xTaskCreate(app_pin_test_task, "nrf24_pin_test", 3072, &pin_test_ctx, 6, NULL);
}
#endif

#if defined(CONFIG_NRF24_ROLE_RX)
static void app_start_rx_mode(void)
{
    /* RX 模式：初始化队列、挂 IRQ、启动监听和消费任务。 */
    s_irq_evt_queue = xQueueCreate(16, sizeof(uint32_t));
    s_rx_payload_queue = xQueueCreate(16, sizeof(nrf24_rx_payload_t));
    ESP_ERROR_CHECK(s_irq_evt_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(s_rx_payload_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_ERROR_CHECK(nrf24_irq_queue_install(s_irq_evt_queue));
    ESP_ERROR_CHECK(nrf24_start_listening());
    xTaskCreate(app_irq_task, "nrf24_irq", 4096, NULL, 9, NULL);
    xTaskCreate(app_rx_consumer_task, "nrf24_rx", 4096, NULL, 7, NULL);
}
#else
static void app_start_rx_mode(void)
{
    /* 非 RX 角色时，确保无线模块处于非监听状态。 */
    ESP_ERROR_CHECK(nrf24_stop_listening());
}
#endif

#if defined(CONFIG_NRF24_ROLE_TX)
static void app_start_tx_mode(void)
{
    /* TX 模式：可选无 ACK 诊断，随后启动发送任务。 */
#if CONFIG_NRF24_TX_DISABLE_AUTO_ACK_TEST
    ESP_LOGW(TAG, "TX diagnostic mode: auto-ack disabled");
    ESP_ERROR_CHECK(nrf24_set_auto_ack_mask(0x00));
#else
    ESP_LOGI(TAG, "TX mode: auto-ack enabled");
#endif

    xTaskCreate(app_tx_task, "nrf24_tx", 4096, NULL, 8, NULL);
}
#else
static void app_start_tx_mode(void)
{
}
#endif

void app_main(void)
{
    /*
     * 主流程（新手版）：
     * 1) 构建配置
     * 2) 可选进入 PIN 测试
     * 3) 初始化驱动
     * 4) 设置地址和重发参数
     * 5) 按角色启动 RX/TX
     */
    nrf24_config_t cfg = {
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

#if CONFIG_NRF24_PIN_TEST_MODE
    app_run_pin_test_mode(&cfg);
    return;
#endif

    ESP_ERROR_CHECK(nrf24_init(&cfg));

    app_log_startup_config(&cfg);

    ESP_ERROR_CHECK(app_nrf24_setup_addresses());
    ESP_ERROR_CHECK(nrf24_config_retransmit(CONFIG_NRF24_AUTO_RETR_DELAY_US, CONFIG_NRF24_AUTO_RETR_COUNT));

    app_start_rx_mode();
    app_start_tx_mode();

    ESP_LOGI(TAG, "NRF24 app started. role=%s", app_role_name());
}
