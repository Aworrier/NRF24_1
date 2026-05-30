# ESP32-S3 + NRF24L01+ 无线通信教学项目

基于 ESP-IDF 的无线通信教学项目，使用两块 ESP32-S3 开发板搭配 NRF24L01+ 2.4GHz 收发模块，实现可靠的无线数据传输。适合嵌入式开发和物联网通信初学者。

> 目标芯片：ESP32-S3（其他目标仅保留历史配置）。

## 项目功能

- **NRF24L01+ 驱动**：完整的 SPI 驱动，支持寄存器级配置（信道、速率、功率、地址、CRC、自动重传、动态/静态负载长度）。
- **双角色模式**：通过 menuconfig 切换 TX（发送）和 RX（接收）角色，两块板配合工作。
- **可配置 MAC 层**：
  - **ALOHA**：纯随机接入，在每个时隙以可配置概率 `q` 发送。
  - **CSMA**：载波侦听多路访问，发送前通过 RPD 寄存器检测信道忙闲，信道忙时随机退避。
- **自定义帧协议**：8 字节头部 + 最多 22 字节负载 + 2 字节 CRC16-CCITT 校验，含魔数、版本号、序列号、标志位等字段。
- **实时统计**：TX 端跟踪发送数、成功/失败数、重试次数；RX 端跟踪收包数、CRC 错误、帧解析错误、序列重复/乱序/丢包检测。
- **多控制接口**：
  - **UART 串口命令**：类 shell 交互界面，支持 `HELP`、`STATUS`、`ENABLE`、`MAC`、`BURST`、`BURSTHEX` 等命令。
  - **WiFi/TCP 远程控制**：ESP32 创建 SoftAP 热点，提供 TCP Server（端口 3333），支持远程命令控制和认证。
- **PC 上位机**：基于 Python Tkinter 的 GUI 工具，通过 UART 或 WiFi/TCP 连接，提供图形化配置和实时统计显示。
- **引脚自检模式**：调试连接问题，循环翻转引脚并记录结果。

## 硬件需求

| 组件 | 说明 |
|------|------|
| 主控 | ESP32-S3 开发板 ×2 |
| 射频模块 | NRF24L01+ ×2 |
| 电源去耦 | 10uF + 0.1uF 电容并联在模块 3.3V/GND |

### 默认引脚连接 (ESP32-S3)

| NRF24L01+ 引脚 | ESP32-S3 GPIO |
|----------------|---------------|
| MOSI | GPIO13 |
| MISO | GPIO12 |
| SCK | GPIO14 |
| CSN | GPIO11 |
| CE | GPIO10 |
| IRQ | GPIO3 |

> SPI 主机：SPI2_HOST。引脚可在 menuconfig 中修改。

## 软件架构

```
  +---------------------------------------------------------------+
  |  PC 上位机 GUI (tools/pc_nrf24_controller.py)                 |
  |  (Tkinter, UART 或 WiFi/TCP 通信)                             |
  +---------------------------------------------------------------+
          | UART (USB CDC) 或 WiFi/TCP (socket)
          v
  +---------------------------------------------------------------+
  |  控制/接口层                                                   |
  |  app_control.c       -- UART 命令解析器                        |
  |  app_wifi_control.c  -- WiFi SoftAP + TCP Server               |
  +---------------------------------------------------------------+
          |
          v
  +---------------------------------------------------------------+
  |  应用逻辑层                                                    |
  |  app_main.c    -- 入口、初始化                                 |
  |  app_config.c  -- menuconfig 映射、地址设置                    |
  |  app_proto.c   -- 帧构建/解析、CRC16                           |
  |  app_stats.c   -- 统计跟踪、序列分析                           |
  |  app_tx.c      -- 发送任务、时隙调度、ALOHA/CSMA               |
  |  app_rx.c      -- 接收任务、IRQ 处理、帧消费                   |
  +---------------------------------------------------------------+
          |
          v
  +---------------------------------------------------------------+
  |  NRF24 驱动层 (nrf24.c / nrf24.h)                              |
  |  数据面：发送流程 (CE脉冲→轮询IRQ)、接收流程 (FIFO drain)      |
  |  控制面：SPI 事务、寄存器读写、无线配置、电源管理               |
  +---------------------------------------------------------------+
          |
          v
  +---------------------------------------------------------------+
  |  ESP-IDF 基础层                                                |
  |  SPI 驱动、GPIO 驱动、WiFi 栈、lwIP TCP/IP、FreeRTOS          |
  +---------------------------------------------------------------+
          |
          v
  +---------------------------------------------------------------+
  |  NRF24L01+ 模块 (外部硬件)                                     |
  |  2.4 GHz 收发器，SPI 接口                                      |
  +---------------------------------------------------------------+
```

