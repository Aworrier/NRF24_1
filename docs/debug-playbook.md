# Debug Playbook / 排障剧本

A short checklist for the most common NRF24 issues on ESP32-S3.
面向 ESP32-S3 + NRF24 的常见问题排查清单。

## Case A: TX timeout / TX 一直超时

Symptoms / 现象:
- TX log keeps printing `ESP_ERR_TIMEOUT`

Checklist / 检查顺序:
1. Make sure TX is not in PIN TEST mode
2. Disable auto-ack on TX to test one-way link
3. Match channel, data rate, addr width, pipe0/tx addr
4. Drop air data rate to 250K and set PA to -12dBm
5. Add 10uF + 0.1uF decoupling on NRF24 VCC/GND

## Case B: RX alive but no payload / RX 只有 alive 没数据

Symptoms / 现象:
- RX only prints `RX alive ... status=...`

Checklist / 检查顺序:
1. TX is actually sending (not PIN TEST)
2. TX addr == RX pipe0 addr
3. Try no-ACK mode to confirm raw link
4. Re-enable auto-ack after one-way link works

## Case C: Suspected wiring / 引脚疑似错误

Steps / 操作:
1. Enable `NRF24_PIN_TEST_MODE`
2. Check CE/CSN expected vs actual
3. Check SPI(tx, rx) patterns
4. Use MISO probe logs to detect shorts
