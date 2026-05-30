# START HERE / 新手入口

> 文件创建日期: 2026-05-30
> 最后修订: 2026-05-30
> 适用项目: NRF24_1 (ESP32-S3 + NRF24L01+)

## 1. Goal / 目标

- 中文：让新手在 10 分钟内跑通 ESP32-S3 + NRF24L01+ 的最小收发
- English: Get a minimal TX/RX link working in ~10 minutes on ESP32-S3 + NRF24L01+

## 2. Wiring (ESP32-S3) / 接线

| ESP32-S3 | NRF24L01+ | Note / 说明 |
|---|---|---|
| GPIO13 | MOSI | SPI MOSI |
| GPIO12 | MISO | SPI MISO |
| GPIO14 | SCK  | SPI SCK |
| GPIO11 | CSN  | SPI CSN |
| GPIO10 | CE   | RF enable |
| GPIO3  | IRQ  | Interrupt |
| 3.3V   | VCC  | 3.3V only |
| GND    | GND  | Common ground |

Tip: add 10uF + 0.1uF decoupling on VCC/GND.
提示：建议在 VCC/GND 旁加 10uF + 0.1uF 去耦。

## 3. First Run / 第一次跑通

1. `./flash.sh` — 一键编译+烧录（自动选串口）
   或手动: `idf.py set-target esp32s3` → `idf.py menuconfig` → `idf.py build` → `idf.py -p <PORT> flash monitor`
2. Project mode: Tutorial Debug
3. Application role: RX (first board)

Repeat for TX board (role = TX) and keep channel/data rate/address consistent.
然后把另一块板改为 TX，参数保持一致。

## 4. Success Signs / 成功标志

- TX: `TX ok ...`
- RX: `RX ok pipe=... len=...`

## 5. Next Steps / 下一步

- Minimal steps: [quickstart.md](quickstart.md)
- One-click flash: `./flash.sh`
- PC GUI controller: [pc_gui_workflow.md](pc_gui_workflow.md)
- Code reading order: [CODE_WALKTHROUGH.md](CODE_WALKTHROUGH.md)
- Troubleshooting: [debug-playbook.md](debug-playbook.md)
