# 上位机GUI工作流程（TX突发任务 -> RX统计）

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
2. 若日志包含GUI_STAT行，GUI将其写入“发送统计详情”面板。
3. 检测到任务完成标记后，GUI更新摘要并停止统计收集。

## 7. 终止与复查

- 点击“停止发送”可中止当前burst（发送 STOP）。
- 可再次点击“查询状态”确认统计。
- 可“重置统计”后重新发起任务，确保数据干净。
