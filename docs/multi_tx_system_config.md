# 多TX+1RX 系统架构与发送配置说明

## 1. 系统架构

### 硬件拓扑

```
┌──────────────────────────────────────────────────────────────┐
│  PC (pc_nrf24_controller.py)                                 │
│                                                              │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────────┐  │
│  │ TX_1     │  │ TX_2     │  │ ...TX_N  │  │ JAM (干扰源)│  │
│  │ (串口/TCP)│  │ (串口/TCP)│  │ (串口/TCP)│  │ (串口/TCP) │  │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └─────┬──────┘  │
│       │             │             │               │         │
│  ┌────┴─────────────┴─────────────┴───────────────┴──────┐  │
│  │ RX (串口/TCP)                                          │  │
│  └────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────┘
       │             │             │               │
   [ESP32]      [ESP32]      [ESP32]        [ESP32]
   TX固件       TX固件        TX固件         TX固件(仅JAM)
                                                +
                                          [ESP32] RX固件
```

- 每个 TX 设备是一个独立的 ESP32 + NRF24L01 节点
- TX 固件编译选项: `CONFIG_NRF24_ROLE_TX=y`
- RX 固件编译选项: `CONFIG_NRF24_ROLE_RX=y`
- 干扰源 (JAM) 本质上是运行TX固件的设备,仅使用 `JAM ON/OFF` 功能
- 每个设备通过独立串口(或WiFi TCP)连接到PC

### 固件任务全景

| 任务名 | 优先级 | 角色 | 职责 |
|--------|--------|------|------|
| uart_cmd | 9 | TX/RX | UART 命令行解析 |
| nrf24_irq | 9 | RX | 接收 IRQ + 排空 RX FIFO |
| nrf24_tx | 8 | TX | 发送 burst 调度 |
| tcp_ctrl | 8 | TX/RX | TCP Wi-Fi 控制台 |
| nrf24_jam | 10 | TX | Jammer 紧循环发送 |
| nrf24_rx | 7 | RX | 载荷解析 + 统计 + 日志 |

---

## 2. MAC 协议支持

### ALOHA (纯随机接入)

- 每个时隙以概率 q% 决定是否发送
- q=100: 每个时隙都发送 (无退避, 最大吞吐)
- q<100: 跳过 (100-q)% 的时隙 (概率退避, 降低冲突)
- 无载波侦听, 实现简单

### CSMA (载波侦听多址)

- 发送前执行载波侦听 (NRF24 RPD 能量检测, -64dBm 阈值)
- 信道空闲 (RPD=0): 以概率 q% 发送
- 信道繁忙 (RPD=1): 退避 `csma_window_slots + 1` 个时隙
- 退避窗口中不重复侦听, 避免频繁模式切换
- RPD 检测的是"任意信号能量", 不区分本网络/其他干扰

---

## 3. 当前硬件配置

| 参数 | 值 | 说明 |
|------|-----|------|
| 空口速率 | **250Kbps** | 最稳定模式, 适合Demo |
| 发射功率 | -12dBm | 中等功率, 兼顾稳定和干扰控制 |
| 载荷大小 | 32 字节 | NRF24 最大载荷 |
| 信道 | 40 (2.440GHz) | |
| 地址宽度 | 5 字节 | |
| 自动重发延迟 | 750us | 芯片编码为 (750+249)/250=3, 实际约1000us |
| 自动重发次数 | 10 次 | |
| CRC | 2 字节 | |

---

## 4. 报文空中传输时间计算

### 250Kbps 数据包结构

```
+----------+--------+--------+---------+------+
| 前导码   | 地址   | 控制字段| 载荷    | CRC  |
| 8 bits   | 40 bits| 13 bits | 256 bits|16bits|
+----------+--------+--------+---------+------+
```

- **数据包总比特数**: 8 + 40 + 13 + 256 + 16 = **333 bits**
- **数据包空中时间**: 333 / 250000 = **1.33ms**

