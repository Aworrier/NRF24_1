# C Language Primer for This Repo / 本仓库的 C 语言基础

## How to use / 使用方式

- English: Read this after [docs/CODE_WALKTHROUGH.md](CODE_WALKTHROUGH.md). It explains the C and FreeRTOS ideas used in the code.
- 中文：建议先读 [docs/CODE_WALKTHROUGH.md](CODE_WALKTHROUGH.md)，再读本页了解代码中用到的 C/FreeRTOS 原理。

## 1. Pointers and arrays / 指针与数组

- English:
  - `char *` and `uint8_t *` are used to pass buffers without copying.
  - Example: `app_control_handle_line` parses a command line in place, moving a `char *` cursor.
  - See [main/app_control.c](../main/app_control.c).
- 中文：
  - `char *` 与 `uint8_t *` 用于传递缓冲区指针，避免拷贝。
  - 例如 `app_control_handle_line` 直接在原地解析命令，通过移动指针完成分词。
  - 参考 [main/app_control.c](../main/app_control.c)。

## 2. Stack vs heap / 栈与堆

- English:
  - Stack is fast and per-task; heap is shared and dynamic.
  - `calloc` in `app_gui_stat_init` allocates statistics buffers on the heap.
  - Freeing is required to avoid leaks; see `free` before re-allocating.
  - See [main/app_tx.c](../main/app_tx.c).
- 中文：
  - 栈是每个任务私有且速度快；堆是共享且动态的。
  - `app_gui_stat_init` 用 `calloc` 在堆上分配统计数组。
  - 重新分配前先 `free`，否则会泄漏。
  - 参考 [main/app_tx.c](../main/app_tx.c)。

## 3. Struct layout and alignment / 结构体布局与对齐

- English:
  - C structs are laid out in memory with alignment rules; padding may exist.
  - `app_proto_frame_t` is a logical frame; the real over-the-air bytes are built explicitly in `app_proto_build_frame`.
  - See [main/app_proto.c](../main/app_proto.c).
- 中文：
  - 结构体在内存中会按对齐规则布局，可能有填充字节。
  - `app_proto_frame_t` 只是逻辑描述，真实空口字节由 `app_proto_build_frame` 手工拼装。
  - 参考 [main/app_proto.c](../main/app_proto.c)。

## 4. Endianness / 字节序

- English:
  - Multi-byte fields must define byte order explicitly.
  - `seq` is serialized as little-endian: low byte first, then high byte.
  - See [main/app_proto.c](../main/app_proto.c).
- 中文：
  - 多字节字段必须明确字节序。
  - `seq` 采用小端序：低字节在前，高字节在后。
  - 参考 [main/app_proto.c](../main/app_proto.c)。

## 5. Bit operations / 位运算

- English:
  - Registers are configured with masks and bit fields.
  - Example: `NRF24_CONFIG_PWR_UP` and friends in `nrf24.c`.
  - See [main/nrf24.c](../main/nrf24.c) and [main/nrf24.h](../main/nrf24.h).
- 中文：
  - 外设寄存器通常用位掩码配置。
  - 例如 `NRF24_CONFIG_PWR_UP` 等位定义用于组合配置。
  - 参考 [main/nrf24.c](../main/nrf24.c) 与 [main/nrf24.h](../main/nrf24.h)。

## 6. Function pointers and callbacks / 函数指针与回调

- English:
  - `app_control_send_fn_t` is a function pointer used to abstract output (UART vs TCP).
  - ISR registration uses a function pointer to `nrf24_irq_isr`.
  - See [main/app_control.h](../main/app_control.h) and [main/nrf24.c](../main/nrf24.c).
- 中文：
  - `app_control_send_fn_t` 通过函数指针抽象输出方式（串口或 TCP）。
  - 中断服务注册使用函数指针指向 `nrf24_irq_isr`。
  - 参考 [main/app_control.h](../main/app_control.h) 与 [main/nrf24.c](../main/nrf24.c)。

## 7. FreeRTOS tasks / FreeRTOS 任务

