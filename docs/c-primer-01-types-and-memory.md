# C 语言基础 01：类型、内存与数据结构

> 文件创建日期: 2026-05-30
> 适用项目: NRF24_1 (ESP32-S3 + NRF24L01+)
> 前置阅读: [CODE_WALKTHROUGH.md](CODE_WALKTHROUGH.md)
> 后续阅读: [c-primer-02-bit-operations-and-preprocessor.md](c-primer-02-bit-operations-and-preprocessor.md)

---

## 一、整数类型

本项目使用精确宽度整数类型，避免平台相关的 `int`/`long` 大小差异。

### 常用类型一览

| 类型 | 宽度 | 用途 | 典型位置 |
|------|------|------|---------|
| `uint8_t` | 8 bit | 寄存器值、管道号、载荷字节、标志位 | `nrf24.c` 所有寄存器操作 |
| `uint16_t` | 16 bit | CRC 累加器、序列号、统计阈值 | `app_proto.c` 帧序列号字段 |
| `uint32_t` | 32 bit | 统计计数器（tx_ok, frame_ok 等） | `app_stats.h` 所有计数器 |
| `uint64_t` | 64 bit | 单调时隙计数器、GPIO 位掩码 | `app_tx.c` `s_slot_seq` |
| `size_t` | 平台相关 | 缓冲区长度、`sizeof` 结果 | 所有 `memcpy`/`memset` 调用 |
| `bool` | 1 byte | 配置开关、状态标志 | `<stdbool.h>`，所有模块 |
| `TickType_t` | FreeRTOS 内部 | 系统 tick 计数、超时值 | `nrf24_send_payload` 参数 |
| `BaseType_t` | FreeRTOS 内部 | ISR 唤醒标志类型 | `xQueueSendFromISR` 中 `high_wakeup` |

### 类型转换要点

```c
/* 缩小转换需要显式强转 */
uint32_t val = get_value();
uint8_t  byte = (uint8_t)(val & 0xFF);   // app_proto.c seq 打包

/* 寄存器命令需要组合并截断到 8 bit */
uint8_t cmd = (uint8_t)(NRF24_CMD_W_REGISTER | (reg & 0x1F));  // nrf24.c

/* printf 格式化需要匹配类型 */
printf("count=%lu\n", (unsigned long)u32_val);   // 32-bit → unsigned long
printf("seq=%u\n",   (unsigned)u16_val);          // 16-bit → unsigned int
```

**关键理解：** 项目不使用 `int`/`long` 存储持久数据——所有数据结构字段均为精确宽度，确保跨平台一致性。

---

## 二、指针与缓冲区传递

### 输出参数模式（指针传出）

```c
/* nrf24.h: 调用者提供结构体内存，函数通过指针填充 */
esp_err_t nrf24_read_rx_payload(nrf24_rx_payload_t *payload);
esp_err_t nrf24_get_irq_status(nrf24_irq_status_t *irq);
esp_err_t nrf24_read_rpd(bool *busy);

/* app_proto.h: 输出缓冲区由调用者分配，函数写入 */
size_t app_proto_build_frame(uint8_t *out, size_t out_size,
                             const app_proto_frame_t *in);
```

**原理：** 被调函数不负责分配内存，调用者明确控制内存生命周期（栈或堆），避免忘记释放。

### 输入指针（const 保护）

```c
esp_err_t nrf24_init(const nrf24_config_t *cfg);           // 只读配置
esp_err_t nrf24_send_payload(const uint8_t *data, size_t len, ...); // 只读载荷
```

`const` 向读者保证"我不会修改你传入的数据"，是一份编译器级别的合同。

### 指针解引用赋值的值拷贝

```c
/* nrf24.c:363 — 将配置整体复制到静态上下文 */
static nrf24_ctx_t s_nrf24 = {0};
s_nrf24.cfg = *cfg;   // 结构体值拷贝，非指针共享
```

### 空指针防御

```c
ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "null config");
ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, TAG, "null payload");
```

所有公开 API 都通过 `ESP_RETURN_ON_FALSE` 检查空指针，避免因调用者错误导致崩溃。

### 指针转换（void* 通用性）

```c
/* ISR 回调传入队列句柄 */
gpio_isr_handler_add(pin, nrf24_irq_isr, (void *)queue);

/* 从 void* 还原 */
QueueHandle_t queue = (QueueHandle_t)arg;

/* 整数到指针的技巧（TCP fd 传递） */
(void *)(intptr_t)client_fd;   // app_wifi_control.c
```

---

## 三、数组与字符串

### 定长缓冲区（栈分配）

```c
/* nrf24.c: SPI 事务缓冲区，最大 33 字节（1 命令 + 32 载荷） */
uint8_t tx[33], rx[33];

/* app_control.c: 命令解析行的栈缓冲区 */
char line[192];    // UART 命令任务
char line[256];    // app_control_replyf 格式化缓冲

/* app_tx.c: 载荷存储 */
uint8_t data[32];  // NRF24 最大 32 字节
```

