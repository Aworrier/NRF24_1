# Code Walkthrough / 代码阅读指南

## Reading Order / 阅读顺序

1. [main/app_main.c](../main/app_main.c) - high-level startup flow
2. [main/app_config.c](../main/app_config.c) - menuconfig mapping and radio address setup
3. [main/app_proto.c](../main/app_proto.c) + [main/app_stats.c](../main/app_stats.c) - frame format and statistics
4. [main/app_tx.c](../main/app_tx.c) + [main/app_rx.c](../main/app_rx.c) - TX/RX tasks and runtime data path
5. [main/nrf24.c](../main/nrf24.c) + [main/nrf24.h](../main/nrf24.h) - SPI + register driver
6. [main/app_control.c](../main/app_control.c) + [main/app_wifi_control.c](../main/app_wifi_control.c) - UART/TCP control channel
7. [main/app_pin_test.c](../main/app_pin_test.c) - GPIO self-test mode
8. [docs/CODE_DEEP_DIVE.md](CODE_DEEP_DIVE.md) - function-level intent and deeper reasons
9. [docs/C_LANGUAGE_PRIMER.md](C_LANGUAGE_PRIMER.md) - C and FreeRTOS fundamentals in this repo

## Module Map / 模块地图

- [main/app_main.c](../main/app_main.c): app entry, initialize modules, start tasks
- [main/app_config.c](../main/app_config.c): build `nrf24_config_t`, address setup, log summary
- [main/app_proto.c](../main/app_proto.c): build/parse NRF24 frames, CRC16
- [main/app_stats.c](../main/app_stats.c): TX/RX counters and seq gap/dup tracking
- [main/app_tx.c](../main/app_tx.c): burst sender, MAC gate (ALOHA/CSMA), GUI_STAT logs
- [main/app_rx.c](../main/app_rx.c): IRQ + FIFO polling, parse frames, print RX log
- [main/app_control.c](../main/app_control.c): UART command parser and STATUS output
- [main/app_wifi_control.c](../main/app_wifi_control.c): SoftAP + TCP control server
- [main/app_pin_test.c](../main/app_pin_test.c): pin test mode (CE/CSN/SPI probes)

## Data Flow / 数据流

TX path:
- UART/TCP command -> `app_control_handle_line`
- queue burst -> `app_tx_task`
- build frame -> `app_proto_build_frame`
- send -> `nrf24_send_payload`

RX path:
- IRQ/poll -> `app_irq_task`
- payload -> `app_rx_consumer_task`
- parse frame -> `app_proto_parse_frame`
- update stats -> `app_stats_rx`