### ACK 包结构 (无载荷)

```
+----------+--------+--------+------+
| 前导码   | 地址   | 控制字段| CRC  |
| 8 bits   | 40 bits| 13 bits |16bits|
+----------+--------+--------+------+
```

- **ACK包总比特数**: 8 + 40 + 13 + 16 = **77 bits**
- **ACK包空中时间**: 77 / 250000 = **0.31ms**

### 完整 TX+ACK 周期

```
数据包(1.33ms) + 芯片模式转换(0.13ms) + ACK包(0.31ms) ≈ 1.77ms
```

---

## 5. WiFi 802.11 参考与Demo适配

### WiFi 802.11b 标准参数

| 参数 | 802.11b 值 | 说明 |
|------|-----------|------|
| Slot Time | 20us | 基本时间单位 |
| SIFS | 10us | 最短帧间间隔 |
| DIFS | 50us | DCF帧间间隔 = SIFS + 2×Slot |
| CWmin | 31 slots | 最小竞争窗口 |
| CWmax | 1023 slots | 最大竞争窗口 |

### NRF24 vs WiFi 差异

- NRF24 **不是** WiFi 设备, 没有真正的 CCA 硬件
- RPD 是简单能量检测 (阈值 -64dBm), 非完整 CCA
- NRF24 最高速率 2Mbps, 远低于 WiFi 最低 1Mbps (802.11b)
- carrier_sense 函数需要模式切换 (130us TX→RX 稳定延迟)

### Demo 适配原则

1. **slot_ms 应对齐 TX+ACK 周期**: 刚好容纳一次完整收发, 消除死时间
2. **CSMA 侦听窗口 ≥ 数据包空中时间**: 确保能检测到正在发送的包
3. **退避窗口足够可见**: Demo 中能观察到明显的 CSMA 退避行为
4. **ALOHA 与 CSMA 报文长度一致**: 使用相同的 burst payload

---

## 6. 发送配置推荐值

| 参数 | 旧值 | 新值 | 理由 |
|------|------|------|------|
| **slot_ms** | 20ms | **2ms** | 刚好容纳 TX+ACK 周期 (~1.77ms), 消除93%死时间 |
| **CSMA carrier sense listen_us** | 200us | **1500us** | 130us(稳定) + 1500us(侦听) = 1630us, 覆盖完整数据包(1330us) |
| **csma_window_slots** | 1 | **4** | 有效退避 5个时隙 = 10ms, Demo 中可观察到明显退避 |
| **mac_q** | 100 | 100 (不变) | CSMA模式下信道空闲时总是发送; ALOHA时可按需调低 |

### 时隙时间线示意 (slot_ms=2ms)

```
时隙 N        时隙 N+1      时隙 N+2      时隙 N+3      时隙 N+4
|--2ms--|    |--2ms--|    |--2ms--|    |--2ms--|    |--2ms--|
|TX数据包|    |        |    |TX数据包|    |        |    |TX数据包|
|+ACK   |    | 空闲   |    |+ACK   |    | 空闲   |    |+ACK   |
|________|    |________|    |________|    |________|    |________|

CSMA 侦听窗口 (1500us) 覆盖整个数据包空中时间 (1330us):

   |<-- 130us 稳定 -->|<-- 1500us 侦听窗口 -->|
                     |<-- 数据包 1330us -->|  ← 可完整检测
```

---

## 7. PC GUI 与固件命令对应关系

### 完全对应的命令

