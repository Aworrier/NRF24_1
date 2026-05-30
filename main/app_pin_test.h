#pragma once

#include "nrf24.h"

/*
 * app_pin_test.h — NRF24 引脚自测接口
 *
 * 模块职责：
 *   提供一个纯 GPIO 层的硬件引脚测试模式。
 *   当启用此模式时，固件不会初始化 NRF24 驱动，
 *   而是通过手动操作 GPIO 来验证：
 *     - 引脚焊接是否正确（无短路/断路）。
 *     - 电平输出是否符合预期（高/低翻转）。
 *     - SPI 信号线（MOSI/MISO/SCK/CSN）是否能正常通信。
 *
 * 使用场景:
 *   - 新 PCB 上电后验证硬件连接。
 *   - 排查 NRF24 初始化失败是否是硬件引脚问题。
 *   - 用示波器或逻辑分析仪观察信号波形。
 *
 * 注意:
 *   - 此模式通过 menuconfig 中的 CONFIG_NRF24_PIN_TEST_MODE 启用。
 *   - 启用后正常 NRF24 收发功能完全禁用。
 *   - 日志输出到 UART，可通过串口监视器观察。
 */

/*
 * 启动引脚自测任务。
 *
 * 参数:
 *   cfg: NRF24 配置（仅使用其中的引脚映射字段，不初始化 SPI/NRF24）。
 *
 * 测试内容:
 *   1. 调用 gpio_dump_io_configuration 打印引脚配置。
 *   2. 创建周期任务（默认间隔由 CONFIG_NRF24_PIN_TEST_PERIOD_MS 指定）:
 *      a. 翻转 CE/CSN 引脚电平（CE=high 时 CSN=low，CE=low 时 CSN=high）。
 *      b. 探测 MISO 引脚在不同 MOSI 电平下的状态。
 *      c. 通过手动 bit-bang SPI 发送预设的字节模式并回读 MISO。
 *      d. 读取所有引脚的当前电平并输出日志。
 */
void app_pin_test_start(const nrf24_config_t *cfg);
