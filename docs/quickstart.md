# Quickstart / 快速开始

## Goal / 目标

- English: minimal TX/RX link on two ESP32-S3 boards
- 中文：两块 ESP32-S3 最小收发跑通

## Wiring (ESP32-S3) / 接线

- MOSI: GPIO13
- MISO: GPIO12
- SCK:  GPIO14
- CSN:  GPIO11
- CE:   GPIO10
- IRQ:  GPIO3
- VCC:  3.3V (add 10uF + 0.1uF)
- GND:  GND

## RX Board / RX 板

1. `idf.py set-target esp32s3`
2. `idf.py menuconfig`
   - Project mode: Tutorial Debug
   - Application role: RX
3. `idf.py build`
4. `idf.py -p <RX_PORT> flash monitor`

## TX Board / TX 板

1. `idf.py menuconfig`
   - Project mode: Tutorial Debug
   - Application role: TX
   - Keep channel/data rate/address consistent with RX
2. `idf.py build`
3. `idf.py -p <TX_PORT> flash monitor`

## Success Signs / 成功标志

- TX: `TX ok ...`
- RX: `RX ok pipe=... len=...`
