---
name: flash
description: Build, flash, and monitor the ESP32-S3 NRF24 firmware
disable-model-invocation: true
---

# Flash firmware

Activate the ESP-IDF environment and flash the firmware to the ESP32-S3 device.

## Workflow

1. Check USB serial devices: `ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null`
2. If no devices found, tell user to run `usbipd wsl attach --busid <ID>` from Windows PowerShell (admin)
3. Activate environment: `source /home/dell/.espressif/tools/activate_idf_v6.0.1.sh`
4. Ask user whether to:
   - `flash` — build + flash
   - `monitor` — build + flash + serial monitor
   - `build` — build only
5. Run: `idf.py -p <PORT> <mode>`
6. If monitor mode, remind user: press `Ctrl+]` to exit

## Common issues

- Permission denied: `sudo chmod 666 /dev/ttyUSB0`
- Wrong chip: `idf.py set-target esp32s3`
- CMake error: `idf.py fullclean && idf.py build`
