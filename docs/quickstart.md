# Quickstart

## 1. 目标
在两块 ESP32-S3 + NRF24L01+ 上完成最小收发。

## 2. 必要接线
- MOSI: GPIO13
- MISO: GPIO12
- SCK: GPIO14
- CSN: GPIO11
- CE: GPIO10
- IRQ: GPIO3
- VCC: 3.3V
- GND: GND

## 3. RX 板
1. `idf.py menuconfig`
2. NRF24 Configuration:
   - Project mode: Tutorial Debug
   - Application role: RX
3. `idf.py build`
4. `idf.py -p <RX_PORT> flash monitor`

## 4. TX 板
1. `idf.py menuconfig`
2. NRF24 Configuration:
   - Project mode: Tutorial Debug
   - Application role: TX
   - 与 RX 保持相同的 channel/data rate/address width/pipe0
3. `idf.py build`
4. `idf.py -p <TX_PORT> flash monitor`

## 5. 成功标志
- TX: `TX ok ...`
- RX: `RX pipe=... len=... data='...'`
