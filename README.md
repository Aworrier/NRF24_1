# ESP32-S3 + NRF24L01+ Tutorial Repo / 教学仓库

A beginner-friendly ESP-IDF project to learn NRF24 TX/RX on ESP32-S3.
面向新手的 ESP-IDF 教学仓库，帮助你快速跑通 NRF24 收发。

> Focused target: ESP32-S3 (other targets are legacy configs only).
> 目标芯片：ESP32-S3（其他目标仅保留历史配置）。

## Quick Start / 快速开始

1. Wire the module (ESP32-S3):
   - MOSI GPIO13, MISO GPIO12, SCK GPIO14
   - CSN GPIO11, CE GPIO10, IRQ GPIO3
   - 3.3V + GND (add 10uF + 0.1uF decoupling)
2. `idf.py set-target esp32s3`
3. `idf.py menuconfig`
   - Project mode: Tutorial Debug
   - Application role: RX (first board)
4. `idf.py build`
5. `idf.py -p <RX_PORT> flash monitor`

Repeat for TX board and keep channel/data rate/address consistent.
再把另一块板切换为 TX，并保持参数一致。

## Repository Map / 仓库结构

- [main/app_main.c](main/app_main.c): app entry, starts all modules
- [main/app_config.c](main/app_config.c): menuconfig mapping and radio address setup
- [main/app_tx.c](main/app_tx.c) / [main/app_rx.c](main/app_rx.c): TX/RX runtime tasks
- [main/app_proto.c](main/app_proto.c): frame format + CRC16
- [main/nrf24.c](main/nrf24.c) / [main/nrf24.h](main/nrf24.h): SPI + register driver
- [docs/README.md](docs/README.md): learning path and debugging notes
- [tools/pc_nrf24_controller.py](tools/pc_nrf24_controller.py): PC GUI for UART/TCP control

## Docs / 文档入口

- [docs/START_HERE.md](docs/START_HERE.md)
- [docs/README.md](docs/README.md)
- [docs/quickstart.md](docs/quickstart.md)
- [docs/CODE_WALKTHROUGH.md](docs/CODE_WALKTHROUGH.md)
- [docs/CODE_DEEP_DIVE.md](docs/CODE_DEEP_DIVE.md)
- [docs/C_LANGUAGE_PRIMER.md](docs/C_LANGUAGE_PRIMER.md)
- [docs/debug-playbook.md](docs/debug-playbook.md)

## Build & Flash / 编译与烧录

Recommended path in VS Code: [docs/vscode-workflow.md](docs/vscode-workflow.md)
CLI basics:

```bash
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p <PORT> flash monitor
```
