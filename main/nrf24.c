#include "nrf24.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * nrf24.c
 *
 * 分层视角（便于把代码和无线协议栈对齐）：
 * - 应用层（app_main.c）：决定何时发、发什么、如何统计。
 * - 本文件上半部分：SPI 访问原语 + 寄存器读写（可视为 PHY 控制面）。
 * - 本文件下半部分：收发流程、ACK/重发、IRQ/FIFO（可视为简化 MAC 数据面）。
 *
 * 教学导向阅读建议：
 * 1) 先看命令/寄存器宏，建立芯片操作地图。
 * 2) 再看 nrf24_spi_transfer / read/write_register，理解最小 SPI 事务。
 * 3) 然后看 nrf24_init / nrf24_config_radio，掌握初始化路径。
 * 4) 最后看 nrf24_send_payload / nrf24_read_rx_payload，理解收发流程。
 */

/* SPI 命令字定义。 */
#define NRF24_CMD_R_REGISTER        0x00
#define NRF24_CMD_W_REGISTER        0x20
#define NRF24_CMD_R_RX_PAYLOAD      0x61
#define NRF24_CMD_W_TX_PAYLOAD      0xA0
#define NRF24_CMD_FLUSH_TX          0xE1
#define NRF24_CMD_FLUSH_RX          0xE2
#define NRF24_CMD_REUSE_TX_PL       0xE3
#define NRF24_CMD_R_RX_PL_WID       0x60
#define NRF24_CMD_W_ACK_PAYLOAD     0xA8
#define NRF24_CMD_W_TX_PAYLOAD_NOACK 0xB0
#define NRF24_CMD_NOP               0xFF

/* 常用寄存器地址定义。
 * 备注：RPD 可用于“是否检测到较强信号”的粗略判断（非完整 CSMA/CCA）。
 * 本驱动目前没有在发送前读取 RPD，因此发送路径不是 LBT/CSMA 流程。
 */
#define NRF24_REG_CONFIG            0x00
#define NRF24_REG_EN_AA             0x01
#define NRF24_REG_EN_RXADDR         0x02
#define NRF24_REG_SETUP_AW          0x03
#define NRF24_REG_SETUP_RETR        0x04
#define NRF24_REG_RF_CH             0x05
#define NRF24_REG_RF_SETUP          0x06
#define NRF24_REG_STATUS            0x07
#define NRF24_REG_OBSERVE_TX        0x08
#define NRF24_REG_RPD               0x09
#define NRF24_REG_RX_ADDR_P0        0x0A
#define NRF24_REG_RX_ADDR_P1        0x0B
#define NRF24_REG_RX_ADDR_P2        0x0C
#define NRF24_REG_RX_ADDR_P3        0x0D
#define NRF24_REG_RX_ADDR_P4        0x0E
#define NRF24_REG_RX_ADDR_P5        0x0F
#define NRF24_REG_TX_ADDR           0x10
#define NRF24_REG_RX_PW_P0          0x11
#define NRF24_REG_RX_PW_P1          0x12
#define NRF24_REG_RX_PW_P2          0x13
#define NRF24_REG_RX_PW_P3          0x14
#define NRF24_REG_RX_PW_P4          0x15
#define NRF24_REG_RX_PW_P5          0x16
#define NRF24_REG_FIFO_STATUS       0x17
#define NRF24_REG_DYNPD             0x1C
#define NRF24_REG_FEATURE           0x1D

/* CONFIG 寄存器位定义。 */
#define NRF24_CONFIG_MASK_RX_DR     (1U << 6)
#define NRF24_CONFIG_MASK_TX_DS     (1U << 5)
#define NRF24_CONFIG_MASK_MAX_RT    (1U << 4)
#define NRF24_CONFIG_EN_CRC         (1U << 3)
#define NRF24_CONFIG_CRCO           (1U << 2)
#define NRF24_CONFIG_PWR_UP         (1U << 1)
#define NRF24_CONFIG_PRIM_RX        (1U << 0)

