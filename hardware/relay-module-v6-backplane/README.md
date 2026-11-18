# 继电器模块板 v6（Backplane 版）

与 `../relay-module-v6/` 同一设计，由 Backplane agent（Claude Code）在 Backplane 里修完的版本：GND 铺铜已填、J3/J8 丝印三角缩短 0.3mm，DRC 0 违规、0 未连接。

## 重要：这是单向门版本

- 本工程由 **Backplane 自带的 KiCad 分支版（10.99.0 开发版）**写出，PCB 文件格式 version 为 `20261031`。
- **正式 KiCad（9 / 10.0.8）打不开它**，会报“文件由更新版本创建”。用正式 KiCad 打开保存后也不能再回 Backplane 编辑。
- 想在正式 KiCad 里继续改，请用 `../relay-module-v6/`（KiCad 10.0.8 版，改动已手工搬回，DRC 同样清零）。
- 想继续在 Backplane 里改、看 3D view，就用本目录。

## 文件

- `relay-module-v6.kicad_pcb` / `.kicad_sch` / `.kicad_pro` / `.kicad_dru`：Backplane KiCad 10.99 工程
- `TerminalBlock_3.5mm.pretty/`：3.5mm 弯针拔插端子定制封装（孔 Ø1.40）
- `.backplane.json` / `.backplane/`：Backplane 项目文件（`intent.md`、`review.md`、`design-review.json` 为 agent 的设计复核记录；`.backplane/history.git` 为 Backplane 内部快照历史，未随附）
- BOM 与 Gerber 见 `../relay-module-v6/`（同一设计，共用）

## 规格与注意事项

见 `../relay-module-v6/README.md`（尺寸、电气、ULN2003 反相、NO/NC 核对、SMT 料号均相同）。
