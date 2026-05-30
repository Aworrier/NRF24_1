# C 语言基础 06：协议设计与 ESP-IDF 外设 API

> 文件创建日期: 2026-05-30
> 适用项目: NRF24_1 (ESP32-S3 + NRF24L01+)
> 前置阅读: [c-primer-05-freertos-tasks-and-queues.md](c-primer-05-freertos-tasks-and-queues.md)
> 后续阅读: [CODE_DEEP_DIVE.md](CODE_DEEP_DIVE.md)

---

## 一、CRC16-CCITT 算法

### 算法参数

| 参数 | 值 | 说明 |
|------|-----|------|
| 多项式 | `0x1021` | x^16 + x^12 + x^5 + 1（CCITT 标准） |
| 初始值 | `0xFFFF` | 全 1 初始化 |
| 反射输入 | 否 | 逐字节高位在前处理 |
| 反射输出 | 否 | 输出不翻转 |
| 最终异或 | 无 | 不执行最终异或 |

### 逐位实现

```c
/* app_proto.c:33-47 */
static uint16_t app_crc16_ccitt(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFFU;                    /* 初始值 */
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;         /* 将输入字节移到高 8 位并异或 */
        for (int b = 0; b < 8; b++) {
            if ((crc & 0x8000U) != 0) {        /* 测试最高位 */
                crc = (uint16_t)((crc << 1) ^ 0x1021U);  /* 左移并异或多项式 */
            } else {
                crc = (uint16_t)(crc << 1);     /* 仅左移 */
            }
        }
    }
    return crc;
}
```

**逐位处理的直觉理解：** 将数据视为多项式的系数，用长除法计算除以生成多项式的余数。CRC 值就是这个余数。

### 帧中的 CRC 位置

```
空口帧布局:
┌──────┬──────┬──────┬──────┬──────┬──...──┬──────┬──────┐
│ MAG0 │ MAG1 │ VER  │ ...  │ PAYLOAD... │ CRC_L│ CRC_H│
│ byte0│ byte1│ byte2│      │  header+pl │      │      │
└──────┴──────┴──────┴──────┴──...──┴──────┴──────┘
                                              ↑
                                    小端序：低字节在前
```

---

## 二、应用帧协议

### 帧格式定义

```
Byte:   0     1     2     3     4     5     6     7     8..7+PL   ..+2
      ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────────┬───────┐
      │0xA5 │0x5A │ VER │SEQ_L│SEQ_H│PL_LEN│FLAGS│ RSV │PAYLOAD  │CRC16  │
      │MAG0 │MAG1 │0x01 │low  │high │      │     │     │max 22 B │LE     │
      └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────────┴───────┘
       固定头部 8 字节                     ↑                 CRC=2字节
        (APP_PROTO_HEADER_SIZE)            变长载荷
```

### 构建帧

```c
/* app_proto.c:110 */
size_t app_proto_build_frame(uint8_t *out, size_t out_size,
                              const app_proto_frame_t *in) {
    size_t pl = in->payload_len;
    size_t total = APP_PROTO_HEADER_SIZE + pl + APP_PROTO_CRC_SIZE;
    if (total > out_size) return 0;

    memset(out, 0, out_size);                     /* 清零 */
    out[0] = APP_PROTO_MAGIC0;                     /* 魔数 0 */
    out[1] = APP_PROTO_MAGIC1;                     /* 魔数 1 */
    out[2] = APP_PROTO_VER;                        /* 版本 */
    out[3] = (uint8_t)(in->seq & 0xFF);           /* seq 低字节 (LE) */
    out[4] = (uint8_t)((in->seq >> 8) & 0xFF);    /* seq 高字节 (LE) */
    out[5] = in->payload_len;                     /* 载荷长度 */
    out[6] = in->flags;                           /* 标志 */
    /* 保留位 out[7] 保持 0 */

    memcpy(&out[8], in->payload, pl);             /* 载荷数据 */

    uint16_t crc = app_crc16_ccitt(out, 8 + pl);  /* CRC 覆盖头部+载荷 */
    out[8 + pl]     = (uint8_t)(crc & 0xFF);      /* CRC 低字节 */
    out[8 + pl + 1] = (uint8_t)((crc >> 8) & 0xFF); /* CRC 高字节 */

    return total;
}
```

### 解析帧（带状态机验证）