/* STATUS 寄存器位定义。 */
#define NRF24_STATUS_RX_DR          (1U << 6)
#define NRF24_STATUS_TX_DS          (1U << 5)
#define NRF24_STATUS_MAX_RT         (1U << 4)
#define NRF24_STATUS_RX_P_NO_MASK   (7U << 1)

/* RF_SETUP 中空口速率和发射功率相关位。 */
#define NRF24_RF_DR_LOW             (1U << 5)
#define NRF24_RF_DR_HIGH            (1U << 3)

#define NRF24_RF_PWR_LOW            (1U << 1)
#define NRF24_RF_PWR_HIGH           (1U << 2)

/* FIFO 状态位。 */
#define NRF24_FIFO_STATUS_RX_EMPTY  (1U << 0)
#define NRF24_FIFO_STATUS_TX_FULL   (1U << 5)

/* 若上层没给 SPI 时钟，就使用这个默认值。 */
#define NRF24_SPI_DEFAULT_CLOCK_HZ  (8 * 1000 * 1000)

static const char *TAG = "nrf24";

typedef struct {
    /* 驱动是否完成初始化。 */
    bool initialized;

    /* 已绑定的 SPI 设备句柄。 */
    spi_device_handle_t spi;

    /* 初始化时保存的配置副本，后续 API 会直接使用。 */
    nrf24_config_t cfg;

    /* 应用层如果启用 IRQ 队列，这里保存其句柄。 */
    QueueHandle_t irq_queue;
} nrf24_ctx_t;

static nrf24_ctx_t s_nrf24 = {0};

/* CE 是 RF 发送/接收使能脚。
 * - RX 模式：CE=1 持续监听。
 * - TX 模式：CE 给一个短脉冲触发一次发射。
 */
static inline void nrf24_set_ce(int level)
{
    gpio_set_level(s_nrf24.cfg.pin_ce, level);
}

/*
 * IRQ ISR：只做一件事，把事件投递到队列。
 * 原则：ISR 内部不要做重逻辑，真实处理放到任务上下文。
 */
