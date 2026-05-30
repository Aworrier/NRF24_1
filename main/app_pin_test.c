#include "app_pin_test.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/*
 * app_pin_test.c — NRF24 引脚自测实现
 *
 * 模块职责:
 *   通过 bit-bang（软件模拟）方式操作 NRF24 的 GPIO 引脚，
 *   实现对硬件连接的基本验证。
 *
 * 测试原理:
 *   1. GPIO 配置打印: 调用 ESP-IDF 的 gpio_dump_io_configuration 输出
 *      所有相关引脚的当前配置状态（方向、上下拉、中断等）。
 *   2. 电平翻转测试: CE 和 CSN 交替输出高低电平，用万用表或示波器验证。
 *   3. MISO 静态探测: 固定 MOSI 为低和高，读取 MISO 电平，
 *      用于检测 MISO 线是否有上拉/下拉/短路。
 *   4. SPI bit-bang 回环测试: 手动模拟 SPI 时序发送字节，同时读取 MISO。
 *      如果 NRF24 正常应答，MISO 上应有 STATUS 寄存器的回读数据。
 *
 * bit-bang SPI 时序（CPOL=0, CPHA=0）:
 *   SCK 空闲为低电平。CSN=0 选中芯片后：
 *     for each bit (MSB first):
 *       MOSI = bit_value
 *       delay 20us (等待信号稳定)
 *       SCK = 1  (上升沿：NRF24 采样 MOSI，同时驱动 MISO)
 *       delay 20us
 *       读取 MISO
 *       SCK = 0  (下降沿：准备下一个 bit)
 *   CSN = 1 完成传输。
 *
 *   注意 NRF24 的 STATUS 寄存器在每字节传输中总是首先返回
 *   （作为 SPI 第一个字节的 MISO 数据），因此 bit-bang 回读的值
 *   是 STATUS 寄存器的内容，不是 NRF24 内存内容。
 */

static const char *TAG = "nrf24_app";

#if CONFIG_NRF24_PIN_TEST_MODE

/*
 * 打印 NRF24 相关引脚的 GPIO 配置信息。
 *
 * 使用 gpio_dump_io_configuration 打印引脚矩阵中的寄存器状态，
 * 帮助确认引脚是否被正确配置为预期的方向和功能。
 *
 * 参数:
 *   cfg: NRF24 配置（读取所有 6 个引脚的 GPIO 编号）。
 */
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

/*
 * 引脚测试上下文：保存所有引脚的 GPIO 编号。
 */
typedef struct {
    gpio_num_t pin_ce;
    gpio_num_t pin_csn;
    gpio_num_t pin_mosi;
    gpio_num_t pin_miso;
    gpio_num_t pin_sck;
    gpio_num_t pin_irq;
} nrf24_pin_test_ctx_t;

/*
 * 引脚自测任务主函数。
 *
 * 周期性（默认每 CONFIG_NRF24_PIN_TEST_PERIOD_MS 毫秒）执行一轮测试:
 *
 * 每轮测试流程:
 *   1. 翻转 CE 和 CSN 电平（CE=level, CSN=!level）。
 *   2. 静态探测 MISO:
 *      - MOSI=0 时读取 MISO 电平。
 *      - MOSI=1 时读取 MISO 电平。
 *      （如果 MISO 焊接正常且 NRF24 回应，两个读数应可能不同）。
 *   3. 拉低 CSN 选中 NRF24，发送一个预设字节（轮转 0xA5/0x5A/0x3C/0xC3），
 *      同时读取 MISO 的 bit-bang SPI 回读数据。
 *   4. 拉高 CSN 释放芯片。
 *   5. 读取所有 6 个引脚的当前电平值。
 *   6. 输出综合测试日志。
 *   7. 轮转到下一个测试模式字节。
 *   8. 休眠 CONFIG_NRF24_PIN_TEST_PERIOD_MS 毫秒，然后重复。
 *
 * 测试模式字节（轮转）:
 *   0xA5 (10100101): 交替的 1 和 0 的 bit 模式。
 *   0x5A (01011010): 与 0xA5 互补的模式。
 *   0x3C (00111100): 连续 4 个 1 的块。
 *   0xC3 (11000011): 与 0x3C 互补的模式。
 *   不同的 bit 模式有助于检测特定的短路或串扰问题。
 */