```c
/* app_proto.c:173 */
app_proto_parse_result_t app_proto_parse_frame(
    const uint8_t *buf, size_t len, app_proto_frame_t *out) {

    /* 第 1 层：长度检查 */
    if (len < APP_PROTO_HEADER_SIZE + APP_PROTO_CRC_SIZE)
        return APP_PROTO_PARSE_ERR_LEN;

    uint8_t pl = buf[5];  /* 载荷长度字段 */
    size_t expected = APP_PROTO_HEADER_SIZE + pl + APP_PROTO_CRC_SIZE;
    if (len < expected)
        return APP_PROTO_PARSE_ERR_LEN;

    /* 第 2 层：魔数 + 版本验证 */
    if (buf[0] != APP_PROTO_MAGIC0 || buf[1] != APP_PROTO_MAGIC1
        || buf[2] != APP_PROTO_VER)
        return APP_PROTO_PARSE_ERR_MAGIC;

    /* 第 3 层：CRC 完整性验证 */
    uint16_t crc_rcvd = (uint16_t)buf[8 + pl] | ((uint16_t)buf[9 + pl] << 8);
    uint16_t crc_calc = app_crc16_ccitt(buf, 8 + pl);
    if (crc_rcvd != crc_calc)
        return APP_PROTO_PARSE_ERR_CRC;

    /* 第 4 层：字段提取 */
    out->seq         = (uint16_t)buf[3] | ((uint16_t)buf[4] << 8);
    out->flags       = buf[6];
    out->payload_len = pl;
    memcpy(out->payload, &buf[8], pl);

    return APP_PROTO_PARSE_OK;
}
```

**分层验证顺序：** 长度 → 魔数 → CRC → 字段提取。每层失败立即返回，避免在无效数据上浪费计算。

---

## 三、GPIO 操作

### 配置 GPIO

```c
/* nrf24.c — 配置 CE/IRQ 引脚 */
gpio_config_t ce_cfg = {
    .pin_bit_mask = 1ULL << cfg->pin_ce,
    .mode         = GPIO_MODE_INPUT_OUTPUT,
    .pull_up_en   = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type    = GPIO_INTR_DISABLE,
};
gpio_config(&ce_cfg);
```

### 引脚电平控制

```c
gpio_set_level((gpio_num_t)pin, 1);   /* 拉高 */
gpio_set_level((gpio_num_t)pin, 0);   /* 拉低 */

int level = gpio_get_level((gpio_num_t)pin);  /* 读取电平 */
```

### GPIO ISR 安装与卸载

```c
/* 安装 ISR 服务 + 注册回调 */
gpio_install_isr_service(0);
gpio_isr_handler_add(pin, isr_handler, arg);

/* 卸载 */
gpio_isr_handler_remove(pin);
gpio_uninstall_isr_service();
/* 注意：gpio_install_isr_service 可能返回 ESP_ERR_INVALID_STATE
   （服务已由其他模块安装），项目正确处理了这种情况 */
```

---

## 四、SPI 操作

### 初始化 SPI 总线

```c
spi_bus_config_t bus_cfg = {
    .mosi_io_num     = cfg->pin_mosi,
    .miso_io_num     = cfg->pin_miso,
    .sclk_io_num     = cfg->pin_sck,
    .max_transfer_sz = 64,
};
spi_bus_initialize(cfg->spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
```

### 添加 SPI 设备

```c
spi_device_interface_config_t dev_cfg = {
    .command_bits   = 0,
    .address_bits   = 0,
    .mode           = 0,         /* SPI MODE 0: CPOL=0, CPHA=0 */
    .clock_speed_hz = cfg->spi_clock_hz,  /* 默认 8 MHz */
    .spics_io_num   = cfg->pin_csn,
    .queue_size     = 3,
};
spi_bus_add_device(cfg->spi_host, &dev_cfg, &s_nrf24.spi);
```

### 执行 SPI 事务

```c
/* nrf24.c:150 — 单次 SPI 传输 */
static esp_err_t nrf24_spi_transfer(
    const uint8_t *tx, uint8_t *rx, size_t len) {
    spi_transaction_t t = {
        .length    = len * 8,   /* 位数 */
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_transmit(s_nrf24.spi, &t);
}
```

### 完整的寄存器读操作

```c
/* 命令字节 + N 个数据字节组成 SPI 事务 */
uint8_t tx[33] = {0};
uint8_t rx[33] = {0};

tx[0] = (uint8_t)(NRF24_CMD_R_REGISTER | (reg & 0x1F));  /* 读命令 */
memset(&tx[1], NRF24_CMD_NOP, len);                         /* 填充 NOP 以读取 */

nrf24_spi_transfer(tx, rx, 1 + len);
memcpy(buf, &rx[1], len);                                   /* 提取数据 */
```

---

## 五、微秒延迟

```c
/* nrf24.c — CE 脉冲需要精确 15µs */
nrf24_set_ce(1);
esp_rom_delay_us(15);   /* 精确微秒延迟 */
nrf24_set_ce(0);

/* nrf24.c — 载波侦听需要等待 PLL 稳定 */
esp_rom_delay_us(130);  /* PLL 锁定时间 */
esp_rom_delay_us(listen_us);  /* 能量累积时间 */
```

`esp_rom_delay_us` 是 ROM 中的忙等延迟函数，精度约 ±1µs，适合硬件时序要求。不阻塞其他任务——它是忙循环，不影响调度器。

---

## 六、硬件随机数

```c
/* app_tx.c — MAC 概率判决 */
uint32_t r = esp_random() % 100U;   /* 0~99 均匀分布 */
```