static void IRAM_ATTR nrf24_irq_isr(void *arg)
{
    BaseType_t high_wakeup = pdFALSE;
    QueueHandle_t queue = (QueueHandle_t)arg;
    uint32_t sig = 1;
    if (queue != NULL) {
        xQueueSendFromISR(queue, &sig, &high_wakeup);
    }
    if (high_wakeup == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/*
 * 最小 SPI 事务封装。
 * len 使用字节数，内部转换成 bit 长度给 ESP-IDF 驱动。
 */
static esp_err_t nrf24_spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_transmit(s_nrf24.spi, &t);
}

/* 发送一个简单命令（如 NOP/FLUSH），可选返回 STATUS。 */
static esp_err_t nrf24_cmd(uint8_t cmd, uint8_t *status)
{
    uint8_t tx[2] = {cmd, NRF24_CMD_NOP};
    uint8_t rx[2] = {0};
    ESP_RETURN_ON_ERROR(nrf24_spi_transfer(tx, rx, sizeof(tx)), TAG, "spi cmd failed");
    if (status != NULL) {
        *status = rx[0];
    }
    return ESP_OK;
}

/* 写单字节寄存器。 */
static esp_err_t nrf24_write_register(uint8_t reg, uint8_t val, uint8_t *status)
{
    uint8_t tx[2] = {(uint8_t)(NRF24_CMD_W_REGISTER | (reg & 0x1F)), val};
    uint8_t rx[2] = {0};
    ESP_RETURN_ON_ERROR(nrf24_spi_transfer(tx, rx, sizeof(tx)), TAG, "write reg failed");
    if (status != NULL) {
        *status = rx[0];
    }
    return ESP_OK;
}

/* 读单字节寄存器。 */
static esp_err_t nrf24_read_register(uint8_t reg, uint8_t *val, uint8_t *status)
{
    uint8_t tx[2] = {(uint8_t)(NRF24_CMD_R_REGISTER | (reg & 0x1F)), NRF24_CMD_NOP};
    uint8_t rx[2] = {0};
    ESP_RETURN_ON_ERROR(nrf24_spi_transfer(tx, rx, sizeof(tx)), TAG, "read reg failed");
    if (status != NULL) {
        *status = rx[0];
    }
    *val = rx[1];
    return ESP_OK;
}

/* 写多字节寄存器（地址类寄存器常用）。 */
static esp_err_t nrf24_write_buf_reg(uint8_t reg, const uint8_t *buf, size_t len, uint8_t *status)
{
    ESP_RETURN_ON_FALSE(len <= 32, ESP_ERR_INVALID_ARG, TAG, "invalid len");
    uint8_t tx[33] = {0};
    uint8_t rx[33] = {0};

    tx[0] = (uint8_t)(NRF24_CMD_W_REGISTER | (reg & 0x1F));
    memcpy(&tx[1], buf, len);

    ESP_RETURN_ON_ERROR(nrf24_spi_transfer(tx, rx, len + 1), TAG, "write reg buf failed");
    if (status != NULL) {
        *status = rx[0];
    }
    return ESP_OK;
}

/* 装载 TX payload，支持 no_ack 命令变体。 */
static esp_err_t nrf24_write_payload(const uint8_t *data, size_t len, bool no_ack)
{
    ESP_RETURN_ON_FALSE(len <= 32, ESP_ERR_INVALID_ARG, TAG, "invalid payload len");

    uint8_t tx[33] = {0};
    uint8_t rx[33] = {0};

    /* no_ack=true 时走 W_TX_PAYLOAD_NOACK，可用于单向链路排障。 */
    tx[0] = no_ack ? NRF24_CMD_W_TX_PAYLOAD_NOACK : NRF24_CMD_W_TX_PAYLOAD;
    memcpy(&tx[1], data, len);

    ESP_RETURN_ON_ERROR(nrf24_spi_transfer(tx, rx, len + 1), TAG, "write payload failed");
    return ESP_OK;
}

/* 读取 RX payload。 */
static esp_err_t nrf24_read_payload(uint8_t *data, size_t len, uint8_t *status)
{
    ESP_RETURN_ON_FALSE(len <= 32, ESP_ERR_INVALID_ARG, TAG, "invalid payload len");

    uint8_t tx[33] = {0};
    uint8_t rx[33] = {0};

    tx[0] = NRF24_CMD_R_RX_PAYLOAD;
    memset(&tx[1], NRF24_CMD_NOP, len);

    ESP_RETURN_ON_ERROR(nrf24_spi_transfer(tx, rx, len + 1), TAG, "read payload failed");
    if (status != NULL) {
        *status = rx[0];
    }
    memcpy(data, &rx[1], len);
    return ESP_OK;
}

/* 把 us 单位的重发延迟编码为 NRF24 寄存器字段。 */
static uint8_t nrf24_encode_retr_delay(uint16_t delay_us)
{
    if (delay_us < 250) {
        return 0;
    }
    if (delay_us > 4000) {
        return 0x0F;
    }
    uint16_t step = (uint16_t)((delay_us + 249U) / 250U);
    if (step > 16) {
        step = 16;
    }
    return (uint8_t)(step - 1U);
}

/*
 * 根据配置写入射频参数：信道、速率、功率、地址宽度、重发、动态载荷。
 * 这是初始化阶段最重要的一步。
 */
static esp_err_t nrf24_config_radio(void)
{
    uint8_t reg = 0;

    /* PHY: 固定在单一信道工作；双端必须一致。 */
    ESP_RETURN_ON_ERROR(nrf24_write_register(NRF24_REG_RF_CH, s_nrf24.cfg.channel, NULL), TAG, "set channel failed");

    reg = 0;
    switch (s_nrf24.cfg.data_rate) {
        case NRF24_DR_2MBPS:
            reg |= NRF24_RF_DR_HIGH;
            break;
        case NRF24_DR_250KBPS:
            reg |= NRF24_RF_DR_LOW;
            break;
        case NRF24_DR_1MBPS:
        default:
            break;
    }

    switch (s_nrf24.cfg.pa_level) {
        case NRF24_PA_NEG18DBM:
            break;
        case NRF24_PA_NEG12DBM:
            reg |= NRF24_RF_PWR_LOW;
            break;
        case NRF24_PA_NEG6DBM:
            reg |= NRF24_RF_PWR_HIGH;
            break;
        case NRF24_PA_0DBM:
        default:
            reg |= (NRF24_RF_PWR_HIGH | NRF24_RF_PWR_LOW);
            break;
    }

    /* PHY: 速率 + 发射功率。 */
    ESP_RETURN_ON_ERROR(nrf24_write_register(NRF24_REG_RF_SETUP, reg, NULL), TAG, "set rf setup failed");

    /* 地址宽度编码：3/4/5 字节映射到 1/2/3。 */
    uint8_t setup_aw = (uint8_t)(s_nrf24.cfg.address_width - 2U);
    ESP_RETURN_ON_ERROR(nrf24_write_register(NRF24_REG_SETUP_AW, setup_aw, NULL), TAG, "set addr width failed");

    /* MAC: 自动重发策略（延迟 + 次数）。 */
    ESP_RETURN_ON_ERROR(nrf24_config_retransmit((uint16_t)s_nrf24.cfg.retr_delay_us * 250U, s_nrf24.cfg.retr_count), TAG, "set retransmit failed");

    /* MAC: 默认对全部 pipe 开启 auto-ack。 */
    ESP_RETURN_ON_ERROR(nrf24_write_register(NRF24_REG_EN_AA, 0x3F, NULL), TAG, "enable auto ack failed");

    if (s_nrf24.cfg.enable_dyn_payload) {
        /* FEATURE.EN_DPL + DYNPD：动态载荷。 */
        ESP_RETURN_ON_ERROR(nrf24_write_register(NRF24_REG_FEATURE, 0x04, NULL), TAG, "set feature failed");
        ESP_RETURN_ON_ERROR(nrf24_write_register(NRF24_REG_DYNPD, 0x3F, NULL), TAG, "set dynpd failed");
    } else {
        ESP_RETURN_ON_ERROR(nrf24_write_register(NRF24_REG_FEATURE, 0x00, NULL), TAG, "clear feature failed");
        ESP_RETURN_ON_ERROR(nrf24_write_register(NRF24_REG_DYNPD, 0x00, NULL), TAG, "clear dynpd failed");
    }

    /* 静态载荷模式下，每个 pipe 都写固定 payload 宽度。 */
    for (uint8_t pipe = 0; pipe < 6; ++pipe) {
        ESP_RETURN_ON_ERROR(nrf24_set_payload_width(pipe, s_nrf24.cfg.payload_size), TAG, "set payload width failed");
    }

    return ESP_OK;
}

/*
 * 初始化入口：GPIO -> SPI -> 上电 -> 无线参数 -> 清 FIFO/中断。
 * 任何步骤失败都会跳转 err 分支，释放已经申请的资源。
 */
esp_err_t nrf24_init(const nrf24_config_t *cfg)
{
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is null");
    ESP_RETURN_ON_FALSE(cfg->payload_size > 0 && cfg->payload_size <= 32, ESP_ERR_INVALID_ARG, TAG, "invalid payload size");
    ESP_RETURN_ON_FALSE(cfg->address_width >= 3 && cfg->address_width <= 5, ESP_ERR_INVALID_ARG, TAG, "invalid address width");

    if (s_nrf24.initialized) {
        return ESP_OK;
    }

    s_nrf24.cfg = *cfg;

    gpio_config_t ce_cfg = {
        .pin_bit_mask = 1ULL << cfg->pin_ce,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&ce_cfg), TAG, "ce gpio config failed");
    nrf24_set_ce(0);

    gpio_config_t irq_cfg = {
        .pin_bit_mask = 1ULL << cfg->pin_irq,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&irq_cfg), TAG, "irq gpio config failed");

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = cfg->pin_mosi,
        .miso_io_num = cfg->pin_miso,
        .sclk_io_num = cfg->pin_sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };

    ESP_RETURN_ON_ERROR(spi_bus_initialize(cfg->spi_host, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "spi bus init failed");

    spi_device_interface_config_t dev_cfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .mode = 0,
        .clock_speed_hz = cfg->spi_clock_hz > 0 ? cfg->spi_clock_hz : NRF24_SPI_DEFAULT_CLOCK_HZ,
        .spics_io_num = cfg->pin_csn,
        .queue_size = 2,
    };

    ESP_GOTO_ON_ERROR(spi_bus_add_device(cfg->spi_host, &dev_cfg, &s_nrf24.spi), err, TAG, "spi add dev failed");

    ESP_GOTO_ON_ERROR(nrf24_power_up(), err, TAG, "power up failed");
    ESP_GOTO_ON_ERROR(nrf24_config_radio(), err, TAG, "config radio failed");
    ESP_GOTO_ON_ERROR(nrf24_clear_irq_flags(), err, TAG, "clear irq failed");
    ESP_GOTO_ON_ERROR(nrf24_flush_rx(), err, TAG, "flush rx failed");
    ESP_GOTO_ON_ERROR(nrf24_flush_tx(), err, TAG, "flush tx failed");

    s_nrf24.initialized = true;
    ESP_LOGI(TAG, "nrf24 init done (ch=%u, payload=%u)", cfg->channel, cfg->payload_size);
    return ESP_OK;

