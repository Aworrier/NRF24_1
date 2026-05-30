# C 语言基础 02：位运算与预处理器

> 文件创建日期: 2026-05-30
> 适用项目: NRF24_1 (ESP32-S3 + NRF24L01+)
> 前置阅读: [c-primer-01-types-and-memory.md](c-primer-01-types-and-memory.md)
> 后续阅读: [c-primer-03-control-flow-and-error-handling.md](c-primer-03-control-flow-and-error-handling.md)

---

## 一、位掩码定义

本项目通过 `(1U << N)` 定义所有外设寄存器位掩码，确保每位位置一目了然：

```c
/* nrf24.c — CONFIG 寄存器的 7 个位定义 */
#define NRF24_CONFIG_MASK_RX_DR  (1U << 6)   /* 0b01000000: 屏蔽 RX 中断 */
#define NRF24_CONFIG_MASK_TX_DS  (1U << 5)   /* 0b00100000: 屏蔽 TX 成功中断 */
#define NRF24_CONFIG_MASK_MAX_RT (1U << 4)   /* 0b00010000: 屏蔽 TX 重传达上限中断 */
#define NRF24_CONFIG_EN_CRC      (1U << 3)   /* 0b00001000: 使能 CRC */
#define NRF24_CONFIG_CRCO        (1U << 2)   /* 0b00000100: CRC 编码方案 */
#define NRF24_CONFIG_PWR_UP      (1U << 1)   /* 0b00000010: 芯片上电 */
#define NRF24_CONFIG_PRIM_RX     (1U << 0)   /* 0b00000001: 主接收模式 */

/* nrf24.c — STATUS 寄存器的 4 个位定义 */
#define NRF24_STATUS_RX_DR        (1U << 6)  /* RX 数据就绪 */
#define NRF24_STATUS_TX_DS        (1U << 5)  /* TX 数据发送完成 */
#define NRF24_STATUS_MAX_RT       (1U << 4)  /* TX 重发达上限 */
#define NRF24_STATUS_RX_P_NO_MASK (7U << 1)  /* 管道号掩码：bit3..bit1 */

/* nrf24.c — RF_SETUP 寄存器位定义 */
#define NRF24_RF_DR_LOW   (1U << 5)  /* 速率选择低 bit */
#define NRF24_RF_DR_HIGH  (1U << 3)  /* 速率选择高 bit */
#define NRF24_RF_PWR_HIGH (1U << 2)  /* 功率选择高 bit */
#define NRF24_RF_PWR_LOW  (1U << 1)  /* 功率选择低 bit */

/* nrf24.c — FIFO_STATUS 位定义 */
#define NRF24_FIFO_STATUS_RX_EMPTY (1U << 0)
#define NRF24_FIFO_STATUS_TX_FULL  (1U << 5)
```

**关键约定：**
- 所有位掩码使用 `U` 后缀（无符号），避免有符号移位未定义行为
- `(1U << N)` 确保掩码宽度至少为 `unsigned int`
- GPIO 64-bit 掩码使用 `1ULL << pin`

---

## 二、位运算四大操作

### 位测试（AND 检测某位是否为 1）

```c
/* nrf24.c — 检查 IRQ 状态 */
if ((status & NRF24_STATUS_RX_DR) != 0) {
    irq->rx_ready = true;
}

/* 检查 FIFO 是否为空 */
if (fifo & NRF24_FIFO_STATUS_RX_EMPTY) {
    return ESP_ERR_NOT_FOUND;
}

/* CRC 计算中检测最高位 */
if ((crc & 0x8000U) != 0) {
    crc = (uint16_t)((crc << 1) ^ 0x1021U);
}
```

### 位设置（OR 将指定位置 1）

```c
/* nrf24.c — 上电时同时使能 CRC */
config |= NRF24_CONFIG_EN_CRC | NRF24_CONFIG_PWR_UP;

/* 进入接收模式 */
config |= NRF24_CONFIG_PRIM_RX | NRF24_CONFIG_PWR_UP;
```

### 位清除（AND NOT 将指定位清零）

```c
/* 下电 */
config &= (uint8_t)~NRF24_CONFIG_PWR_UP;

/* 退出接收模式 */
config &= (uint8_t)~NRF24_CONFIG_PRIM_RX;
```

### 位提取（AND 掩码后右移）