**原理：** 栈缓冲区无分配/释放开销，函数返回时自动回收。适合固定上限的小缓冲区。

### 字符串操作函数清单

| 函数 | 用途 | 位置 |
|------|------|------|
| `strlen(hex)` | 验证 hex 字符串长度 = width*2 | `app_hex_to_addr` |
| `strcmp(cmd, "STATUS")` | 精确匹配命令名 | `app_control_handle_line` |
| `strncmp(cmd, "MAC", 3)` | 前缀匹配（带参数的命令） | `app_control_handle_line` |
| `strtoul(s, &end, 10)` | 十进制字符串转 uint32 | `app_parse_u32_token` |
| `snprintf(ssid, size, ...)` | 安全格式化到定长缓冲 | `app_build_wifi_ssid` |
| `vsnprintf(line, size, fmt, ap)` | 变参安全格式化 | `app_control_replyf` |
| `fgets(line, size, stdin)` | 标准输入读行 | `app_uart_cmd_task` |

### Hex 字符串与二进制互转

```c
/* 字符串 "E7E7E7E7E7" → uint8_t[5] */
static bool app_hex_to_addr(const char *hex, uint8_t *out, size_t width)
// 实现：逐字符分类 isxdigit()，高半字节 << 4 | 低半字节

/* 二进制 → hex 文本（查找表法） */
void app_proto_bytes_to_hex(const uint8_t *data, size_t len,
                             char *out, size_t out_size)
// out[pos++] = "0123456789ABCDEF"[(b >> 4) & 0x0F];
```

### 内存操作函数

| 函数 | 用途 | 典型位置 |
|------|------|---------|
| `memset(out, 0, out_size)` | 帧缓冲区清零 | `app_proto_build_frame` |
| `memset(&stats, 0, sizeof(stats))` | 统计结构体清零 | `app_stats_reset` |
| `memcpy(&tx[1], buf, len)` | 载荷复制到 SPI 缓冲 | `nrf24_write_payload` |
| `memcpy(data, &rx[1], len)` | SPI 读回数据复制 | `nrf24_read_payload` |

**注意：** `memset`/`memcpy` 不检查越界，调用者必须确保目标缓冲区足够大。

---

## 四、结构体

### 结构体定义规范

```c
/* nrf24.h: 带详细注释的公开结构体 */
typedef struct {
    int  pin_mosi;             /* MOSI GPIO */
    int  pin_miso;             /* MISO GPIO */
    /* ...11 个字段 */
    uint8_t  payload_size;     /* 静态载荷长度 1-32 */
    uint16_t retr_delay_us;    /* 自动重发间隔 */
    uint8_t  retr_count;       /* 自动重发最大次数 */
} nrf24_config_t;
```

### 指定初始化器（C99）

```c
/* 只初始化需要的字段，其余归零 */
gpio_config_t ce_cfg = {
    .pin_bit_mask = 1ULL << cfg->pin_ce,
    .mode         = GPIO_MODE_INPUT_OUTPUT,
};

spi_bus_config_t bus_cfg = {
    .mosi_io_num     = cfg->pin_mosi,
    .miso_io_num     = cfg->pin_miso,
    .sclk_io_num     = cfg->pin_sck,
    .max_transfer_sz = 64,
};
```

**优势：** 字段顺序无关，未指定的字段自动归零，代码意图清晰。

### 复合字面量（C99）

```c
/* app_config.c:212 — 一次性初始化整个结构体 */
*cfg = (nrf24_config_t){
    .spi_host   = cfg_host,
    .pin_mosi   = CONFIG_NRF24_PIN_MOSI,
    /* ... */
};

/* app_control.c:533 — 带函数指针的结构体 */
const app_control_io_t io = {
    .send_line = app_control_send_uart,
    .user      = NULL,
};
```

### 项目中的结构体汇总

| 结构体 | 用途 | 文件 |
|--------|------|------|
| `nrf24_config_t` | NRF24 初始化参数（11 字段） | `nrf24.h` |
| `nrf24_irq_status_t` | IRQ 状态解码（4 字段） | `nrf24.h` |
| `nrf24_rx_payload_t` | 接收载荷（管道号+长度+数据） | `nrf24.h` |
| `app_proto_frame_t` | 应用帧逻辑描述（4 字段） | `app_proto.h` |
| `app_tx_stats_t` | TX 统计（8 字段） | `app_stats.h` |
| `app_rx_stats_t` | RX 统计（11 字段） | `app_stats.h` |
| `app_control_io_t` | I/O 通道抽象（函数指针+上下文） | `app_control.h` |
| `app_tx_burst_cmd_t` | TX 突发命令（4 字段） | `app_tx.c` |
| `slot_stat_t` | GUI_STAT 单时隙记录（4 字段） | `app_tx.c` |
| `tx_stat_context_t` | GUI_STAT 上下文（5 字段） | `app_tx.c` |
| `nrf24_pin_test_ctx_t` | 引脚测试上下文（6 字段） | `app_pin_test.c` |
| `nrf24_ctx_t` | NRF24 驱动私有上下文（4 字段） | `nrf24.c` |

