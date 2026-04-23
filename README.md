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
- [docs/ble_mobile_control.md](docs/ble_mobile_control.md)：手机 BLE 上位机控制 NRF24 的完整流程

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

- 选择串口或无线 TCP 并连接
- 勾选/取消 Enable TX（对应 `ENABLE 1/0`）
- 配置 Count、Interval、Payload
- 选择 ASCII 或 HEX 模式并发送 burst
- 停止当前发送、查询状态、重置统计
- 实时显示 ACK 统计与 RX 可靠性统计面板（解析 `STAT ...`）

无线模式说明：

- 设备会开启 SoftAP，TX 和 RX 的 SSID 不同
- TX 默认规则：`NRF24_TX_1`、`NRF24_TX_2` ...，通过 menuconfig 中的 TX 编号区分
- RX 默认 SSID：`NRF24_RX`
- 默认连接地址为 `192.168.4.1:3333`
- 连接后会自动发送 `AUTH <token>`，默认 token 为 `nrf24`
- 认证成功后即可像串口一样发送 `STATUS`、`BURST`、`BURSTHEX`、`ENABLE` 等命令

独立运行说明：

- `tools/pc_nrf24_controller.py` 不依赖当前工程的其他 Python 文件，另一台笔记本只要安装 `pyserial` 就能直接运行
- 支持命令行参数覆盖默认连接配置，例如 `--mode TCP --host 192.168.4.1 --port 3333 --token nrf24`
- 程序会把最近一次使用的连接参数保存到用户目录，下一次打开可直接复用

### 9.3 典型联调步骤

1. TX 板烧录并打开 monitor，确认看到 `NRF24 app started. role=TX`
2. 运行上位机 GUI，连接对应 COM 口，或切换到无线 TCP 模式连接 `192.168.4.1:3333`
3. 先点一次 Enable TX
4. 输入 `count=20`、`interval=20`、payload（如 `HELLO`）后发送
5. 在 TX 日志观察 `TX ok seq=...`，在 RX 板观察 `RX pipe=...`

如果发送失败较多，可先把 Air data rate 调到 250K，再检查供电去耦与地址/频道一致性。

<<<<<<< HEAD
### 9.4 无线调试详细流程

下面是连接热点之后的推荐调试流程。当前无线控制的设计是“ESP32 开热点，上位机通过 TCP 发命令”，因此**最推荐的调试终端仍然是电脑上的 GUI**。如果你只是用手机连上热点，也可以先用来确认热点是否正常，但真正发送命令还需要一个 TCP 客户端。

#### 9.4.1 准备阶段

1. 烧录固件后，让 ESP32 上电启动。
2. 串口日志里确认看到类似：
	- `Wi-Fi AP started: SSID=NRF24_CTRL ...`
	- `TCP control server listening on 3333`
3. 用手机或电脑搜索 Wi-Fi，连接热点：
	- SSID：`NRF24_CTRL`
	- 密码：`nrf24ctrl`

#### 9.4.2 电脑上位机连接流程

1. 打开 `tools/pc_nrf24_controller.py`。
2. 在连接方式里选择 `TCP`。
3. 填写：
	- 主机：`192.168.4.1`
	- 端口：`3333`
	- Token：`nrf24`
4. 点击“连接”。
5. 连接成功后，GUI 会自动先发 `AUTH nrf24`。
6. 如果认证成功，日志会显示 `OK AUTH`，随后就可以继续发 `STATUS`、`RESETSTATS`、`ENABLE 1`、`BURST`、`BURSTHEX`。
7. 如果要做定时发送，先确保连接状态是 TCP 已建立，再在“定时发送”区域输入时间并点击定时发送按钮。

#### 9.4.3 推荐的调试顺序

1. 先点一次 `查询状态`，确认能收到 `STAT ...` 返回。
2. 点 `重置统计`，清空历史计数，保证后续数据干净。
3. 点 `启用发送`，确保 TX 端允许发包。
4. 设置 `发送次数`、`间隔(ms)`、`模式`、`内容`。
5. 先发一组小包，例如：
	- 次数：`5`
	- 间隔：`100`
	- 模式：`ASCII`
	- 内容：`HELLO`
6. 观察日志和统计面板：
	- TX 端看 `ack_ok / ack_fail / retries_sum / retries_max`
	- RX 端看 `rx_pkt / frame_ok / crc_fail / gap / dup / ooo`
7. 如果连通但统计不变，先确认两件事：
	- 你连接的是正确角色的板子
	- 另一端 RX 板也已经上电并处于监听状态

#### 9.4.4 手机连接后的说明

