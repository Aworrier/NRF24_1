# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Auto-load on session start

On every new conversation, invoke: `superpowers:using-superpowers` via the Skill tool. This loads the full superpowers workflow system (brainstorming, TDD, systematic debugging, etc.).

When writing C code in this project, also invoke: `project-conventions` (auto-loaded as a Claude-only skill from `.claude/skills/project-conventions/SKILL.md`).

## Environment

- **Platform**: WSL2 (Ubuntu), ESP-IDF v6.0.1
- **Target**: ESP32-S3 (set via `idf.py set-target esp32s3`)
- Each new shell must activate: `source /home/dell/.espressif/tools/activate_idf_v6.0.1.sh`

## Build Commands

```bash
# Activate environment (every new terminal)
source /home/dell/.espressif/tools/activate_idf_v6.0.1.sh

# Build
idf.py build

# Full clean rebuild
idf.py fullclean && idf.py build

# Flash + monitor (requires USB attached via usbipd from Windows)
idf.py -p /dev/ttyUSB0 flash monitor

# Menuconfig (change TX/RX role, pins, RF params)
idf.py menuconfig

# Quick script that auto-detects serial port
bash tools/flash.sh          # flash
bash tools/flash.sh monitor  # flash + monitor
bash tools/flash.sh build    # build only
```

## Architecture

This is an ESP-IDF FreeRTOS-based firmware for NRF24L01+ 2.4GHz wireless modules. One ESP32-S3 acts as TX (sender), another as RX (listener). The TX/RX role is a **compile-time** choice set via `menuconfig` → `Application role` (`CONFIG_NRF24_ROLE_TX`).

### Layer stack (bottom-up)

1. **`nrf24.c`** — SPI driver for NRF24L01+. Register read/write, FIFO drain, CE pulse for TX, IRQ handling. Pure hardware layer — no application logic.
2. **`app_proto.c`** — Custom frame protocol: 8-byte header (magic 0xA5/0x5A, version, 16-bit sequence LE, payload length 0-22, flags, reserved) + payload + 2-byte CRC16-CCITT.
3. **`app_tx.c`** — TX task. Slot scheduler that gates transmission through either ALOHA (probabilistic `q` per slot) or CSMA (carrier-sense via RPD register, random backoff on busy). Handles `BURST`/`BURSTHEX` commands from control layer.
4. **`app_rx.c`** — RX side. IRQ ISR drains RX FIFO → FreeRTOS queue → `nrf24_rx` consumer task parses frames, validates CRC, tracks sequence gaps/duplicates via `app_stats.c`.
5. **`app_control.c`** — UART shell. Reads stdin in a FreeRTOS task, parses commands (ENABLE, MAC, BURST, BURSTHEX, STATUS, etc.).
6. **`app_wifi_control.c`** — Optional WiFi SoftAP + TCP server (port 3333). Same command set as UART, plus AUTH token.
7. **`app_config.c`** — Reads `sdkconfig` (Kconfig values) into `nrf24_config_t` struct.

### Task priorities (higher number = higher priority)

| Task | Priority | Role |
|------|----------|------|
| `uart_cmd` | 9 | UART command parser |
| `nrf24_irq` | 9 | RX IRQ ISR → FIFO drain |
| `nrf24_tx` | 8 | TX burst scheduler |
| `tcp_ctrl` | 8 | WiFi TCP server |
| `nrf24_rx` | 7 | Frame parsing + stats |

The IRQ task must be highest priority alongside UART to avoid RX FIFO overflow. The RX consumer runs lower to avoid blocking real-time paths.

### Command flow

PC GUI (`tools/pc_nrf24_controller.py`, Tkinter) or serial terminal → UART/TCP → `app_control.c` parses → pushes to TX command queue → `app_tx.c` consumes.

### Key config pattern

All hardware params (pins, channel, rate, power, addresses) come from `menuconfig`/`sdkconfig`, not hardcoded. The `Kconfig.projbuild` defines the menu structure. `app_config.c` reads these into `nrf24_config_t`. When changing pins or RF params, run `idf.py menuconfig`, not code edits.

## Windows/WSL Boundary

- `.bat`/`.vbs` files MUST use CRLF line endings. Auto-fix hook in settings.json runs `sed 's/\\r*$/\\r/'` after every Edit/Write on these files.
- USB devices require `usbipd wsl attach --busid <ID>` from Windows PowerShell (admin) before they appear as `/dev/ttyUSB*` in WSL.
- If serial permission denied: `sudo chmod 666 /dev/ttyUSB0`.

## Automation tools available

| Type | Name | Location | Purpose |
|------|------|----------|---------|
| MCP | context7 | `.mcp.json` | Live ESP-IDF API docs lookup |
| Skill | `/flash` | `.claude/skills/flash/` | One-click build+flash+monitor |
| Skill | `project-conventions` | `.claude/skills/project-conventions/` | Auto-applied NRF24 coding rules |
| Agent | `build-verifier` | `.claude/agents/` | Verify project builds after changes |
| Agent | `embedded-reviewer` | `.claude/agents/` | Embedded C review (ISR/task/SPI safety) |
| Hook | CRLF auto-fix | `.claude/settings.json` | Auto-convert `.bat`/`.vbs` to CRLF on Edit/Write |
| Hook | Idle notification | `.claude/settings.json` | Alert when Claude is waiting for input |
| Plugin | superpowers | installed | Full workflow system (brainstorm, TDD, debug, etc.) |
| Plugin | claude-code-setup | installed | Project setup automation recommender |
