# ============================================================================
# NRF24 应用组件 CMake 构建配置
#
# 本文件定义了 "main" 组件的源文件列表、头文件搜索路径和依赖项。
# ESP-IDF 构建系统（基于 CMake）使用此文件来编译和链接本项目的所有业务代码。
#
# 文件结构:
#   SRCS         — 需要编译的 C 源文件列表（按模块分组）。
#   INCLUDE_DIRS — 头文件搜索路径（"." 表示当前 main/ 目录）。
#   REQUIRES     — 本组件依赖的 ESP-IDF 系统组件。
#                   构建系统会根据这些依赖自动链接所需的库。
#
# 依赖项说明:
#   esp_event        — 事件循环，Wi-Fi 初始化需要。
#   esp_driver_gpio  — GPIO 驱动，nrf24 引脚操作和 pin_test 需要。
#   esp_driver_spi   — SPI 驱动，nrf24 与芯片通信需要。
#   esp_netif        — 网络接口抽象，Wi-Fi SoftAP 需要。
#   esp_wifi         — Wi-Fi 驱动，TCP 控制台需要。
#   nvs_flash        — NVS 非易失存储，Wi-Fi 校准数据存储需要。
#   lwip             — 轻量 TCP/IP 协议栈，TCP socket 需要。
# ============================================================================

idf_component_register(
    SRCS
        "app_main.c"          # 应用主入口
        "app_config.c"        # 配置管理（menuconfig → 驱动参数）
        "app_control.c"       # UART 命令控制台
        "app_pin_test.c"      # 引脚硬件自测
        "app_proto.c"         # 自定义通信协议（帧 + CRC）
        "app_rx.c"            # 接收端应用（IRQ + FIFO + 解析）
        "app_stats.c"         # 通信统计（TX/RX 计数器）
        "app_tx.c"            # 发送端应用（burst + MAC + 时隙）
        "app_wifi_control.c"  # Wi-Fi TCP 控制台
        "nrf24.c"             # NRF24 底层驱动（SPI + 寄存器）
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_event
        esp_driver_gpio
        esp_driver_spi
        esp_netif
        esp_wifi
        nvs_flash
        lwip
)
