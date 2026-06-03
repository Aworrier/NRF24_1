# 构建与烧录完全指南 / Build & Flash Guide

> 最后修订: 2026-06-03
> 适用项目: NRF24_1 (ESP32-S3 + NRF24L01+)

本文覆盖本项目**所有**编译和烧录途径，新手按编号顺序阅读，选一种最适合你的。

---

## 0. 前置条件

无论用哪种方法，都需要：

1. **ESP32-S3 开发板** 通过 USB 线连接到电脑
2. **WSL2 用户**：先在 Windows PowerShell（管理员）中挂载 USB 设备：
   ```
   usbipd wsl list                        # 查看设备 BUSID
   usbipd wsl attach --busid <BUSID>      # 挂载到 WSL
   ```
   然后在 WSL 终端确认：`ls /dev/ttyUSB*` 能看到设备
3. **串口权限**（Linux/WSL）：如遇 `Permission denied`，运行 `sudo chmod 666 /dev/ttyUSB0`

---

## 方法对比一览

| 方法 | 适用场景 | 难度 |
|------|----------|------|
| [1. Claude Code `/flash`](#方法-1claude-code-内置-flash) | 已用 Claude Code 开发，一键完成 | ★☆☆ |
| [2. `bash tools/flash.sh`](#方法-2终端一键脚本) | 终端手动操作，自动检测串口 | ★☆☆ |
| [3. `source tools/env.sh` + `idf.py`](#方法-3手动-idfpy-命令) | 需要完全控制编译参数 | ★★☆ |
| [4. VS Code + ESP-IDF 插件](#方法-4vs-code--esp-idf-插件) | GUI 图形界面操作 | ★★☆ |
| [5. PC 上位机 GUI](#方法-5pc-上位机-gui) | WiFi 无线控制 TX/RX | ★★★ |

---

## 方法 1：Claude Code 内置 `/flash`

在 Claude Code 对话中直接输入 `/flash`，AI 会自动完成：检测串口 → 激活环境 → 编译 → 烧录。

```
/flash
```

提示选择模式时：
- `flash` — 编译 + 烧录（最常用）
- `monitor` — 编译 + 烧录 + 打开串口监视器（按 `Ctrl+]` 退出）
- `build` — 仅编译，不烧录

**优点**：零命令记忆，AI 自动处理串口检测、权限问题、环境激活。

---

## 方法 2：终端一键脚本

### 最常用：编译 + 烧录

```bash
bash tools/flash.sh
```

脚本会自动：
1. 激活 ESP-IDF 环境
2. 扫描可用串口（多个时列出让你选）
3. 编译 + 烧录到 ESP32-S3

### 其他模式

```bash
bash tools/flash.sh flash      # 编译 + 烧录（默认）
bash tools/flash.sh monitor    # 编译 + 烧录 + 串口监视
bash tools/flash.sh build      # 仅编译，不烧录
```

### 指定串口（跳过自动检测）

```bash
bash tools/flash.sh -p /dev/ttyUSB0 flash
```

### 切换 TX/RX 角色

编译前需通过 menuconfig 切换角色：

```bash
idf.py menuconfig
# 进入 Application role → 选择 TX (Sender) 或 RX (Receiver)
# 按 S 保存，按 Q 退出
```

或直接用 Claude Code：`"切换成 TX 固件，重新编译"`。

**优点**：无需手动激活环境，自动串口检测，适合日常开发。

---

## 方法 3：手动 `idf.py` 命令

需要完全控制每个步骤时使用。

### 第 1 步：激活环境

```bash
source tools/env.sh
```

这行命令会激活 ESP-IDF 工具链 + 自动检测串口。输出示例：
```
ESP-IDF v6.0.1 | 串口: /dev/ttyUSB0 (自动检测) | 目标: esp32s3
```

也可以指定串口：
```bash
source tools/env.sh /dev/ttyUSB0
```

或仅查看可用串口（不激活）：
```bash
source tools/env.sh --list
```

### 第 2 步：编译 + 烧录

```bash
# 仅编译
idf.py build

# 编译 + 烧录
idf.py -p /dev/ttyUSB0 flash

# 编译 + 烧录 + 串口监视
idf.py -p /dev/ttyUSB0 flash monitor
```

### 切换 TX/RX 角色

```bash
idf.py menuconfig
# Application role → 选 TX 或 RX → 保存退出
idf.py build
```

### 完全清理重建

```bash
idf.py fullclean
idf.py build
```

**优点**：完全透明，适合调试构建问题或 CI 集成。

---

## 方法 4：VS Code + ESP-IDF 插件

参考完整文档：[vscode-workflow.md](vscode-workflow.md)

简要步骤：
1. VS Code 安装 "Espressif IDF" 插件
2. 插件配置 ESP-IDF 路径
3. 左下角设置目标芯片为 `esp32s3`
4. 底部工具栏：齿轮(Build) → 选择串口 → 闪电(Flash) → 显示器(Monitor)

---

## 方法 5：PC 上位机 GUI

通过 WiFi 无线控制 TX/RX 设备，无需串口线。

```bash
source tools/env.sh       # 先激活环境
python tools/pc_nrf24_controller.py
```

启动后通过 GUI 界面连接 TX/RX 设备，发送命令、查看统计、控制 JAM 干扰源。

详见：[pc_gui_workflow.md](pc_gui_workflow.md)

---

## 常见问题

### 看不到串口设备

```bash
ls /dev/ttyUSB* /dev/ttyACM*
# 无输出 → USB 未连接或未挂载到 WSL
```

- **Windows 直接连**：检查设备管理器是否有 "COMx"
- **WSL2**：`usbipd wsl attach --busid <ID>`
- **Linux**：`sudo dmesg | grep tty` 查看内核是否识别到设备

### 串口权限被拒

```bash
sudo chmod 666 /dev/ttyUSB0
```

### "should be sourced, not executed"

已修复。`tools/flash.sh` 和 `tools/env.sh` 不再依赖 `source activate`，可直接 `/bin/bash` 执行。

### 编译失败

```bash
idf.py fullclean
idf.py build
```

### 烧录失败（芯片不匹配）

确认目标芯片正确：
```bash
idf.py set-target esp32s3
```

### 串口监视器退出

按 `Ctrl + ]`（不是 Ctrl+C）。
