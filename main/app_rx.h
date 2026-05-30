#pragma once

/*
 * app_rx.h — 接收端（RX）应用层接口
 *
 * 模块职责：
 *   管理 NRF24 接收端的 IRQ 中断事件和 RX FIFO 数据读取。
 *
 * 架构概览（RX 角色）:
 *
 *   NRF24 硬件 IRQ 引脚（下降沿触发）
 *       |
 *       | nrf24_irq_isr (ISR, nrf24.c 中注册)
 *       |   → xQueueSendFromISR: 投递事件到 s_irq_evt_queue
 *       |
 *   app_irq_task (高优先级 FreeRTOS 任务)
 *       |   → 接收 IRQ 事件
 *       |   → 轮询 RX FIFO 读取原始载荷
 *       |   → 投递到 s_rx_payload_queue
 *       |
 *   app_rx_consumer_task (消费者任务)
 *       |   → 接收原始载荷
 *       |   → app_proto_parse_frame: 协议帧解析 + CRC 校验
 *       |   → app_rx_stats_on_parse_result: 更新错误统计
 *       |   → app_rx_stats_on_frame_ok: 更新序列号连续性统计
 *       |   → ESP_LOGI: 打印接收日志（hex + ascii）
 *
 * 双队列设计理由:
 *   - IRQ 事件队列: 解耦 ISR 和 FIFO 轮询，ISR 只做最小工作。
 *   - 载荷队列: 解耦 FIFO 读取和帧解析，避免解析阻塞 FIFO 排空。
 */

/*
 * 启动 RX 接收流程。
 *
 * 当编译为 RX 角色时（CONFIG_NRF24_ROLE_RX 未定义，即非 TX）:
 *   1. 创建 IRQ 事件队列和载荷队列。
 *   2. 安装 IRQ 中断回调（nrf24_irq_queue_install）。
 *   3. 使 NRF24 进入接收监听模式（nrf24_start_listening）。
 *   4. 创建 IRQ 处理任务（app_irq_task，优先级 9）。
 *   5. 创建载荷消费者任务（app_rx_consumer_task，优先级 7）。
 *
 * 当编译为 TX 角色时:
 *   仅调用 nrf24_stop_listening 确保芯片不处于接收模式。
 */
void app_rx_start(void);
