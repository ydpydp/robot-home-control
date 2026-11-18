# Design review

issues · scope save · 3 must change, 0 should, 5 notes

## Must change

- **J1, J2: no manufacturer part number** (bom: J1, J2) Conn_01x08 Connector_PinHeader_2.54mm:PinHeader_1x08_P2.54mm_Vertical has no MPN field, so it cannot be bought or assembled. Fix: Find the part (prefer the fab's basic or preferred parts: pcbparts jlc_search) and set the symbol's MPN field.
- **J3, J5, J7, J8: no manufacturer part number** (bom: J3, J5, J7, J8) Conn_01x02 TerminalBlock_3.5mm:TerminalBlock_3.5mm_1x02_P3.50mm_Horizontal_D1.4 has no MPN field, so it cannot be bought or assembled. Fix: Find the part (prefer the fab's basic or preferred parts: pcbparts jlc_search) and set the symbol's MPN field.
- **J4: no manufacturer part number** (bom: J4) Conn_01x03 TerminalBlock_3.5mm:TerminalBlock_3.5mm_1x03_P3.50mm_Horizontal_D1.4 has no MPN field, so it cannot be bought or assembled. Fix: Find the part (prefer the fab's basic or preferred parts: pcbparts jlc_search) and set the symbol's MPN field.

## Notes

- **Board setup minimum min_clearance is 0.0 mm, the fab's is 0.1 mm** (fab) Source: https://jlcpcb.com/capabilities/pcb-capabilities, read 2026-11-17. The fab's section in the DRC rules already checks it; raising the board setup's own minimum keeps it if the section is ever removed. Fix: The person applies it from the Fab panel (Apply); an agent names it, never changes the board setup itself.
- **Board setup minimum min_track_width is 0.0 mm, the fab's is 0.1 mm** (fab) Source: https://jlcpcb.com/capabilities/pcb-capabilities, read 2026-11-17. The fab's section in the DRC rules already checks it; raising the board setup's own minimum keeps it if the section is ever removed. Fix: The person applies it from the Fab panel (Apply); an agent names it, never changes the board setup itself.
- **Board setup minimum min_text_height is 0.8 mm, the fab's is 1.0 mm** (fab) Source: https://jlcpcb.com/capabilities/pcb-capabilities, read 2026-11-17. The fab's section in the DRC rules already checks it; raising the board setup's own minimum keeps it if the section is ever removed. Fix: The person applies it from the Fab panel (Apply); an agent names it, never changes the board setup itself.
- **Board setup minimum min_text_thickness is 0.08 mm, the fab's is 0.15 mm** (fab) Source: https://jlcpcb.com/capabilities/pcb-capabilities, read 2026-11-17. The fab's section in the DRC rules already checks it; raising the board setup's own minimum keeps it if the section is ever removed. Fix: The person applies it from the Fab panel (Apply); an agent names it, never changes the board setup itself.
- **No Gerber and drill files in mfg/ yet** (outputs) A buy thread exports them when it orders; nothing to check before. Fix: Export Gerbers and drill files into mfg/<date>/ (kicad-cli pcb export gerbers / drill).

## Not checked

- BOM stock: lookup pending for C2, C4; U3
- BOM stock: 3 lines not checked (no lookup key or no answer)
- DRC and ERC: run beside the design (the Checks panel); a full review waits for them fresh
- Agent-judged checks (datasheets against footprints): on demand and before an order