static void app_pin_test_task(void *arg)
{
    nrf24_pin_test_ctx_t *ctx = (nrf24_pin_test_ctx_t *)arg;
    int level = 0;
    size_t pattern_idx = 0;
    static const uint8_t patterns[] = {0xA5, 0x5A, 0x3C, 0xC3};

    while (1) {
        /* 翻转 CE/CSN 电平 */
        level = !level;
        int ce_expected = level;
        int csn_expected = !level;
        uint8_t tx_pattern = patterns[pattern_idx];
        uint8_t rx_sample = 0;
        int miso_probe_low = 0;
        int miso_probe_high = 0;

        /* 设置 CE 和 CSN 引脚电平 */
        gpio_set_level(ctx->pin_ce, ce_expected);
        gpio_set_level(ctx->pin_csn, csn_expected);

        /* 静态 MISO 探测 */
        gpio_set_level(ctx->pin_sck, 0);
        gpio_set_level(ctx->pin_mosi, 0);
        esp_rom_delay_us(20);
        miso_probe_low = gpio_get_level(ctx->pin_miso);   /* MOSI=0 时读取 MISO */

        gpio_set_level(ctx->pin_mosi, 1);
        esp_rom_delay_us(20);
        miso_probe_high = gpio_get_level(ctx->pin_miso);  /* MOSI=1 时读取 MISO */

        /*
         * Bit-bang SPI 发送
         *
         * CSN=0 选中 NRF24，然后逐 bit 输出 tx_pattern (MSB first)，
         * 在 SCK 上升沿读取 MISO 状态并移入 rx_sample。
         */
        gpio_set_level(ctx->pin_csn, 0);  /* 拉低 CSN：选中 NRF24 */

        for (int bit = 7; bit >= 0; --bit) {
            int mosi_bit = (tx_pattern >> bit) & 0x01;

            /* 设置 MOSI，准备 SCK 上升沿 */
            gpio_set_level(ctx->pin_mosi, mosi_bit);
            gpio_set_level(ctx->pin_sck, 0);
            esp_rom_delay_us(20);

            /* SCK 上升沿：NRF24 采样 MOSI，同时驱动 MISO */
            gpio_set_level(ctx->pin_sck, 1);
            esp_rom_delay_us(20);

            /* 读取 MISO 并移入接收字节（MSB 在前） */
            rx_sample = (uint8_t)((rx_sample << 1) | (gpio_get_level(ctx->pin_miso) & 0x01));
        }

        gpio_set_level(ctx->pin_sck, 0);   /* SCK 回到空闲低电平 */
        gpio_set_level(ctx->pin_csn, csn_expected); /* 释放 NRF24 */

        /*
         * 输出综合测试日志。
         *
         * 日志字段含义:
         *   CE(exp=X,act=Y):   CE 引脚的预期电平和实际电平。
         *   CSN(exp=X,act=Y):  CSN 引脚的预期电平和实际电平。
         *   SPI(tx=0xXX,rx=0xYY): SPI 发送和回读的字节值。
         *     如果 NRF24 芯片存在且工作正常，rx 值应是 NRF24 STATUS 寄存器内容。
         *   MISOprobe(0->N,1->M): 在固定 MOSI 电平下的 MISO 探测结果。
         *   MOSI/MISO/SCK/IRQ=X: 各引脚的当前实际电平（0 或 1）。
         */
        ESP_LOGI(TAG,
                 "PIN TEST: CE(exp=%d,act=%d) CSN(exp=%d,act=%d) SPI(tx=0x%02X,rx=0x%02X) "
                 "MISOprobe(0->%d,1->%d) MOSI=%d MISO=%d SCK=%d IRQ=%d",
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

        /* 轮转到下一个测试模式 */
        pattern_idx = (pattern_idx + 1) % (sizeof(patterns) / sizeof(patterns[0]));

        /* 休眠指定毫秒数，等待下一轮测试 */
        vTaskDelay(pdMS_TO_TICKS(CONFIG_NRF24_PIN_TEST_PERIOD_MS));
    }
}
#endif

/*
 * 启动引脚自测（对外接口）。
 *
 * 配置所有 6 个 GPIO 引脚:
 *   - CE/CSN/MOSI/SCK: 输入输出模式（GPIO_MODE_INPUT_OUTPUT），
 *     既可输出控制信号，也可回读实际电平。
 *   - MISO/IRQ: 纯输入模式（GPIO_MODE_INPUT），上拉使能，
 *     避免浮空输入造成不确定状态。
 *
 * 然后打印引脚配置并创建自测周期任务。
 *
 * 注意: 此函数仅在 CONFIG_NRF24_PIN_TEST_MODE 启用时有效。
 *       非测试模式下为空函数（参数 cfg 被标记为未使用）。
 */
void app_pin_test_start(const nrf24_config_t *cfg)
{
#if CONFIG_NRF24_PIN_TEST_MODE
    ESP_LOGW(TAG, "PIN self-test mode enabled. Normal NRF24 TX/RX is disabled.");

    /* 配置输出/双向引脚: CE, CSN, MOSI, SCK */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << cfg->pin_ce) | (1ULL << cfg->pin_csn) |
                        (1ULL << cfg->pin_mosi) | (1ULL << cfg->pin_sck),
        .mode = GPIO_MODE_INPUT_OUTPUT,       /* 双向模式：可输出也可回读 */
        .pull_up_en = GPIO_PULLUP_DISABLE,     /* 禁用上拉 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE, /* 禁用下拉 */
        .intr_type = GPIO_INTR_DISABLE,        /* 禁用中断 */
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));

    /* 配置输入引脚: MISO, IRQ */
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << cfg->pin_miso) | (1ULL << cfg->pin_irq),
        .mode = GPIO_MODE_INPUT,               /* 纯输入模式 */
        .pull_up_en = GPIO_PULLUP_ENABLE,      /* 使能上拉：防止浮空 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));

    /* 打印引脚配置到 stdout */
    app_dump_nrf_pins(cfg);

    /* 初始化测试上下文 */
    static nrf24_pin_test_ctx_t pin_test_ctx;
    pin_test_ctx.pin_ce   = cfg->pin_ce;
    pin_test_ctx.pin_csn  = cfg->pin_csn;
    pin_test_ctx.pin_mosi = cfg->pin_mosi;
    pin_test_ctx.pin_miso = cfg->pin_miso;
    pin_test_ctx.pin_sck  = cfg->pin_sck;
    pin_test_ctx.pin_irq  = cfg->pin_irq;

    /* 创建自测周期任务（较低优先级 6，不影响其他系统任务） */
    xTaskCreate(app_pin_test_task, "nrf24_pin_test", 3072, &pin_test_ctx, 6, NULL);
#else
    (void)cfg;
#endif
}
