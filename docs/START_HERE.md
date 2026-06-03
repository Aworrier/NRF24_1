# START HERE / 新手入口

> 最后修订: 2026-06-03
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

**第一步：编译烧录。** 选一种你最方便的方式：

| 方式 | 命令 | 说明 |
|------|------|------|
| Claude Code | `/flash` | AI 自动处理，最简单 |
| 终端一键 | `bash tools/flash.sh` | 自动检测串口 |
| 手动命令 | `source tools/env.sh && idf.py -p /dev/ttyUSB0 flash` | 完全控制 |

**第二步：配置角色。**
- 第一块板设为 RX（接收）
- 第二块板设为 TX（发送）
- 两块板的 channel / data rate / address 保持一致

切换角色：`idf.py menuconfig` → Application role → 选 TX 或 RX → 重新编译。

**完整的构建烧录指南** → [quickstart.md](quickstart.md)（包含所有 5 种方法 + 排障）

## 4. Success Signs / 成功标志

- TX: `TX ok ...`
- RX: `RX ok pipe=... len=...`

## 5. Next Steps / 下一步

- 完整构建指南（5 种方法）: [quickstart.md](quickstart.md)
- PC GUI 无线控制: [pc_gui_workflow.md](pc_gui_workflow.md)
- 代码阅读顺序: [CODE_WALKTHROUGH.md](CODE_WALKTHROUGH.md)
- 排障手册: [debug-playbook.md](debug-playbook.md)
