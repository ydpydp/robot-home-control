# 继电器模块板 v6 — LCSC 料号对照表

核查时间：2026-11-17 17:33–17:35 CST，直接在 lcsc.com 逐一打开详情页核对；未登录、未购买。
库存与价格均为当时页面显示，美元计价，库存会变动，下单前在立创商城再核一次。
BOM 底本：`bom-v6.csv`（10 行、14 个焊装位置）。

## 能走嘉立创 SMT 贴片的（均为 Extended 扩展库，非 Basic 库）

| 位号 | 规格 | LCSC 编号 | 制造商型号 | 库存（核查时） | 单价（核查时） | 备注 |
|---|---|---|---|---|---|---|
| U2 | ULN2003，SOIC-16 | C908113 | TI ULN2003AD | 5,735 | $0.2196（5+，起订 5） | https://www.lcsc.com/product-detail/C908113.html |
| C1, C3 | 100uF 25V SMD 铝电解，D6.3×L5.4mm | C52268577 | JARSON XS1E101M0605 | 27,060 | $0.0329（20+） | 首选；尺寸与板上 CP_Elec_6.3x5.3 封装匹配。https://www.lcsc.com/product-detail/C52268577.html |
| C2, C4 | 100nF 50V X7R 0805 | C129641 | KEMET C0805C104K5RACAUTO | 48,794 | $0.0524（1+） | https://www.lcsc.com/product-detail/C129641.html |

注意：
- 三个 SMT 件都是 Extended 库，下单时扩展库件有额外费用（按嘉立创当时的 SMT 计费规则），不是 Basic 库免费件。
- C2/C4 不要写不带 AUTO 后缀的 C0805C104K5RACTU（LCSC C3018562），核查时库存为 0；AUTO 版参数相同、有货。
- C1/C3 备选 6.3×7.7mm：C49338535（FOLLON EFFT025ADA101M0677），库存 990，$0.0938——但高度 7.7mm，超过用户定的 5.5mm 高度，只作备选。

## 在 LCSC 有售、但不能走标准 SMT 的通孔件（手焊 / 另购）

| 位号 | 规格 | LCSC 编号 | 制造商型号 | 库存（核查时） | 单价（核查时） | 备注 |
|---|---|---|---|---|---|---|
| K1 | SRD-12VDC-SL-C 继电器 | C30431 | Songle SRD-12VDC-SL-C | 149,136 | $0.4394（1+） | Through Hole 19.2×15.6mm，5 脚。https://www.lcsc.com/product-detail/C30431.html |
| U3 | 7805 TO-220 | C111887 | ST L7805CV | 88,590 | $0.3029（5+，起订 5） | TO-220。https://www.lcsc.com/product-detail/C111887.html |

这两款详情页也显示 "SMT Extended" 标签，但封装字段明确是 Through Hole / TO-220，
属于插装件，不能仅凭该标签认定可贴片。不要把它们填进 SMT 的 BOM/CPL，作手焊件处理。

## 未核查（同样是通孔件，不进 SMT）

- J1、J2：2.54mm 1x08 排针/排母，通孔。
- J3、J4、J5、J7、J8：3.5mm 弯针拔插端子，通孔。

## 本地操作（kicad-jlcpcb-tools）

在 Mac 的 KiCad 里打开 v6 工程，用插件给 U2、C1/C3、C2/C4 填上表里的 LCSC 编号，
生成 BOM + CPL；Gerber 从本地 KiCad 导出；三样直接去嘉立创下单打样加贴片。
K1、U3、端子、排针不在 SMT 单里，另行采购手焊。