err:
    if (s_nrf24.spi != NULL) {
        spi_bus_remove_device(s_nrf24.spi);
        s_nrf24.spi = NULL;
    }
    spi_bus_free(cfg->spi_host);
    memset(&s_nrf24, 0, sizeof(s_nrf24));
    return ret;
}

/* 反初始化：释放 IRQ 和 SPI 资源。 */
void nrf24_deinit(void)
{
    if (!s_nrf24.initialized) {
        return;
    }

    nrf24_irq_queue_remove();
    nrf24_power_down();

    if (s_nrf24.spi != NULL) {
        spi_bus_remove_device(s_nrf24.spi);
        s_nrf24.spi = NULL;
    }
    spi_bus_free(s_nrf24.cfg.spi_host);

    s_nrf24.initialized = false;
}

/* 上电并配置 CRC 位。 */
esp_err_t nrf24_power_up(void)
{
    uint8_t config = 0;
    ESP_RETURN_ON_ERROR(nrf24_read_register(NRF24_REG_CONFIG, &config, NULL), TAG, "read config failed");
    config |= NRF24_CONFIG_EN_CRC | NRF24_CONFIG_PWR_UP;
    if (s_nrf24.cfg.crc_2bytes) {
        config |= NRF24_CONFIG_CRCO;
    }
    ESP_RETURN_ON_ERROR(nrf24_write_register(NRF24_REG_CONFIG, config, NULL), TAG, "write config failed");
    vTaskDelay(pdMS_TO_TICKS(2));
    return ESP_OK;
}

