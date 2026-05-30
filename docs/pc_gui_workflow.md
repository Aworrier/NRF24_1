# 上位机GUI工作流程（TX突发任务 -> RX统计）

> 文件创建日期: 2026-05-30
> 最后修订: 2026-05-30
English summary: the GUI sends BURST/BURSTHEX over UART/TCP, TX builds frames and sends, RX validates and updates STAT counters.
英文摘要：GUI 通过 UART/TCP 发送 BURST/BURSTHEX，TX 构帧发送，RX 校验并更新 STAT 统计。

本文描述上位机GUI从发起TX突发任务到RX接收并统计的完整链路。

## 0. 前置条件

- 两块板子分别配置为TX和RX，且频道/速率/地址等参数一致。
- TX板处于可接收控制命令状态，RX板处于监听状态。

## 1. 上位机连接与初始化

1. 启动上位机GUI（tools/pc_nrf24_controller.py）。
2. 选择连接方式：
   - 串口：选择COM口与波特率后连接。
   - TCP：填写主机/端口/Token后连接，GUI自动发送 AUTH。
3. 连接成功后，GUI自动发送 STATUS，并开始按1秒周期轮询（如启用自动轮询）。

## 2. 配置发送参数

1. 勾选“启用发送”（发送 ENABLE 1）。
2. 可选：发送 RESETSTATS 清空统计。
3. 设置MAC/时隙参数并应用：
   - MAC ALOHA/CSMA 与 q。
   - 时隙长度与CSMA窗口。
   - 时隙上限（任务最多运行T个时隙）。
4. 设置突发任务参数：count、模式(ASCII/HEX)、payload。

## 3. 触发TX突发任务

1. 点击“发送突发包”。
2. GUI在本地进入“收集统计”状态，并清空面板。
3. GUI下发命令：
   - ASCII: BURST <count> <interval_ms> <payload>
   - HEX:   BURSTHEX <count> <interval_ms> <hex_payload>

## 4. TX侧链路执行（固件）

1. TX应用层解析命令，构造业务帧（含序号/CRC等字段）。
2. 进入nRF24发送流程：
   - 写入payload并触发CE脉冲发射。
   - 轮询IRQ判断成功(TX_DS)或失败(MAX_RT)。
   - 记录ack_ok/ack_fail与重发统计。
3. TX按burst参数持续发送，直到完成或被STOP中止。

## 5. RX侧接收与统计（固件）

1. RX保持监听，IRQ触发后读取FIFO数据。
2. 解析帧与CRC，统计:
   - rx_pkt, frame_ok, crc_fail
   - gap(序号丢失), dup(重复), ooo(乱序)
3. 通过STATUS命令返回统计汇总行（STAT ...）。

## 6. GUI统计展示与任务完成

1. GUI读取串口/TCP日志并解析STAT行，刷新ACK与RX面板。
2. 若日志包含 GUI_STAT 行，GUI解析结构化数据：
   - 每时隙格式: `GUI_STAT: slot=<n> attempted=<0|1> success=<0|1> reason=<code>`
   - 汇总格式: `GUI_STAT: Sent <sent>/<target> packets in <N> slots [(timeout)]`
3. 解析后的数据以表格形式渲染在「发送统计详情」面板（时隙号/尝试/成功/原因码），底部带合计行。
4. 原因码说明: 0=跳过/忙, 1=超时MAX_RT, 2=概率拒绝, 3=其他错误。
5. 检测到汇总行后，GUI更新摘要标签并停止统计收集。

## 7. JAM 干扰信号

JAM (Jammer) 是 TX 端的持续载波发射功能，用于产生信道干扰以验证 CSMA/RPD 的抗干扰能力。

### 7.1 工作原理

- JAM ON 时，固件启动独立 FreeRTOS 任务 `nrf24_jam`（优先级 10，高于 TX 任务的优先级 8）。
- 该任务紧循环发送满载 32 字节 0xFF 的无线帧，**禁用 Auto-ACK** 以最大化发送速率。
- 新旧 jammer 对比：

