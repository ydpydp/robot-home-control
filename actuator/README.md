# actuator（执行设备，v1 开发中）

- 板子：ESP32-S3 N8R8，BLE peripheral + 继电器。
- 板子注意：N8R8 无 USB 串口桥、Type-C CC 悬空，刷机须经 hub / USB-A 转接或 5.1kΩ CC 对地；继电器 GPIO 避开 strapping 引脚（GPIO0/3/45/46）；板载 WS2812（GPIO47）可作配对 / 状态指示。
- 协议：见 `docs/语音控家电v1计划.md` 第三节（GATT 命令写 + 状态 notify）。
