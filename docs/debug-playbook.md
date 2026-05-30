# Debug Playbook / 排障剧本

> 文件创建日期: 2026-05-30
> 最后修订: 2026-05-30
> 适用项目: NRF24_1 (ESP32-S3 + NRF24L01+)

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
5. Try switching MAC mode: `MAC ALOHA 100` (bypass CSMA carrier sense)
6. If using CSMA, check if RPD is constantly high: `MAC ALOHA 100` then `MAC CSMA 100` to compare
7. Add 10uF + 0.1uF decoupling on NRF24 VCC/GND

## Case B: RX alive but no payload / RX 只有 alive 没数据

Symptoms / 现象:
- RX only prints `RX alive ... status=...`

Checklist / 检查顺序:
1. TX is actually sending (not PIN TEST)
2. TX addr == RX pipe0 addr
3. Try no-ACK mode to confirm raw link
4. Verify MAC mode: use `MAC ALOHA 100` to eliminate CSMA false-busy as a cause
5. Re-enable auto-ack after one-way link works

## Case C: Suspected wiring / 引脚疑似错误

Steps / 操作:
1. Enable `NRF24_PIN_TEST_MODE`
2. Check CE/CSN expected vs actual
3. Check SPI(tx, rx) patterns
4. Use MISO probe logs to detect shorts

## Case D: JAM ON but RPD shows idle / JAM 开启但信道空闲

Symptoms / 现象:
- JAM ON 后 RPD 偶尔=1，大部分时间=0
- 或 JAM ON 后短暂 BUSY 然后恢复空闲

Checklist / 检查顺序:
1. 确认固件已更新到最新版本（jammer v2，占空比 >95%）
2. 在被干扰的板子上查看 RPD，而非干扰源板子（jammer 运行时 TX 自身的 CSMA idle poll 已暂停）
3. 确认两块板子的信道/速率配置一致
4. 测试方法: 干扰板 `JAM ON` → 被干扰板 `MAC CSMA 100` → 查看被干扰板串口日志

## Case E: GUI serial error "device reports readiness to read" / GUI 串口错误

Symptoms / 现象:
- GUI 日志终端弹出 `Reader error: device reports readiness to read but returned no data`

Checklist / 检查顺序:
1. 检查是否有其他程序占用同一串口（如 `idf.py monitor` 未关闭）
2. 运行 `sudo lsof /dev/ttyUSB0` 查看串口占用情况
3. 关闭占用程序后，GUI reader 线程会在 2~15s 内自动恢复
4. 如持续报错，检查 USB 线是否松动
