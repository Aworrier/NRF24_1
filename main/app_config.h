#pragma once

#include "nrf24.h"

/*
 * app_config.h — 应用层配置管理接口
 *
 * 模块职责：
 *   将 ESP-IDF menuconfig（Kconfig）中的编译期配置映射为 NRF24 驱动的运行时参数。
 *   本模块是整个项目的"配置翻译层"——它读取 sdkconfig 中的宏定义，
 *   构造 nrf24_config_t 结构体，供 app_main 初始化驱动使用。
 *
 * 配置来源：
 *   - 引脚映射（MOSI/MISO/SCK/CSN/CE/IRQ）：来自 menuconfig 的 CONFIG_NRF24_PIN_*。
 *   - 空口参数（信道/速率/功率/地址宽度/载荷大小）：来自 menuconfig。
 *   - MAC 参数（重发延迟/次数）：来自 menuconfig，有编译期默认值。
 *
 * 两套配置体系说明：
 *   - 编译期静态配置（menuconfig → sdkconfig → CONFIG_* 宏）：
 *     定义引脚、信道、地址等"固定不变"的硬件参数。修改需要重新编译烧录。
 *   - 运行时动态配置（UART/TCP 命令）：
 *     定义 MAC 模式（ALOHA/CSMA）、时隙参数、burst 参数。可在设备运行时通过控制台修改。
 */

/*
 * 返回当前编译角色的可读名称。
 * 返回: "TX" 或 "RX"，取决于 CONFIG_NRF24_ROLE_TX 是否定义。
 */
const char *app_role_name(void);

/*
 * 将 menuconfig 中的速率选项映射为驱动枚举值。
 * 返回: NRF24_DR_1MBPS / NRF24_DR_2MBPS / NRF24_DR_250KBPS。
 */
nrf24_data_rate_t app_cfg_data_rate(void);

/*
 * 将 menuconfig 中的发射功率选项映射为驱动枚举值。
 * 返回: NRF24_PA_NEG18DBM ~ NRF24_PA_0DBM。
 */
nrf24_pa_level_t app_cfg_pa_level(void);

/*
 * 返回空口速率的可读名称字符串（用于启动日志）。
 * 返回: "1Mbps" / "2Mbps" / "250Kbps"。
 */
const char *app_cfg_data_rate_name(void);

/*
 * 返回发射功率的可读名称字符串（用于启动日志）。
 * 返回: "-18dBm" / "-12dBm" / "-6dBm" / "0dBm"。
 */
const char *app_cfg_pa_level_name(void);

/*
 * 根据 menuconfig 构建 NRF24 驱动所需的完整配置结构体。
 *
 * 这是 app_main 在初始化阶段第一个调用的配置函数，
 * 它汇集所有编译期参数填充 nrf24_config_t。
 *
 * 参数:
 *   cfg: 输出参数，指向待填充的配置结构体（调用者分配）。
 *
 * 填充内容包括:
 *   - SPI 引脚映射（MOSI/MISO/SCK/CSN/CE/IRQ）
 *   - SPI 时钟频率（固定 8MHz）
 *   - 空口信道、载荷大小、地址宽度
 *   - 数据速率、发射功率
 *   - CRC 配置（2 字节 CRC，禁用动态载荷）
 *   - 自动重发参数
 */
void app_build_nrf24_config(nrf24_config_t *cfg);

/*
 * 配置 NRF24 的地址管道。
 *
 * 流程:
 *   1. 从 menuconfig 的 hex 字符串（如 "E7E7E7E7E7"）解析 TX 和 PIPE0 的地址字节。
 *   2. 写入 TX 地址寄存器。
 *   3. 写入 PIPE0 地址寄存器（RX 模式必备）。
 *   4. 如果启用了 PIPE1，同样解析写入并设置使能掩码为 0x03（pipe0+pipe1），
 *      否则仅使能 pipe0（掩码 0x01）。
 *
 * 地址说明:
 *   - TX 地址：发送端写入 NRF24_REG_TX_ADDR，发送数据包的目标地址。
 *   - PIPE0 地址：接收端管道 0 地址，RX 模式下监听此地址的数据。
 *     注意 ESP32 的 Enhanced ShockBurst 模式下，TX 也需要 PIPE0 地址
 *     与 TX 地址一致，用于接收 ACK 包。
 *   - PIPE1 地址：可选的第二接收管道地址（多对一通信）。
 *
 * 返回值:
 *   ESP_OK: 地址配置成功。
 *   其他:   hex 字符串解析失败或 SPI 写入失败。
 */
esp_err_t app_nrf24_setup_addresses(void);

/*
 * 打印启动配置日志。
 *
 * 在调试模式（CONFIG_NRF24_MODE_TUTORIAL_DEBUG）下打印完整引脚映射和射频参数；
 * 在普通模式下仅打印信道和速率摘要信息。
 *
 * 参数:
 *   cfg: 已初始化的 NRF24 配置结构体指针。
 */
void app_log_startup_config(const nrf24_config_t *cfg);
