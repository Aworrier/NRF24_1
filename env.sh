#!/bin/bash
# ESP-IDF v6.0.1 环境激活脚本
# 每次新终端使用前: source env.sh
source /home/dell/.espressif/tools/activate_idf_v6.0.1.sh
export ESPPORT=/dev/ttyUSB0
echo "ESP-IDF v6.0.1 已激活 | 串口: $ESPPORT | 目标: esp32s3"
