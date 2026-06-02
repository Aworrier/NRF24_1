#!/bin/bash
# ============================================================
# VS Code 集成终端启动脚本
# 在 VS Code settings.json 中通过 terminal.integrated.profiles.linux 引用
# 作用：自动激活 ESP-IDF 环境
# ============================================================

# 先加载默认 bashrc
if [ -f ~/.bashrc ]; then
    source ~/.bashrc
fi

# 激活 ESP-IDF 环境
if [ -f /home/dell/.espressif/tools/activate_idf_v6.0.1.sh ]; then
    source /home/dell/.espressif/tools/activate_idf_v6.0.1.sh
    echo "✅ ESP-IDF v6.0.1 环境已自动激活 | 目标: esp32s3"
else
    echo "⚠️  ESP-IDF 激活脚本未找到"
fi
