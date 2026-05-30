# Advanced Topics / 进阶主题

> 文件创建日期: 2026-05-30
> 最后修订: 2026-05-30
> 适用项目: NRF24_1 (ESP32-S3 + NRF24L01+)

Short notes for tuning reliability and experimenting with ACK/IRQ.
关于可靠性调参与 ACK/IRQ 的简要笔记。

## 1. MAC mode: ALOHA vs CSMA / MAC 模式选择

- ALOHA: pure random access, send with probability `q` each slot
- CSMA: carrier sense (RPD) before send, random backoff if busy
- Switch via UART/TCP: `MAC ALOHA <q>` or `MAC CSMA <q>`
- CSMA 在密集干扰环境中可显著降低冲突，但会增加发送延迟

ALOHA 为纯随机接入；CSMA 在发送前通过 RPD 侦听信道，忙则随机退避。可通过 `MAC` 命令在线切换。

## 2. Why 250K is more stable / 250K 更稳的原因

- Lower air rate usually improves sensitivity
- Trade-off: lower throughput

空口速率越低，接收灵敏度通常越高；代价是吞吐降低。

## 3. ACK vs no-ACK / 有 ACK 与无 ACK

- ACK: reliable delivery, retries on loss
- no-ACK: good for one-way link bring-up

有 ACK 时链路更可靠，但失败会触发重发；无 ACK 适合先打通单向链路。

## 4. IRQ vs polling / IRQ 与轮询

- IRQ is faster but wiring-sensitive
- Polling is simpler for beginners

IRQ 响应快但接线要求高；轮询更简单，适合新手。

## 5. JAM interference testing / JAM 干扰测试

JAM (Jammer) 是 TX 端的持续载波发射功能，用于产生信道干扰以验证 CSMA/RPD 抗干扰能力。

- 命令: `JAM ON` / `JAM OFF`，或通过 GUI 按钮操作
- 原理: 独立高优先级任务紧循环发送满载包（32B 0xFF，禁用 ACK），占空比 >95%
- Jammer 运行时 TX 任务自动跳过 CSMA idle poll，避免 NRF24 硬件争抢
- 典型测试: 干扰 TX 板 JAM ON → 被测 TX/RX 对执行 burst → 观察 ACK_FAIL 上升 → JAM OFF → 对比恢复

详见 [pc_gui_workflow.md](pc_gui_workflow.md) 第 7 节。

## 6. Recommended modes / 模式建议

- Debug: Tutorial Debug
- Long run: Release Lite
