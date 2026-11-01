# robot-home-control

智能语音控制系统：语音助手（ESP32-S3 N16R8）+ 蓝牙执行设备（ESP32-S3 N8R8）+ ECS 后端，语音控家电 v1。

## 目录

- `voice-assistant/`：语音助手固件（种子：ASR-TTS-ESP32S3-N16R8 main 20ca9c4a，小箭小箭唤醒版）
- `actuator/`：执行设备固件（N8R8，BLE peripheral + 继电器，v1 开发中）
- `backend/`：ECS 后端 `robot_backend.py`（种子：2026-10-27 部署版，Kokoro TTS）
- `docs/`：智能家居常用词汇表、语音控家电 v1 计划

## 安全规矩（硬性）

- WiFi、device token 只在本地 menuconfig / 服务器 env 里，永不进仓库。
- 鉴权目标（设计已定，见计划第九节）：每个设备独立 token + 服务器设备注册表；v1 先单用户单 token 跑通，注册表在开放给第一个朋友前落地。公开仓库的代码不含任何凭证。
- 详见 `docs/语音控家电v1计划.md`。

## 状态

种子提交（2026-11-01）；v1：蓝牙直连、无服务器 MQTT、命令搭语音回复下发。
