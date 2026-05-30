#pragma once

/*
 * app_wifi_control.h — WIFI TCP 控制台接口
 *
 * 模块职责：
 *   启动 ESP32 的 SoftAP（软接入点）+ TCP 服务器，
 *   使上位机（PC/手机）能够通过 Wi-Fi 连接并发送命令来控制 NRF24 设备。
 *
 * 工作模式:
 *   ESP32 作为 Wi-Fi 热点（AP 模式），PC/手机作为客户端连接。
 *   热点 SSID 和密码在 menuconfig 中配置。
 *   连接后 PC 通过 TCP 连接到指定端口（默认在 menuconfig 中配置），
 *   发送与 UART 完全相同格式的命令文本。
 *
 * 安全机制:
 *   - 客户端连接后需要先发送 "AUTH <token>" 进行认证，
 *     只有认证通过后才能执行命令。
 *   - token 在 menuconfig 中的 CONFIG_NRF24_CONTROL_TOKEN 配置。
 *   - 认证失败会断开连接。
 *
 * 限制:
 *   - 同时仅支持 1 个 TCP 客户端（ESP32 SoftAP 资源有限）。
 *   - 仅当 CONFIG_NRF24_CONTROL_WIFI_ENABLE 在 menuconfig 中启用时才生效。
 */

/*
 * 启动 Wi-Fi 控制台。
 *
 * 如果 menuconfig 中启用了 CONFIG_NRF24_CONTROL_WIFI_ENABLE：
 *   1. 初始化 NVS（非易失存储，Wi-Fi 子系统需要）。
 *   2. 初始化 TCP/IP 协议栈。
 *   3. 配置并启动 SoftAP。
 *   4. 创建 TCP 服务器任务，监听并处理客户端连接。
 *
 * 如果未启用，此函数为空操作。
 */
void app_wifi_control_start(void);
