# Goal
单路 12V 继电器模块，插 ESP32-C3 Super Mini，蓝牙直连控制家里灯具（客厅 + 卧室）。配合 ESP32-S3 N16R8 语音助手。46.23 × 53.59 mm；尺寸未冻结（等灯具机械结构与 220V→12V 电源模块选型）。

# Parts
- relay-module-v6 (.) — 单路继电器板，KiCad 9 工程，PCB 接近下单状态。要订。

# Notes
- 布局是平铺：.kicad_pro/.kicad_sch/.kicad_pcb 直接在项目根目录，没进 boards/<name>/ 子文件夹；按 Backplane 文档「布局不同则一次提议、不擅自搬」，先不动。
- GND 铺铜已固化到板子 + JLCPCB 工艺规则写进 .kicad_dru，DRC 0 违规（2026-11-18）。
- ULN2003 反相：GPIO 高电平 = 继电器吸合，与 C3 直驱极性相反，固件里翻；PCB 不动。
- NO/NC 核对：datasheet 歧义，上电前万用表通断核对。
- 装配：3 个 SMT 件（U2 ULN2003 C908113、C1/C3 100µF 铝电解 C52268577、C2/C4 100nF 0805 C129641），全在 JLC Extended 库；K1 继电器 + U3 7805 是通孔件，手焊，不进 SMT 的 BOM/CPL。
- 流程：KiCad → kicad-jlcpcb-tools 出 BOM/CPL → Gerber → 嘉立创打样 + 贴片，全程不碰嘉立创 EDA。