/* 省电模式：先拉低 CE，再清除 PWR_UP。 */
esp_err_t nrf24_power_down(void)
{
    uint8_t config = 0;
    ESP_RETURN_ON_ERROR(nrf24_read_register(NRF24_REG_CONFIG, &config, NULL), TAG, "read config failed");
    config &= (uint8_t)~NRF24_CONFIG_PWR_UP;
    nrf24_set_ce(0);
    return nrf24_write_register(NRF24_REG_CONFIG, config, NULL);
}

/* 写 TX 地址。 */
esp_err_t nrf24_set_tx_address(const uint8_t *addr, size_t len)
{
    ESP_RETURN_ON_FALSE(addr != NULL, ESP_ERR_INVALID_ARG, TAG, "null addr");
    ESP_RETURN_ON_FALSE(len == s_nrf24.cfg.address_width, ESP_ERR_INVALID_ARG, TAG, "invalid tx addr len");
    return nrf24_write_buf_reg(NRF24_REG_TX_ADDR, addr, len, NULL);
}

/* 写 RX 地址：pipe0/1 全地址，pipe2-5 低字节。 */
esp_err_t nrf24_set_rx_address(uint8_t pipe, const uint8_t *addr, size_t len)
{
    ESP_RETURN_ON_FALSE(pipe < 6, ESP_ERR_INVALID_ARG, TAG, "invalid pipe");
    ESP_RETURN_ON_FALSE(addr != NULL, ESP_ERR_INVALID_ARG, TAG, "null addr");

    if (pipe <= 1) {
        ESP_RETURN_ON_FALSE(len == s_nrf24.cfg.address_width, ESP_ERR_INVALID_ARG, TAG, "invalid rx addr len");
        return nrf24_write_buf_reg((uint8_t)(NRF24_REG_RX_ADDR_P0 + pipe), addr, len, NULL);
    }

    ESP_RETURN_ON_FALSE(len == 1, ESP_ERR_INVALID_ARG, TAG, "pipe2-5 require 1 byte");
    return nrf24_write_register((uint8_t)(NRF24_REG_RX_ADDR_P0 + pipe), addr[0], NULL);
}

