#!/bin/bash
# ============================================================
# NRF24 一键烧录脚本
# 用法: ./flash.sh [monitor]
#   ./flash.sh          → 选串口 → 编译 + 烧录
#   ./flash.sh monitor  → 选串口 → 编译 + 烧录 + 打开串口监视
#   ./flash.sh build    → 选串口 → 仅编译（不烧录）
# ============================================================
set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$PROJECT_DIR"

# ---- Step 1: 激活 ESP-IDF 环境 ----
echo "==> 激活 ESP-IDF 环境..."
source /home/dell/.espressif/tools/activate_idf_v6.0.1.sh

# ---- Step 2: 扫描可用串口 ----
echo "==> 扫描串口..."
mapfile -t ports < <(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true)

if [ ${#ports[@]} -eq 0 ]; then
    echo "ERROR: 未检测到任何串口设备 (/dev/ttyUSB* 或 /dev/ttyACM*)"
    echo "请检查 USB 线是否连接，或运行: ls /dev/ttyUSB* /dev/ttyACM*"
    exit 1
fi

# ---- Step 3: 用户选择串口 ----
echo ""
echo "可用串口:"
for i in "${!ports[@]}"; do
    echo "  [$((i+1))]  ${ports[$i]}"
done
echo ""
read -r -p "请选择串口 [1-${#ports[@]}] (默认 1): " choice

if [ -z "$choice" ]; then
    choice=1
fi

if ! [[ "$choice" =~ ^[0-9]+$ ]] || [ "$choice" -lt 1 ] || [ "$choice" -gt ${#ports[@]} ]; then
    echo "ERROR: 无效选择"
    exit 1
fi

PORT="${ports[$((choice-1))]}"
export ESPPORT="$PORT"
echo "==> 已选择串口: $ESPPORT"

# ---- Step 4: 执行操作 ----
MODE="${1:-flash}"

case "$MODE" in
    monitor)
        echo ""
        echo "==> 编译 + 烧录 + 监视 ($ESPPORT)..."
        idf.py -p "$ESPPORT" flash monitor
        ;;
    build)
        echo ""
        echo "==> 仅编译..."
        idf.py build
        echo ""
        echo "编译完成。如需烧录: ./flash.sh"
        ;;
    flash)
        echo ""
        echo "==> 编译 + 烧录 ($ESPPORT)..."
        idf.py -p "$ESPPORT" flash
        echo ""
        echo "烧录完成。如需监视: ./flash.sh monitor"
        ;;
    *)
        echo "用法: ./flash.sh [flash|monitor|build]"
        exit 1
        ;;
esac
