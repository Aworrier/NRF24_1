# NRF24 ESP-IDF CLI 命令参考手册

## 1. 环境激活

每个新终端必须先执行：

```bash
source env.sh
```

等价于手动执行：

```bash
source /home/dell/.espressif/tools/activate_idf_v6.0.1.sh
export ESPPORT=/dev/ttyUSB0
```

---

## 2. 编译 & 烧录 & 监视

```bash
# 仅编译
idf.py build

# 编译 + 烧录 + 打开串口监视 (一键)
idf.py -p /dev/ttyUSB0 flash monitor

# 仅烧录
idf.py -p /dev/ttyUSB0 flash

# 仅监视串口
idf.py -p /dev/ttyUSB0 monitor

# 烧录并监视 (指定波特率)
idf.py -p /dev/ttyUSB0 -b 921600 flash monitor
```

`monitor` 模式下按 `Ctrl+]` 退出。

---

## 3. 清理 & 重新配置

```bash
# 深度清理 (包括 CMake 缓存，解决大部分构建问题)
idf.py fullclean

# 仅清理编译产物 (保留 CMake 缓存)
idf.py clean

# 重新生成 CMake 配置 (改了 menuconfig 后可能需要)
idf.py reconfigure

# 完全重建
idf.py fullclean && idf.py build
```

---

## 4. 项目配置

```bash
# 打开 menuconfig 图形配置界面
idf.py menuconfig

# 设置目标芯片 (仅首次或更换芯片时)
idf.py set-target esp32s3

# 查看当前项目大小
idf.py size

# 查看各组件占用
idf.py size-components

# 查看详细的固件分区大小
idf.py size-files
```

---

## 5. WSL2 USB 串口管理

### 5.1 可用串口查看

```bash
# ===== 在 WSL 终端中运行 =====

# 列出所有 USB 串口设备
ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null

# 列出所有串口 (包括虚拟串口)
ls /dev/ttyS* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null

# 查看串口详细属性 (包括芯片型号、序列号)
ls -la /dev/serial/by-id/ 2>/dev/null

# 查看串口对应的 USB 路径
ls -la /dev/serial/by-path/ 2>/dev/null

# 使用 udevadm 查看串口详细信息
udevadm info /dev/ttyUSB0

# 查看当前 USB 设备列表
lsusb

# 检测串口是否被占用
sudo lsof /dev/ttyUSB0 2>/dev/null || echo "端口空闲"
```

### 5.2 USB 设备挂载 (usbipd)

```powershell
# 在 Windows PowerShell (管理员) 中运行:

# 查看所有 USB 设备及其 BUSID
usbipd wsl list

# 挂载 ESP32-S3 到 WSL (替换 <BUSID> 为实际值，如 1-1)
usbipd wsl attach --busid <BUSID>

# 查看已挂载的设备
usbipd wsl list

# 断开挂载
usbipd wsl detach --busid <BUSID>
```

### 5.3 串口权限修复

```bash
# 如果提示权限拒绝 (Permission denied)
sudo chmod 666 /dev/ttyUSB0

# 永久解决：将用户加入 dialout 组 (需重新登录)
sudo usermod -aG dialout $USER
```

---

## 6. NRF24 应用层命令

以下命令在串口监视器 (`idf.py -p /dev/ttyUSB0 monitor`) 中直接输入:

```bash
# 查看所有命令
HELP

# 查看当前状态和统计
STATUS

# 清零统计计数器
RESETSTATS

# 启用/禁用 TX 发送 (0=禁用, 1=启用)
ENABLE 1
ENABLE 0

# 设置 MAC 模式 (ALOHA 或 CSMA) 和概率门限 (0-100)
MAC ALOHA 100
MAC CSMA 50

# 设置时隙间隔 (ms) 和 CSMA 退避窗口 (ms)
SLOT 100 200

# 设置猝发任务最大时隙数
SLOTLIMIT 100

# 发送 ASCII 负载 (<个数> <间隔ms> <内容>)
BURST 10 500 Hello

# 发送十六进制负载 (<个数> <间隔ms> <hex>)
BURSTHEX 5 200 AABBCCDD

# 中止当前猝发任务
STOP

# 重置统计
RESETSTATS

# TCP 认证 (WiFi 模式下)
AUTH <token>
```

---

## 7. 常见问题速查

| 问题 | 解决 |
|------|------|
| `idf.py: command not found` | 先执行 `source env.sh` |
| CMake 配置报错 | `idf.py fullclean && idf.py build` |
| 找不到 `/dev/ttyUSB0` | 检查 usbipd 挂载: `ls /dev/tty*` |
| 串口权限拒绝 | `sudo chmod 666 /dev/ttyUSB0` |
| 烧录失败 "wrong chip" | 检查芯片型号: `idf.py set-target esp32s3` |
| 编译产物过大 | `idf.py size` 查看占用 |
| WSL 无法访问 USB | 在 Windows 端执行 `usbipd wsl attach` |

---

## 8. 快速操作速查卡

```
# ===== 日常开发 =====
source env.sh                              # 激活环境
idf.py build                               # 编译
idf.py -p /dev/ttyUSB0 flash monitor       # 烧录+监视

# ===== 清理重建 =====
idf.py fullclean && idf.py build           # 完全清理+编译

# ===== 首次配置 =====
idf.py set-target esp32s3                  # 设置芯片
idf.py menuconfig                          # 配置项目
idf.py build                               # 编译

# ===== WSL 串口 =====
ls /dev/ttyUSB* /dev/ttyACM*              # 列出可用串口
ls -la /dev/serial/by-id/                  # 查看串口详细信息
lsusb                                      # 查看 USB 设备
sudo chmod 666 /dev/ttyUSB0               # 修复串口权限

# ===== 调试 =====
idf.py -p /dev/ttyUSB0 monitor             # 查看串口输出
idf.py size                                # 查看固件大小
```
