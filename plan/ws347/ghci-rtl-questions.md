<!-- awesome-plan project=zedbsd record=ws347-ghci-rtl-questions -->

# GHCI の仕様と Amaterrace GPU の RTL の台帳

- 役割分担（2026-11-11 ユーザーの決定、AGENTS.md §7）: Claude は仕様と RTL を読んで監査し、driver を書く。Gemini は Claude の指揮の下で RTL を実装する。
  `docs/ghci.md` が正。食い違いは A（仕様の文が明確で RTL が違う → Gemini への修正依頼）と B（仕様が黙っている・表せない・設計の意図が要る → ユーザーの判断）に分ける。迷ったら B。
- この file が唯一の台帳。決定が変わったら行を書き換える（訂正を書き足さない）。担当は会話でなくこの file を読む。
- Gemini への依頼の文: [gemini-request-01.md](gemini-request-01.md)（A1〜A11 を T1〜T4 に。2026-11-11 作成、ユーザーが渡す）。
- Gemini への質問の文: [gemini-questions-01.md](gemini-questions-01.md)（B4〜B10 の RTL の意図と事実。2026-11-11 作成、ユーザーが渡す）。1 次回答と Q1 の照合: [gemini-answers-01.md](gemini-answers-01.md)。2026-11-11 ユーザーが修正方針を承認し、依頼の文 [gemini-request-02.md](gemini-request-02.md)（W1〜W7）を作った。
- 読んだ版: amaterrace-gpu main 50f13348（2026-11-11 15:28、Gemini の修正 5fb4ad16・1e80cfb6・50f13348 の後）。ghci.md は bace7d66 から VA_BITS_ENC の 2 行だけ違う。RTL は読んだだけで、sim・実機では確かめていない。監査の全文は [audit-50f13348.md](audit-50f13348.md)。

## A: RTL の修正依頼（Gemini へ）

