<!-- awesome-plan project=zedbsd record=ws347-ghci-rtl-questions -->

# GHCI の仕様と Amaterrace GPU の RTL の台帳

- 役割分担（2026-11-11 ユーザーの決定、AGENTS.md §7）: Claude は仕様と RTL を読んで監査し、driver を書く。Gemini は Claude の指揮の下で RTL を実装する。
  `docs/ghci.md` が正。食い違いは A（仕様の文が明確で RTL が違う → Gemini への修正依頼）と B（仕様が黙っている・表せない・設計の意図が要る → ユーザーの判断）に分ける。迷ったら B。
- この file が唯一の台帳。決定が変わったら行を書き換える（訂正を書き足さない）。担当は会話でなくこの file を読む。
- 読んだ版: amaterrace-gpu main 6949b248（ghci.md は bace7d66 から VA_BITS_ENC の 2 行だけ違う）。RTL は読んだだけで、sim・実機では確かめていない。

## A: RTL の修正依頼（Gemini へ）

| # | 食い違い | 仕様 | RTL | 期待 | 状態 |
| --- | --- | --- | --- | --- | --- |
| A1 | KC705 の board top が atg_top に `SLIM_MMIO` を渡さず既定 0（旧の map）。GHCI 1.0 の page 0（MAGIC・DEVICE_CAPS・EXT_CAP_*）が host から見えない見込み | §3 の page 0 の表 | `boards/kc705/atg_kc705_top.v`、`rtl/gpu/top/atg_top.v:97`、`atg_cp_regs.v` の非 SLIM の分岐 | KC705 で Slim MMIO の page 0 | 依頼前 |
| A2 | `DEVICE_CAPS` の並び: [24] が SA、[25] が TRILINEAR、[26]・[30] が 1 固定 | §3.2.1: [24] PROT、[25] TEX_FFILT、[26] ROP_PACK、[30] DISP | `rtl/gpu/cp/atg_cp_regs.v` の `ATG_DEF_DEVICE_CAPS` | 構成から仕様の bit | 依頼前 |
| A3 | `EXT_CAP_FEATURE` が古い並び（[0] LOGICOP・[1] EARLY_Z・[2] CURSOR・[3] 1・[7:4] MRT 数）。include の `GHCI_EXT_FEAT_*` も古い並び | §3.2.2: [3:0] VA_BITS_ENC（VA − 30）・[7:4] PA_BITS_ENC・[8] DUAL・[9] COARSE・[10] FAST_CLEAR・[11] LOGIC_OP・[12] EARLY_Z・[13] HW_CURSOR・[14] COLOR_LUT・[15] MRT_BLEND・[19:16] MAX_MRT・[27:20] LDS_KB・[28] FP16・[29] SUBGROUP・[30] CLIP_POLY・[31] BLT_SCALE | `ATG_DEF_EXT_CAP_FEATURE`、`include/ghci_regs.h`・`.vh` | 仕様の並び（minimal は VA_BITS_ENC 0x0） | 依頼前 |
| A4 | `EXT_CAP_FMT0[9] CAP_SRGB` が 0 固定（minimal は sRGB を有効に構成） | §3.2.2 の EXT_CAP_FMT0 | `ATG_DEF_EXT_CAP_FMT0` | 構成の sRGB | 依頼前 |
| A5 | `config.mk.template` に `CONFIG_TU_L1_CACHE_KB`・`CONFIG_MC_L2_WAYS`・`CONFIG_VF_CACHE_LINES` が無く、生成した `atg_config.vh` の macro が空で `atg_tu` などが構文 error（P1 の読み） | — | `config.mk.template`、Makefile の `include/atg_config.vh` の規則 | 既定の構成で elaborate が通る | 依頼前 |

## B: 仕様の問い（ユーザーへ）

| # | 問い | 根拠 | 状態 |
| --- | --- | --- | --- |
| B1 | `CAP_BLT_SCALE=0` の時、copy・1:1 の blit の src と dst の address に何 byte の揃えが要るか（RTL は `atg_blt_align` が外れ、line の中の位置がずれた copy は壊れる見込み） | §3.2.2 [31]、`atg_blt_2d.v`（P2 の読み） | 判断待ち（minimal は Blitter 無しで実機に影響しない） |
| B2 | flat（`CAP_HW_WALKER=0`）の時、VA_BITS は GPU の出す address を縛るか。Q1 は「縛らない、host への到達は MEM の HOST_LIMIT と PA_BITS」と仮に P2 に伝えた（仕様は変えていない） | §11.6、§11 の MEM の表、KC705 の host の窓 [0, 0x8000_0000) | 判断待ち（Q1 の仮の判断の確認） |
| B3 | minimal の LDS は 1 KiB、§3.2.2 の文は「KC705 は 4 KiB を報告」 | `config/minimal.mk`、§3.2.2 [27:20] | 判断待ち（文を直すか） |

## 決定済み

| # | 件 | 決定（2026-11-11 ユーザー） |
| --- | --- | --- |
| D1 | SFU（rcp・rsq・exp2・log2・sin・cos・fsqrt.s） | 仕様上必須。minimal の RTL はひとまず SFU 無しのまま。疎通の試験は compiler が SFU を使わない範囲で |
| D2 | VA の幅 | 30 bit 以上。`VA_BITS_ENC` = VA − 30（6949b248）。page table の説明は変えない |
| D3 | 命令の規則 | CAPS に無い命令は使ってはいけない |

## 情報（P1 の読み）

- TU の BC decoder（`atg_tu_decp`）はどこからも instance されていない。GPU の top に `atg_mmu` が無く、VA_BITS は使われていない。KC705 は minimal でも HAS_CPU=1。
