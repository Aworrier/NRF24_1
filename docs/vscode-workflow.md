# VS Code + ESP-IDF 开发流程（Windows）

本文档面向本仓库新手，目标是让你在 VS Code 里完成：

1. 环境安装
2. 驱动安装
3. 插件使用
4. 编译、烧录、监视
5. 资料检索与问题定位

---

## 1. 先准备什么

- Windows 10/11
- 一根稳定的 USB 数据线（不是仅充电线）
- 一块 ESP32 开发板（本仓库常用 ESP32-S3）
- VS Code

建议先关闭各种串口占用软件（串口助手、上位机、其他 monitor），避免端口被占用。

---

## 2. VS Code 必装插件

### 2.1 ESP-IDF（核心）

- 插件名：`Espressif IDF`
- 发布者：`Espressif Systems`
- 作用：一键配置工具链、设置目标芯片、build/flash/monitor、menuconfig。

安装后，按 `Ctrl+Shift+P` 打开命令面板，输入 `ESP-IDF: Configure ESP-IDF extension`，按向导完成安装。

### 2.2 C/C++（建议）

- 插件名：`C/C++`
- 发布者：`Microsoft`
- 作用：跳转定义、语义高亮、错误提示。

### 2.3 CMake Tools（可选）

- 插件名：`CMake Tools`
- 作用：辅助查看 CMake 项目结构（ESP-IDF 核心流程仍以 Espressif 插件为主）。

---

## 3. Windows 驱动怎么装

ESP32 板子常见 USB 转串口芯片对应驱动如下：

- `CP210x`（Silicon Labs）
- `CH340/CH341`（WCH）
- `FT232`（FTDI）

如果你不确定芯片型号：

1. 接上开发板
2. 打开“设备管理器”
3. 查看“端口 (COM 和 LPT)”或“其他设备”中的新设备名称

### 3.1 判断驱动是否正确

驱动正确时：

- 设备管理器能看到 `USB Serial Port (COMx)` 或厂商对应名称（不能是微软作为厂商的串口）
- VS Code/ESP-IDF 能列出串口
- 运行 monitor 有输出

驱动不正确时通常会出现：

- 设备带黄色感叹号
- 找不到串口
- 烧录时报 `port not found` / `permission denied`

### 3.2 常见补充

- 更换 USB 口（优先主板后置口）
- 更换数据线
- 关闭占用串口的软件
- 安装驱动后重新插拔板子

---

## 4. 打开仓库后的标准流程

以下流程适用于本仓库 `NRF24_1`：

### 4.1 打开项目

在 VS Code 里选择“打开文件夹”，打开本仓库根目录。

### 4.2 配置 ESP-IDF 环境

命令面板执行：

- `ESP-IDF: Configure ESP-IDF extension`
- `ESP-IDF: Doctor command`（检查环境完整性）

### 4.3 选择目标芯片

命令面板执行：

- `ESP-IDF: Set Espressif Device Target`

本项目常用：`esp32s3`。

### 4.4 选择串口

命令面板执行：

- `ESP-IDF: Select Port to Use`

选择对应 `COMx`。

### 4.5 配置参数

命令面板执行：

- `ESP-IDF: SDK Configuration editor (Menuconfig)`

本仓库重点配置在 NRF24 菜单：

- 角色（TX / RX）
- channel
- data rate
- 地址宽度与 pipe0 地址

### 4.6 编译 / 烧录 / 监视

你可以在状态栏按钮或命令面板里执行：

- `ESP-IDF: Build your project`
- `ESP-IDF: Flash your project`
- `ESP-IDF: Monitor your device`
- 或一步到位：`ESP-IDF: Build, Flash and Monitor`

---

## 5. 本仓库双板联调建议（TX/RX）

1. 先把 A 板配置为 RX，build+flash+monitor
2. 再把 B 板配置为 TX，build+flash+monitor
3. 两边参数保持一致：channel/data rate/address width/pipe0
4. 观察日志：
   - TX 有 `TX ok ...`
   - RX 有 `RX pipe=... len=... data='...'`

若 TX 超时，先回到 Tutorial 模式按 `docs/debug-playbook.md` 做 ACK 诊断。

---

## 6. 插件怎么高效使用

### 6.1 最常用命令（建议收藏）

- `ESP-IDF: Build your project`
- `ESP-IDF: Flash your project`
- `ESP-IDF: Monitor your device`
- `ESP-IDF: Set Espressif Device Target`
- `ESP-IDF: Select Port to Use`
- `ESP-IDF: SDK Configuration editor (Menuconfig)`

你可以在命令面板里反复输入关键字 `ESP-IDF` 快速定位。

### 6.2 看日志的习惯

- 先看第一条错误，不要只看最后一条
- 把 `monitor` 输出和 `build` 输出分开看
- 失败时先确认串口和目标芯片是否选对

---

## 7. 资料怎么查（推荐顺序）

建议按“官方优先、由浅入深”的顺序：

1. ESP-IDF 官方文档（概念和 API 最权威）
2. 芯片官方 Datasheet / TRM（寄存器、硬件细节）
3. 本仓库文档：
   - `docs/quickstart.md`
   - `docs/debug-playbook.md`
   - `docs/advanced.md`
4. 官方 GitHub issue / discussion（查历史问题）

### 7.1 关键词模板（可直接套用）

- `ESP-IDF nrf24 esp32s3 timeout`
- `ESP32-S3 monitor no serial data`
- `idf.py flash failed COMx`
- `nrf24 auto ack timeout`

### 7.2 检索技巧

- 先搜报错原文，保留关键错误码
- 加上芯片型号：`esp32s3`
- 加上版本词：`esp-idf v5`
- 查到方案后，优先找“可复现步骤”而非只看结论

---

## 8. 常见问题速查（VS Code 视角）

### Q1: 插件装好了但命令不可用

- 重启 VS Code
- 重新执行 `ESP-IDF: Configure ESP-IDF extension`
- 用 `ESP-IDF: Doctor command` 检查依赖

### Q2: 找不到串口

- 检查驱动是否正确
- 换 USB 数据线
- 换 USB 口
- 关闭占用串口的软件

### Q3: 能烧录但 monitor 没日志

- 确认波特率设置一致
- 按开发板 `EN/RESET` 看是否有启动日志
- 检查是否选择了错误 COM 口

---

## 9. 推荐工作节奏（新手）

1. 先保证驱动和串口正常
2. 再保证 `build` 成功
3. 再做 `flash + monitor`
4. 最后再改参数和功能

遇到问题时，记录三件事：芯片型号、ESP-IDF 版本、完整错误日志首段。这样排查速度会明显提升。
