# Advanced Topics / 进阶主题

Short notes for tuning reliability and experimenting with ACK/IRQ.
关于可靠性调参与 ACK/IRQ 的简要笔记。

## 1. Why 250K is more stable / 250K 更稳的原因

- Lower air rate usually improves sensitivity
- Trade-off: lower throughput

空口速率越低，接收灵敏度通常越高；代价是吞吐降低。

## 2. ACK vs no-ACK / 有 ACK 与无 ACK

- ACK: reliable delivery, retries on loss
- no-ACK: good for one-way link bring-up

有 ACK 时链路更可靠，但失败会触发重发；无 ACK 适合先打通单向链路。

## 3. IRQ vs polling / IRQ 与轮询

- IRQ is faster but wiring-sensitive
- Polling is simpler for beginners

IRQ 响应快但接线要求高；轮询更简单，适合新手。

## 4. Recommended modes / 模式建议

- Debug: Tutorial Debug
- Long run: Release Lite
