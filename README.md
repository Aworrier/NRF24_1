# ESP32-S3 + NRF24L01+ 教学仓库

这个仓库的目标是让新手可以看懂并跑通 NRF24 通信。

仓库现在采用同一工程双模式：

- Tutorial Debug（教学调试版）：日志更全，方便学习和定位问题
- Release Lite（正式精简版）：日志更少，配置更少，路径更短

## 1. 先理解两个速率

- SPI 速率：ESP32 与 NRF24 芯片之间的有线传输速率（本项目默认 8MHz）
- Air data rate：NRF24 在 2.4GHz 空口发包速率（250K / 1M / 2M）

这两个速率不同层面，互不相等。通常 SPI 明显高于空口速率即可。

## 2. 硬件连线（ESP32-S3）

| ESP32-S3 | NRF24L01+ | 说明 |
|---|---|---|
| GPIO13 | MOSI | 主出从入 |
| GPIO12 | MISO | 主入从出 |
| GPIO14 | SCK | SPI 时钟 |
| GPIO11 | CSN | SPI 片选 |
| GPIO10 | CE | 射频使能 |
| GPIO3  | IRQ | 中断输入 |
| 3.3V | VCC | 仅 3.3V |
| GND | GND | 共地 |

建议：在 NRF24 模块 VCC/GND 旁边加 10uF + 0.1uF 去耦。

### 2.1 经典 ESP32_32（WROOM）连线建议

如果你使用的是经典 ESP32（非 S2/S3/C3），请避免使用 GPIO6~GPIO11（通常连接板载 Flash）。

推荐映射（VSPI）：

| ESP32 | NRF24L01+ |
|---|---|
| GPIO23 | MOSI |
| GPIO19 | MISO |
| GPIO18 | SCK |
| GPIO5  | CSN |
| GPIO17 | CE |
| GPIO16 | IRQ |

如果你的开发板这几个管脚已被占用，可在 menuconfig 的 NRF24 菜单中改 pin 配置。

## 3. 仓库结构

- [main/nrf24.h](main/nrf24.h)：驱动接口（尽量保持稳定）
- [main/nrf24.c](main/nrf24.c)：SPI + 寄存器 + 收发实现
- [main/app_main.c](main/app_main.c)：应用入口（角色任务、日志、教学模式）
- [main/Kconfig.projbuild](main/Kconfig.projbuild)：菜单配置（模式、角色、参数）
- [sdkconfig.defaults.tutorial](sdkconfig.defaults.tutorial)：教学调试模板
- [sdkconfig.defaults.release](sdkconfig.defaults.release)：正式精简模板

## 3.1 文档分层

- [docs/quickstart.md](docs/quickstart.md)：从接线到双板收发的最短路径
- [docs/debug-playbook.md](docs/debug-playbook.md)：常见故障的排查剧本
- [docs/advanced.md](docs/advanced.md)：ACK、速率、IRQ 等进阶主题
- [docs/vscode-workflow.md](docs/vscode-workflow.md)：基于 VS Code 的开发流程（插件、驱动、资料检索）

## 4. 模式说明

### Tutorial Debug

适合：第一次接线、双板联调、问题定位。

特点：

- 可开启 PIN 自检
- 可开启 TX 无 ACK 诊断
- 启动时打印更完整参数（速率、功率、重发等）

### Release Lite

适合：功能已经跑通，准备长期运行。

特点：

- 默认简化日志
- 隐藏大部分高阶配置
- 保留新手必要参数（角色、引脚、频道、空口速率）

## 5. 快速开始（双板）

### 第一步：RX 板

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

在 NRF24 菜单中：

1. 选择 Project mode
2. 选择 Application role = RX
3. 确认 channel / data rate / pipe0 addr

然后：

```bash
idf.py build
idf.py -p <RX_PORT> flash monitor
```

### 第二步：TX 板

在同一工程切换：

1. Application role = TX
2. 参数与 RX 保持一致（channel/data rate/address width/pipe0）

然后：