/* 设置固定载荷长度。 */
esp_err_t nrf24_set_payload_width(uint8_t pipe, uint8_t width)
{
    ESP_RETURN_ON_FALSE(pipe < 6, ESP_ERR_INVALID_ARG, TAG, "invalid pipe");
    ESP_RETURN_ON_FALSE(width <= 32, ESP_ERR_INVALID_ARG, TAG, "invalid width");
    return nrf24_write_register((uint8_t)(NRF24_REG_RX_PW_P0 + pipe), width, NULL);
}

/* 使能接收管道位图。 */
esp_err_t nrf24_enable_rx_pipes(uint8_t mask)
{
    return nrf24_write_register(NRF24_REG_EN_RXADDR, (uint8_t)(mask & 0x3F), NULL);
}

/* 使能自动应答位图。 */
esp_err_t nrf24_set_auto_ack_mask(uint8_t mask)
{
    return nrf24_write_register(NRF24_REG_EN_AA, (uint8_t)(mask & 0x3F), NULL);
}

/* 配置自动重发延迟和次数。 */
esp_err_t nrf24_config_retransmit(uint16_t delay_us, uint8_t count)
{
    uint8_t delay_field = nrf24_encode_retr_delay(delay_us);
    uint8_t value = (uint8_t)((delay_field << 4) | (count & 0x0F));
    return nrf24_write_register(NRF24_REG_SETUP_RETR, value, NULL);
}

/* 进入 RX 监听状态。 */
esp_err_t nrf24_start_listening(void)
{
    uint8_t config = 0;

    /* 进入 PRX：PRIM_RX=1 + PWR_UP=1，然后拉高 CE 持续监听。 */
    ESP_RETURN_ON_ERROR(nrf24_read_register(NRF24_REG_CONFIG, &config, NULL), TAG, "read config failed");
    config |= NRF24_CONFIG_PRIM_RX | NRF24_CONFIG_PWR_UP;
    ESP_RETURN_ON_ERROR(nrf24_write_register(NRF24_REG_CONFIG, config, NULL), TAG, "write config failed");
    ESP_RETURN_ON_ERROR(nrf24_clear_irq_flags(), TAG, "clear irq failed");
    nrf24_set_ce(1);
    vTaskDelay(pdMS_TO_TICKS(2));
    return ESP_OK;
}

/* 退出 RX 监听状态。 */
esp_err_t nrf24_stop_listening(void)
{
    uint8_t config = 0;

    /* 退出 PRX：先 CE=0，再清 PRIM_RX，防止模式切换中误触发。 */
    nrf24_set_ce(0);
    ESP_RETURN_ON_ERROR(nrf24_read_register(NRF24_REG_CONFIG, &config, NULL), TAG, "read config failed");
    config &= (uint8_t)~NRF24_CONFIG_PRIM_RX;
    ESP_RETURN_ON_ERROR(nrf24_write_register(NRF24_REG_CONFIG, config, NULL), TAG, "write config failed");
    vTaskDelay(pdMS_TO_TICKS(1));
    return ESP_OK;
}