- English:
  - `xTaskCreate` creates a task with its own stack.
  - Tasks in this repo include TX sender, RX consumer, UART command reader, TCP control.
  - See [main/app_tx.c](../main/app_tx.c), [main/app_rx.c](../main/app_rx.c), [main/app_control.c](../main/app_control.c), [main/app_wifi_control.c](../main/app_wifi_control.c).
- 中文：
  - `xTaskCreate` 创建任务并分配独立栈。
  - 本仓库的任务包括 TX 发送、RX 消费、串口命令、TCP 控制等。
  - 参考 [main/app_tx.c](../main/app_tx.c)、[main/app_rx.c](../main/app_rx.c)、[main/app_control.c](../main/app_control.c)、[main/app_wifi_control.c](../main/app_wifi_control.c)。

## 8. FreeRTOS queues / FreeRTOS 队列

- English:
  - `xQueueCreate` allocates a queue; `xQueueSend`/`xQueueReceive` pass data safely between tasks.
  - IRQ pushes events into a queue using `xQueueSendFromISR` to avoid heavy work in ISR.
  - See [main/app_rx.c](../main/app_rx.c) and [main/nrf24.c](../main/nrf24.c).
- 中文：
  - `xQueueCreate` 创建队列，`xQueueSend`/`xQueueReceive` 在任务间安全传递数据。
  - IRQ 使用 `xQueueSendFromISR` 投递事件，避免在中断中做重逻辑。
  - 参考 [main/app_rx.c](../main/app_rx.c) 与 [main/nrf24.c](../main/nrf24.c)。

## 9. Volatile and shared state / volatile 与共享状态

- English:
  - `volatile` tells the compiler the value can change outside the current flow.
  - `s_tx_enabled` and `s_tx_abort` are toggled by command task and read by TX task.
  - `volatile` prevents aggressive optimization but does not replace proper synchronization.
  - See [main/app_tx.c](../main/app_tx.c).
- 中文：
  - `volatile` 告诉编译器该值可能被异步修改。
  - `s_tx_enabled` 与 `s_tx_abort` 在命令任务中写，在 TX 任务中读。
  - `volatile` 只保证可见性，不等同于完整同步。
  - 参考 [main/app_tx.c](../main/app_tx.c)。

## 10. Time and delays / 时间与延迟

- English:
  - `vTaskDelay` and `vTaskDelayUntil` schedule task sleep in ticks.
  - `esp_rom_delay_us` provides short microsecond delays for hardware timing.
  - See [main/app_tx.c](../main/app_tx.c) and [main/nrf24.c](../main/nrf24.c).
- 中文：
  - `vTaskDelay` 与 `vTaskDelayUntil` 以 tick 为单位休眠。
  - `esp_rom_delay_us` 适合硬件时序微延迟。
  - 参考 [main/app_tx.c](../main/app_tx.c) 与 [main/nrf24.c](../main/nrf24.c)。

## 11. Error handling / 错误处理

- English:
  - ESP-IDF uses `esp_err_t` plus macros like `ESP_ERROR_CHECK` and `ESP_RETURN_ON_ERROR`.
  - These patterns keep control flow explicit and readable.
  - See [main/nrf24.c](../main/nrf24.c) and [main/app_main.c](../main/app_main.c).
- 中文：
  - ESP-IDF 使用 `esp_err_t` 与 `ESP_ERROR_CHECK`、`ESP_RETURN_ON_ERROR` 等宏。
  - 这类模式让错误路径显式、易读。
  - 参考 [main/nrf24.c](../main/nrf24.c) 与 [main/app_main.c](../main/app_main.c)。

## 12. Sockets and TCP / 套接字与 TCP

- English:
  - TCP control uses `socket`, `bind`, `listen`, `accept`, `read`, `write`.
  - Line-based protocol is built on top of the byte stream.
  - See [main/app_wifi_control.c](../main/app_wifi_control.c).
- 中文：
  - TCP 控制基于 `socket`、`bind`、`listen`、`accept`、`read`、`write`。
  - 在字节流之上实现行协议。
  - 参考 [main/app_wifi_control.c](../main/app_wifi_control.c)。
