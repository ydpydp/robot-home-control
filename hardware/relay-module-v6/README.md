# 继电器模块板 v6

插 ESP32-C3 Super Mini 的通用继电器模块板：1 路继电器 + WS2812 接口，12V 供电，7805 降压，ULN2003 驱动。配合 `actuator/` 固件（C3 执行端）做灯控。

## 文件

- `relay-module-v6.kicad_pcb` / `.kicad_sch` / `.kicad_pro`：KiCad 工程（**KiCad 10.0.8**，与 ECS 和 MacBook 统一版本）
- `TerminalBlock_3.5mm.pretty/`：3.5mm 弯针拔插端子定制封装（孔 Ø1.40）
- `bom-v6.md` / `bom-v6.csv` / `bom-v6-lcsc.md`：BOM 与立创商城料号
- `gerbers/`：嘉立创下单用 Gerber + 钻孔文件（2026-11-18 导出，DRC 0 违规）

## 规格

- 尺寸 46.23 × 53.59 mm，2 层、1.6mm、1oz、绿油、无铅喷锡
- 插座：ESP32-C3 Super Mini 2×8（针距 2.54mm、排距 15.24mm）
  - J1 = 5V/GND/3V3/GPIO4/3/2/1/0；J2 = GPIO5/6/7/8/9/10/20/21
- 继电器：SRD-12VDC-SL-C（12V/400Ω/约 30mA），3P 座；COM/NO/NC 由 J4 端子引出
- 驱动：ULN2003（SOP-16，只用通道 1，GPIO10 → K1）
- 电源：12V DC 由 J3 输入；7805 降压供 Super Mini 与 WS2812；贴片电解 100uF × 2、100nF × 2
- 端子：J3 12V 输入、J4 继电器触点、J5 WS2812、J7 12V、J8 5V（3.5mm 弯针拔插）
- 安装孔：四角各一，M3，5mm 内缩

## 注意事项（必读）

1. **ULN2003 反相**：GPIO 高电平 = 继电器吸合，与 C3 直驱低电平开通相反，固件极性要翻。
2. **NO/NC 有歧义**：上电前用万用表通断档核对（线圈不通电 COM–NC 通、通电 COM–NO 通）。
3. **GND 铺铜**：本工程的铺铜已填好并随文件保存（KiCad 10.0.8 `--refill-zones`）。以后改版后出 Gerber 前务必先填铜（KiCad 里按 B），否则 DRC 会报 GND 未连接。
4. **SMT 贴片**：只有 3 件可贴——U2 ULN2003（C908113）、C1/C3 100uF（C52268577）、C2/C4 100nF（C129641，须用 AUTO 版）。继电器、7805、排针、端子均为通孔手焊，不进 SMT BOM/CPL。
5. **尺寸未冻结**：等灯具机械结构与电源模块选型后再定。

## 下单

`gerbers/` 打包上传嘉立创即可（2 层、1.6mm、1oz、绿油、无铅喷锡，5 片）。
SMT 另按 `bom-v6-lcsc.md` 的 3 件料号下单。

## 版本历史

- v6（2026-11-16）：v5 + 右上 MH4，四角安装孔；顶边端子左移。
- 2026-11-18：Backplane agent 修完 DRC（铺铜填好、J3/J8 丝印三角缩短 0.3mm）；因 Backplane 的 KiCad 分支是 10.99 开发版、文件格式高于正式 KiCad，改动已手工搬回本工程（KiCad 10.0.8），DRC 0 违规、0 未连接。
