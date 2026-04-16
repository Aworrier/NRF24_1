#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ble_ctrl.h"
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

#define APP_PROTO_MAGIC0 0xA5
#define APP_PROTO_MAGIC1 0x5A
#define APP_PROTO_VER 0x01
#define APP_PROTO_HEADER_SIZE 8
#define APP_PROTO_CRC_SIZE 2
#define APP_PROTO_MAX_FRAME_SIZE 32
#define APP_PROTO_MAX_USER_PAYLOAD (APP_PROTO_MAX_FRAME_SIZE - APP_PROTO_HEADER_SIZE - APP_PROTO_CRC_SIZE)

typedef struct {
    uint16_t seq;
    uint8_t flags;
    uint8_t payload_len;
    uint8_t payload[APP_PROTO_MAX_USER_PAYLOAD];
} app_proto_frame_t;

typedef struct {
    uint32_t burst_queued;
    uint32_t frame_sent;
    uint32_t tx_ok;
    uint32_t tx_fail;
    uint32_t retries_sum;
    uint32_t retries_max;
    uint16_t next_seq;
} app_tx_stats_t;

typedef struct {
    uint32_t rx_packets;
    uint32_t frame_ok;
    uint32_t crc_fail;
    uint32_t magic_fail;
    uint32_t len_fail;
    uint32_t seq_dup;
    uint32_t seq_out_of_order;
    uint32_t seq_gap;
    bool has_last_seq;
    uint16_t last_seq;
} app_rx_stats_t;

static app_tx_stats_t s_tx_stats = {0};
static app_rx_stats_t s_rx_stats = {0};

typedef enum {
    APP_CMD_SRC_UART = 0,
    APP_CMD_SRC_BLE,
} app_cmd_src_t;

static uint16_t app_crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            if ((crc & 0x8000U) != 0) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static void app_bytes_to_hex(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';

    if (data == NULL || len == 0) {
        return;
    }

    size_t max_bytes = (out_size - 1) / 2;
    if (len > max_bytes) {
        len = max_bytes;
    }

    static const char *hex = "0123456789ABCDEF";
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        out[i * 2] = hex[(b >> 4) & 0x0F];
        out[i * 2 + 1] = hex[b & 0x0F];
    }
    out[len * 2] = '\0';
}

static size_t app_proto_build_frame(uint8_t *out, size_t out_size, const app_proto_frame_t *in)
{
    if (out == NULL || in == NULL || out_size < (APP_PROTO_HEADER_SIZE + APP_PROTO_CRC_SIZE)) {
        return 0;
    }

    size_t max_payload_by_out = out_size - APP_PROTO_HEADER_SIZE - APP_PROTO_CRC_SIZE;
    uint8_t pl = in->payload_len > APP_PROTO_MAX_USER_PAYLOAD ? APP_PROTO_MAX_USER_PAYLOAD : in->payload_len;
    if (pl > max_payload_by_out) {
        pl = (uint8_t)max_payload_by_out;
    }
    size_t used = APP_PROTO_HEADER_SIZE + pl + APP_PROTO_CRC_SIZE;

    memset(out, 0, out_size);
    out[0] = APP_PROTO_MAGIC0;
    out[1] = APP_PROTO_MAGIC1;
    out[2] = APP_PROTO_VER;
    out[3] = (uint8_t)(in->seq & 0xFF);
    out[4] = (uint8_t)((in->seq >> 8) & 0xFF);
    out[5] = pl;
    out[6] = in->flags;
    out[7] = 0;
    if (pl > 0) {
        memcpy(&out[APP_PROTO_HEADER_SIZE], in->payload, pl);
    }

    uint16_t crc = app_crc16_ccitt(out, APP_PROTO_HEADER_SIZE + pl);
    out[APP_PROTO_HEADER_SIZE + pl] = (uint8_t)(crc & 0xFF);
    out[APP_PROTO_HEADER_SIZE + pl + 1] = (uint8_t)((crc >> 8) & 0xFF);
    return used;
}