如果你只是用手机连上 ESP32 的热点，这一步通常只能证明无线网络已经打通；**当前工程没有手机原生 App**。手机要真正发命令，需要额外安装支持 TCP 连接的调试工具，或者继续用电脑 GUI。

简单判断方式：

1. 手机能看到热点并成功连入，说明 SoftAP 正常。
2. 电脑或手机上的 TCP 客户端能连到 `192.168.4.1:3333`，说明命令通道正常。
3. 收到 `OK AUTH` 后，说明控制权限已经建立。
4. 收到 `STAT ...`，说明状态查询链路正常。
5. 收到 `OK queued` 后，说明发包命令已经进入 ESP32 的发送队列。

#### 9.4.5 常见问题排查

1. 连接不上热点：检查密码是否为 `nrf24ctrl`，SSID 是否为 `NRF24_CTRL`。
2. 能连 Wi-Fi 但 TCP 不通：检查是不是连到了别的 Wi-Fi，或 IP 不在 `192.168.4.x` 段。
3. 认证失败：检查 Token 是否为 `nrf24`。
4. 能发命令但 RX 没反应：检查两块板的频道、地址、角色、供电和天线。
5. ACK 很差：先把空中速率降到 `250Kbps`，再重新测试。

#### 9.4.6 手机端调试方案

如果你想**完全用手机来调试**，当前工程最稳妥的做法是：手机先连 ESP32 热点，再用一个支持 TCP 连接的“网络调试助手”类 App 连接控制端口。这样不需要电脑 GUI，也能完成和电脑相同的命令调试。

手机端推荐流程：

1. 手机连接 ESP32 热点 `NRF24_CTRL`，密码 `nrf24ctrl`。
2. 打开手机上的 TCP 客户端工具，新增一个 TCP 连接：
	- 主机/IP：`192.168.4.1`
	- 端口：`3333`
	- 连接类型：TCP Client
3. 建立连接后，先发送认证命令：
	- `AUTH nrf24`
4. 如果收到 `OK AUTH`，再按下面顺序调试：
	- `STATUS`
	- `RESETSTATS`
	- `ENABLE 1`
	- `BURST 5 100 HELLO`
5. 观察返回内容：
	- `OK queued`：说明命令进入发送队列
	- `STAT role=...`：说明统计查询正常
	- `RX ok ...`：说明 RX 端已经收到包
6. 如果要发十六进制数据，可直接发：
	- `BURSTHEX 5 100 101010`

手机端调试的要点：

- 手机只负责“连热点 + 发 TCP 命令”，不需要额外串口线。
- 这条链路本质上和电脑 GUI 发的是同一批命令，所以行为一致。
- 如果手机 App 支持保存常用指令，可以把 `AUTH nrf24`、`STATUS`、`RESETSTATS`、`BURST ...` 设为快捷按钮，测试会快很多。

手机端常见限制：

- 有些手机 TCP 客户端默认会加自动换行设置，建议保持“发送行尾换行”开启。
- 如果 App 支持“发送 HEX/ASCII 切换”，请确认发的是文本命令，不是二进制报文。
- 若手机系统限制后台网络，调试时保持 App 处于前台。
=======
## 10. 方案B：手机 BLE 上位机控制 ESP32 触发 nRF24 发包

项目现在支持 BLE GATT 控制通道，手机端可以直接下发命令控制 NRF24 收发。

### 10.1 固件侧配置

在 `menuconfig -> NRF24 Configuration -> Control Interfaces` 中配置：

- `NRF24_CONTROL_IF_BLE = y`
- `NRF24_BLE_DEVICE_NAME = NRF24-BLE-S3`（可自定义）

然后执行：

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

### 10.2 手机上位机工程

Android 工程位于：`mobile/nrf24_ble_controller_android`

功能：

- 扫描 BLE 设备并自动按服务 UUID 连接
- 发送 `ENABLE / STATUS / RESETSTATS / STOP`
- 发送 `BURST / BURSTHEX`
- 订阅 Notify 接收 `OK/ERR/STAT` 回包

### 10.3 BLE UUID

- Service: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- CMD(Write): `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
- Notify: `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`

### 10.4 命令协议（与串口一致）

- `HELP`
- `STATUS`
- `RESETSTATS`
- `ENABLE 1|0`
- `STOP`
- `BURST <count> <interval_ms> <ascii_payload>`
- `BURSTHEX <count> <interval_ms> <hex_payload>`

详细流程、验收清单与风险规避请参考：

- [docs/ble_mobile_control.md](docs/ble_mobile_control.md)
>>>>>>> 60d7e4e3087daa288b53c0249b3cc0d8a3e662e1
