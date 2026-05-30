#include "app_config.h"
#include "app_control.h"
#include "app_pin_test.h"
#include "app_rx.h"
#include "app_tx.h"
#include "app_wifi_control.h"
#include "nrf24.h"

#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"

/*
 * app_main.c — NRF24 应用系统主入口
 *
 * 这是整个固件的启动入口（ESP-IDF 框架中的 app_main），
 * 负责按顺序初始化所有子模块并启动运行时任务。
 *
 * ======== 系统启动流程 ========
 *
 *   app_main()
 *     │
 *     ├─ 1. app_build_nrf24_config(&cfg)
 *     │     从 menuconfig（sdkconfig）读取硬件参数，构建 nrf24_config_t。
 *     │
 *     ├─ 2. [条件] CONFIG_NRF24_PIN_TEST_MODE
 *     │     如果启用了引脚自测模式：
 *     │       app_pin_test_start(&cfg) → 进入 GPIO 引脚测试循环。
 *     │       return（不再执行后续的正常 TX/RX 流程）。
 *     │
 *     ├─ 3. nrf24_init(&cfg)
 *     │     初始化 SPI 总线、GPIO、NRF24 寄存器、射频参数。
 *     │     任何步骤失败会触发 ESP_ERROR_CHECK 的 abort。
 *     │
 *     ├─ 4. app_log_startup_config(&cfg)
 *     │     打印启动配置日志（引脚 + 射频参数）。
 *     │
 *     ├─ 5. app_nrf24_setup_addresses()
 *     │     解析并配置 TX/PIPE0/PIPE1 地址。
 *     │
 *     ├─ 6. nrf24_config_retransmit(delay, count)
 *     │     配置自动重发参数（从 menuconfig 默认值）。
 *     │
 *     ├─ 7. app_control_start_uart()
 *     │     创建 UART 控制台任务（读取 stdin，解析命令）。
 *     │
 *     ├─ 8. app_wifi_control_start()
 *     │     如果启用了 Wi-Fi 控制台：启动 SoftAP + TCP 服务器。
 *     │
 *     ├─ 9. app_rx_start()
 *     │     RX 角色: 创建 IRQ 队列 + 安装 ISR + 进入监听模式 + 创建 RX 任务。
 *     │     TX 角色: 调用 nrf24_stop_listening() 确保芯片处于 PTX 模式。
 *     │
 *     └─ 10. app_tx_init()
 *            TX 角色: 创建命令队列 + 创建 TX 发送任务。
 *            RX 角色: 空操作。
 *
 *   启动完成后，app_main 返回，
 *   FreeRTOS 调度器接管所有已创建的任务并行运行。
 *
 * ======== 任务全景图 ========
 *
 *   任务名称      优先级  角色     职责
 *   ────────────  ─────  ───────  ──────────────────────────
 *   uart_cmd      9      TX/RX    UART 命令行解析
 *   nrf24_irq     9      RX       接收 IRQ + 排空 RX FIFO
 *   nrf24_tx      8      TX       发送 burst 调度
 *   tcp_ctrl      8      TX/RX    TCP Wi-Fi 控制台
 *   nrf24_rx      7      RX       载荷解析 + 统计 + 日志
 *   IDLE          0      TX/RX    空闲任务（FreeRTOS 自动创建）
 *
 *   优先级设计原则:
 *     - UART 和 IRQ 任务优先级最高，保证交互响应和 FIFO 排空。
 *     - TX 和 TCP 任务优先级中等，负责实际收发。
 *     - 消费者任务优先级较低，日志和统计不阻塞实时路径。
 */

static const char *TAG = "nrf24_app";

/*
 * 自动重发参数编译期默认值。
 *
 * 这些值在 menuconfig 的 Kconfig 中有对应选项，
 * 此处作为 fallback（若 menuconfig 未定义对应的宏）。
 *
 * CONFIG_NRF24_AUTO_RETR_DELAY_US = 750:
 *   NRF24 芯片内部以 250us 为单位编码延迟。
 *   750us → 编码值 3 → 实际延迟 (3+1)*250 = 1000us。
 *
 * CONFIG_NRF24_AUTO_RETR_COUNT = 10:
 *   最多自动重发 10 次。
 *   芯片硬件支持最多 15 次（4-bit 寄存器字段）。
 */
#ifndef CONFIG_NRF24_AUTO_RETR_DELAY_US
#define CONFIG_NRF24_AUTO_RETR_DELAY_US 750
#endif

#ifndef CONFIG_NRF24_AUTO_RETR_COUNT
#define CONFIG_NRF24_AUTO_RETR_COUNT 10
#endif

/*
 * ESP-IDF 应用主入口函数。
 *
 * FreeRTOS 在硬件初始化完成后调用此函数。
 * 调用时机：
 *   - 第一阶段 bootloader 验证并加载 app 分区。
 *   - 第二阶段 bootloader 初始化 Flash、PSRAM。
 *   - CPU 启动第二个核心。
 *   - FreeRTOS 初始化并创建 main 任务来运行 app_main。
 */
void app_main(void)
{
    /* 步骤1: 构建 NRF24 驱动配置 */
    nrf24_config_t cfg = {0};
    app_build_nrf24_config(&cfg);

    /*
     * 步骤2: 引脚自测模式检查
     *
     * 当 menuconfig 中启用了 CONFIG_NRF24_PIN_TEST_MODE 时，
     * 固件进入纯 GPIO 测试模式，不断翻转引脚并监测电平。
     * 此模式下 NRF24 正常收发功能完全禁用。
     *
     * 用途：验证 PCB 焊接和 GPIO 映射是否正确。
     */
#if CONFIG_NRF24_PIN_TEST_MODE
    app_pin_test_start(&cfg);
    return;
#endif

    /* 步骤3: 初始化 NRF24 驱动 */
    ESP_ERROR_CHECK(nrf24_init(&cfg));

    /* 步骤4: 打印启动诊断日志 */
    app_log_startup_config(&cfg);

    /* 步骤5-6: 配置地址和重发参数 */
    ESP_ERROR_CHECK(app_nrf24_setup_addresses());
    ESP_ERROR_CHECK(nrf24_config_retransmit(CONFIG_NRF24_AUTO_RETR_DELAY_US, CONFIG_NRF24_AUTO_RETR_COUNT));

    /* 步骤7-8: 启动控制台通道（UART + 可选的 Wi-Fi TCP） */
    app_control_start_uart();
    app_wifi_control_start();

    /* 步骤9: 启动 RX 接收流程（或确保芯片处于 PTX 模式） */
    app_rx_start();

    /* 步骤10: 初始化 TX 发送模块 */
    app_tx_init();

    /*
     * 启动完成日志。
     * role 由 CONFIG_NRF24_ROLE_TX 宏决定。
     */
    ESP_LOGI(TAG, "NRF24 app started. role=%s", app_role_name());
}