#if defined(CONFIG_NRF24_ROLE_RX)
static bool app_proto_parse_frame(const uint8_t *buf, size_t len, app_proto_frame_t *out)
{
    if (buf == NULL || out == NULL || len < (APP_PROTO_HEADER_SIZE + APP_PROTO_CRC_SIZE)) {
        s_rx_stats.len_fail++;
        return false;
    }

    if (buf[0] != APP_PROTO_MAGIC0 || buf[1] != APP_PROTO_MAGIC1 || buf[2] != APP_PROTO_VER) {
        s_rx_stats.magic_fail++;
        return false;
    }

    uint8_t pl = buf[5];
    if (pl > APP_PROTO_MAX_USER_PAYLOAD) {
        s_rx_stats.len_fail++;
        return false;
    }

    size_t used = APP_PROTO_HEADER_SIZE + pl + APP_PROTO_CRC_SIZE;
    if (used > len) {
        s_rx_stats.len_fail++;
        return false;
    }

    uint16_t crc_rx = (uint16_t)buf[APP_PROTO_HEADER_SIZE + pl] |
                      ((uint16_t)buf[APP_PROTO_HEADER_SIZE + pl + 1] << 8);
    uint16_t crc_calc = app_crc16_ccitt(buf, APP_PROTO_HEADER_SIZE + pl);
    if (crc_rx != crc_calc) {
        s_rx_stats.crc_fail++;
        return false;
    }

    out->seq = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
    out->payload_len = pl;
    out->flags = buf[6];
    if (pl > 0) {
        memcpy(out->payload, &buf[APP_PROTO_HEADER_SIZE], pl);
    }
    return true;
}
#endif

#if defined(CONFIG_NRF24_ROLE_RX)
static QueueHandle_t s_irq_evt_queue;
static QueueHandle_t s_rx_payload_queue;
#endif

#if defined(CONFIG_NRF24_ROLE_TX)
typedef struct {
    uint32_t count;
    uint32_t interval_ms;
    uint8_t data[32];
    size_t len;
} app_tx_burst_cmd_t;

static QueueHandle_t s_tx_cmd_queue;
static volatile bool s_tx_enabled = true;
static volatile bool s_tx_abort = false;
#endif

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
        /* 从 RX 队列中取包并进行协议帧校验与统计。 */
        if (xQueueReceive(s_rx_payload_queue, &payload, portMAX_DELAY) == pdTRUE) {
            app_proto_frame_t frame = {0};
            s_rx_stats.rx_packets++;

            if (!app_proto_parse_frame(payload.data, payload.len, &frame)) {
                ESP_LOGW(TAG, "RX invalid frame pipe=%u len=%u", payload.pipe, payload.len);
                continue;
            }

            s_rx_stats.frame_ok++;
            if (!s_rx_stats.has_last_seq) {
                s_rx_stats.has_last_seq = true;
                s_rx_stats.last_seq = frame.seq;
            } else {
                uint16_t expected = (uint16_t)(s_rx_stats.last_seq + 1U);
                if (frame.seq == s_rx_stats.last_seq) {
                    s_rx_stats.seq_dup++;
                } else if (frame.seq == expected) {
                    /* 连续帧，统计正常。 */
                } else if (frame.seq > expected) {
                    s_rx_stats.seq_gap += (uint32_t)(frame.seq - expected);
                } else {
                    s_rx_stats.seq_out_of_order++;
                }
                s_rx_stats.last_seq = frame.seq;
            }

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
            app_bytes_to_hex(frame.payload, frame.payload_len, hex, sizeof(hex));

            ESP_LOGI(TAG,
                     "RX ok pipe=%u seq=%u pl=%u hex=%s txt='%s' crc_ok=%lu",
                     payload.pipe,
                     (unsigned)frame.seq,
                     (unsigned)frame.payload_len,
                     hex,
                     txt,
                     (unsigned long)s_rx_stats.frame_ok);
        }
    }
}
#endif

static char *app_trim_left(char *s)
{
    while (*s != '\0' && isspace((int)(unsigned char)*s)) {
        ++s;
    }
    return s;
}

