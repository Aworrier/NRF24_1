#include "app_config.h"

#include <ctype.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

/*
 * app_config.c — 应用层配置管理实现
 *
 * 模块职责：
 *   1. 编译期 menuconfig → 运行时 nrf24_config_t 的映射。
 *   2. Hex 地址字符串解析（"E7E7E7E7E7" → uint8_t[5]）。
 *   3. NRF24 地址管道配置（TX / PIPE0 / PIPE1）。
 *   4. 启动诊断日志输出。
 *
 * 关键设计点：
 *   - 本模块是编译期常量到运行时的唯一桥梁。所有 CONFIG_NRF24_* 宏的
 *     读取都在本文件中进行，其他模块不应直接引用 sdkconfig。
 *   - 地址字符串在 menuconfig 中以大写 hex 格式存储，app_hex_to_addr
 *     负责解析并转为二进制。
 *   - 地址宽度支持 3/4/5 字节，对应 NRF24 硬件能力。
 */

static const char *TAG = "nrf24_app";

/*
 * 编译期默认地址（当 menuconfig 中未配置 PIPE1 地址时使用）。
 * "C2C2C2C2C2" 是 NRF24 社区常用的默认调试地址。
 */
#ifndef CONFIG_NRF24_PIPE1_ADDR
#define CONFIG_NRF24_PIPE1_ADDR "C2C2C2C2C2"
#endif

/*
 * 自动重发参数的编译期默认值。
 * 如果 menuconfig 中未配置，使用以下：
 *   延迟: 750us（NRF24 内部编码为 250us * 3）
 *   次数: 10 次（芯片支持最多 15 次）
 */
#ifndef CONFIG_NRF24_AUTO_RETR_DELAY_US
#define CONFIG_NRF24_AUTO_RETR_DELAY_US 750
#endif

#ifndef CONFIG_NRF24_AUTO_RETR_COUNT
#define CONFIG_NRF24_AUTO_RETR_COUNT 10
#endif

/*
 * 返回当前编译角色的可读名称。
 *
 * CONFIG_NRF24_ROLE_TX 在 menuconfig 中设置：
 *   启用  → 编译为发送端（TX），启动后可通过控制台发送数据。
 *   禁用 → 编译为接收端（RX），启动后持续监听空中数据。
 */
const char *app_role_name(void)
{
#if defined(CONFIG_NRF24_ROLE_TX)
    return "TX";
#else
    return "RX";
#endif
}

/*
 * 将 hex 字符串解析为原始地址字节数组。
 *
 * 解析规则:
 *   - hex 字符串长度必须恰好为 width * 2。
 *   - 每个字符必须是 0-9 或 A-F（大小写均可）。
 *   - 每两个 hex 字符合成一个字节，高位在前。
 *
 * 示例: "E7E7E7" (width=3) → {0xE7, 0xE7, 0xE7}
 *
 * 参数:
 *   hex:   输入 hex 字符串（以 '\0' 结尾）。
 *   out:   输出字节数组。
 *   width: 期望的地址宽度（字节数）。
 *
 * 返回: true=解析成功, false=格式错误。
 */
static bool app_hex_to_addr(const char *hex, uint8_t *out, size_t width)
{
    if (hex == NULL || out == NULL) {
        return false;
    }

    /* 长度检查：期望 hex 字符串长度恰好为 width*2 */
    size_t hex_len = strlen(hex);
    if (hex_len != width * 2) {
        return false;
    }

    for (size_t i = 0; i < width; ++i) {
        char hi = (char)toupper((int)hex[i * 2]);
        char lo = (char)toupper((int)hex[i * 2 + 1]);

        /* 字符校验：必须是合法的 hex 字符 */
        if (!isxdigit((int)hi) || !isxdigit((int)lo)) {
            return false;
        }

        /* hex 字符 → 数值：
         * '0'-'9' → 0-9
         * 'A'-'F' → 10-15 */
        uint8_t val_hi = (uint8_t)(hi <= '9' ? hi - '0' : hi - 'A' + 10);
        uint8_t val_lo = (uint8_t)(lo <= '9' ? lo - '0' : lo - 'A' + 10);
        out[i] = (uint8_t)((val_hi << 4) | val_lo);
    }
    return true;
}

/*
 * 将 menuconfig 数据速率选项映射为驱动枚举值。
 *
 * 映射关系:
 *   CONFIG_NRF24_DATA_RATE_2M    → NRF24_DR_2MBPS   (2Mbps, 高吞吐)
 *   CONFIG_NRF24_DATA_RATE_250K  → NRF24_DR_250KBPS (250Kbps, 高稳定性)
 *   默认                          → NRF24_DR_1MBPS   (1Mbps, 平衡)
 *
 * 注意: menuconfig 中这三个选项互斥，此函数依赖编译期条件编译。
 */
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

