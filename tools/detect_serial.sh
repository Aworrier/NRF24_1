#!/bin/bash
# detect_serial.sh — WSL 侧串口设备诊断
# 用法: bash tools/detect_serial.sh

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo "========================================"
echo "  NRF24 项目 — WSL 串口设备诊断"
echo "========================================"

# ── 0. 先检查内核 usbip 支持 ──
echo ""
echo -e "${CYAN}[0] WSL 内核 usbip 支持${NC}"
if lsmod 2>/dev/null | grep -q usbip; then
    echo -e "  ${GREEN}✓ usbip 内核模块已加载${NC}"
    lsmod 2>/dev/null | grep usbip | while read -r mod _; do echo "    $mod"; done
elif modprobe -n usbip-core 2>/dev/null; then
    echo -e "  ${YELLOW}⚠ usbip 模块可用但未加载${NC} (设备挂载时会自动加载)"
else
    echo -e "  ${RED}✗ 内核不支持 usbip${NC} — 请升级 WSL2 内核"
fi

# ── 1. USB 总线设备（Windows 挂载过来会出现在这里） ──
echo ""
echo -e "${CYAN}[1] USB 总线设备 (lsusb)${NC}"
devcount=$(lsusb 2>/dev/null | wc -l)
echo "  USB 设备总数: $devcount"
if lsusb 2>/dev/null | grep -qiE "CP210|CH340|CH341|FTDI|FT232|PL2303|Silicon|QinHeng|WCH|Prolific|Espressif|Serial|UART|Arduino|RP2040|STM32"; then
    echo -e "  ${GREEN}✓ 检测到串口芯片:${NC}"
    lsusb 2>/dev/null | grep -iE "CP210|CH340|CH341|FTDI|FT232|PL2303|Silicon|QinHeng|WCH|Prolific|Espressif|Serial|UART|Arduino|RP2040|STM32"
else
    echo "  (无串口芯片 — 需先在 Windows 运行 attach-esp32.bat)"
fi

# ── 2. /dev/tty* 设备 ──
echo ""
echo -e "${CYAN}[2] /dev 串口设备${NC}"

has_usb=0
# USB 串口 (真正有用的)
for pat in ttyUSB ttyACM; do
    for dev in /dev/${pat}*; do
        [ -e "$dev" ] || continue
        has_usb=1
        perm=$(stat -c "%a %U:%G" "$dev" 2>/dev/null || echo "?")
        rw=""; [ -r "$dev" ] && [ -w "$dev" ] && rw="${GREEN}读写${NC}" || rw="${RED}无权限${NC}"
        echo -e "  ${YELLOW}$dev${NC}  ($perm)  $rw"
        udevadm info --query=property --name="$dev" 2>/dev/null | grep -iE "ID_MODEL=|ID_VENDOR=|ID_SERIAL_SHORT=" | while IFS='=' read -r k v; do
            echo "    $k=$v"
        done
    done
done
if [ "$has_usb" -eq 0 ]; then
    echo -e "  ${RED}✗ 无 /dev/ttyUSB* 或 /dev/ttyACM*${NC}"
fi

# 虚拟串口 (WSL 默认的 COM 端口，无实际硬件)
echo "  /dev/ttyS*: $(ls /dev/ttyS* 2>/dev/null | wc -l) 个 (虚拟 COM 口，非 USB)"

# ── 3. usbip 连接状态 ──
echo ""
echo -e "${CYAN}[3] usbip 连接状态${NC}"
if command -v usbip &>/dev/null; then
    ports=$(usbip port 2>/dev/null)
    if echo "$ports" | grep -q "Port [0-9]"; then
        echo "$ports" | grep -B1 -A3 "Port [0-9]"
        echo -e "  ${GREEN}✓ 有设备通过 usbip 挂载${NC}"
    else
        echo "  (无 usbip 连接)"
    fi
else
    echo "  usbip 工具未安装 (不影响使用, 可选)"
fi

# ── 4. dmesg ──
echo ""
echo -e "${CYAN}[4] dmesg 串口日志 (最近 6 条)${NC}"
dmesg 2>/dev/null | grep -iE "ttyUSB|ttyACM|cp210|ch34|ftdi|pl2303|cdc_acm|usb.*serial|usbip" | tail -6 || echo "  (无)"

# ── 5. 诊断结论 ──
echo ""
echo "====== 诊断结论 ======"
if [ "$has_usb" -eq 1 ]; then
    echo -e "  ${GREEN}USB 串口设备已就绪。${NC}"
    echo "  export ESPPORT=/dev/ttyUSB0"
else
    echo -e "  ${RED}WSL 中没有串口设备。${NC}"
    echo ""
    echo "  原因: WSL 无法直接访问 Windows 的 USB 硬件。"
    echo "  必须先在 Windows 侧将设备 '借给' WSL。"
    echo ""
    echo -e "  ${YELLOW}操作步骤:${NC}"
    echo "  1. 在 Windows 桌面双击 attach-esp32.bat"
    echo "  2. UAC 弹窗点 '是' 获取管理员权限"
    echo "  3. 在出现的菜单中选择你的设备 (输入序号)"
    echo "  4. 回到 WSL 终端, 重新运行: bash tools/detect_serial.sh"
    echo ""
    echo "  注意: 每次 WSL 重启后需要重新挂载。"
fi
echo ""