static void app_trim_right(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && isspace((int)(unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        --len;
    }
}

static bool app_parse_hex_payload(const char *hex, uint8_t *out, size_t *out_len)
{
    size_t hex_len = strlen(hex);
    if ((hex_len == 0) || (hex_len % 2 != 0) || (hex_len / 2 > 32)) {
        return false;
    }

    size_t n = hex_len / 2;
    for (size_t i = 0; i < n; ++i) {
        char hi = (char)toupper((int)(unsigned char)hex[i * 2]);
        char lo = (char)toupper((int)(unsigned char)hex[i * 2 + 1]);
        if (!isxdigit((int)hi) || !isxdigit((int)lo)) {
            return false;
        }
        uint8_t v_hi = (uint8_t)(hi <= '9' ? hi - '0' : hi - 'A' + 10);
        uint8_t v_lo = (uint8_t)(lo <= '9' ? lo - '0' : lo - 'A' + 10);
        out[i] = (uint8_t)((v_hi << 4) | v_lo);
    }

    *out_len = n;
    return true;
}

static bool app_parse_u32_token(char **p, uint32_t *out)
{
    char *s = app_trim_left(*p);
    if (*s == '\0') {
        return false;
    }

    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s) {
        return false;
    }

    *out = (uint32_t)v;
    *p = end;
    return true;
}

static void app_send_reply(app_cmd_src_t src, const char *msg)
{
    if (msg == NULL) {
        return;
    }

    if (src == APP_CMD_SRC_UART) {
        printf("%s\n", msg);
        return;
    }

    if (ble_ctrl_send_line(msg) != ESP_OK) {
        ESP_LOGW(TAG, "BLE reply dropped (not connected): %s", msg);
    }
}

static void app_reset_stats(void)
{
    memset(&s_tx_stats, 0, sizeof(s_tx_stats));
    memset(&s_rx_stats, 0, sizeof(s_rx_stats));
}

static void app_reply_stats(app_cmd_src_t src)
{
    char line[192] = {0};

#if defined(CONFIG_NRF24_ROLE_TX)
    snprintf(line,
             sizeof(line),
             "STAT role=TX enabled=%d queued=%lu sent=%lu ack_ok=%lu ack_fail=%lu retries_sum=%lu retries_max=%lu next_seq=%u",
             s_tx_enabled ? 1 : 0,
             (unsigned long)s_tx_stats.burst_queued,
             (unsigned long)s_tx_stats.frame_sent,
             (unsigned long)s_tx_stats.tx_ok,
             (unsigned long)s_tx_stats.tx_fail,
             (unsigned long)s_tx_stats.retries_sum,
             (unsigned long)s_tx_stats.retries_max,
             (unsigned)s_tx_stats.next_seq);
#else
    snprintf(line,
             sizeof(line),
             "STAT role=RX rx_pkt=%lu frame_ok=%lu crc_fail=%lu magic_fail=%lu len_fail=%lu dup=%lu ooo=%lu gap=%lu last_seq=%u",
             (unsigned long)s_rx_stats.rx_packets,
             (unsigned long)s_rx_stats.frame_ok,
             (unsigned long)s_rx_stats.crc_fail,
             (unsigned long)s_rx_stats.magic_fail,
             (unsigned long)s_rx_stats.len_fail,
             (unsigned long)s_rx_stats.seq_dup,
             (unsigned long)s_rx_stats.seq_out_of_order,
             (unsigned long)s_rx_stats.seq_gap,
             (unsigned)s_rx_stats.last_seq);
#endif

    app_send_reply(src, line);
}

