# ESP32-S3 BLE 控制 NRF24 全流程开发指南

本文给出一套可直接执行的闭环方案：

1. ESP32-S3 通过 BLE GATT 暴露命令通道。
2. 手机端通过 BLE 写入命令控制 NRF24 发包/停包/统计。
3. ESP32 执行命令并通过 BLE Notify 回传状态与结果。

## 1. 架构

- 硬件控制面：ESP32-S3 通过 SPI + GPIO 控制 NRF24L01+。
- 无线控制面：手机通过 BLE 与 ESP32 通信。
- 业务数据面：NRF24 空口收发 payload。

控制链路如下：

手机 App -> BLE CMD Characteristic -> ESP32 命令解析 -> NRF24 发送/接收控制 -> BLE Notify Characteristic -> 手机日志/状态显示

## 2. BLE GATT 设计

采用 NUS 风格 UUID（便于手机端快速复用）：

- Service UUID: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- CMD Characteristic (Write): `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
- Notify Characteristic (Notify): `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`

说明：

- 手机向 CMD 特征写入 ASCII 命令（支持换行结尾）。
- ESP32 将执行结果通过 Notify 特征返回（文本行协议）。

## 3. 固件命令协议

所有命令与原串口命令统一，便于串口/BLE 双通道联调。

- `HELP`
- `STATUS`
- `RESETSTATS`
- `ENABLE 1`
- `ENABLE 0`
- `STOP`
- `BURST <count> <interval_ms> <ascii_payload>`
- `BURSTHEX <count> <interval_ms> <hex_payload>`

典型返回：

- `OK queued`
- `OK ENABLED`
- `ERR invalid count`
- `STAT role=TX ...`
- `STAT role=RX ...`

## 4. ESP32-S3 侧关键实现点

### 4.1 模块划分

- `main/ble_ctrl.c`：BLE GATT server、广告、连接、写命令接收、通知发送。
- `main/ble_ctrl.h`：BLE 控制接口定义。
- `main/app_main.c`：命令解析中心，UART/BLE 复用一套命令处理函数。

### 4.2 关键流程

1. `app_main()` 初始化 NRF24 驱动。
2. 启动 UART 命令任务（可配置开关）。
3. 初始化 NVS 与 BLE（可配置开关）。
4. BLE 连接后，手机写 CMD 特征触发命令解析。
5. 命令执行结果通过 `ble_ctrl_send_line()` 通知给手机。

### 4.3 menuconfig 关键项

路径：`NRF24 Configuration -> Control Interfaces`

- `NRF24_CONTROL_IF_UART`：启用串口命令接口。
- `NRF24_CONTROL_IF_BLE`：启用 BLE 命令接口。
- `NRF24_BLE_DEVICE_NAME`：BLE 广播设备名（建议唯一化）。

## 5. 手机上位机（Android）

工程路径：`mobile/nrf24_ble_controller_android`

功能：

- 扫描并按服务 UUID 自动发现目标设备。
- 建立 GATT 连接并订阅 Notify。
- 一键发送 `ENABLE/STATUS/RESETSTATS/STOP`。
- 参数化发送 `BURST/BURSTHEX`。
- 实时显示上下行日志。

注意：Android 12+ 需要 `BLUETOOTH_SCAN` 与 `BLUETOOTH_CONNECT` 权限。

## 6. 联调步骤（严格顺序）

1. 确认 ESP32-S3 与 NRF24 接线、电源、去耦正确。
2. `idf.py set-target esp32s3`。
3. 在 menuconfig 中启用 BLE 控制接口并设置设备名。
4. `idf.py build flash monitor`，观察日志出现 BLE 广播提示。
5. 手机安装并打开 Android 上位机，点击“扫描并连接”。
6. 连接后先发 `STATUS` 验证命令通路。
7. 发 `ENABLE 1`。
8. 发 `BURST 20 30 HELLO_NRF24` 或 `BURSTHEX 20 30 A1B2C3`。
9. 观察 TX/RX 两端统计是否符合预期。

## 7. 验收清单

- 手机能发现指定 BLE 设备名。
- 手机连接后可收到 Notify 文本。
- `STATUS` 返回字段完整，且数值随业务动作变化。
- `ENABLE 0` 后不再发包。
- `STOP` 能中断正在进行的 burst。
- `BURSTHEX` 对非法 hex 输入有错误返回。

## 8. 风险与规避

- BLE 已连接但无通知：检查 CCCD 是否写成功。
- 写命令失败：检查手机 BLE 权限与 GATT 特征权限。
- NRF24 发包失败高：先降低空口速率到 250K，检查供电与地址一致性。
- 统计异常：先用 `RESETSTATS` 清零后做短批量验证。

## 9. 建议的生产化增强项

- 为命令增加鉴权 token，防止陌生手机控制。
- 对 BLE 命令加入长度与速率限制，防止刷写阻塞。
- 把文本协议升级为 TLV/CBOR，便于二进制扩展。
- 增加 OTA 通道与版本上报命令。