```bash
idf.py build
idf.py -p <TX_PORT> flash monitor
```

## 6. 如何判断是否联调成功

- TX 端看到 `TX ok ...`
- RX 端看到 `RX pipe=... len=... data='...'`

如果 TX 一直超时，可先用 Tutorial Debug 模式开启 `TX disable auto-ack` 做单向链路诊断。

## 7. 常见问题速查

- RX 只有 `RX alive` 没有数据：先确认 TX 不是 PIN TEST 模式
- TX 一直 `ESP_ERR_TIMEOUT`：先关 ACK 诊断单向链路，再回到 ACK
- 速率不稳：把 Air data rate 降到 250K，PA 调到 -12dBm
- 仍异常：优先检查供电去耦和模块质量

## 8. 给新手的建议路径

1. 先用 Tutorial Debug 跑通
2. 再切到 Release Lite
3. 最后只保留必要日志和固定参数

## 9. 方案A：上位机串口控制 ESP32 触发 nRF24 发包

当前工程已支持在 TX 角色下，通过串口命令控制“是否发送”和“连续发送 N 包”。

### 9.1 固件侧命令

串口默认波特率使用 `CONFIG_ESP_CONSOLE_UART_BAUDRATE`（默认 115200）。

- `ENABLE 1`：允许发送
- `ENABLE 0`：禁止发送（并中止正在发送的 burst）
- `STATUS`：查询当前是否允许发送
- `RESETSTATS`：清空发送/接收统计计数
- `STOP`：中止当前 burst
- `BURST <count> <interval_ms> <ascii_payload>`：发送 ASCII 载荷
- `BURSTHEX <count> <interval_ms> <hex_payload>`：发送 HEX 载荷（例如 `A1B2C3`）

注意：nRF24 单包上限 32 字节；如果 payload 超过 32 字节会自动截断。

### 9.1.1 无线二进制协议帧（用于可靠性测试）

固件现在发送固定二进制协议帧（含序号与 CRC16）：

- `magic0`(1B): `0xA5`
- `magic1`(1B): `0x5A`
- `ver`(1B): `0x01`
- `seq`(2B): 小端递增序号
- `payload_len`(1B): 有效载荷长度
- `flags`(1B): 预留
- `reserved`(1B): 预留
- `payload`(0~22B)
- `crc16`(2B): CRC16-CCITT（对 header+payload 计算）

RX 端会对该帧做完整校验并统计：`crc_fail`、`seq_gap`、`dup`、`ooo`（乱序）等指标。

### 9.1.2 STATUS 返回格式

- TX 角色返回：
	- `STAT role=TX ... ack_ok=... ack_fail=... retries_sum=... retries_max=... next_seq=...`
- RX 角色返回：
	- `STAT role=RX ... rx_pkt=... frame_ok=... crc_fail=... gap=... dup=... ooo=...`

### 9.2 上位机 GUI（Python）

仓库已提供上位机脚本：`tools/pc_nrf24_controller.py`。

安装依赖：

```bash
pip install pyserial
```

运行：

```bash
python tools/pc_nrf24_controller.py
```

GUI 功能：

- 选择串口并连接
- 勾选/取消 Enable TX（对应 `ENABLE 1/0`）
- 配置 Count、Interval、Payload
- 选择 ASCII 或 HEX 模式并发送 burst
- 停止当前发送、查询状态、重置统计
- 实时显示 ACK 统计与 RX 可靠性统计面板（解析 `STAT ...`）

### 9.3 典型联调步骤

1. TX 板烧录并打开 monitor，确认看到 `NRF24 app started. role=TX`
2. 运行上位机 GUI，连接对应 COM 口
3. 先点一次 Enable TX
4. 输入 `count=20`、`interval=20`、payload（如 `HELLO`）后发送
5. 在 TX 日志观察 `TX ok seq=...`，在 RX 板观察 `RX pipe=...`

如果发送失败较多，可先把 Air data rate 调到 250K，再检查供电去耦与地址/频道一致性。
