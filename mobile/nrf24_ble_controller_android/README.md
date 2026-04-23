# NRF24 BLE Android Controller

这是用于控制 ESP32-S3 + NRF24 固件的手机上位机 Android 工程。

## 1. 环境要求

- Android Studio Iguana 或更高版本
- Android SDK 34
- 真机（建议 Android 12 及以上）

## 2. 打开方式

1. 打开 Android Studio
2. 选择 `Open`
3. 打开当前目录：`mobile/nrf24_ble_controller_android`
4. 等待 Gradle 同步完成
5. 连接手机并运行 `app`

## 3. 使用步骤

1. 手机打开蓝牙
2. 启动 APP 后点击“扫描并连接”
3. 首次运行按提示授予蓝牙权限
4. 连接成功后点击 `STATUS` 测试
5. 点击 `ENABLE 1`
6. 配置 `count/interval/payload` 后点击“发送突发包”

## 4. 协议与 UUID

- Service: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- CMD(Write): `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
- Notify: `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`

命令与固件串口命令一致：

- `ENABLE 1`
- `ENABLE 0`
- `STATUS`
- `RESETSTATS`
- `STOP`
- `BURST ...`
- `BURSTHEX ...`

## 5. 常见问题

- 扫描不到设备：确认固件侧已启用 BLE 控制接口并处于广播状态。
- 连接后无回包：确认 APP 已成功写入通知描述符（CCCD）。
- 命令发送失败：检查 Android 12+ 蓝牙权限是否被系统回收。