### 结构体内存对齐

```c
/* app_proto.h: 逻辑帧结构体 ≠ 空口字节布局 */
typedef struct {
    uint16_t seq;        /* 2 字节 */
    uint8_t  flags;      /* 1 字节 */
    uint8_t  payload_len;/* 1 字节 */
    uint8_t  payload[22];/* 22 字节 */
} app_proto_frame_t;
```

**关键理解：** 此结构体仅用于业务逻辑，不代表空口字节布局。真实帧由 `app_proto_build_frame` 手工拼装，避免编译器自动填充带来的跨平台不一致。

---

## 五、枚举（Enum）

### typedef 枚举规范

```c
/* nrf24.h */
typedef enum {
    NRF24_DR_1MBPS = 0,
    NRF24_DR_2MBPS,
    NRF24_DR_250KBPS,
} nrf24_data_rate_t;

/* app_tx.h */
typedef enum {
    APP_MAC_ALOHA = 0,  /* 纯随机接入 */
    APP_MAC_CSMA,        /* 载波侦听 + 退避 */
} app_mac_mode_t;

/* app_proto.h */
typedef enum {
    APP_PROTO_PARSE_OK = 0,
    APP_PROTO_PARSE_ERR_LEN,
    APP_PROTO_PARSE_ERR_MAGIC,
    APP_PROTO_PARSE_ERR_CRC,
} app_proto_parse_result_t;
```

**规范：** 所有枚举使用 `typedef` 定义类型别名，值从 0 开始递增，0 表示正常/默认状态。

---

## 六、栈与堆

### 栈分配（快速、自动回收）

```c
/* 函数返回时自动释放 */
void my_function(void) {
    uint8_t buf[32];     // 栈上 32 字节
    char    line[256];   // 栈上 256 字节
    // ... 使用 buf, line
}  // ← 自动释放，无需 free
```

项目中大部分缓冲区为栈分配：SPI 事务缓冲、命令解析行、格式化输出缓冲。

### 堆分配（动态、需手动释放）

```c
/* app_tx.c:144-159 — 通过 calloc 在堆上分配 */
static void app_gui_stat_init(uint32_t target_count, size_t max_slots) {
    /* 先释放旧数据，防止泄漏 */
    free(s_gui_stat_ctx.stats);
    s_gui_stat_ctx.stats = NULL;

    s_gui_stat_ctx.stats = calloc(max_slots, sizeof(slot_stat_t));
    if (!s_gui_stat_ctx.stats) {
        ESP_LOGE(TAG, "GUI_STAT alloc failed");
        return;
    }
}
```

**堆使用规则：**
- `calloc` 优于 `malloc`：自动清零，减少未初始化 bug
- 重新分配前先 `free` 旧指针，防止内存泄漏
- 检查返回值是否为 NULL
- 项目中仅 `app_gui_stat_init` 一处使用堆分配

---

## 七、字节序

### 小端序序列化

```c
/* app_proto.c:app_proto_build_frame — 16 位 seq 的小端序打包 */
out[3] = (uint8_t)(in->seq & 0xFF);          /* 低字节在前 */
out[4] = (uint8_t)((in->seq >> 8) & 0xFF);   /* 高字节在后 */

/* CRC16 同样小端序 */
out[pl + 7] = (uint8_t)(crc & 0xFF);         /* CRC 低字节 */
out[pl + 8] = (uint8_t)((crc >> 8) & 0xFF);  /* CRC 高字节 */
```

### 小端序解包

```c
/* app_proto.c:app_proto_parse_frame */
uint16_t seq = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
uint16_t crc = (uint16_t)buf[pl + 7] | ((uint16_t)buf[pl + 8] << 8);
```

### 网络字节序（大端序）

```c
/* app_wifi_control.c: TCP 套接字使用标准网络字节序 */
addr.sin_port        = htons((uint16_t)listen_port);  /* 主机→网络（大端） */
addr.sin_addr.s_addr = htonl(INADDR_ANY);             /* 同上 */
```

**规则总结：**
- 空口协议帧：小端序（手动打包/解包）
- 网络套接字：大端序（通过 `htons`/`htonl`）
- NRF24 寄存器数据：以芯片手册为准（通常单字节，无字节序问题）

---

## 八、参考来源

- ISO/IEC 9899:1999 (C99) — designated initializers, compound literals, stdint.h, stdbool.h
- NRF24 项目源码 `main/nrf24.h` 结构体定义
- NRF24 项目源码 `main/app_proto.c` 帧序列化实现

---

*本文档属于 NRF24_1 项目技术笔记系列，随项目演进持续更新。*
