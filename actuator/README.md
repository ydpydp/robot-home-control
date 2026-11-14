# actuator（执行设备，C3 工作台版）

- 板子：ESP32-C3 Super Mini（V1601，芯片 ESP32-C3FH4、4MB flash），BLE peripheral（NimBLE）+ LED 作继电器替身。
- 继电器控制脚：**GPIO10**，低电平开通 / 高电平关断（`main.c` 顶部 `ACTUATOR_CTRL_GPIO`，改脚只改这一行）；上电默认高电平即关断。
  GPIO10 是这块板上最干净的脚之一（不参与启动、无 USB、无 LED/按键）。板子无电源指示 GPIO——板载电源 LED 已够用，灯本体版的指示灯按设计单走硬件（3.3V—电阻—LED—GND，不占 GPIO）。
  GPIO8 是板载 LED、GPIO9 是 BOOT 键，别占；GPIO18/19 是原生 USB，刷机/monitor 走板载 USB-C。
- 身份：每台灯一个广播名，central 按名挑设备。编译时传 `-D`：
  `idf.py -DRHC_DEVICE_NAME=RHC-Livingroom build`（客厅灯，默认不传也是它）、
  `idf.py -DRHC_DEVICE_NAME=RHC-Bedroom build`（卧室灯）。一台灯一个 bin。
  名字放在扫描响应里（广播包 31 字节装不下全名 + 128 位服务 UUID，central 主动扫描来读）。
- 协议：见 `docs/语音控家电v1计划.md` 第三节（GATT 命令写 + 状态 notify）。
  服务 UUID `c71566d8-7307-4f3e-a82f-bb0be617b22d`，
  命令 characteristic `8e2e563b-410a-4ce4-8a16-c1a43b3d5f13`（write），
  状态 characteristic `fe07c9e6-ece5-4cc7-8467-531233dc6565`（notify + read）。
- 命令（可读文本，幂等）：`ON` 开通（IO10 拉低）、`OFF` 关断（IO10 拉高）、`BRI:<n>`（n>0 开通、0 关断；继电器无亮度）；
  每次接受命令都回 notify 作回执（含重复命令）；未知命令忽略；上电默认关断；断线后自动重广播；看门狗 10 秒。
- 编译（ESP-IDF 6.x）：`idf.py build`（目标已在 `sdkconfig.defaults` 定为 esp32c3）；
  刷机 `idf.py -p /dev/cu.usbmodem* flash`，看日志 `idf.py -p /dev/cu.usbmodem* monitor`。
- 历史：N8R8 版执行端（GPIO12 + GPIO48 指示）在 commit `4fd2e166` 之前；N8R8 板上已刷的固件仍可用，仓库主线自 2026-11-14 起只走 C3。