```c
/* 从 STATUS 提取管道号 (bit3..bit1) */
uint8_t pipe_no = (uint8_t)((status & NRF24_STATUS_RX_P_NO_MASK) >> 1);

/* 从 OBSERVE_TX 提取丢包计数和重发计数 */
uint8_t plos  = (value >> 4) & 0x0F;   /* bit7..bit4 */
uint8_t arc   = value & 0x0F;           /* bit3..bit0 */
```

---

## 三、寄存器字段打包

```c
/* nrf24.c — SETUP_RETR 寄存器：高 4 bit 延时，低 4 bit 重试次数 */
uint8_t d = nrf24_encode_retr_delay(delay_us);  /* 250µs 步进编码 */
uint8_t c = count & 0x0F;                        /* 最大 15 */
uint8_t val = (uint8_t)((d << 4) | c);           /* 打包为一个字节 */
nrf24_write_register(NRF24_REG_SETUP_RETR, val);
```

**原理：**
```
SETUP_RETR 寄存器:
┌──────────┬──────────┬──────────┬──────────┬──────────┬──────────┬──────────┬──────────┐
│   ARD3   │   ARD2   │   ARD1   │   ARD0   │  ARC3    │  ARC2    │  ARC1    │  ARC0    │
│  bit7    │  bit6    │  bit5    │  bit4    │  bit3    │  bit2    │  bit1    │  bit0    │
└──────────┴──────────┴──────────┴──────────┴──────────┴──────────┴──────────┴──────────┘
      ←── 重发延时 (ARD) ──→                     ←── 重试次数 (ARC) ──→
```

---

## 四、Hex 高低半字节提取

```c
/* app_config.c — hex 字符转半字节 */
char c = hex[i];
uint8_t val_hi, val_lo;
if (c >= '0' && c <= '9') val_hi = (uint8_t)(c - '0');
else if (c >= 'A' && c <= 'F') val_hi = (uint8_t)(c - 'A' + 10);
// ...
out[i] = (uint8_t)((val_hi << 4) | val_lo);    /* 高半字节 << 4 | 低半字节 */

/* app_proto.c — 字节转 hex 字符（查找表法） */
char hex[] = "0123456789ABCDEF";
out[pos++] = hex[(b >> 4) & 0x0F];   /* 高 4 bit */
out[pos++] = hex[b & 0x0F];          /* 低 4 bit */
```

---

## 五、预处理器常量

### 寄存器地址定义

```c
/* nrf24.c — 21 个寄存器地址 */
#define NRF24_REG_CONFIG      0x00
#define NRF24_REG_EN_AA       0x01
#define NRF24_REG_EN_RXADDR   0x02
#define NRF24_REG_SETUP_AW    0x03
#define NRF24_REG_SETUP_RETR  0x04
#define NRF24_REG_RF_CH       0x05
#define NRF24_REG_RF_SETUP    0x06
#define NRF24_REG_STATUS      0x07
#define NRF24_REG_OBSERVE_TX  0x08
#define NRF24_REG_RPD         0x09
/* ... 共 21 个寄存器定义 */
```

### SPI 命令定义

```c
#define NRF24_CMD_R_REGISTER          0x00  /* 读寄存器 */
#define NRF24_CMD_W_REGISTER          0x20  /* 写寄存器 */
#define NRF24_CMD_R_RX_PAYLOAD        0x61  /* 读 RX 载荷 */
#define NRF24_CMD_W_TX_PAYLOAD        0xA0  /* 写 TX 载荷 */
#define NRF24_CMD_FLUSH_TX            0xE1  /* 清空 TX FIFO */
#define NRF24_CMD_FLUSH_RX            0xE2  /* 清空 RX FIFO */
#define NRF24_CMD_REUSE_TX_PL         0xE3  /* 重用上次 TX 载荷 */
#define NRF24_CMD_R_RX_PL_WID         0x60  /* 读 RX 载荷宽度 */
#define NRF24_CMD_W_ACK_PAYLOAD       0xA8  /* 写 ACK 载荷 */
#define NRF24_CMD_W_TX_PAYLOAD_NOACK  0xB0  /* 写 TX 载荷（无 ACK） */
#define NRF24_CMD_NOP                 0xFF  /* 空操作 */
```

### 项目常量

