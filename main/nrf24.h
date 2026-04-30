#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * nrf24.h
 *
 * 这个头文件是应用层和驱动层之间的“合同”。
 * 新手可以按下面顺序阅读：
 * 1) 枚举类型（空口速率、发射功率）
 * 2) 配置结构体（引脚、速率、地址宽度、重发参数）
 * 3) 数据结构体（IRQ 状态、RX 载荷）
 * 4) API 分组（初始化、配置、收发、状态）
 */

typedef enum {
    /* 1Mbps：常用默认值，速度与稳定性平衡。 */
    NRF24_DR_1MBPS = 0,
    /* 2Mbps：吞吐高，但链路稳定性对环境更敏感。 */
    NRF24_DR_2MBPS,
    /* 250Kbps：最稳，适合新手联调和较远距离。 */
    NRF24_DR_250KBPS,
} nrf24_data_rate_t;

typedef enum {
    /* 发射功率最小，近距离测试可用。 */
    NRF24_PA_NEG18DBM = 0,
    /* 推荐调试功率，兼顾稳定和干扰控制。 */
    NRF24_PA_NEG12DBM,
    /* 中高发射功率。 */
    NRF24_PA_NEG6DBM,
    /* 最大发射功率。 */
    NRF24_PA_0DBM,
} nrf24_pa_level_t;

typedef struct {
    /* SPI 主机：ESP32 上一般是 SPI2_HOST 或 SPI3_HOST。 */
    spi_host_device_t spi_host;

    /* SPI 线与控制线引脚映射。 */
    gpio_num_t pin_mosi;
    gpio_num_t pin_miso;
    gpio_num_t pin_sck;
    gpio_num_t pin_csn;
    gpio_num_t pin_ce;
    gpio_num_t pin_irq;

    /* SPI 时钟频率（Hz），例如 8MHz。 */
    int spi_clock_hz;

    /* 空口参数。 */
    uint8_t channel;
    uint8_t payload_size;
    uint8_t address_width;
    nrf24_data_rate_t data_rate;
    nrf24_pa_level_t pa_level;

    /* CRC 与载荷策略。 */
    bool crc_2bytes;
    bool enable_dyn_payload;

    /* 自动重发参数：延迟（内部编码步进 250us）与重发次数。 */
    uint8_t retr_delay_us;
    uint8_t retr_count;
} nrf24_config_t;

typedef struct {
    /* STATUS 寄存器原始值，便于进一步调试。 */
    uint8_t status;

    /* IRQ 关键信号位。 */
    bool tx_success;
    bool tx_failed;
    bool rx_ready;
} nrf24_irq_status_t;

typedef struct {
    /* 当前数据来自哪个 RX pipe。 */
    uint8_t pipe;

    /* 有效载荷长度（字节）。 */
    uint8_t len;

    /* 最多 32 字节数据。 */
    uint8_t data[32];
} nrf24_rx_payload_t;

/* Step 1: lifecycle */
/* 初始化驱动、配置 SPI 和基础寄存器。重复调用是安全的。 */
esp_err_t nrf24_init(const nrf24_config_t *cfg);
/* 释放中断和 SPI 资源。 */
void nrf24_deinit(void);

/* Step 2: power state */
/* 进入上电状态（PWR_UP=1），并按配置启用 CRC。 */
esp_err_t nrf24_power_up(void);
/* 进入省电状态（PWR_UP=0），同时拉低 CE。 */
esp_err_t nrf24_power_down(void);

/* Step 3: radio address and payload settings */
/* 设置 TX 地址，长度必须与 address_width 一致。 */
esp_err_t nrf24_set_tx_address(const uint8_t *addr, size_t len);
/* 设置 RX 地址。pipe0/1 需要完整地址，pipe2-5 只需最低字节。 */
esp_err_t nrf24_set_rx_address(uint8_t pipe, const uint8_t *addr, size_t len);
/* 设置每个 pipe 的固定载荷长度。 */
esp_err_t nrf24_set_payload_width(uint8_t pipe, uint8_t width);
/* 使能哪些接收管道（位掩码）。 */
esp_err_t nrf24_enable_rx_pipes(uint8_t mask);
/* 配置自动应答使能位（位掩码）。 */
esp_err_t nrf24_set_auto_ack_mask(uint8_t mask);

/* Step 4: TX/RX runtime mode */
/* 配置自动重发延迟和次数。 */
esp_err_t nrf24_config_retransmit(uint16_t delay_us, uint8_t count);
/* 进入 RX 监听模式（PRIM_RX=1，CE=1）。 */
esp_err_t nrf24_start_listening(void);
/* 退出 RX 监听模式（PRIM_RX=0，CE=0）。 */
esp_err_t nrf24_stop_listening(void);

/* Step 5: data path */
/* 发送一帧载荷并在 wait_ticks 时间内等待结果。 */
esp_err_t nrf24_send_payload(const uint8_t *data, size_t len, TickType_t wait_ticks);
/* 从 RX FIFO 取一帧数据；若无数据返回 ESP_ERR_NOT_FOUND。 */
esp_err_t nrf24_read_rx_payload(nrf24_rx_payload_t *payload);

/* Step 6: status and maintenance */
/* 清除 RX_DR/TX_DS/MAX_RT 中断标志。 */
esp_err_t nrf24_clear_irq_flags(void);
/* 读取并解析 IRQ 状态。 */
esp_err_t nrf24_get_irq_status(nrf24_irq_status_t *irq);
/* 清空 TX FIFO。 */
esp_err_t nrf24_flush_tx(void);
/* 清空 RX FIFO。 */
esp_err_t nrf24_flush_rx(void);

/* 读取 OBSERVE_TX：可拿到丢包计数和重发计数。 */
esp_err_t nrf24_get_lost_and_retries(uint8_t *lost, uint8_t *retries);
/* 读取 STATUS 寄存器原始值。 */
uint8_t nrf24_get_status(void);

/* 读取 RPD（接收功率检测）位：true 表示最近检测到较强信号。 */
esp_err_t nrf24_read_rpd(bool *busy);
/* 进行一次载波侦听：临时切到 RX、等待一小段时间、读取 RPD。 */
esp_err_t nrf24_carrier_sense(uint16_t listen_us, bool *busy);

/* Optional: IRQ queue helper for task-based application design */
/* 安装 IRQ 回调并把事件投递到 queue。 */
esp_err_t nrf24_irq_queue_install(QueueHandle_t queue);
/* 移除 IRQ 回调并清理 ISR 服务。 */
void nrf24_irq_queue_remove(void);
