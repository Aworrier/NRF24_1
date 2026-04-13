# Debug Playbook

## Case A: TX 一直超时
现象:
- TX 日志持续 `ESP_ERR_TIMEOUT`

检查顺序:
1. 确认 TX 不是 PIN TEST 模式
2. 打开 `TX disable auto-ack`，先验证单向链路
3. RX/TX 参数一致: channel, data rate, pipe0, tx addr, addr width
4. 空口速率降到 250K，功率先设 -12dBm
5. 给 NRF24 电源加 10uF + 0.1uF 去耦

## Case B: RX 只有 alive 没有 payload
现象:
- RX 只打印 `RX alive... status=...`

检查顺序:
1. 看 TX 是否确实在正常发包（不是 PIN TEST）
2. 确认 TX 使用的 tx addr 等于 RX 的 pipe0 addr
3. 先用无 ACK 模式验证能否收到 payload
4. 再恢复 ACK

## Case C: 引脚疑似错误
操作:
1. 开启 `NRF24_PIN_TEST_MODE`
2. 看 CE/CSN 的 exp/act 是否一致
3. 看 `SPI(tx, rx)` 是否稳定
4. 通过 `MISOprobe` 判断是否短接或串线