```c
/* app_proto.h — 帧协议常量 */
#define APP_PROTO_MAGIC0          0xA5   /* 帧头魔数 1 */
#define APP_PROTO_MAGIC1          0x5A   /* 帧头魔数 2 */
#define APP_PROTO_VER             0x01   /* 协议版本 */
#define APP_PROTO_HEADER_SIZE     8      /* 头部字节数 */
#define APP_PROTO_CRC_SIZE        2      /* CRC 字节数 */
#define APP_PROTO_MAX_FRAME_SIZE  32     /* NRF24 最大载荷 */
#define APP_PROTO_MAX_USER_PAYLOAD 22    /* 32-8-2=22 用户可用 */

/* app_wifi_control.c */
#define APP_CONTROL_LINE_MAX       256    /* 命令行长上限 */
#define APP_WIFI_CONTROL_MAX_CLIENTS 1    /* 最大 TCP 客户端数 */
```

---

## 六、条件编译

### 互斥配置选择

```c
/* app_config.c — 从 menuconfig 选项映射到驱动枚举 */
static nrf24_data_rate_t app_cfg_data_rate(void) {
#if CONFIG_NRF24_DATA_RATE_2M
    return NRF24_DR_2MBPS;
#elif CONFIG_NRF24_DATA_RATE_250K
    return NRF24_DR_250KBPS;
#else
    return NRF24_DR_1MBPS;   /* 默认 1M */
#endif
}
```

`#if`/`#elif`/`#else`/`#endif` 用于从 Kconfig 互斥选项中选择编译路径。三个分支只有一个被编译进最终二进制。

### 功能门控（Feature Gate）

```c
/* app_wifi_control.c — 仅当 menuconfig 启用 Wi-Fi 时编译 */
#if CONFIG_NRF24_CONTROL_WIFI_ENABLE
/* ...Wi-Fi 功能实现... */
#endif

/* app_pin_test.c — 引脚自检模式 */
#if CONFIG_NRF24_PIN_TEST_MODE
/* ...引脚测试代码... */
#endif
```

### 角色隔离

```c
/* app_control.c — TX 专有命令仅 TX 角色编译 */
#if defined(CONFIG_NRF24_ROLE_TX)
    /* BURST, ENABLE, MAC, SLOT 等命令处理 */
#endif

/* app_rx.c / app_tx.c — 整个模块条件编译 */
#if defined(CONFIG_NRF24_ROLE_RX)
/* ... RX 模块全部实现 ... */
#else
/* ... 空桩函数 ... */
#endif
```

### 编译期默认值

```c
/* app_config.c — 当 menuconfig 未提供时使用默认值 */
#ifndef CONFIG_NRF24_PIPE1_ADDR
#define CONFIG_NRF24_PIPE1_ADDR "C2C2C2C2C2"
#endif

#ifndef CONFIG_NRF24_AUTO_RETR_DELAY_US
#define CONFIG_NRF24_AUTO_RETR_DELAY_US 750
#endif
```

`#ifndef`/`#define` 提供 fallback 默认值，确保即使 Kconfig 未导出也能编译通过。

---

## 七、头文件保护

```c
/* 所有 .h 文件的第一行 */
#pragma once
```

`#pragma once` 等价于传统的 `#ifndef`/`#define` 包含保护，但更简洁，且所有主流编译器均支持。

---

## 八、预处理器最佳实践总结

| 规则 | 说明 |
|------|------|
| 寄存器地址用 `#define` | 编译器零成本，且便于在调试器中查看原始数值 |
| Kconfig 选项用 `CONFIG_` 前缀 | ESP-IDF 标准约定，menuconfig 自动生成 |
| 宏名全大写+下划线 | 一眼区分宏与变量/函数 |
| `(1U << N)` 不用十进制 | `0x04` 不如 `(1U << 2)` 语义清晰 |
| 条件编译隔离整段代码 | 比运行时 `if` 更高效，且可以让角色/功能不可用时编译报错 |
| 避免宏函数（本项目不使用 `do-while(0)` 宏） | 内联函数或普通函数更安全、易调试 |

---

## 九、参考来源

- nRF24L01+ Product Specification — 寄存器位定义
- ESP-IDF Build System 文档 — Kconfig 与 `sdkconfig.h` 生成规则
- NRF24 项目源码 `main/nrf24.c` — 位掩码定义与寄存器操作
- NRF24 项目源码 `main/app_config.c` — Kconfig 到运行时的映射

---

*本文档属于 NRF24_1 项目技术笔记系列，随项目演进持续更新。*