`esp_random()` 使用 ESP32 硬件 TRNG（真随机数发生器），适合 MAC 协议中的随机退避。不要用 `rand()`（伪随机且需种子）。

---

## 七、ESP_LOG 日志宏

### 三级日志

```c
/* 信息 */
ESP_LOGI(TAG, "TX ok seq=%u", seq);                        // 正常流程
ESP_LOGI(TAG, "GUI_STAT: slot=%lu attempted=1 success=1 reason=0", ...);

/* 警告 */
ESP_LOGW(TAG, "RX overflow, drop oldest");                  // 可恢复异常
ESP_LOGW(TAG, "Slot=%lu timed out", slot);

/* 错误 */
ESP_LOGE(TAG, "GUI_STAT alloc failed");                     // 需关注的错误
ESP_LOGE(TAG, "TCP socket error: %d", errno);
```

### 每个文件的 TAG

```c
static const char *TAG = "nrf24";      /* nrf24.c */
static const char *TAG = "nrf24_app";  /* app_tx.c, app_rx.c 等 */
```

TAG 是日志行的模块标识，帮助在多模块运行时区分日志来源。

### 条件日志

```c
/* 仅在 Tutorial Debug 模式下打印详细日志 */
#if CONFIG_NRF24_MODE_TUTORIAL_DEBUG
    ESP_LOGI(TAG, "Detailed pin map: ...");
#endif

/* 仅在 Verbose 模式下打印驱动级日志 */
#if CONFIG_NRF24_LOG_LEVEL_VERBOSE
    ESP_LOGI(TAG, "nrf24_send_payload: CE pulse done");
#endif
```

---

## 八、Wi-Fi SoftAP 配置

```c
/* app_wifi_control.c — Wi-Fi 初始化序列 */
nvs_flash_init();                               /* 1. 初始化 NVS（校准数据） */
esp_netif_init();                               /* 2. 初始化 TCP/IP 栈 */
esp_event_loop_create_default();                /* 3. 创建事件循环 */
esp_netif_create_default_wifi_ap();             /* 4. 创建 AP 网卡 */

wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
esp_wifi_init(&cfg);                            /* 5. 初始化 Wi-Fi 驱动 */

wifi_config_t ap_config = {
    .ap = {
        .ssid           = "NRF24_CTRL",
        .ssid_len       = strlen("NRF24_CTRL"),
        .password       = "nrf24ctrl",
        .max_connection = 1,
        .authmode       = WIFI_AUTH_WPA2_PSK,
    },
};
esp_wifi_set_mode(WIFI_MODE_AP);                /* 6. 设置 AP 模式 */
esp_wifi_set_config(WIFI_IF_AP, &ap_config);    /* 7. 应用配置 */
esp_wifi_start();                               /* 8. 启动射频 */
```

---

## 九、TCP Socket API

### 服务器端完整流程

```c
/* 1. 创建套接字 */
int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

/* 2. 设置 SO_REUSEADDR（快速重启） */
int opt = 1;
setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

/* 3. 设置接收超时 */
struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

/* 4. 绑定地址 */
struct sockaddr_in addr = {0};
addr.sin_family      = AF_INET;
addr.sin_port        = htons(3333);
addr.sin_addr.s_addr = htonl(INADDR_ANY);
bind(sock, (struct sockaddr *)&addr, sizeof(addr));

/* 5. 监听 */
listen(sock, 1);

/* 6. 接受连接 */
struct sockaddr_in client_addr;
socklen_t addr_len = sizeof(client_addr);
int client_fd = accept(sock, (struct sockaddr *)&client_addr, &addr_len);

/* 7. 读写 */
read(client_fd, buf, sizeof(buf));       /* 读命令 */
write(client_fd, resp, strlen(resp));    /* 写响应 */

/* 8. 关闭 */
shutdown(client_fd, SHUT_RDWR);
close(client_fd);
close(sock);
```

### 逐字节行读取

```c
/* app_wifi_control.c — 从 TCP 流中逐字节读取，直到遇到换行 */
static bool app_socket_read_line(int sock, char *buf, size_t buf_size) {
    size_t pos = 0;
    while (pos < buf_size - 1) {
        char c;
        int n = read(sock, &c, 1);
        if (n <= 0) return (pos > 0);   /* EOF/错误 → 若有数据则返回成功 */
        if (c == '\n') break;            /* 换行 = 行结束 */
        if (c == '\r') continue;         /* 跳过回车 */
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return true;
}
```

---

## 十、参考来源

- ISO/IEC 13239 — CRC16-CCITT 标准定义
- ESP-IDF Programming Guide — SPI Master Driver, GPIO, Wi-Fi Driver
- lwIP Raw/native API 文档 — Socket API
- NRF24 项目源码 `main/app_proto.c` — CRC 与帧协议实现
- NRF24 项目源码 `main/app_wifi_control.c` — TCP 控制实现

---

*本文档属于 NRF24_1 项目技术笔记系列，随项目演进持续更新。*
