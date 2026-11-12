<!-- awesome-plan project=zedbsd record=ws347-p003 -->

# ws347-p003 driver を最新の ghci.md に合わせる（実装と host 試験）

Status: in-progress（2026-11-13 Q1、q2025-i01 P4。11-11 の記述から乖離の確かめを先に）

- 親: [WS347](../ws.md)。状態: planned。依存: p002（[spec-diff.md](../spec-diff.md)）。
- 2026-11-11 ユーザーから、driver は仕様（ghci.md）に合わせて直す、という趣旨の指示を受けた。正本は amaterrace-gpu main の `docs/ghci.md`（a7f5ca47 以降、p006 で include の bit の定義が直る）。

## 範囲（`src/drivers/gpu/ghci/`、HAL は変えない）

1. 新しい CAP の bit を `ghci-regs.h`・`caps.c`（`ghci_caps_has` と decode）・`check.c` の表示に足す: EXT_CAP_FEATURE[30] CLIP_POLY・[31] BLT_SCALE、EXT_CAP_FMT0[16] TU_BILINEAR・[17] BLT_BLEND・[18] BLT_TILE（と FMT0 の SRGB・ETC2・FFILT・MAX_COLOR_ATTACHMENTS のうち未 decode の物）。
2. 各 bit が 0 の時の扱い（spec-diff.md §1）:
   - BLT_SCALE 0: 拡大縮小の blit は blitter に出さず描画の道（vk-meta の描画の blit）。BLT_BLEND 0・BLT_TILE 0: その blitter の操作を出さず shader の道。CAP_BLT 0 は今の道のまま。
   - TU_BILINEAR 0: 全 format の FILTER_LINEAR を報告しない（vk-caps・format）、linear の sampler・blit の filter は nearest に落とすか拒む（Vulkan の規則に従う）。
   - CLIP_POLY 0: guard band の外・near 面をまたぐ primitive の扱い（仕様 §12.10 の文）。driver が何をすべきかを決め、要れば software の clip の代替、少なくとも報告と記録。
3. N79（attachment・MSAA の row_pitch_bytes をそのまま stride、P_min）と N81（occlusion query の BEGIN・END を同じ流れに、session の切り替えを挟まない）に今の code が合っているか確かめ、違えば直す。
4. VS の属性の register の連続の割り当て（`GHCI_VS_REG_ATTR(base, comp)`）に compiler が合っているか確かめる。
5. host 試験（WS290 の試験の道具: `plan/ws290/tests/` ほか）に、新しい bit の 0・1 の場面を足し、全部 PASS。riscv64（VF2 の config に `CONFIG_DRIVER_PCI_GHCI=y`）と amd64 の kernel が warning 0。
- 仕様の誤り・未定義に当たったら推しで埋めずに Q1 へ（Q1 は今は仕様の持ち主）。

## 受け入れ

- 上の 1〜4 の実装、host 試験 PASS、両 arch の kernel の build warning 0。実機は p004。