static void app_handle_command_line(char *line, app_cmd_src_t src)
{
    app_trim_right(line);
    char *cmd = app_trim_left(line);
    if (*cmd == '\0') {
        return;
    }

    if (strcmp(cmd, "STATUS") == 0) {
        app_reply_stats(src);
        return;
    }

    if (strcmp(cmd, "RESETSTATS") == 0) {
        app_reset_stats();
        app_send_reply(src, "OK RESET");
        return;
    }

    if (strcmp(cmd, "HELP") == 0) {
#if defined(CONFIG_NRF24_ROLE_TX)
        app_send_reply(src, "CMD: ENABLE <0|1>, BURST <count> <interval_ms> <ascii>, BURSTHEX <count> <interval_ms> <hex>, STOP, STATUS, RESETSTATS");
#else
        app_send_reply(src, "CMD: STATUS, RESETSTATS");
#endif
        return;
    }

#if defined(CONFIG_NRF24_ROLE_TX)
    if (strncmp(cmd, "ENABLE", 6) == 0) {
        char *p = cmd + 6;
        uint32_t enabled = 0;
        if (!app_parse_u32_token(&p, &enabled) || (enabled > 1)) {
            app_send_reply(src, "ERR usage: ENABLE <0|1>");
            return;
        }
        s_tx_enabled = (enabled == 1);
        if (!s_tx_enabled) {
            s_tx_abort = true;
        }
        app_send_reply(src, s_tx_enabled ? "OK ENABLED" : "OK DISABLED");
        return;
    }

    if (strcmp(cmd, "STOP") == 0) {
        s_tx_abort = true;
        app_send_reply(src, "OK STOPPED");
        return;
    }

    bool is_hex = false;
    if (strncmp(cmd, "BURSTHEX", 8) == 0) {
        is_hex = true;
        cmd += 8;
    } else if (strncmp(cmd, "BURST", 5) == 0) {
        cmd += 5;
    } else {
        app_send_reply(src, "ERR unknown command");
        return;
    }

    uint32_t count = 0;
    uint32_t interval_ms = 0;
    if (!app_parse_u32_token(&cmd, &count) || count == 0) {
        app_send_reply(src, "ERR invalid count");
        return;
    }
    if (!app_parse_u32_token(&cmd, &interval_ms)) {
        app_send_reply(src, "ERR invalid interval_ms");
        return;
    }

    char *payload = app_trim_left(cmd);
    if (*payload == '\0') {
        app_send_reply(src, "ERR empty payload");
        return;
    }

    app_tx_burst_cmd_t req = {0};
    req.count = count;
    req.interval_ms = interval_ms;

    if (is_hex) {
        if (!app_parse_hex_payload(payload, req.data, &req.len)) {
            app_send_reply(src, "ERR invalid hex payload");
            return;
        }
    } else {
        size_t src_len = strlen(payload);
        req.len = src_len < sizeof(req.data) ? src_len : sizeof(req.data);
        memcpy(req.data, payload, req.len);
    }

    if (xQueueSend(s_tx_cmd_queue, &req, pdMS_TO_TICKS(30)) != pdTRUE) {
        app_send_reply(src, "ERR queue full");
        return;
    }

    s_tx_stats.burst_queued++;

    app_send_reply(src, "OK queued");
#else
    app_send_reply(src, "ERR RX role: only STATUS/RESETSTATS supported");
#endif
}

static void app_uart_cmd_task(void *arg)
{
    (void)arg;

    char line[192] = {0};
    app_send_reply(APP_CMD_SRC_UART, "READY type HELP for commands");

    while (1) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        app_handle_command_line(line, APP_CMD_SRC_UART);
    }
}

static void app_ble_command_cb(const char *line, void *ctx)
{
    (void)ctx;

    if (line == NULL) {
        return;
    }

    char buf[192] = {0};
    size_t cp = strlen(line);
    if (cp >= sizeof(buf)) {
        cp = sizeof(buf) - 1;
    }
    memcpy(buf, line, cp);
    buf[cp] = '\0';

    app_handle_command_line(buf, APP_CMD_SRC_BLE);
}

