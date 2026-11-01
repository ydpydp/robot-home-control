# 新固件 v1 骨架（ESP-IDF，从头写，不走 gadget 方案）

板子：N16R8，接线沿用已验证的 gadget 期接线（INMP441 SCK 4 / WS 5 / SD 7；
MAX98357A BCLK 4 / LRC 5 / DIN 6；WS2812 GPIO48；BOOT GPIO0）。

## 编译（云 VM，IDF v6.0.1）
```
. /home/hatch/esp/esp-idf-v6/export.sh
IDF_SKIP_CHECK_SUBMODULES=1 idf.py -DIDF_TARGET=esp32s3 build
```
先 `idf.py menuconfig` → Robot v1 config 填 WiFi、后端 URL、device token，
再 build。刷机注意（2026-10-24 纠正，勿套用 gadget 规矩）：新固件用默认分区表，
app 在 0x10000，不在 0x20000；第一次从 gadget 换过来必须整包刷
（`idf.py -p PORT flash`，含 bootloader + 分区表 + app@0x10000），
之后改代码再只刷 app 到 0x10000。

## 交互（2026-10-30 起：唤醒词）
- 待机绿灯、麦常开，板上 ESP-SR WakeNet 本地听唤醒词（2026-10-31 起
  用内置 wn9_xiaojianxiaojian_tts2，即“小箭小箭”，之前是
  wn9_nihaoxiaoxin_tts“你好小鑫”、wn9_hilexin“Hi,乐鑫”），命中先播
  一声“我在”再开始录音；BOOT 键仍可用，按一下直接开始录音
  （手动备用，不播“我在”）。
- 录音结束：说话中静音 1.5 秒自动停，或再按 BOOT 停，最长 60 秒。
  停录后先播一声“收到”（Kokoro 合成、内嵌在 ack_shoudao.h），再上传。
  另有保护：录音开头 0.5 秒不计入语音判定（防“我在”的回声误触发），
  语音/静音阈值按待机时测到的环境噪声自适应（见 main.c 的 VAD 部分）。
- 5 秒内一直没检测到人声，或整段太短，本轮直接丢弃，不上传、
  不烧 ASR 额度，灯回绿。
- 灯：绿闲置 / 蓝录音 / 黄等回复 / 紫播放 / 红出错或未连 WiFi。
- 首次刷这个版本必须整包刷（`idf.py -p PORT flash`）：分区表多了
  model 分区（0x210000，2MB，装 WakeNet 模型，`idf.py flash` 会自动
  写入），之后只改代码仍可只刷 app；换模型选择后要再整包刷一次。
- 编译前确认两点：`idf.py menuconfig` → ESP Speech Recognition 里
  只勾 wn9_xiaojianxiaojian_tts2（“小箭小箭”），不要同时勾其他
  词（模型是多个独立开关，不是单选，勾几个、几个词都能唤醒）；
  esp-sr 由 idf_component.yml 拉取（≥2.4.1，2.4.1 起才实验性支持
  IDF 6.0，属已知前提）。

## 交互（旧版，BOOT 键时代）
- 按 BOOT 一下开始录音，再按一下停止并上传（最长 60 秒）。
- 灯：绿闲置 / 蓝录音 / 黄等回复 / 紫播放 / 红出错或未连 WiFi。
- 音频：录音 16kHz 单声道 s16 → WAV 上传；回复整句下完再播
  （48kHz 立体声 s32，v18 教训），播放缓冲 4MB（约 10 秒，超长回复
  会被截断——这是骨架已知上限，联调时按实测再定）。

## 已知骨架上限（联调前要处理）
- 播放缓冲 4MB，超长回复截断；录音 60 秒封顶。
- INMP441 的 32→16 位取高 16 位，未做增益（gadget 期 v15 是 ×4 增益；
  新固件如音量偏小，在 record_audio 里加增益再测）。
- WiFi 断线只重连，不做 captive portal；SSID/密码进 menuconfig。
