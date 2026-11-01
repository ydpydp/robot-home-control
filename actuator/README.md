# actuator（执行设备，v1 工作台骨架）

- 板子：ESP32-S3 N8R8，BLE peripheral（NimBLE）+ LED 作继电器替身。
- LED：接 **GPIO2**（`main.c` 顶部 `ACTUATOR_LED_GPIO`，改脚只改这一行）。
  GPIO19 不用——ESP32-S3 原生 USB D-，N8R8 刷机走原生 USB，挂负载可能影响枚举。
- 板子注意：N8R8 无 USB 串口桥、Type-C CC 悬空，刷机须经 hub / USB-A 转接或 5.1kΩ CC 对地；
  继电器 GPIO 避开 strapping 引脚（GPIO0/3/45/46）；板载 WS2812（GPIO47）可作配对 / 状态指示。
- 协议：见 `docs/语音控家电v1计划.md` 第三节（GATT 命令写 + 状态 notify）。
  服务 UUID `c71566d8-7307-4f3e-a82f-bb0be617b22d`，
  命令 characteristic `8e2e563b-410a-4ce4-8a16-c1a43b3d5f13`（write），
  状态 characteristic `fe07c9e6-ece5-4cc7-8467-531233dc6565`（notify + read）。
- 命令（可读文本，幂等）：`ON` 点亮（= 继电器闭合）、`OFF` 熄灭（= 断开）、`BRI:<n>`（n>0 亮、0 灭；LED 无亮度）；
  每次接受命令都回 notify 作回执（含重复命令）；未知命令忽略；上电默认 OFF；断线后自动重广播；看门狗 10 秒。
- 编译（ESP-IDF 6.x）：`idf.py set-target esp32s3`，然后 `idf.py build`；刷机 `idf.py flash`（走 hub / CC 处理后的 Type-C）。
- 待做：语音助手侧 BLE central（扫描该服务 UUID、常连、等回执再播确认音）；正式继电器版把 LED 脚换成继电器驱动脚即可，命令与回执格式不变。
