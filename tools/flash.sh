#!/bin/bash
# ============================================================
# NRF24 一键烧录脚本（多串口支持）
# 用法:
#   tools/flash.sh              → 选串口 → 编译 + 烧录
#   tools/flash.sh monitor      → 选串口 → 编译 + 烧录 + 监视
#   tools/flash.sh build        → 选串口 → 仅编译
#   tools/flash.sh -p /dev/ttyACM0  → 指定串口烧录
#
# 环境变量 ESPPORT 优先: export ESPPORT=/dev/ttyUSB0
# ============================================================
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

# ── 激活 ESP-IDF ──
echo "==> 激活 ESP-IDF 环境..."
source /home/dell/.espressif/tools/activate_idf_v6.0.1.sh

# ── 解析参数: -p 指定串口 ──
MODE="flash"
USER_PORT=""
for arg in "$@"; do
    case "$arg" in
        -p) shift; continue ;;  # next arg is port, handled below
        monitor|flash|build) MODE="$arg" ;;
        /dev/*) USER_PORT="$arg" ;;
    esac
done
# Parse -p PORT
while [ $# -gt 0 ]; do
    case "$1" in
        -p) USER_PORT="$2"; shift 2 ;;
        *) shift ;;
    esac
done

# ── 确定串口 ──
if [ -n "$USER_PORT" ]; then
    PORT="$USER_PORT"
    echo "==> 使用指定串口: $PORT"
elif [ -n "$ESPPORT" ] && [ -e "$ESPPORT" ]; then
    PORT="$ESPPORT"
    echo "==> 使用环境串口: $PORT (来自 \$ESPPORT)"
else
    # 扫描可用串口
    mapfile -t ports < <(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true)

    if [ ${#ports[@]} -eq 0 ]; then
        echo ""
        echo "ERROR: 未检测到任何串口设备"
        echo "  1. 先在 Windows 桌面双击 attach-esp32.bat"
        echo "  2. 再运行: bash tools/detect_serial.sh"
        exit 1
    fi

    echo ""
    echo "可用串口:"
    for i in "${!ports[@]}"; do
        info=""
        mfg=$(udevadm info --query=property --name="${ports[$i]}" 2>/dev/null | grep ID_VENDOR_FROM_DATABASE= | cut -d= -f2)
        [ -n "$mfg" ] && info="  ← $mfg"
        echo "  [$((i+1))] ${ports[$i]}$info"
    done

    if [ ${#ports[@]} -eq 1 ]; then
        echo ""
        PORT="${ports[0]}"
        echo "唯一串口，自动选择: $PORT"
    else
        echo ""
        read -r -p "选择串口 [1-${#ports[@]}] (默认 1): " choice
        [ -z "$choice" ] && choice=1
        if ! [[ "$choice" =~ ^[0-9]+$ ]] || [ "$choice" -lt 1 ] || [ "$choice" -gt ${#ports[@]} ]; then
            echo "ERROR: 无效选择"
            exit 1
        fi
        PORT="${ports[$((choice-1))]}"
    fi
    export ESPPORT="$PORT"
fi

echo "==> 串口: $PORT"
echo ""

# ── 执行 ──
case "$MODE" in
    monitor)
        echo "==> 编译 + 烧录 + 监视..."
        idf.py -p "$PORT" flash monitor
        ;;
    build)
        echo "==> 仅编译..."
        idf.py build
        echo "编译完成。烧录: tools/flash.sh"
        ;;
    flash)
        echo "==> 编译 + 烧录..."
        idf.py -p "$PORT" flash
        echo ""
        echo "烧录完成。监视: tools/flash.sh monitor"
        ;;
    *)
        echo "用法: tools/flash.sh [flash|monitor|build] [-p /dev/ttyXXX]"
        exit 1
        ;;
esac
