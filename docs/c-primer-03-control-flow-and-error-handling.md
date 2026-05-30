# C 语言基础 03：控制流与错误处理

> 文件创建日期: 2026-05-30
> 适用项目: NRF24_1 (ESP32-S3 + NRF24L01+)
> 前置阅读: [c-primer-02-bit-operations-and-preprocessor.md](c-primer-02-bit-operations-and-preprocessor.md)
> 后续阅读: [c-primer-04-function-pointers-and-concurrency.md](c-primer-04-function-pointers-and-concurrency.md)

---

## 一、static 的作用

### static 函数：文件作用域封装

```c
/* nrf24.c — 所有 SPI 原语为 static，模块外不可见 */
static esp_err_t nrf24_spi_transfer(...);
static esp_err_t nrf24_cmd(...);
static esp_err_t nrf24_write_register(...);
static esp_err_t nrf24_read_register(...);

/* app_control.c — 命令解析内部函数 */
static void app_control_send_uart(...);
static void app_control_reply(...);
static void app_trim_left(...);
static void app_parse_u32_token(...);
static void app_uart_cmd_task(...);

/* app_tx.c — 任务与门控逻辑 */
static void app_gui_stat_init(...);
static void app_tx_task(...);
static app_tx_gate_result_t app_tx_gate_decide(...);
```

**原理：** `static` 限定了函数的链接范围（仅本编译单元可见）。这提供了 C 语言级别的封装——外部模块无法调用、也无法看到函数签名。

### static 变量：模块级状态

```c
/* nrf24.c — 驱动全局上下文 */
static nrf24_ctx_t s_nrf24 = {0};

/* app_stats.c — 全局统计 */
static app_tx_stats_t s_tx_stats;
static app_rx_stats_t s_rx_stats;

/* app_tx.c — 所有运行时状态 */
static QueueHandle_t       s_tx_cmd_queue;
static volatile bool       s_tx_enabled = true;
static volatile bool       s_tx_abort   = false;
static app_mac_mode_t      s_mac_mode   = APP_MAC_ALOHA;
static uint8_t             s_mac_q_percent = 100;
static uint32_t            s_slot_ms    = 20;
static uint64_t            s_slot_seq   = 0;
```

**命名约定：** 项目使用 `s_` 前缀表示模块级 static 变量。

---

## 二、volatile：跨上下文可见性

```c
/* app_tx.c — 命令任务写，TX 任务读 */
static volatile bool s_tx_enabled;
static volatile bool s_tx_abort;
```

### 使用场景

```c
/* 命令任务中（例如 UART 线程） */
void app_tx_set_enabled(bool enabled) {
    s_tx_enabled = enabled;
    if (!enabled) {
        s_tx_abort = true;   /* 同时设置中止标志 */
    }
}

/* TX 任务中（nrf24_tx 线程） */
if (!s_tx_enabled || s_tx_abort) {
    break;  /* 跳过本周期发送 */
}
```

### volatile 的局限性

```c
/* volatile 只保证每次访问都从内存读取，不等于原子操作 */
s_tx_abort = true;   // 赋值是原子的（bool 单字节）
s_slot_seq++;        // 自增不是原子操作（读-改-写），但本项目单写者保证安全
```

**关键理解：**
- `volatile` 告诉编译器"此值可能被异步修改"，禁止将其缓存在寄存器中
- `volatile` ≠ 线程同步，不提供原子性保证
- 本项目通过"单生产者单消费者"模式规避了并发问题

---

## 三、inline 函数

```c
/* nrf24.c — 简短、高频调用的函数适合 inline */
static inline void nrf24_set_ce(int level) {
    gpio_set_level((gpio_num_t)s_nrf24.cfg.pin_ce, level);
}
```

`static inline` 的含义：
- `inline`：建议编译器将函数体嵌入调用点，省去函数调用开销
- `static`：仅在当前编译单元可见，防止多个 .c 文件中同名 inline 函数冲突

本项目不使用头文件中的 `inline`，所有内联函数均定义在 .c 文件中。

---

## 四、ESP-IDF 错误处理体系

### esp_err_t 返回类型

```c
typedef int32_t esp_err_t;  /* ESP-IDF 内部定义 */

/* 成功 */
#define ESP_OK                   0
/* 通用错误 */
#define ESP_FAIL                -1
#define ESP_ERR_INVALID_ARG     -2   /* 参数无效（NULL 指针等） */
#define ESP_ERR_INVALID_STATE   -3
#define ESP_ERR_TIMEOUT         -4   /* 操作超时 */
#define ESP_ERR_NOT_FOUND       -5   /* 资源未找到 */
#define ESP_ERR_INVALID_SIZE    -6
```

项目中所有公开函数均返回 `esp_err_t`：
```c
esp_err_t nrf24_init(const nrf24_config_t *cfg);
esp_err_t nrf24_send_payload(const uint8_t *data, size_t len, TickType_t wait_ticks);
esp_err_t nrf24_read_rx_payload(nrf24_rx_payload_t *payload);
```

### ESP_RETURN_ON_ERROR：错误传播

