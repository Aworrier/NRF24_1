---
name: project-conventions
description: NRF24 ESP-IDF firmware coding conventions. Apply automatically when writing or modifying C code.
user-invocable: false
---

# NRF24 Firmware Coding Conventions

Apply these patterns when writing or modifying C code in this project.

## Configuration flow

All hardware parameters (pins, channel, rate, power, addresses) come from `sdkconfig` (Kconfig), never hardcoded. `app_config.c:app_build_nrf24_config()` reads them into `nrf24_config_t`. New configs go in `Kconfig.projbuild` → `app_config.c` → `app_config.h`.

## Error handling

Use `ESP_ERROR_CHECK()` wrapping for all NRF24 driver calls that return `esp_err_t`. Critical init failures should abort — no partial init fallbacks.

## Task priorities (do not change without reason)

| Priority | Tasks |
|----------|-------|
| 9 | `uart_cmd`, `nrf24_irq` — must not be delayed |
| 8 | `nrf24_tx`, `tcp_ctrl` — real-time TX path |
| 7 | `nrf24_rx` — consumer, can lag |

IRQ task must drain RX FIFO before it overflows. Never add blocking calls to priority 9 tasks.

## Frame format (app_proto.c)

All frames use: 0xA5 0x5A magic + 1B version + 2B sequence (LE) + 1B payload length (0-22) + 1B flags + 1B reserved + payload + 2B CRC16-CCITT. Max total = 32 bytes (NRF24 payload size). Use `app_proto_build_payload()` / `app_proto_parse_payload()` — never construct frames manually.

## SPI usage

All NRF24 register access goes through `nrf24.c`. CE must be pulsed (HIGH→10μs→LOW) to trigger TX. IRQ pin is active-low, level-triggered — ISR must read STATUS register to clear.

## Dual-role compile

TX vs RX is a compile-time choice via `CONFIG_NRF24_ROLE_TX`. Use `#if CONFIG_NRF24_ROLE_TX` guards for role-specific code in shared modules. Runtime config should work for both roles.

## Naming

- Public functions: `app_<module>_<action>()` or `nrf24_<action>()`
- Static helpers: `prv_<action>()` or `static` without prefix
- Log tags: uppercase module name like `"nrf24_app"`, `"nrf24_tx"`