#if defined(CONFIG_NRF24_ROLE_TX)
static void app_tx_task(void *arg)
{
    (void)arg;
    app_tx_burst_cmd_t burst = {0};

    while (1) {
        if (xQueueReceive(s_tx_cmd_queue, &burst, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!s_tx_enabled) {
            ESP_LOGW(TAG, "TX disabled, drop burst(count=%lu)", (unsigned long)burst.count);
            continue;
        }

        s_tx_abort = false;

        for (uint32_t i = 0; i < burst.count; ++i) {
            if (!s_tx_enabled || s_tx_abort) {
                ESP_LOGW(TAG, "TX burst aborted at %lu/%lu", (unsigned long)i, (unsigned long)burst.count);
                break;
            }

            /* 固定长度载荷发送：不足补零，和静态 payload 配置对齐。 */
#if CONFIG_NRF24_TX_POWER_SAVE
            nrf24_power_up();
#endif
            uint8_t payload[CONFIG_NRF24_PAYLOAD_SIZE] = {0};
            app_proto_frame_t frame = {0};
            frame.seq = s_tx_stats.next_seq++;
            frame.flags = 0;
            frame.payload_len = burst.len > APP_PROTO_MAX_USER_PAYLOAD ? APP_PROTO_MAX_USER_PAYLOAD : (uint8_t)burst.len;
            if (frame.payload_len > 0) {
                memcpy(frame.payload, burst.data, frame.payload_len);
            }

            size_t packed = app_proto_build_frame(payload, sizeof(payload), &frame);
            if (packed == 0) {
                ESP_LOGE(TAG, "TX frame build failed, payload cfg too small");
                s_tx_stats.tx_fail++;
                break;
            }

            esp_err_t err = nrf24_send_payload(payload, sizeof(payload), pdMS_TO_TICKS(120));
            s_tx_stats.frame_sent++;
            if (err == ESP_OK) {
                uint8_t lost = 0;
                uint8_t retries = 0;
                nrf24_get_lost_and_retries(&lost, &retries);
                s_tx_stats.tx_ok++;
                s_tx_stats.retries_sum += retries;
                if (retries > s_tx_stats.retries_max) {
                    s_tx_stats.retries_max = retries;
                }
                ESP_LOGI(TAG,
                         "TX ok burst=%lu/%lu seq=%u retries=%u lost=%u payload=%u",
                         (unsigned long)(i + 1),
                         (unsigned long)burst.count,
                         (unsigned)frame.seq,
                         retries,
                         lost,
                         (unsigned)frame.payload_len);
            } else {
                uint8_t status = nrf24_get_status();
                uint8_t lost = 0;
                uint8_t retries = 0;
                nrf24_get_lost_and_retries(&lost, &retries);
                s_tx_stats.tx_fail++;
                ESP_LOGW(TAG, "TX failed err=%s status=0x%02X retries=%u lost=%u", esp_err_to_name(err), status, retries, lost);
            }

        #if CONFIG_NRF24_TX_POWER_SAVE
            nrf24_power_down();
        #endif
            if (burst.interval_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(burst.interval_ms));
            }
        }
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

    s_tx_cmd_queue = xQueueCreate(8, sizeof(app_tx_burst_cmd_t));
    ESP_ERROR_CHECK(s_tx_cmd_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    xTaskCreate(app_tx_task, "nrf24_tx", 4096, NULL, 8, NULL);
}
#else
static void app_start_tx_mode(void)
{
}
#endif

static void app_start_uart_console(void)
{
#if CONFIG_NRF24_CONTROL_IF_UART
    xTaskCreate(app_uart_cmd_task, "uart_cmd", 4096, NULL, 9, NULL);
#else
    ESP_LOGI(TAG, "UART command interface disabled by config");
#endif
}

static void app_start_ble_console(void)
{
#if CONFIG_NRF24_CONTROL_IF_BLE
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(ble_ctrl_init(CONFIG_NRF24_BLE_DEVICE_NAME, app_ble_command_cb, NULL));
    ESP_LOGI(TAG, "BLE control ready. name=%s", CONFIG_NRF24_BLE_DEVICE_NAME);
#else
    ESP_LOGI(TAG, "BLE control disabled by config");
#endif
}

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

    app_start_uart_console();
    app_start_ble_console();
    app_start_rx_mode();
    app_start_tx_mode();

    ESP_LOGI(TAG, "NRF24 app started. role=%s", app_role_name());
}
