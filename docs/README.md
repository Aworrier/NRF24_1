# Docs Index / 文档索引

> 文件创建日期: 2026-05-30
> 最后修订: 2026-05-30
> 适用项目: NRF24_1 (ESP32-S3 + NRF24L01+)

> This repo focuses on ESP32-S3. Other targets remain only as legacy configs.
> 本仓库聚焦 ESP32-S3，其他目标仅保留历史配置，不再维护。

## Start Here / 新手入口

- [START_HERE.md](START_HERE.md): 10-minute path and first success checklist
- [quickstart.md](quickstart.md): minimal TX/RX bring-up steps
- [vscode-workflow.md](vscode-workflow.md): VS Code + ESP-IDF setup and commands

## Learn the Code / 读代码

- [CODE_WALKTHROUGH.md](CODE_WALKTHROUGH.md): module map and reading order
- [CODE_DEEP_DIVE.md](CODE_DEEP_DIVE.md): function-level intent and deeper reasons
- [nrf24_tx_rx_phy_mac_mindmap.md](nrf24_tx_rx_phy_mac_mindmap.md): TX/RX flow mindmaps

## C Language Primer / C 语言基础（系列）

- [c-primer-01-types-and-memory.md](c-primer-01-types-and-memory.md): 类型、内存与数据结构
- [c-primer-02-bit-operations-and-preprocessor.md](c-primer-02-bit-operations-and-preprocessor.md): 位运算与预处理器
- [c-primer-03-control-flow-and-error-handling.md](c-primer-03-control-flow-and-error-handling.md): 控制流与错误处理
- [c-primer-04-function-pointers-and-concurrency.md](c-primer-04-function-pointers-and-concurrency.md): 函数指针与并发模式
- [c-primer-05-freertos-tasks-and-queues.md](c-primer-05-freertos-tasks-and-queues.md): FreeRTOS 任务与队列
- [c-primer-06-protocol-and-esp-idf.md](c-primer-06-protocol-and-esp-idf.md): 协议设计与 ESP-IDF 外设 API

## Tools / 工具

- `../flash.sh` — 一键烧录脚本（选串口 → 编译 → 烧录 → 监视）
- [pc_gui_workflow.md](pc_gui_workflow.md): 上位机 GUI 工作流程与 JAM 干扰源使用
- `../CLI_REFERENCE.md` — ESP-IDF 命令行参考手册

## Troubleshooting / 排障

- [debug-playbook.md](debug-playbook.md): common failure patterns and fixes

## Advanced / 进阶

- [advanced.md](advanced.md): MAC mode, ACK, rate, IRQ notes and experiments
- [nrf24_rpd_register.md](nrf24_rpd_register.md): NRF24L01+ RPD 寄存器技术笔记

---

*本文档属于 NRF24_1 项目技术笔记系列，随项目演进持续更新。*
