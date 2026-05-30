# C 语言基础 04：函数指针与并发模式

> 文件创建日期: 2026-05-30
> 适用项目: NRF24_1 (ESP32-S3 + NRF24L01+)
> 前置阅读: [c-primer-03-control-flow-and-error-handling.md](c-primer-03-control-flow-and-error-handling.md)
> 后续阅读: [c-primer-05-freertos-tasks-and-queues.md](c-primer-05-freertos-tasks-and-queues.md)

---

## 一、函数指针类型定义

```c
/* app_control.h:47 — 抽象输出通道的回调类型 */
typedef void (*app_control_send_fn_t)(void *user, const char *line);
```

**拆解：**
- `app_control_send_fn_t`：类型别名
- `void (*)(void *, const char *)`：函数指针——指向返回 void、接受 void* + const char* 的函数
- `user`：上下文指针（UART 模式下为 NULL，TCP 模式下为客户 fd）

---

## 二、结构体中的函数指针字段

```c
/* app_control.h:55-58 — I/O 通道抽象 */
typedef struct {
    app_control_send_fn_t send_line; /* 发送一行的函数指针 */
    void                 *user;      /* 通道上下文 */
} app_control_io_t;
```

### UART 通道实现

```c
/* app_control.c:43 — UART 输出 = printf */
static void app_control_send_uart(void *user, const char *line) {
    (void)user;                              /* UART 不需要上下文 */
    printf("%s\n", line);
}

/* 组装 I/O 通道 */
const app_control_io_t io = {
    .send_line = app_control_send_uart,
    .user      = NULL,
};
```

### TCP 通道实现

```c
/* app_wifi_control.c:100 — TCP 输出 = write(fd) */
static void app_control_send_socket(void *user, const char *line) {
    int fd = (int)(intptr_t)user;            /* 从 void* 恢复 fd */
    write(fd, line, strlen(line));
    write(fd, "\n", 1);
}
```

### 调用点：统一的命令处理

```c
/* app_control.c */
void app_control_handle_line(const app_control_io_t *io, char *line) {
    /* ... 解析命令 ... */
    io->send_line(io->user, "OK BURST accepted");  /* 不管通道是 UART 还是 TCP */
}
```

**设计意图：** `app_control_handle_line` 不关心输出走串口还是网络。新增输出通道只需提供一个新的 `send_line` 实现，命令解析逻辑完全复用。

---

## 三、ISR 函数指针注册

```c
/* nrf24.c:803 — 注册 GPIO 中断回调 */
gpio_isr_handler_add(
    s_nrf24.cfg.pin_irq,    /* 中断引脚 */
    nrf24_irq_isr,           /* 回调函数指针 */
    (void *)queue             /* 传递给回调的参数 */
);
```

### ISR 函数：IRAM_ATTR 属性

```c
/* nrf24.c:132 — ISR 必须放在 IRAM 中，确保 flash 操作期间仍可执行 */
static void IRAM_ATTR nrf24_irq_isr(void *arg) {
    QueueHandle_t queue = (QueueHandle_t)arg;
    BaseType_t high_wakeup = pdFALSE;
    uint32_t sig = 1;     /* 信号：有 IRQ 事件发生 */

    xQueueSendFromISR(queue, &sig, &high_wakeup);
    if (high_wakeup == pdTRUE) {
        portYIELD_FROM_ISR();   /* 唤醒更高优先级任务 */
    }
}
```

**IRAM_ATTR 必要性：** ESP32 的 flash 读取和代码执行不能同时进行。若 ISR 代码在 flash 中，当系统正在写 flash 时中断触发，CPU 无法加载 ISR 指令，导致看门狗复位。`IRAM_ATTR` 将 ISR 函数体放在 SRAM 中，确保 flash 操作期间仍可响应中断。

---

## 四、ISR 最小工作量原则

```
中断触发
  │
  ▼
nrf24_irq_isr()          ← ISR 上下文（IRAM_ATTR）
  │
  ├─ 仅做一件事：xQueueSendFromISR(queue, &sig, &high_wakeup)
  │   把一个 uint32_t 信号推入队列
  │
  └─ portYIELD_FROM_ISR()  ← 若唤醒了更高优先级任务
        │
        ▼
app_irq_task()           ← 任务上下文（正常优先级 9）
  │
  ├─ xQueueReceive() 等待信号
  ├─ 排空 RX FIFO（读取多个 payload）
  ├─ 写入载荷队列
  └─ 清除 IRQ 标志
```

**为什么这样设计：**
- ISR 中不能调用大多数 FreeRTOS API（仅限 `...FromISR` 系列）
- ISR 中不能长时间阻塞（影响其他中断响应）
- ISR 中栈有限，不能分配大缓冲区
- 将工作推迟到任务中，可以使用所有 FreeRTOS 设施，也便于调试

---

## 五、volatile 跨任务标志（无锁模式）

### 单生产者单消费者

```c
/* app_tx.c */
static volatile bool s_tx_enabled;  /* 写者：命令任务；读者：TX 任务 */
static volatile bool s_tx_abort;    /* 写者：命令任务；读者：TX 任务 */
```

### 使用流程

```
命令任务（UART/TCP）                    TX 任务（nrf24_tx）
  │                                        │
  │  用户输入 "ENABLE 0"                    │
  ├─ app_tx_set_enabled(false)             │
  │    ├─ s_tx_enabled = false             │
  │    └─ s_tx_abort   = true              │
  │                                        │
  │                                        ├─ while (1) {
  │                                        │     xQueueReceive(...);
  │                                        │     for (...) {
  │                                        │       if (!s_tx_enabled  ← 读到 false
  │                                        │           || s_tx_abort) ← 读到 true
  │                                        │           break;            ← 中止 burst
  │                                        │     }
  │                                        │   }
```

### 为什么不需要互斥锁

- `bool` 类型：单字节，内存访问是原子的（ARM 架构保证对齐的单字节读/写）
- 写者唯一：每个 volatile 变量只有一个写者
- 即时可见：读者在下一个循环迭代立即看到新值

**注意：** 对于更大类型（如 `uint64_t s_slot_seq`），本项目通过单写者设计规避并发问题——TX 任务是唯一写者。

---

## 六、并发模式总结

| 模式 | 应用场景 | 同步机制 |
|------|---------|---------|
| ISR → 任务 | IRQ 引脚触发 → IRQ 任务 | `xQueueSendFromISR` + `xQueueReceive` |
| 任务 → 任务（载荷） | IRQ 任务 → RX 消费任务 | `xQueueSend` + `xQueueReceive` |
| 任务 → 任务（命令） | 控制台 → TX 任务 | `xQueueSend` + `xQueueReceive` |
| 跨任务标志 | 启停/中止控制 | `volatile bool`（单写者） |
| 统计访问 | STATUS 命令读统计 | 单写者 + const 只读接口 |

---

## 七、参考来源

- ESP-IDF GPIO Interrupt 文档 — `IRAM_ATTR` 与 `gpio_isr_handler_add`
- FreeRTOS ISR API 文档 — `xQueueSendFromISR` / `portYIELD_FROM_ISR`
- NRF24 项目源码 `main/nrf24.c` — ISR 实现
- NRF24 项目源码 `main/app_control.h` — 函数指针抽象模式

---

*本文档属于 NRF24_1 项目技术笔记系列，随项目演进持续更新。*