### FreeRTOS 任务

| 任务名 | 优先级 | 功能 |
|--------|--------|------|
| `uart_cmd` | 9 | 从 stdin 读取命令行并分发 |
| `tcp_ctrl` | 8 | 接受 TCP 客户端，认证，转发命令 |
| `nrf24_tx` | 8 | 时隙调度、MAC 门控、数据发送 |
| `nrf24_irq` | 9 | 等待 IRQ 事件，从 RX FIFO 取出负载 |
| `nrf24_rx` | 7 | 消费 RX 负载队列，帧解析，统计更新 |

## 应用层帧格式 (app_proto)

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | Magic0 | 0xA5 |
| 1 | 1 | Magic1 | 0x5A |
| 2 | 1 | Version | 0x01 |
| 3-4 | 2 | Sequence | 16 位序列号，小端序 |
| 5 | 1 | Payload Length | 0-22 |
| 6 | 1 | Flags | 用户自定义标志 |
| 7 | 1 | Reserved | 保留 |
| 8..N | N | Payload | 最多 22 字节 |
| N+1..N+2 | 2 | CRC16-CCITT | 多项式 0x1021，初始值 0xFFFF |

> 帧总长度 ≤32 字节，匹配 NRF24L01+ 默认静态负载长度。

## 控制命令

所有命令通过 UART 或 TCP 发送，换行符结尾：

| 命令 | 说明 |
|------|------|
| `ENABLE 0\|1` | 启用/禁用 TX 发送 |
| `MAC ALOHA\|CSMA <q%>` | 设置 MAC 模式和概率门限 (0-100) |
| `SLOT <ms> <csma_window>` | 设置时隙间隔和 CSMA 退避窗口 |
| `SLOTLIMIT <max>` | 设置猝发任务最大时隙数 |
| `BURST <cnt> <interval_ms> <payload>` | 发送 N 个 ASCII 负载 |
| `BURSTHEX <cnt> <interval_ms> <hex>` | 发送 N 个十六进制负载 |
| `STOP` | 中止当前猝发任务 |
| `STATUS` | 查询统计状态 |
| `RESETSTATS` | 清零所有计数器 |
| `AUTH <token>` | TCP 连接认证 |

## 快速开始

### 1. 硬件接线

按默认引脚连接 NRF24L01+ 模块到 ESP32-S3，模块电源端并联 10uF + 0.1uF 电容去耦。

### 2. 编译烧录 RX 板

```bash
idf.py set-target esp32s3
idf.py menuconfig
# Project mode → Tutorial Debug
# Application role → RX
idf.py build
idf.py -p <RX_PORT> flash monitor
```

### 3. 编译烧录 TX 板

```bash
idf.py menuconfig
# Application role → TX
idf.py build
idf.py -p <TX_PORT> flash monitor
```

> 确保两块板的信道、数据速率、地址配置一致。

### 4. 测试通信

在 TX 板串口终端输入：
```
ENABLE 1
MAC ALOHA 100
BURST 10 500 Hello
```

RX 板应收到负载并打印统计信息。使用 `STATUS` 命令查看详细数据。

## 仓库结构