/*
 * 发送一帧数据并等待结果。
 * 成功条件：TX_DS 置位。
 * 失败条件：MAX_RT 置位或等待超时。
 */
esp_err_t nrf24_send_payload(const uint8_t *data, size_t len, TickType_t wait_ticks)
{
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "null payload");

    nrf24_irq_status_t irq = {0};
    const TickType_t poll_interval = pdMS_TO_TICKS(2);

    /* TX 状态机（简化 MAC）：
     * 1) 切到 PTX（stop_listening）。
     * 2) 清中断，写入 payload。
     * 3) CE 脉冲触发一次空口发送。
     * 4) 轮询 IRQ：TX_DS 成功，MAX_RT 失败。
     *
     * 新手测试改动点：
     * - 想验证 ACK 依赖：可配合上层关闭 EN_AA，再看成功率变化。
     * - 想加“发送前侦听”：可在第 2 步前插入 RPD 读取 + 随机退避。
     * - 想调时序鲁棒性：可微调 CE 脉冲宽度或 poll_interval。
     */

    ESP_RETURN_ON_ERROR(nrf24_stop_listening(), TAG, "stop listening failed");
    ESP_RETURN_ON_ERROR(nrf24_clear_irq_flags(), TAG, "clear irq failed");
    ESP_RETURN_ON_ERROR(nrf24_write_payload(data, len, false), TAG, "load tx payload failed");

    nrf24_set_ce(1);
    esp_rom_delay_us(15);
    nrf24_set_ce(0);

    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < wait_ticks) {
        ESP_RETURN_ON_ERROR(nrf24_get_irq_status(&irq), TAG, "get irq failed");
        if (irq.tx_success) {
            ESP_RETURN_ON_ERROR(nrf24_clear_irq_flags(), TAG, "clear irq failed");
            return ESP_OK;
        }
        if (irq.tx_failed) {
            ESP_RETURN_ON_ERROR(nrf24_clear_irq_flags(), TAG, "clear irq failed");
            ESP_RETURN_ON_ERROR(nrf24_flush_tx(), TAG, "flush tx failed");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(poll_interval);
    }

    ESP_RETURN_ON_ERROR(nrf24_flush_tx(), TAG, "flush tx failed");
    return ESP_ERR_TIMEOUT;
}

/*
 * 从 RX FIFO 读取一帧。
 * 返回 ESP_ERR_NOT_FOUND 表示当前没有可读数据。
 */
esp_err_t nrf24_read_rx_payload(nrf24_rx_payload_t *payload)
{
    ESP_RETURN_ON_FALSE(payload != NULL, ESP_ERR_INVALID_ARG, TAG, "null payload");

    uint8_t status = 0;
    uint8_t fifo = 0;

    /* 先看 FIFO 是否为空，避免无效 SPI 读 payload。 */
    ESP_RETURN_ON_ERROR(nrf24_read_register(NRF24_REG_FIFO_STATUS, &fifo, &status), TAG, "read fifo failed");
    if (fifo & NRF24_FIFO_STATUS_RX_EMPTY) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t pipe_no = (uint8_t)((status & NRF24_STATUS_RX_P_NO_MASK) >> 1);
    if (pipe_no > 5) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 静态载荷：长度来自配置；动态载荷：长度来自 R_RX_PL_WID。 */
    uint8_t len = s_nrf24.cfg.payload_size;
    if (s_nrf24.cfg.enable_dyn_payload) {
        uint8_t tx[2] = {NRF24_CMD_R_RX_PL_WID, NRF24_CMD_NOP};
        uint8_t rx[2] = {0};
        ESP_RETURN_ON_ERROR(nrf24_spi_transfer(tx, rx, sizeof(tx)), TAG, "read dyn len failed");
        len = rx[1];
        /* 芯片手册要求：动态长度异常时应 FLUSH_RX 以恢复 FIFO 状态。 */
        if (len == 0 || len > 32) {
            nrf24_flush_rx();
            return ESP_ERR_INVALID_SIZE;
        }
    }

    payload->pipe = pipe_no;
    payload->len = len;
    ESP_RETURN_ON_ERROR(nrf24_read_payload(payload->data, len, &status), TAG, "read payload failed");
    /* 读取完成后清 IRQ，通知上层“该帧已处理”。 */
    ESP_RETURN_ON_ERROR(nrf24_clear_irq_flags(), TAG, "clear irq failed");

    return ESP_OK;
}

