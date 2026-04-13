# ESP32-S3 + NRF24L01+ 教学仓库

这个仓库的目标是让新手可以看懂并跑通 NRF24 通信。

仓库现在采用同一工程双模式：

- Tutorial Debug（教学调试版）：日志更全，方便学习和定位问题
- Release Lite（正式精简版）：日志更少，配置更少，路径更短

## 1. 先理解两个速率

- SPI 速率：ESP32 与 NRF24 芯片之间的有线传输速率（本项目默认 8MHz）
- Air data rate：NRF24 在 2.4GHz 空口发包速率（250K / 1M / 2M）

这两个速率不同层面，互不相等。通常 SPI 明显高于空口速率即可。

## 2. 硬件连线（ESP32-S3）

| ESP32-S3 | NRF24L01+ | 说明 |
|---|---|---|
| GPIO13 | MOSI | 主出从入 |
| GPIO12 | MISO | 主入从出 |
| GPIO14 | SCK | SPI 时钟 |
| GPIO11 | CSN | SPI 片选 |
| GPIO10 | CE | 射频使能 |
| GPIO3  | IRQ | 中断输入 |
| 3.3V | VCC | 仅 3.3V |
| GND | GND | 共地 |

建议：在 NRF24 模块 VCC/GND 旁边加 10uF + 0.1uF 去耦。

## 3. 仓库结构

- [main/nrf24.h](main/nrf24.h)：驱动接口（尽量保持稳定）
- [main/nrf24.c](main/nrf24.c)：SPI + 寄存器 + 收发实现
- [main/app_main.c](main/app_main.c)：应用入口（角色任务、日志、教学模式）
- [main/Kconfig.projbuild](main/Kconfig.projbuild)：菜单配置（模式、角色、参数）
- [sdkconfig.defaults.tutorial](sdkconfig.defaults.tutorial)：教学调试模板
- [sdkconfig.defaults.release](sdkconfig.defaults.release)：正式精简模板

## 3.1 文档分层

- [docs/quickstart.md](docs/quickstart.md)：从接线到双板收发的最短路径
- [docs/debug-playbook.md](docs/debug-playbook.md)：常见故障的排查剧本
- [docs/advanced.md](docs/advanced.md)：ACK、速率、IRQ 等进阶主题

## 4. 模式说明

### Tutorial Debug

适合：第一次接线、双板联调、问题定位。

特点：

- 可开启 PIN 自检
- 可开启 TX 无 ACK 诊断
- 启动时打印更完整参数（速率、功率、重发等）

### Release Lite

适合：功能已经跑通，准备长期运行。

特点：

- 默认简化日志
- 隐藏大部分高阶配置
- 保留新手必要参数（角色、引脚、频道、空口速率）

## 5. 快速开始（双板）

### 第一步：RX 板

```bash
idf.py set-target esp32s3
idf.py menuconfig
```

在 NRF24 菜单中：

1. 选择 Project mode
2. 选择 Application role = RX
3. 确认 channel / data rate / pipe0 addr

然后：

```bash
idf.py build
idf.py -p <RX_PORT> flash monitor
```

### 第二步：TX 板

在同一工程切换：

1. Application role = TX
2. 参数与 RX 保持一致（channel/data rate/address width/pipe0）

然后：

```bash
idf.py build
idf.py -p <TX_PORT> flash monitor
```

## 6. 如何判断是否联调成功

- TX 端看到 `TX ok ...`
- RX 端看到 `RX pipe=... len=... data='...'`

如果 TX 一直超时，可先用 Tutorial Debug 模式开启 `TX disable auto-ack` 做单向链路诊断。

## 7. 常见问题速查

- RX 只有 `RX alive` 没有数据：先确认 TX 不是 PIN TEST 模式
- TX 一直 `ESP_ERR_TIMEOUT`：先关 ACK 诊断单向链路，再回到 ACK
- 速率不稳：把 Air data rate 降到 250K，PA 调到 -12dBm
- 仍异常：优先检查供电去耦和模块质量

## 8. 给新手的建议路径

1. 先用 Tutorial Debug 跑通
2. 再切到 Release Lite
3. 最后只保留必要日志和固定参数
