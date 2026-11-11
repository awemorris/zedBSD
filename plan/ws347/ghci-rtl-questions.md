<!-- awesome-plan project=zedbsd record=ws347-ghci-rtl-questions -->

# GHCI の仕様と Amaterrace GPU の RTL への疑問（ユーザーの判断を待つ）

- 2026-11-11 ユーザーの決定: Claude は RTL を触らない。仕様と RTL への疑問は、ここにまとめてユーザーの判断を仰ぐ。
- 読んだ版: amaterrace-gpu main 6949b248（ghci.md は bace7d66 から VA_BITS_ENC の 2 行だけ違う）。RTL は読んだだけで、sim・実機では確かめていない。
- 詳しい表は [spec-diff.md](spec-diff.md) の §2。

| # | 疑問 | 根拠 | 状態 |
| --- | --- | --- | --- |
| R1 | KC705 の board top（`boards/kc705/atg_kc705_top.v`）は atg_top に `SLIM_MMIO` を渡さず、既定 0（旧の map）。GHCI 1.0 の page 0（MAGIC・DEVICE_CAPS・EXT_CAP_*）は host から見えないのでは | `rtl/gpu/top/atg_top.v:97` の既定、`atg_cp_regs.v` の非 SLIM の分岐（P1 の読み） | 判断待ち |
| R2 | `DEVICE_CAPS` の RTL の並びが仕様と違う: [24] が SA（仕様 PROT）、[25] が TRILINEAR（仕様 TEX_FFILT）、[26]・[30] が 1 固定（仕様 ROP_PACK・DISP） | `atg_cp_regs.v` の `ATG_DEF_DEVICE_CAPS` | 判断待ち |
| R3 | `EXT_CAP_FEATURE` の RTL の並びが古い形（[0] LOGICOP・[1] EARLY_Z・[2] CURSOR・[3] 1・[7:4] MRT 数）。仕様は [3:0] VA_BITS_ENC・[7:4] PA_BITS_ENC・[11] LOGIC_OP ほか。`include/ghci_regs.h`・`.vh` の `GHCI_EXT_FEAT_*` も古い並び | `ATG_DEF_EXT_CAP_FEATURE`、include | 判断待ち |
| R4 | `EXT_CAP_FMT0[9] CAP_SRGB` が 0 固定（minimal は sRGB を有効に構成） | `ATG_DEF_EXT_CAP_FMT0` | 判断待ち |
| R5 | minimal は SFU 無し。仕様は超越関数を必須とするが RTL はひとまず直さない（ユーザーの決定）。SFU 無しで命令を出すと warp が止まる見込み（`g_no_xu` の `xu_ready` 0） | `atg_su.v` | 決定済み（試験で SFU を使わない） |
| R6 | `CAP_BLT_SCALE=0` の時に、copy・1:1 の blit の src と dst の address に何 byte の揃えが要るかが書かれていない（RTL は `atg_blt_align` が外れ、line の中の位置がずれた copy は壊れる見込み） | ghci.md §3.2.2 [31]、`atg_blt_2d.v`（P2 の読み） | 判断待ち（minimal は Blitter 無しで実機に影響しない） |
| R7 | flat（`CAP_HW_WALKER=0`）の時、VA_BITS は GPU の出す address を縛るか。Q1 は「縛らない、host への到達は MEM の HOST_LIMIT と PA_BITS」と仮に判断して P2 に伝えた（仕様は変えていない） | ghci.md §11.6・§11 の MEM の表、KC705 の host の窓 [0, 0x8000_0000) | Q1 の仮の判断、ユーザーの確認待ち |
| R8 | `config.mk.template` に `CONFIG_TU_L1_CACHE_KB`・`CONFIG_MC_L2_WAYS`・`CONFIG_VF_CACHE_LINES` が無く、生成した `atg_config.vh` の macro が空で `atg_tu` などが構文 error になる（P1 の読み） | Makefile の `include/atg_config.vh` の規則 | 判断待ち |
| R9 | minimal の LDS は 1 KiB、仕様 §3.2.2 の文は「KC705 は 4 KiB を報告」 | `config/minimal.mk`、ghci.md [27:20] | 判断待ち（文の直しか） |
| R10 | TU の BC decoder（`atg_tu_decp`）はどこからも instance されていない。GPU の top に `atg_mmu` が無く VA_BITS は使われていない。KC705 は minimal でも HAS_CPU=1 | P1 の読み | 情報 |
