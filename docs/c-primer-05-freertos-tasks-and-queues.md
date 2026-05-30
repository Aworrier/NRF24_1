# C 语言基础 05：FreeRTOS 任务与队列

> 文件创建日期: 2026-05-30
> 适用项目: NRF24_1 (ESP32-S3 + NRF24L01+)
> 前置阅读: [c-primer-04-function-pointers-and-concurrency.md](c-primer-04-function-pointers-and-concurrency.md)
> 后续阅读: [c-primer-06-protocol-and-esp-idf.md](c-primer-06-protocol-and-esp-idf.md)

---

## 一、任务创建 xTaskCreate

### 函数签名

```c
BaseType_t xTaskCreate(
    TaskFunction_t  pvTaskCode,    /* 任务入口函数 */
    const char     *pcName,        /* 任务名（调试用，限 16 字符） */
    uint32_t        usStackDepth,  /* 栈大小（字，非字节） */
    void           *pvParameters,  /* 传递给任务入口的参数 */
    UBaseType_t     uxPriority,    /* 优先级（数值越大越高） */
    TaskHandle_t   *pxCreatedTask  /* 任务句柄（本项目传 NULL） */
);
```

### 本项目所有任务一览

| 任务名 | 入口函数 | 栈大小 | 优先级 | 职责 | 文件 |
|--------|---------|--------|-------|------|------|
| `uart_cmd` | `app_uart_cmd_task` | 4096 | 9 | UART 命令读取 | `app_control.c:565` |
| `nrf24_irq` | `app_irq_task` | 4096 | 9 | IRQ 事件处理 + FIFO 排空 | `app_rx.c:236` |
| `nrf24_tx` | `app_tx_task` | 4096 | 8 | TX 时隙调度与发送 | `app_tx.c:794` |
| `tcp_ctrl` | `app_tcp_control_task` | 4096 | 8 | TCP 命令监听 | `app_wifi_control.c:504` |
| `nrf24_rx` | `app_rx_consumer_task` | 4096 | 7 | 帧解析与统计 | `app_rx.c:239` |
| `nrf24_pin_test` | `app_pin_test_task` | 3072 | 6 | 引脚自检 | `app_pin_test.c:254` |

### 优先级层级设计

```
高优先 (9): uart_cmd, nrf24_irq    ← 不能丢命令和中断
中优先 (8): nrf24_tx, tcp_ctrl     ← 核心发送与网络
中低 (7):   nrf24_rx               ← 帧解析可以稍微滞后
最低 (6):   nrf24_pin_test         ← 仅诊断模式
空闲 (0):   IDLE                    ← 系统自动创建
```

### 典型创建代码

```c
/* app_rx.c — 创建 IRQ 任务和 RX 消费任务 */
xTaskCreate(app_irq_task,          "nrf24_irq", 4096, NULL, 9, NULL);
xTaskCreate(app_rx_consumer_task,  "nrf24_rx",  4096, NULL, 7, NULL);
```

---

## 二、任务删除

```c
/* app_wifi_control.c:386 — 套接字创建失败时，任务自身终止 */
if (sock < 0) {
    vTaskDelete(NULL);    /* NULL = 删除当前任务 */
}
```

---

## 三、任务延时 API

### vTaskDelay：相对延时

```c
/* nrf24.c — 等待芯片上电稳定 */
vTaskDelay(pdMS_TO_TICKS(2));    /* 阻塞 2ms */

/* nrf24_send_payload — TX 轮询间隔 */
vTaskDelay(poll_interval);       /* 每轮等待 2ms */

/* app_control.c — fgets 失败后退避 */
vTaskDelay(pdMS_TO_TICKS(20));

/* app_pin_test.c — 引脚测试周期间隔 */
vTaskDelay(pdMS_TO_TICKS(CONFIG_NRF24_PIN_TEST_PERIOD_MS));
```

`vTaskDelay` 从调用时刻起延时 N tick，不补偿任务执行时间。若任务被抢占或执行时间波动，可能累积漂移。

### vTaskDelayUntil：精确周期延时

```c
/* app_tx.c — 时隙调度（必须精确） */
static void app_wait_slots(uint32_t n, TickType_t *last_wake,
                            TickType_t slot_ticks) {
    for (uint32_t i = 0; i < n; i++) {
        TickType_t now = xTaskGetTickCount();
        last_wake = xTaskGetTickCount();      // 记录本轮基准
        vTaskDelayUntil(&now, slot_ticks);    // 以绝对时间等待，补偿漂移
        s_slot_seq++;
    }
}
```

**vTaskDelay vs vTaskDelayUntil：**

```
vTaskDelay(N):                    vTaskDelayUntil(&t, N):
  启动 ─┬─ [任务 2ms] ─┬─ [delay 10ms] ─┬─ [任务 3ms] ─┬─ [delay 10ms]...
        │              │                │              │
  实际间隔: 12ms             实际间隔: 13ms → 漂移！

  启动 ─┬─ [任务 2ms] ─┬─ [delay 到 t+N] ─┬─ [任务 3ms] ─┬─ [delay 到 t+2N]...
        │              │                   │              │
  实际间隔: 10ms (补偿了 2ms)    实际间隔: 10ms (补偿了 3ms) → 精确！
```

---

## 四、队列创建

```c
/* 参数: (队列深度, 每个元素大小) */
QueueHandle_t q = xQueueCreate(16, sizeof(uint32_t));
```

### 本项目所有队列