```
NRF24_1/
├── main/
│   ├── app_main.c              # 入口，启动所有模块
│   ├── app_config.c/h          # menuconfig 映射和地址设置
│   ├── app_control.c/h         # UART 命令解析器
│   ├── app_wifi_control.c/h    # WiFi SoftAP + TCP Server
│   ├── app_tx.c/h              # TX 任务：时隙调度、ALOHA/CSMA
│   ├── app_rx.c/h              # RX 任务：IRQ 处理、帧消费
│   ├── app_proto.c/h           # 帧构建/解析 + CRC16
│   ├── app_stats.c/h           # 统计跟踪、序列分析
│   ├── app_pin_test.c/h        # 引脚自检模式
│   ├── nrf24.c/h               # NRF24L01+ SPI 驱动
│   ├── Kconfig.projbuild       # menuconfig 配置项定义
│   └── CMakeLists.txt          # 源文件注册与依赖声明
├── tools/
│   └── pc_nrf24_controller.py  # PC 上位机 GUI
├── docs/                       # 教学文档
│   ├── START_HERE.md
│   ├── quickstart.md
│   ├── CODE_WALKTHROUGH.md
│   ├── CODE_DEEP_DIVE.md
│   ├── C_LANGUAGE_PRIMER.md
│   └── debug-playbook.md
├── partitions.csv              # 分区表 (支持 OTA)
└── CMakeLists.txt              # 顶层构建
```

## WSL 环境编译与烧录

本项目已适配 WSL2 + ESP-IDF v6.0 编译环境。

### 环境初始化（首次）

```bash
# 安装系统依赖（需要 sudo）
sudo apt-get update && sudo apt-get install -y cmake ninja-build python3-pip python3-venv dfu-util ccache libusb-1.0-0

# 克隆并安装 ESP-IDF v6.0
mkdir -p ~/esp && cd ~/esp
git clone --depth 1 --branch v6.0 https://github.com/espressif/esp-idf.git
cd esp-idf && bash install.sh esp32s3
```

### 编译

```bash
# 每次打开新终端都需要先激活环境
source ~/esp/esp-idf/export.sh
idf.py set-target esp32s3   # 仅首次
idf.py build
```

### 烧录（WSL 特殊性）

WSL2 默认无法直接访问 USB 设备。两种方案：

**方案一：usbipd-win（推荐，在 WSL 中完成烧录）**

```powershell
# 在 Windows PowerShell（管理员）中：
usbipd wsl list                          # 查看 USB 设备
usbipd wsl bind --busid <BUSID>          # 绑定 ESP32 到 WSL
usbipd wsl attach --busid <BUSID> --wsl  # 挂载设备
```

```bash
# 回到 WSL 终端：
ls /dev/ttyUSB* /dev/ttyACM*             # 确认串口出现
idf.py -p /dev/ttyUSB0 flash monitor     # 烧录并查看串口
```

**方案二：WSL 编译 + Windows 烧录**

```bash
# WSL 中编译：
idf.py build

# 在 Windows PowerShell 中烧录（使用相同 ESP-IDF 版本）：
idf.py -p COM7 flash monitor
```

> 也可以使用 VS Code ESP-IDF 扩展的 GUI 烧录按钮，扩展会自动处理端口。

## 技术参数

| 参数 | 支持选项 |
|------|----------|
| 空中速率 | 250Kbps / 1Mbps / 2Mbps |
| 发射功率 | -18dBm / -12dBm / -6dBm / 0dBm |
| CRC | 1 字节 / 2 字节 |
| 地址宽度 | 3-5 字节 |
| 重传次数 | 0-15 (自动重传延迟 250us × N) |
| 信道 | 0-125 (2.400-2.525 GHz) |
| 负载长度 | 默认 32 字节 (支持动态负载) |

## 文档入口

- [docs/START_HERE.md](docs/START_HERE.md) — 从这里开始
- [docs/README.md](docs/README.md) — 学习路径与调试笔记
- [docs/quickstart.md](docs/quickstart.md) — 快速入门
- [docs/CODE_WALKTHROUGH.md](docs/CODE_WALKTHROUGH.md) — 代码走读
- [docs/CODE_DEEP_DIVE.md](docs/CODE_DEEP_DIVE.md) — 代码深入分析
- [docs/C_LANGUAGE_PRIMER.md](docs/C_LANGUAGE_PRIMER.md) — C 语言基础
- [docs/debug-playbook.md](docs/debug-playbook.md) — 调试手册