/* 写 STATUS 清中断位。 */
esp_err_t nrf24_clear_irq_flags(void)
{
    /* 写 1 清除 RX_DR/TX_DS/MAX_RT。 */
    return nrf24_write_register(NRF24_REG_STATUS, NRF24_STATUS_RX_DR | NRF24_STATUS_TX_DS | NRF24_STATUS_MAX_RT, NULL);
}

/* 读取并解析 IRQ 状态位。 */
esp_err_t nrf24_get_irq_status(nrf24_irq_status_t *irq)
{
    ESP_RETURN_ON_FALSE(irq != NULL, ESP_ERR_INVALID_ARG, TAG, "null irq");

    uint8_t status = 0;
    ESP_RETURN_ON_ERROR(nrf24_cmd(NRF24_CMD_NOP, &status), TAG, "nop failed");

    irq->status = status;
    irq->rx_ready = (status & NRF24_STATUS_RX_DR) != 0;
    irq->tx_success = (status & NRF24_STATUS_TX_DS) != 0;
    irq->tx_failed = (status & NRF24_STATUS_MAX_RT) != 0;

    return ESP_OK;
}

/* 清 TX FIFO。 */
esp_err_t nrf24_flush_tx(void)
{
    return nrf24_cmd(NRF24_CMD_FLUSH_TX, NULL);
}

/* 清 RX FIFO。 */
esp_err_t nrf24_flush_rx(void)
{
    return nrf24_cmd(NRF24_CMD_FLUSH_RX, NULL);
}

/* 读取 OBSERVE_TX，导出丢包和重发计数。 */
esp_err_t nrf24_get_lost_and_retries(uint8_t *lost, uint8_t *retries)
{
    uint8_t value = 0;

    /* OBSERVE_TX:
     * [7:4] PLOS_CNT 累计丢包计数
     * [3:0] ARC_CNT  最近一次发送的自动重发次数
     */
    ESP_RETURN_ON_ERROR(nrf24_read_register(NRF24_REG_OBSERVE_TX, &value, NULL), TAG, "read observe failed");
    if (lost != NULL) {
        *lost = (uint8_t)((value >> 4) & 0x0F);
    }
    if (retries != NULL) {
        *retries = (uint8_t)(value & 0x0F);
    }
    return ESP_OK;
}

/* 读取 STATUS 原始值，适合调试日志。 */
uint8_t nrf24_get_status(void)
{
    uint8_t status = 0;
    nrf24_cmd(NRF24_CMD_NOP, &status);
    return status;
}

/* 安装 IRQ 事件队列。 */
esp_err_t nrf24_irq_queue_install(QueueHandle_t queue)
{
    ESP_RETURN_ON_FALSE(queue != NULL, ESP_ERR_INVALID_ARG, TAG, "null queue");
    s_nrf24.irq_queue = queue;
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(s_nrf24.cfg.pin_irq, nrf24_irq_isr, (void *)queue), TAG, "irq add failed");
    return ESP_OK;
}

/* 移除 IRQ 事件队列与 ISR 绑定。 */
void nrf24_irq_queue_remove(void)
{
    if (s_nrf24.cfg.pin_irq >= 0) {
        gpio_isr_handler_remove(s_nrf24.cfg.pin_irq);
    }
    if (s_nrf24.irq_queue != NULL) {
        gpio_uninstall_isr_service();
        s_nrf24.irq_queue = NULL;
    }
}
