# Code Deep Dive / 代码深度解析

> 文件创建日期: 2026-05-30
> 最后修订: 2026-05-30
> 适用项目: NRF24_1 (ESP32-S3 + NRF24L01+)

> Function-level intent with surface behavior and deeper reasons.
> 以”函数级”为单位解释表象行为与深层原因。

## How to read / 阅读方式

- English: Each function lists Surface (what it does) and Deeper (why it exists). See [C Language Primer Series](README.md#c-language-primer--c-语言基础系列) for C/RTOS concepts.
- 中文：每个函数包含”表象”和”深层原因”。C/RTOS 基础请先读 [C Primer 系列](README.md#c-language-primer--c-语言基础系列)。

---

## [main/app_main.c](../main/app_main.c)

### `app_main`
- Surface / 表象：构建配置，初始化 NRF24 驱动，启动串口控制、Wi-Fi 控制、RX/TX 任务。
- Deeper / 深层：把“配置-初始化-任务启动”流程集中到单入口，保证启动顺序一致且易于定位问题。
- C 原理：模块化函数调用；通过结构体 `nrf24_config_t` 一次性传参。

---

## [main/app_config.c](../main/app_config.c)

### `app_role_name`
- Surface / 表象：返回当前角色字符串 TX/RX。
- Deeper / 深层：日志与上位机状态输出统一，不必在多处重复判断宏。

### `app_hex_to_addr` (static)
- Surface / 表象：把十六进制字符串转换成地址字节数组。
- Deeper / 深层：避免直接使用字符串，保证地址宽度与寄存器期望一致。
- C 原理：字符分类、指针遍历、位运算拼接高低半字节。

### `app_cfg_data_rate` / `app_cfg_pa_level`
- Surface / 表象：把 menuconfig 选项映射到驱动枚举。
- Deeper / 深层：解耦应用层与驱动层，便于统一修改默认行为。

### `app_cfg_data_rate_name` / `app_cfg_pa_level_name`
- Surface / 表象：输出人类可读的速率/功率字符串。
- Deeper / 深层：提升日志可读性，便于新手对照菜单配置。

### `app_build_nrf24_config`
- Surface / 表象：把所有 menuconfig 选项组装成 `nrf24_config_t`。
- Deeper / 深层：集中配置构建逻辑，避免多个文件重复读取宏。
- C 原理：结构体初始化与值拷贝。

### `app_nrf24_setup_addresses`
- Surface / 表象：配置 pipe0/pipe1/TX 地址并启用接收管道。
- Deeper / 深层：地址配置是 TX/RX 互通的关键步骤，必须在启动收发前完成。

### `app_log_startup_config`
- Surface / 表象：打印关键配置（引脚、速率、功率、重发）。
- Deeper / 深层：日志是新手诊断的第一入口，Tutorial 模式下更全面。

---

## [main/app_proto.c](../main/app_proto.c)

### `app_crc16_ccitt` (static)
- Surface / 表象：计算 CRC16-CCITT 校验。
- Deeper / 深层：为无线链路提供完整性验证，避免误接收。
- C 原理：位运算与循环移位。

### `app_proto_bytes_to_hex`
- Surface / 表象：把二进制 payload 转为 HEX 文本。
- Deeper / 深层：方便串口日志与上位机显示，不影响空口传输格式。

### `app_proto_build_frame`
- Surface / 表象：把业务字段拼成固定帧并追加 CRC。
- Deeper / 深层：将“业务结构”与“空口字节”解耦，避免结构体对齐问题。

### `app_proto_parse_frame`
- Surface / 表象：校验 magic/version/len/CRC 并解析帧字段。
- Deeper / 深层：确保只有正确帧进入业务统计，避免噪声污染。

---

## [main/app_stats.c](../main/app_stats.c)

### `app_stats_reset`
- Surface / 表象：清空 TX/RX 统计。
- Deeper / 深层：保证新一轮测试的统计干净可对比。

### `app_stats_tx` / `app_stats_rx`
- Surface / 表象：返回可写的统计结构体指针。
- Deeper / 深层：统一统计入口，降低全局变量访问分散。

### `app_stats_tx_get` / `app_stats_rx_get`
- Surface / 表象：返回只读统计指针。
- Deeper / 深层：对外暴露读接口，减少误修改。

### `app_rx_stats_on_parse_result`
- Surface / 表象：按解析错误类型累计计数。
- Deeper / 深层：把“解析失败”分桶，方便定位问题。

### `app_rx_stats_on_frame_ok`
- Surface / 表象：更新序号连续性统计（gap/dup/ooo）。
- Deeper / 深层：把链路质量问题量化为统计指标。

---

## [main/app_tx.c](../main/app_tx.c)

### `app_tx_init`
- Surface / 表象：创建 TX 命令队列并启动 TX 任务。
- Deeper / 深层：用队列隔离命令入口和发送执行，避免阻塞控制通道。
- C 原理：FreeRTOS 队列与任务。

### `app_tx_submit_burst`
- Surface / 表象：把 BURST/BURSTHEX 命令转成发送请求并入队。
- Deeper / 深层：统一入口，允许不同控制源共享同一发送路径。

### `app_tx_set_enabled` / `app_tx_abort`
- Surface / 表象：启停发送或中止当前 burst。
- Deeper / 深层：提高调试可控性，避免“失控发送”。
- C 原理：`volatile` 标志位的跨任务通信。

### `app_tx_set_mac_config` / `app_tx_get_mac_config`
- Surface / 表象：设置 ALOHA/CSMA 与概率参数 q。
- Deeper / 深层：提供简单可控的发送门控实验平台。

### `app_tx_set_slot_params` / `app_tx_get_slot_params`
- Surface / 表象：设置时隙长度与 CSMA 窗口。
- Deeper / 深层：把发送时序可配置化，便于实验与教学。

### `app_tx_task` (key path)
- Surface / 表象：按时隙调度发送，构帧，调用 `nrf24_send_payload`。
- Deeper / 深层：把“业务调度”和“驱动发送”清晰分层，便于定位问题。
- C 原理：任务循环、队列阻塞、栈内缓冲区、`vTaskDelayUntil`。

### `app_tx_gate_decide` (static)
- Surface / 表象：根据 CSMA 侦听与概率判定本时隙是否发送。
- Deeper / 深层：模拟简单 MAC 行为，使吞吐与冲突可调。

---

## [main/app_rx.c](../main/app_rx.c)

### `app_rx_start`
- Surface / 表象：创建 IRQ 队列和 RX 队列，启动 IRQ 与消费任务。
- Deeper / 深层：把硬件中断与业务处理解耦，降低 ISR 负担。
- C 原理：队列在 ISR 与任务间传递事件。

### `app_irq_task` (static)
- Surface / 表象：优先处理 IRQ 事件，同时轮询 FIFO 兜底。
- Deeper / 深层：IRQ 可靠性与可用性并存，减少“漏包”可能。

### `app_rx_consumer_task` (static)
- Surface / 表象：解析帧并更新统计，输出日志。
- Deeper / 深层：将解析与统计集中于单任务，便于控制时序和输出。

---

## [main/app_control.c](../main/app_control.c)

### `app_control_start_uart`
- Surface / 表象：启动 UART 命令读取任务。
- Deeper / 深层：把阻塞式输入放在独立任务，避免影响发送路径。

### `app_control_handle_line`
- Surface / 表象：解析命令（STATUS/RESETSTATS/ENABLE/MAC/SLOT/BURST）。
- Deeper / 深层：统一命令入口，串口与 TCP 共用相同逻辑。
- C 原理：字符串原地解析、函数指针输出接口。

### `app_control_reply` / `app_control_replyf`
- Surface / 表象：封装输出接口。
- Deeper / 深层：把输出从解析逻辑中解耦，便于复用。
- C 原理：函数指针 + `va_list` 变参。

---

## [main/app_wifi_control.c](../main/app_wifi_control.c)

### `app_wifi_control_start`
- Surface / 表象：启动 SoftAP 与 TCP 控制任务。
- Deeper / 深层：提供无线控制链路，减少串口依赖。

### `app_tcp_control_task` (static)
- Surface / 表象：接受 TCP 连接、AUTH 认证、转发控制命令。
- Deeper / 深层：通过最小协议提供安全与可控性。
- C 原理：socket API 与阻塞读取。

### `app_socket_read_line` (static)
- Surface / 表象：把 TCP 字节流转为行命令。
- Deeper / 深层：确保命令边界清晰，避免粘包问题。

---

## [main/nrf24.h](../main/nrf24.h)

### Public API groups / 公共 API 分组
- Init and power: `nrf24_init`, `nrf24_deinit`, `nrf24_power_up`, `nrf24_power_down`
- Address and payload: `nrf24_set_tx_address`, `nrf24_set_rx_address`, `nrf24_set_payload_width`
- RX/TX runtime: `nrf24_start_listening`, `nrf24_stop_listening`, `nrf24_send_payload`, `nrf24_read_rx_payload`
- Status and maintenance: `nrf24_clear_irq_flags`, `nrf24_get_irq_status`, `nrf24_flush_tx`, `nrf24_flush_rx`, `nrf24_get_status`
- Diagnostics: `nrf24_get_lost_and_retries`, `nrf24_read_rpd`, `nrf24_carrier_sense`

Surface / 表象：声明驱动能力和结构体“合同”。
Deeper / 深层：稳定的 API 让应用层不直接触碰寄存器细节。

---

## [main/nrf24.c](../main/nrf24.c)

### SPI primitives / SPI 原语
- `nrf24_spi_transfer`, `nrf24_cmd`, `nrf24_write_register`, `nrf24_read_register`, `nrf24_write_buf_reg`, `nrf24_write_payload`, `nrf24_read_payload`
- Surface / 表象：封装 SPI 事务与寄存器访问。
- Deeper / 深层：把硬件细节集中在一个层，应用层只调用高层 API。

### Init and config / 初始化与射频配置
- `nrf24_init`, `nrf24_config_radio`, `nrf24_config_retransmit`
- Surface / 表象：初始化 GPIO、SPI、射频参数与 FIFO。
- Deeper / 深层：把芯片状态固定为“可预测”的初始状态，减少不确定性。

### Power and mode / 供电与模式切换
- `nrf24_power_up`, `nrf24_power_down`, `nrf24_start_listening`, `nrf24_stop_listening`
- Surface / 表象：切换 PRX/PTX 与电源位。
- Deeper / 深层：严格的状态切换避免模式交错导致的收发失败。

### TX path / 发送路径
- `nrf24_send_payload`
- Surface / 表象：装载 payload、触发 CE 脉冲、轮询 IRQ 判断成功或超时。
- Deeper / 深层：用最短路径实现可靠的 TX 反馈，便于教学与调试。
- C 原理：轮询循环、超时处理、错误恢复。

### RX path / 接收路径
- `nrf24_read_rx_payload`
- Surface / 表象：检查 FIFO、读取 payload、清 IRQ。
- Deeper / 深层：避免无效读取并保证 FIFO 状态一致。

### IRQ and queues / IRQ 与队列
- `nrf24_irq_isr`, `nrf24_irq_queue_install`, `nrf24_irq_queue_remove`, `nrf24_get_irq_status`, `nrf24_clear_irq_flags`
- Surface / 表象：中断只投递事件，真正处理在任务中完成。
- Deeper / 深层：降低 ISR 复杂度，提升系统稳定性。

### Diagnostics / 诊断
- `nrf24_get_lost_and_retries`, `nrf24_get_status`, `nrf24_read_rpd`, `nrf24_carrier_sense`
- Surface / 表象：提供链路质量与信道忙闲信息。
- Deeper / 深层：为上层提供“可量化”的链路状态。