| # | 食い違い | 仕様 | RTL | 期待 | 状態 |
| --- | --- | --- | --- | --- | --- |
| A1 | KC705 の board top が atg_top に `SLIM_MMIO` を渡さず既定 0（旧の map）。GHCI 1.0 の page 0（MAGIC・DEVICE_CAPS・EXT_CAP_*）が host から見えない見込み | §3 の page 0 の表 | `boards/kc705/atg_kc705_top.v`、`rtl/gpu/top/atg_top.v:97`、`atg_cp_regs.v` の非 SLIM の分岐 | KC705 で Slim MMIO の page 0 | 一部（SLIM_MMIO は消え Slim の map だけに。page 0 の MAGIC・VERSION・DEVICE_CAPS は読める見込み。残りは A6〜A10） |
| A2 | `DEVICE_CAPS` の並び: [24] が SA、[25] が TRILINEAR、[26]・[30] が 1 固定 | §3.2.1: [24] PROT、[25] TEX_FFILT、[26] ROP_PACK、[30] DISP | `rtl/gpu/cp/atg_cp_regs.v` の `ATG_DEF_DEVICE_CAPS` | 構成から仕様の bit | 未着手（50f13348 で式は不変。`include/ghci_regs.h:300-301` は TASK・MESH の bit も誤り、PROT・TEX_FFILT・ROP_PACK・BLT・DISP・MSAA の定義が無い） |
| A3 | `EXT_CAP_FEATURE` が古い並び（[0] LOGICOP・[1] EARLY_Z・[2] CURSOR・[3] 1・[7:4] MRT 数）。include の `GHCI_EXT_FEAT_*` も古い並び | §3.2.2: [3:0] VA_BITS_ENC（VA − 30）・[7:4] PA_BITS_ENC・[8] DUAL・[9] COARSE・[10] FAST_CLEAR・[11] LOGIC_OP・[12] EARLY_Z・[13] HW_CURSOR・[14] COLOR_LUT・[15] MRT_BLEND・[19:16] MAX_MRT・[27:20] LDS_KB・[28] FP16・[29] SUBGROUP・[30] CLIP_POLY・[31] BLT_SCALE | `ATG_DEF_EXT_CAP_FEATURE`、`include/ghci_regs.h`・`.vh` | 仕様の並び（minimal は VA_BITS_ENC 0x0） | 未着手（50f13348 で式は不変。minimal の値 0x00000018、期待 0x00110000） |
| A4 | `EXT_CAP_FMT0[9] CAP_SRGB` が 0 固定（minimal は sRGB を有効に構成） | §3.2.2 の EXT_CAP_FMT0 | `ATG_DEF_EXT_CAP_FMT0` | 構成の sRGB | 未着手（[15:12] も 0 固定、B5 を参照） |
| A5 | `config.mk.template` に `CONFIG_TU_L1_CACHE_KB`・`CONFIG_MC_L2_WAYS`・`CONFIG_VF_CACHE_LINES` が無く、生成した `atg_config.vh` の macro が空で `atg_tu` などが構文 error（P1 の読み） | — | `config.mk.template`、Makefile の `include/atg_config.vh` の規則 | 既定の構成で elaborate が通る | 未着手（minimal の合成には影響しない） |
| A6 | extended capability の窓への書き込みが全部捨てられる（`hcr_we` は page 0 の 11 個だけ）。DISP2・MEM の WINDOW_CTRL・CPFW・FMT_INDEX・PROT に host が書けない | §3.1 の 0x100..0x3FF は RO/RW、§4.1 | `atg_cp_regs.v:287-301`（5fb4ad16 で表に出た） | 窓の register に書ける | 依頼前 |
| A7 | 答えの無い register: RESIDENT_WARPS 0x04C・GCLK_HZ 0x064・NATIVE_ISA_ID 0x068・MEM 0x1E0〜0x1F3（全部 0 に読める見込み）・PERF_SRC・VEND | §3.1、§3.2.3、§4.1 | `atg_cp_regs.v:560-578` の own の case | 仕様の値（MEM は LOCAL_BASE 0x80000000・LOCAL_SIZE 0x40000000・WINDOW_SIZE 0x04000000・HOST_LIMIT 0x80000000） | 依頼前 |
| A8 | 読みの index の流用で別の register と混ざる: SCRATCH←WDT_CYCLES、SESSION_ACTIVE←RING_WPTR、TIMESTAMP_LO←RING_RPTR、TIMESTAMP_HI←APERTURE_BASE、GCLK_HZ←APERTURE_LIMIT、NATIVE_ISA_ID←FAULT_ADDR、0x06C・0x070←FAULT_INFO・FENCE_LAST。atg_ghci_tb は単独の試験なので見つからない | §3.1 | `atg_cp_regs.v:412-431` の `hcr_reg_ridx` の default | 各 offset が自分の値だけ | 依頼前 |
| A9 | FAULT_INFO 0x018 の W1C が効かない（`hcr_we_ok` に無い）。TIMESTAMP_HI が 0 固定で counter が 32 bit | §3.1 | `atg_cp_regs.v`、`:571` | W1C、64 bit の timestamp | 依頼前 |
| A10 | EXTCAP の配置: 見出しの 3 つ目以降を 0x180 から置き仕様の VEND と重なる。FMT 0x160（仕様 0x140）、PERF 0x110、CPFW 0x120、VEND 0xC0（仕様 0x180）。MEM 0x1E0・DISP2 0x200 は一致。NATIVE_ISA_ID の定数が .h・.vh に無い | §4.1 | `atg_cp_regs.v:507-523`、`atg_cp.v:481` | 仕様の配置 | 依頼前 |
| A11 | `` `elsif defined(...) ``（Verilog の前処理として不正）。macro が無い時に CLIP_POLY・HAS_BLT・TU_BILINEAR が「有り」に倒れる（Makefile を通らない sim・lint で誤る） | — | `atg_cp_regs.v:50,94,136-164` | 正しい前処理、無い時の既定は「無し」か必須の定義 | 依頼前 |

## B: 仕様の問い（ユーザーへ）

| # | 問い | 根拠 | 状態 |
| --- | --- | --- | --- |
| B1 | `CAP_BLT_SCALE=0` の時、copy・1:1 の blit の src と dst の address に何 byte の揃えが要るか（RTL は `atg_blt_align` が外れ、line の中の位置がずれた copy は壊れる見込み） | §3.2.2 [31]、`atg_blt_2d.v`（P2 の読み） | 判断待ち（minimal は Blitter 無しで実機に影響しない） |
| B2 | flat（`CAP_HW_WALKER=0`）の時、VA_BITS は GPU の出す address を縛るか。Q1 は「縛らない、host への到達は MEM の HOST_LIMIT と PA_BITS」と仮に P2 に伝えた（仕様は変えていない） | §11.6、§11 の MEM の表、KC705 の host の窓 [0, 0x8000_0000) | 判断待ち（Q1 の仮の判断の確認） |
| B3 | minimal の LDS は 1 KiB、§3.2.2 の文は「KC705 は 4 KiB を報告」 | `config/minimal.mk`、§3.2.2 [27:20] | 判断待ち（文を直すか） |
| B4 | CP_HEAP_BASE・LIMIT（0x050・0x054）は RTL で旧い APERTURE_BASE・LIMIT に行く。atg_cp_ctl には別に I_HEAP_BASE・LIMIT（index 0x2C・0x2D）がある。§11.2 の heap はどちらか | `atg_cp_regs.v` の hcr_reg_widx・ridx、`atg_cp_ctl.v:126-127` | 方針はユーザーが承認（2026-11-11）、gemini-request-02 の W1 で依頼 |
| B5 | EXT_CAP_FMT0[15:12]: 仕様は「8 を報告」と「EXT_CAP_FEATURE[19:16] を写す」の両方を書く。minimal（MRT 1）は 1 か 8 か | §3.2.2 | 方針はユーザーが承認（2026-11-11）、gemini-request-02 の W2 で依頼 |
| B6 | KC705 に FMT capability が無い（SU_VARY_WORDS 16 で CORE1=0）。仕様は FMT を 0x140 に挙げるが必須とは書かない。minimal に要るか | `atg_top.v:222-223`、kc705_top:352 | 方針はユーザーが承認（2026-11-11）、gemini-request-02 の W3 で依頼 |
| B7 | KC705 は HAS_CPU=1（RV32 の CP）だが minimal は `CONFIG_CP_USE_RV32_CORE=0` で DEVICE_CAPS[3]・[4]（INDIRECT・SECONDARY）は 0。報告すべきか | §3.2.1 | 方針はユーザーが承認（2026-11-11）、gemini-request-02 の W4 で依頼 |
| B8 | EXT_CAP_FEATURE[14] COLOR_LUT・[15] MRT_BLEND に対応する構成の値が仕様に無い | §3.2.2 | 方針はユーザーが承認（2026-11-11）、gemini-request-02 の W5 で依頼 |
| B9 | GCLK_HZ: 仕様 §3.2.3 は「KC705 は 100 MHz」、board top は 200 MHz | kc705_top:109-110 | 方針はユーザーが承認（2026-11-11）、gemini-request-02 の W6 で依頼 |
| B10 | `spec/gpu/cp/atg_cp.md` が CTRL 0x010・STATUS 0x014・SCRATCH 0x048 と書き ghci.md §3.1 と違う。`atg_cp_regs.md` は「GHCI 2.0」のまま | — | 方針はユーザーが承認（2026-11-11）、gemini-request-02 の W7 で依頼 |

## 決定済み

| # | 件 | 決定（2026-11-11 ユーザー） |
| --- | --- | --- |
| D1 | SFU（rcp・rsq・exp2・log2・sin・cos・fsqrt.s） | 仕様上必須。minimal の RTL はひとまず SFU 無しのまま。疎通の試験は compiler が SFU を使わない範囲で |
| D2 | VA の幅 | 30 bit 以上。`VA_BITS_ENC` = VA − 30（6949b248）。page table の説明は変えない |
| D3 | 命令の規則 | CAPS に無い命令は使ってはいけない |

## 情報（P1 の読み）

- TU の BC decoder（`atg_tu_decp`）はどこからも instance されていない。GPU の top に `atg_mmu` が無く、VA_BITS は使われていない。KC705 は minimal でも HAS_CPU=1。
