#!/bin/bash
# env.sh — ESP-IDF 环境激活 + 串口自动检测
# 用法:
#   source tools/env.sh              → 在当前 shell 激活（推荐）
#   source tools/env.sh /dev/ttyACM0 → 手动指定串口
#   source tools/env.sh --list       → 仅列出可用串口（不激活）
#   暂时不可以直接点右上角运行按钮直接运行

ACTIVATE_SCRIPT="/home/dell/.espressif/tools/activate_idf_v6.0.1.sh"
ENV_SH_PATH="$(realpath "${BASH_SOURCE[0]}")"

# ── 检测是否被 source ──
_is_sourced() { [ "${BASH_SOURCE[0]}" != "$0" ]; }

# ── 直接执行分支：用 exec 替换当前进程，启动带 ESP-IDF 环境的新 bash ──
if ! _is_sourced; then
    exec /bin/bash -i --rcfile <(cat "$HOME/.bashrc" 2>/dev/null; echo "source '$ACTIVATE_SCRIPT'"; echo "source '$ENV_SH_PATH' $*")
fi

# ═══════════════════════════════════════════════════════════
# 以下仅在 sourced 模式运行
# ═══════════════════════════════════════════════════════════

if [ "$1" != "--list" ]; then
    source "$ACTIVATE_SCRIPT"
fi

# ── 扫描可用 USB 串口 ──
ports=()
for p in /dev/ttyUSB* /dev/ttyACM*; do
    [ -e "$p" ] && ports+=("$p")
done

# ── 手动指定串口 ──
if [ -n "$1" ] && [ "$1" != "--list" ]; then
    if [ -e "$1" ]; then
        export ESPPORT="$1"
        echo "ESP-IDF v6.0.1 | 串口: $ESPORT (手动指定) | 目标: esp32s3"
        return 0 2>/dev/null || exit 0
    else
        echo "ERROR: 串口 $1 不存在"
        return 1 2>/dev/null || exit 1
    fi
fi

# ── --list 模式 ──
if [ "$1" = "--list" ]; then
    if [ ${#ports[@]} -eq 0 ]; then
        echo "无 USB 串口设备。先在 Windows 运行 attach-esp32.bat"
        return 1 2>/dev/null || exit 1
    fi
    echo "可用串口:"
    for i in "${!ports[@]}"; do
        info=""
        mfg=$(udevadm info --query=property --name="${ports[$i]}" 2>/dev/null | grep ID_VENDOR= | cut -d= -f2)
        model=$(udevadm info --query=property --name="${ports[$i]}" 2>/dev/null | grep ID_MODEL= | cut -d= -f2)
        [ -n "$mfg" ] && info="  ($mfg $model)"
        echo "  [$((i+1))] ${ports[$i]}$info"
    done
    return 0 2>/dev/null || exit 0
fi

# ── 自动检测 ──
if [ ${#ports[@]} -eq 0 ]; then
    echo "WARNING: 无 USB 串口。先在 Windows 运行 attach-esp32.bat"
    echo "ESP-IDF v6.0.1 已激活 | 目标: esp32s3"
    return 0 2>/dev/null || exit 0
elif [ ${#ports[@]} -eq 1 ]; then
    export ESPPORT="${ports[0]}"
    echo "ESP-IDF v6.0.1 | 串口: $ESPORT (自动检测) | 目标: esp32s3"
else
    echo "ESP-IDF v6.0.1 已激活 | 目标: esp32s3"
    echo ""
    echo "检测到 ${#ports[@]} 个串口:"
    for i in "${!ports[@]}"; do
        info=""
        mfg=$(udevadm info --query=property --name="${ports[$i]}" 2>/dev/null | grep ID_VENDOR= | cut -d= -f2)
        [ -n "$mfg" ] && info="  ($mfg)"
        echo "  [$((i+1))] ${ports[$i]}$info"
    done
    echo ""
    read -r -p "选择串口 [1-${#ports[@]}] (默认 1): " choice
    [ -z "$choice" ] && choice=1
    if [ "$choice" -ge 1 ] 2>/dev/null && [ "$choice" -le ${#ports[@]} ]; then
        export ESPPORT="${ports[$((choice-1))]}"
        echo "已选择: $ESPPORT"
    else
        echo "无效选择，使用默认: ${ports[0]}"
        export ESPPORT="${ports[0]}"
    fi
fi