```c
/* nrf24.c — 每个 SPI 调用后检查错误，失败则立即向上传播 */
ESP_RETURN_ON_ERROR(nrf24_stop_listening(), TAG, "stop listening failed");
ESP_RETURN_ON_ERROR(nrf24_clear_irq_flags(), TAG, "clear irq failed");
ESP_RETURN_ON_ERROR(nrf24_write_payload(data, len, false), TAG, "load failed");
```

**行为：** 若 `expr` 返回值 ≠ `ESP_OK`，则从当前函数返回该错误码。保证错误不漏传、不静默吞掉。

### ESP_RETURN_ON_FALSE：条件检查

```c
ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "null config");
ESP_RETURN_ON_FALSE(len <= 32, ESP_ERR_INVALID_ARG, TAG, "invalid len");
ESP_RETURN_ON_FALSE(payload != NULL, ESP_ERR_INVALID_ARG, TAG, "null payload");
```

**行为：** 若条件为 false，返回指定错误码。常用于参数验证。

### ESP_GOTO_ON_ERROR：清理跳转

```c
/* nrf24.c:nrf24_init — 错误时需要释放已分配资源 */
ESP_GOTO_ON_ERROR(spi_bus_add_device(cfg->spi_host, &dev_cfg, &s_nrf24.spi),
                  err, TAG, "add spi device failed");
ESP_GOTO_ON_ERROR(nrf24_config_radio(), err, TAG, "radio config failed");
/* ... */
return ESP_OK;

err:
    spi_bus_remove_device(s_nrf24.spi);
    spi_bus_free(cfg->spi_host);
    s_nrf24.initialized = false;
    return err;
```

**关键理解：** `ESP_GOTO_ON_ERROR` 在失败时跳转到 `err:` 标签，释放已获取的资源后返回错误码。这是嵌入式 C 中常见的 RAII 模式实现——用 `goto` 做清理，而非 C++ 的析构函数。

### ESP_ERROR_CHECK：致命错误断言

```c
/* app_main.c / app_rx.c / app_wifi_control.c — 初始化阶段 */
ESP_ERROR_CHECK(nrf24_init(&cfg));
ESP_ERROR_CHECK(app_nrf24_setup_addresses());
```

**行为：** 若失败，打印错误信息并调用 `abort()`。用于初始化阶段——此时没有恢复可能，直接终止。

---

## 五、任务主循环（while(1) 模式）

### 经典 FreeRTOS 任务循环

```c
/* 所有 6 个 FreeRTOS 任务采用相同结构 */
static void app_tx_task(void *arg) {
    (void)arg;
    while (1) {
        /* 等待事件/数据 */
        xQueueReceive(s_tx_cmd_queue, &burst, portMAX_DELAY);

        /* 处理 */
        for (uint32_t i = 0; i < count; i++) {
            if (!s_tx_enabled || s_tx_abort) break;
            /* ... */
        }
    }
}
```

本项目中 6 个 `while(1)` 任务循环位置：

| 任务名称 | 文件 | 行 | 优先级 |
|---------|------|-----|-------|
| `app_uart_cmd_task` | `app_control.c` | 541 | 9 |
| `app_irq_task` | `app_rx.c` | 80 | 9 |
| `app_rx_consumer_task` | `app_rx.c` | 158 | 7 |
| `app_tx_task` | `app_tx.c` | 511 | 8 |
| `app_tcp_control_task` | `app_wifi_control.c` | 418 | 8 |
| `app_pin_test_task` | `app_pin_test.c` | 114 | 6 |

---

## 六、switch 与 break/continue

```c
/* nrf24.c — 根据枚举配置寄存器 */
switch (s_nrf24.cfg.data_rate) {
case NRF24_DR_2MBPS:
    rf_setup |= NRF24_RF_DR_HIGH;
    break;
case NRF24_DR_250KBPS:
    rf_setup |= NRF24_RF_DR_LOW;
    break;
default:  /* 1Mbps — 不需要额外位设置 */
    break;
}

/* app_stats.c — 按解析错误类型分桶 */
switch (result) {
case APP_PROTO_PARSE_ERR_LEN:   stats->len_fail++;   break;
case APP_PROTO_PARSE_ERR_MAGIC: stats->magic_fail++; break;
case APP_PROTO_PARSE_ERR_CRC:   stats->crc_fail++;   break;
default: break;
}
```

---

## 七、错误处理模式总结

| 场景 | 使用宏 | 失败行为 |
|------|--------|---------|
| 参数验证 | `ESP_RETURN_ON_FALSE` | 立即返回错误码 |
| 子函数调用 | `ESP_RETURN_ON_ERROR` | 传播子函数错误 |
| 需清理资源 | `ESP_GOTO_ON_ERROR` | 跳转到清理标签 |
| 初始化失败 | `ESP_ERROR_CHECK` | 打印 + abort() |

选择原则：越靠近主函数入口（初始化），越可以用 abort；越靠近驱动层，越需要用返回值向上传播。

---

## 八、参考来源

- ESP-IDF Error Handling 官方文档 — `esp_err_t` 与相关宏
- NRF24 项目源码 `main/nrf24.c` — goto 清理模式
- NRF24 项目源码 `main/app_tx.c` — volatile 标志与任务循环

---

*本文档属于 NRF24_1 项目技术笔记系列，随项目演进持续更新。*