| GUI操作 | 下发命令 | 固件处理函数 |
|---------|---------|-------------|
| 查询状态 | `STATUS` | `app_reply_stats()` |
| 清空统计 | `RESETSTATS` | `app_stats_reset()` |
| 启用/禁用TX | `ENABLE 0/1` | `app_tx_set_enabled()` |
| MAC协议配置 | `MAC <ALOHA/CSMA> <q>` | `app_tx_set_mac_config()` |
| 时隙参数 | `SLOT <ms> <win>` | `app_tx_set_slot_params()` |
| 时隙上限 | `SLOTLIMIT <n>` | `app_tx_set_slot_limit()` |
| ASCII突发 | `BURST <count> <interval_ms> <payload>` | `app_tx_submit_burst()` |
| HEX突发 | `BURSTHEX <count> <interval_ms> <hex>` | `app_tx_submit_burst()` |
| 停止发送 | `STOP` | `app_tx_abort()` + `app_tx_jam_stop()` |
| 干扰开启 | `JAM ON` | `app_tx_jam_start()` |
| 干扰关闭 | `JAM OFF` | `app_tx_jam_stop()` |
| TCP认证 | `AUTH <token>` | TCP服务器 token 校验 |

### TX STAT 字段对应

```
固件输出:
STAT role=TX enabled=%d mac=%s q=%u slot_ms=%lu csma_win=%lu slot_limit=%lu
      queued=%lu sent=%lu ack_ok=%lu ack_fail=%lu retries_sum=%lu retries_max=%lu next_seq=%u

GUI 解析: role, mac, q, slot_ms, csma_win, slot_limit, sent, ack_ok, ack_fail,
          retries_sum, retries_max — 全部正确映射
```

### RX STAT 字段对应

```
固件输出:
STAT role=RX rx_pkt=%lu frame_ok=%lu crc_fail=%lu magic_fail=%lu
      len_fail=%lu dup=%lu ooo=%lu gap=%lu last_seq=%u

GUI 解析: role, rx_pkt, frame_ok, crc_fail, magic_fail, len_fail,
          dup, ooo, gap, last_seq — 全部正确映射
```

---

## 8. 代码修改清单

### 固件 (`main/app_tx.c`)

| 修改项 | 位置 | 变更 |
|--------|------|------|
| 默认 slot_ms | L127 | `20` → `2` |
| 默认 csma_window | L128 | `1` → `4` |
| CSMA 侦听窗口 | L396 | `nrf24_carrier_sense(200, ...)` → `nrf24_carrier_sense(1500, ...)` |
| Jammer 统计更新 | L897-942 | 新增 `app_tx_stats_t *stats` 指针, 每1000包更新 `frame_sent`, 退出时补齐尾数 |

### GUI (`tools/pc_nrf24_controller.py`)

| 修改项 | 变更 |
|--------|------|
| 默认值同步 | `slot_ms_var`: 20→2, `csma_win_var`: 1→4 |
| JAM面板MAC配置 | 移除无意义的 MAC/时隙/退避 配置控件, 替换为说明文字 |
| JAM统计面板 | 显示 role/sent/ack_ok/ack_fail (sent 现在有实际数据) |
| TX帧间隔控件 | 新增 `interval_var`, "帧间隔(ms)" 输入框, BURST命令不再硬编码 interval=0 |
| RX统计补全 | 新增 `magic_fail`, `len_fail`, `last_seq` 三个字段 |
| 配置持久化 | `interval` 加入保存/加载列表 |

---

## 9. 系统启动流程

```
1. 编译固件
   TX设备: menuconfig → CONFIG_NRF24_ROLE_TX=y
   RX设备: menuconfig → 不选 TX role (默认RX)

2. 烧录到各自的 ESP32

3. 启动 PC GUI
   python tools/pc_nrf24_controller.py

4. 对每个设备: 选择串口 → 点击"建立连接"

5. 配置每个 TX:
   - MAC模式: ALOHA 或 CSMA
   - q值: 概率门限 (0-100)
   - 时隙: 2ms (推荐)
   - CSMA窗口: 4 (推荐)

6. 配置载荷 → 设置帧间隔 → 点击"发送突发包"

7. 需要干扰测试时: 在 JAM 设备面板点击 "开启干扰"
```