/*
 * 将 menuconfig 发射功率选项映射为驱动枚举值。
 *
 * 映射关系:
 *   CONFIG_NRF24_PA_18DBM → NRF24_PA_NEG18DBM (-18dBm, 最小功率)
 *   CONFIG_NRF24_PA_12DBM → NRF24_PA_NEG12DBM (-12dBm)
 *   CONFIG_NRF24_PA_6DBM  → NRF24_PA_NEG6DBM  (-6dBm)
 *   默认                   → NRF24_PA_0DBM     (0dBm, 最大功率)
 */
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

/*
 * 返回空口速率的可读名称（用于日志输出）。
 */
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

/*
 * 返回发射功率的可读名称（用于日志输出）。
 */
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

/*
 * 构建 NRF24 驱动的完整配置结构体。
 *
 * 此函数是编译期配置 → 运行时的"翻译器"。
 * 所有硬件相关参数在此集中赋值，然后传递给 nrf24_init()。
 *
 * 各字段说明:
 *   spi_host:        使用 SPI2 或 SPI3 主机（取决于 menuconfig 中的 CONFIG_NRF24_SPI_HOST）。
 *   pin_*:           6 个 GPIO 引脚的映射。
 *   spi_clock_hz:    固定 8MHz（NRF24 最高支持 10MHz，8MHz 是保守安全值）。
 *   channel:         空口信道号 (0-125)，对应 2400 + channel MHz。
 *   payload_size:    固定载荷长度（1-32 字节）。
 *   address_width:   地址宽度（3/4/5 字节）。
 *   crc_2bytes:      使用 2 字节 CRC（比 1 字节检测能力更强）。
 *   enable_dyn_payload: 禁用动态载荷（使用固定载荷长度，简化协议设计）。
 *   retr_delay_us:   重发延迟编码值（menuconfig 的微秒值 / 250，映射为芯片内部编码步进）。
 *   retr_count:      自动重发次数上限。
 */
void app_build_nrf24_config(nrf24_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    *cfg = (nrf24_config_t){
        /* SPI 主机选择：SPI3_HOST 或 SPI2_HOST */
        .spi_host = CONFIG_NRF24_SPI_HOST == 3 ? SPI3_HOST : SPI2_HOST,

        /* GPIO 引脚映射：全部来自 menuconfig */
        .pin_mosi = CONFIG_NRF24_PIN_MOSI,
        .pin_miso = CONFIG_NRF24_PIN_MISO,
        .pin_sck  = CONFIG_NRF24_PIN_SCK,
        .pin_csn  = CONFIG_NRF24_PIN_CSN,
        .pin_ce   = CONFIG_NRF24_PIN_CE,
        .pin_irq  = CONFIG_NRF24_PIN_IRQ,

        /* SPI 时钟：8MHz 是经过验证的稳定值 */
        .spi_clock_hz = 8 * 1000 * 1000,

        /* 空口参数 */
        .channel       = CONFIG_NRF24_CHANNEL,       /* 信道号 (0-125) */
        .payload_size  = CONFIG_NRF24_PAYLOAD_SIZE,   /* 载荷大小 (1-32) */
        .address_width = CONFIG_NRF24_ADDR_WIDTH,     /* 地址宽度 (3/4/5) */
        .data_rate     = app_cfg_data_rate(),         /* 空口速率 */
        .pa_level      = app_cfg_pa_level(),          /* 发射功率 */

        /* CRC 策略：使用 2 字节 CRC */
        .crc_2bytes = true,

        /* 禁用动态载荷：使用固定长度载荷简化协议设计 */
        .enable_dyn_payload = false,

        /*
         * 自动重发参数编码：
         * retr_delay_us: menuconfig 中单位为微秒，驱动内部期望的是编码步骤值。
         *   编码公式: step = (delay_us + 249) / 250，芯片手册规定 step ∈ [0, 15]。
         *   例如 delay_us=750 → step=3 → NRF24 寄存器写入 0x03。
         *   实际延迟 = (step + 1) * 250us。step=3 对应 1000us。
         */
        .retr_delay_us = (uint8_t)((CONFIG_NRF24_AUTO_RETR_DELAY_US + 249) / 250),

        /* 自动重发次数上限 (0-15) */
        .retr_count = (uint8_t)CONFIG_NRF24_AUTO_RETR_COUNT,
    };
}