| 项目 | 旧实现 (v1) | 新实现 (v2) |
|------|------------|------------|
| 包间延迟 | ~5ms（每轮 power_up 2ms + stop_listening 1ms） | ~10µs（仅 CE 脉冲间隙） |
| 占空比 | ~4%（200µs 发送 / 5ms 间隔） | ~95%+ |
| TX 任务干扰 | CSMA idle poll 每 50ms 打断 NRF24 状态 | Jammer 期间自动跳过 CSMA idle poll |
| 总包数 | 未统计 | 停止时输出总发包数 |

### 7.2 操作步骤

1. **TX 端**：GUI 连接 TX 板，确保 MAC 模式设为 CSMA（非必须但推荐）。
2. 点击 **JAM ON** → 日志终端显示 `=> JAM ON`，固件回复 `OK JAM ON (continuous TX, no ACK)`。
3. 固件日志输出 `Jammer started` 确认 jammer 任务已启动。
4. **另一对 TX/RX** 正常进行 burst 测试，观察 ACK_OK/ACK_FAIL 变化。
5. 点击 **JAM OFF** → 固件回复 `Jammer stopped (N packets sent)` 并输出发包总数。
6. **GUI 的「停止发送」按钮也可终止 jammer**（固件 STOP 命令同时停止 burst 和 jammer）。

### 7.3 验证干扰效果

- 在 TX 端启用 JAM ON 后，RX 端所在的 TX 板（如果开启了 CSMA 模式）会在串口日志中看到：
  ```
  CSMA idle poll: RPD=1 (CHANNEL BUSY)
  ```
- **注意**: Jammer 运行时 TX 任务的 CSMA idle poll 自动暂停（v2 新增），因此 JAM ON 时 TX 板不会再刷 CHANNEL BUSY 日志。
- 要通过 RPD 验证干扰效果，需**在被干扰的RX端板子**上开启 CSMA 模式查看其日志。
- 观察被干扰的 TX/RX 对的 `ACK_FAIL` 和 `retries_sum` 是否上升，从而量化干扰强度。

### 7.4 注意事项

- **JAM 和 burst 共享同一 NRF24 硬件**，JAM ON 时无法同时执行 burst 发送。
- Jammer 发包极快（~5000 pkt/s），长时间运行注意 NRF24 芯片温度。
- Jammer 优先级 10，高于 TX 任务（8），但每 1000 包会 `taskYIELD` 1 tick 防止看门狗复位和低优先级任务饿死。
- **典型测试流程**:
  ```
  [干扰 TX 板]  JAM ON
  [被测 TX 板]  MAC CSMA 100
  [被测 TX 板]  BURST 100 0 HELLO
  [被测 RX 板]  观察 STAT → ack_fail 应显著上升
  [干扰 TX 板]  JAM OFF
  [被测 TX 板]  BURST 100 0 HELLO  (对比测试：无干扰)
  [被测 RX 板]  观察 STAT → ack_fail 应恢复正常
  ```

## 8. 终止与复查

- 点击「停止发送」可中止当前burst和JAM（发送 STOP）。
- 可再次点击「查询状态」确认统计。
- 可「重置统计」后重新发起任务，确保数据干净。

## 9. 常见问题

- **串口错误 "device reports readiness to read but returned no data"**: 通常是其他程序（如 `idf.py monitor`）同时占用了同一串口。GUI reader 线程会捕获此异常，显示中文提示并自动重试（退避 2s→15s），不会崩溃。关闭其他占用串口的程序即可恢复。
- **JAM ON 后 RPD 很快变回 0**: 检查是否在被干扰的板子上而非干扰源板子上查看 CSMA 日志。JAM v2 运行时 TX 板自身的 CSMA idle poll 已自动暂停。

## 8. 终止与复查

- 点击「停止发送」可中止当前burst和JAM（发送 STOP）。
- 可再次点击「查询状态」确认统计。
- 可「重置统计」后重新发起任务，确保数据干净。

## 9. 常见问题

- **串口错误 "device reports readiness to read but returned no data"**: 通常是其他程序（如 `idf.py monitor`）同时占用了同一串口。GUI reader 线程会捕获此异常，显示中文提示并自动重试（退避 2s→15s），不会崩溃。关闭其他占用串口的程序即可恢复。