| 队列 | 深度 | 元素类型 | 用途 | 位置 |
|------|------|---------|------|------|
| `s_irq_evt_queue` | 16 | `uint32_t` | ISR → IRQ 任务传递事件信号 | `app_rx.c:224` |
| `s_rx_payload_queue` | 16 | `nrf24_rx_payload_t` | IRQ 任务 → RX 消费任务传递载荷 | `app_rx.c:225` |
| `s_tx_cmd_queue` | 8 | `app_tx_burst_cmd_t` | 控制台 → TX 任务传递 burst 命令 | `app_tx.c:791` |

---

## 五、队列发送

### 任务间发送 xQueueSend

```c
/* app_rx.c — 非阻塞发送（队列满则丢包） */
xQueueSend(s_rx_payload_queue, &payload, 0);  /* 超时 0 = 非阻塞 */

/* app_tx.c — 带超时发送 */
xQueueSend(s_tx_cmd_queue, &req, pdMS_TO_TICKS(30));  /* 30ms 超时 */
```

### ISR 中发送 xQueueSendFromISR

```c
/* nrf24.c:138 — 仅 ISR 中可调用 */
BaseType_t high_wakeup = pdFALSE;
xQueueSendFromISR(queue, &sig, &high_wakeup);
if (high_wakeup == pdTRUE) {
    portYIELD_FROM_ISR();   /* 若唤醒了更高优先级的接收任务 */
}
```

`high_wakeup` 机制：若接收任务优先级 > 当前被中断的任务，`high_wakeup` 被设为 `pdTRUE`，ISR 退出后立即调度接收任务而非恢复被中断的任务。

---

## 六、队列接收

### 无限阻塞接收

```c
/* app_tx.c:513 — TX 任务等待 burst 命令（可无限等） */
xQueueReceive(s_tx_cmd_queue, &burst, portMAX_DELAY);

/* app_rx.c:160 — RX 消费任务等待载荷（可无限等） */
xQueueReceive(s_rx_payload_queue, &payload, portMAX_DELAY);
```

### 超时接收

```c
/* app_rx.c:87 — IRQ 任务 100ms 超时接收（用于周期性 FIFO 轮询） */
if (xQueueReceive(s_irq_evt_queue, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
    /* IRQ 触发：立即排空 FIFO */
} else {
    /* 超时：进行 FIFO 轮询兜底 */
}
```

---

## 七、超时类型转换

```c
/* 毫秒 → FreeRTOS tick */
TickeType_t ticks = pdMS_TO_TICKS(100);    /* 100ms 转换为 tick */

/* 无限等待 */
#define portMAX_DELAY  ((TickType_t)0xffffffffUL)
```

`pdMS_TO_TICKS` 根据 `configTICK_RATE_HZ`（通常 100Hz=10ms/tick）将毫秒转为 tick。最小非零延迟为 1 tick。

---

## 八、xTaskGetTickCount

```c
/* nrf24.c:602 — 计算发送等待是否超时 */
TickType_t start = xTaskGetTickCount();
while ((xTaskGetTickCount() - start) < wait_ticks) {
    /* 轮询 IRQ 状态 */
}

/* app_tx.c:508 — 记录时隙基准时间 */
TickType_t last_wake = xTaskGetTickCount();
```

FreeRTOS tick 计数器从调度器启动开始单调递增，溢出后回绕（32 位无符号）。本项目依赖"差值 < 2^31"的溢出安全性（无符号减法的模运算性质）。

---

## 九、数据流全景图

```
┌─────────────────────────────────────────────────────────────────┐
│                          TX 路径                                  │
│                                                                   │
│  UART/TCP 输入                                                    │
│    │                                                              │
│    ├─ app_control_handle_line()                                   │
│    │     └─ app_tx_submit_burst()                                 │
│    │           └─ xQueueSend(s_tx_cmd_queue, ...)                │
│    │                                                              │
│    ▼                                                              │
│  s_tx_cmd_queue ──────► app_tx_task()                            │
│   (深度 8)                 │                                      │
│                            ├─ xQueueReceive(portMAX_DELAY)        │
│                            ├─ 时隙循环 (vTaskDelayUntil)          │
│                            ├─ app_tx_gate_decide() MAC 门控      │
│                            ├─ app_proto_build_frame()             │
│                            └─ nrf24_send_payload()                │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                          RX 路径                                  │
│                                                                   │
│  NRF24 IRQ 引脚                                                   │
│    │                                                              │
│    ├─ nrf24_irq_isr() [IRAM_ATTR]                                 │
│    │     └─ xQueueSendFromISR(s_irq_evt_queue, ...)              │
│    │                                                              │
│    ▼                                                              │
│  s_irq_evt_queue ──────► app_irq_task()                          │
│   (深度 16)                 │                                     │
│                            ├─ 排空 RX FIFO (nrf24_read_rx_payload)│
│                            └─ xQueueSend(s_rx_payload_queue, ...)│
│                                   │                               │
│                                   ▼                               │
│  s_rx_payload_queue ───► app_rx_consumer_task()                  │
│   (深度 16)                 │                                     │
│                            ├─ app_proto_parse_frame()             │
│                            └─ app_rx_stats_on_frame_ok()          │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 十、参考来源

- FreeRTOS API Reference — Task, Queue, and ISR APIs
- Mastering the FreeRTOS Real Time Kernel — Chapter 4 (Task Management), Chapter 5 (Queue Management)
- NRF24 项目源码 `main/app_tx.c` — `vTaskDelayUntil` 精确时隙
- NRF24 项目源码 `main/nrf24.c` — `xQueueSendFromISR` ISR 通信

---

*本文档属于 NRF24_1 项目技术笔记系列，随项目演进持续更新。*