/*
 * 配置 NRF24 地址管道。
 *
 * NRF24 Enhanced ShockBurst 模式的地址机制概述:
 *   - TX 地址：发送数据包的目的地址。
 *   - PIPE0 地址：
 *     在 RX 模式下：接收管道 0 的监听地址。
 *     在 TX 模式下：Enhanced ShockBurst 要求 PIPE0 地址 == TX 地址，
 *       以便接收对端发回的 ACK 包。因为 ACK 包是被当作 pipe0 的接收数据处理的。
 *   - PIPE1 地址（可选）：
 *     提供第二个接收地址，用于多对一星型网络拓扑。
 *     本项目中 PIPE1 主要用于调试和兼容性测试。
 *
 * 流程:
 *   1. 从 menuconfig 的 hex 字符串解析 TX / PIPE0 / PIPE1 地址。
 *   2. 写 TX 地址寄存器 (NRF24_REG_TX_ADDR)。
 *   3. 写 PIPE0 地址寄存器 (NRF24_REG_RX_ADDR_P0)。
 *   4. 如果启用 PIPE1，写 PIPE1 地址并设置使能掩码 0x03（pipe0+pipe1），
 *      否则仅使能 pipe0（掩码 0x01）。
 */
esp_err_t app_nrf24_setup_addresses(void)
{
    uint8_t pipe0[5] = {0};
    uint8_t pipe1[5] = {0};
    uint8_t tx[5]    = {0};
    const size_t aw = CONFIG_NRF24_ADDR_WIDTH;

    /* 解析 TX 地址 hex 字符串 */
    ESP_RETURN_ON_FALSE(app_hex_to_addr(CONFIG_NRF24_PIPE0_ADDR, pipe0, aw),
                        ESP_ERR_INVALID_ARG, TAG, "invalid PIPE0 address");
    ESP_RETURN_ON_FALSE(app_hex_to_addr(CONFIG_NRF24_PIPE1_ADDR, pipe1, aw),
                        ESP_ERR_INVALID_ARG, TAG, "invalid PIPE1 address");
    ESP_RETURN_ON_FALSE(app_hex_to_addr(CONFIG_NRF24_TX_ADDR, tx, aw),
                        ESP_ERR_INVALID_ARG, TAG, "invalid TX address");

    /* 写入 TX 地址和 PIPE0 地址 */
    ESP_RETURN_ON_ERROR(nrf24_set_tx_address(tx, aw), TAG, "set tx address failed");
    ESP_RETURN_ON_ERROR(nrf24_set_rx_address(0, pipe0, aw), TAG, "set pipe0 addr failed");

    ESP_LOGI(TAG, "ADDR cfg: aw=%u pipe0=%s tx=%s", (unsigned)aw,
             CONFIG_NRF24_PIPE0_ADDR, CONFIG_NRF24_TX_ADDR);

    /* 根据 menuconfig 决定是否启用 PIPE1 */
#if CONFIG_NRF24_ENABLE_PIPE1
    ESP_LOGI(TAG, "ADDR cfg: pipe1=%s", CONFIG_NRF24_PIPE1_ADDR);
    ESP_RETURN_ON_ERROR(nrf24_set_rx_address(1, pipe1, aw), TAG, "set pipe1 addr failed");

    /* 使能管道 0 和 1（掩码 bit0=pipe0, bit1=pipe1 → 0x03） */
    ESP_RETURN_ON_ERROR(nrf24_enable_rx_pipes(0x03), TAG, "enable pipes failed");
#else
    /* 仅使能管道 0（掩码 0x01） */
    ESP_RETURN_ON_ERROR(nrf24_enable_rx_pipes(0x01), TAG, "enable pipes failed");
#endif

    return ESP_OK;
}

/*
 * 打印启动配置诊断日志。
 *
 * 在"教程调试模式"（CONFIG_NRF24_MODE_TUTORIAL_DEBUG）下：
 *   输出完整的引脚映射 + 射频参数，方便硬件连接排错。
 *
 * 在普通模式下：
 *   仅输出信道号和数据速率摘要，减少启动日志量。
 *
 * 参数:
 *   cfg: 已初始化的 NRF24 配置结构体。
 */
void app_log_startup_config(const nrf24_config_t *cfg)
{
#if CONFIG_NRF24_MODE_TUTORIAL_DEBUG
    /* 详细模式：打印所有引脚和参数 */
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
    /* 精简模式：仅打印信道和速率 */
    ESP_LOGI(TAG, "RADIO cfg: ch=%u dr=%s", cfg->channel, app_cfg_data_rate_name());
#endif
}
