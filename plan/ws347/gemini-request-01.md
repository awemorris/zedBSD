<!-- awesome-plan project=zedbsd record=ws347-gemini-request-01 -->

# Gemini への作業依頼 01: GHCI 1.0 の host の register を仕様どおりにする（KC705・minimal）

- 依頼元: zedBSD の Q1（Claude、監査の役）。2026-11-11。
- 対象: `~/amaterrace-gpu` main 50f13348 の RTL。基準は `docs/ghci.md`（main の版）。
- 目的: zedBSD の GHCI driver が VF2＋KC705（minimal の preset）で GPU を見つけ、能力を正しく読み、ring を回せるようにする。
- 根拠の台帳: zedBSD `plan/ws347/ghci-rtl-questions.md` の A1〜A11（file:line はそこと `audit-50f13348.md`）。

## 守ってほしい事

1. **`docs/ghci.md` が正。仕様は書き換えない。** 仕様が黙っている・矛盾する所に当たったら、推測で埋めずに止めて、その点を報告に書く（下の「対象外」の件は今回触らない）。
2. amaterrace-gpu の AGENTS.md の規則（Verilog-2005、`rtl/library/arith/` の外に算術の演算子を書かない、`git commit -m WIP`、Vivado は 1 process・2 thread、実行の後の `sync`）。
3. 固定値で辻褄を合わせない。値は構成（`include/atg_config.vh` の macro）と board・top の parameter から作る。macro が無い時の既定は「無い」（0）にする。
4. timing（200 MHz、minimal）を壊さない。register の読みの経路は flop で切ってよい（読みの latency の増加は仕様上問題ない）。

## 作業

### T1. page 0（0x000〜0x0FC）の読みと書きを §3 の表どおりに（A6〜A9）

今は `hcr_reg_ridx` の default が旧い map の index を流用し、別の register の値と混ざる。全 offset について、その offset の register だけが答えるようにする。

| offset | 名前 | 種別 | KC705・minimal で期待する値・動き |
| --- | --- | --- | --- |
| 0x000 | GHCI_MAGIC | RO | 0x47484349 |
| 0x004 | GHCI_VERSION | RO | 0x00010000 |
| 0x008 | DEVICE_CAPS | RO | T2 |
| 0x00C | EXTCAP_PTR | RO | 0x100 |
| 0x010 | STATUS | RO | §3.3 |
| 0x014 | CTRL | RW | §3.3 |
| 0x018 | FAULT_INFO | RO/W1C | **1 を書いた bit が消える**（今は書き込みが捨てられる） |
| 0x01C・0x020 | FAULT_ADDR_LO・HI | RO | |
| 0x024・0x028・0x02C | RING_BASE_LO・HI・RING_SIZE | RW | |
| 0x030・0x034 | RING_WPTR・RING_RPTR | RW・RO | |
| 0x038・0x03C・0x040 | IRQ_STATUS・IRQ_ENABLE・IRQ_SET | W1C・RW・W1S | |
| 0x044 | SCRATCH | RW | 書いた値がそのまま読める（今は WDT_CYCLES と混ざる） |
| 0x048 | WDT_CYCLES | RW | 既定 0x10000000 |
| 0x04C | RESIDENT_WARPS | RO | [15:0] 常駐の warp 数、[23:16] lane あたりの register 数（今は答えが無い） |
| 0x050・0x054 | CP_HEAP_BASE・LIMIT | RW | 書いた値が読める（行き先は対象外の B4。今の接続のままでよい） |
| 0x058 | SESSION_ACTIVE | RO | §11.6（今は RING_WPTR と混ざる） |
| 0x05C・0x060 | TIMESTAMP_LO・HI | RO | **64 bit** の GPU の cycle の counter（今は 32 bit で HI が 0、LO は RING_RPTR と混ざる） |
| 0x064 | GCLK_HZ | RO | 200000000（board top の実際の周波数。今は答えが無い／APERTURE_LIMIT と混ざる） |
| 0x068 | NATIVE_ISA_ID | RO | 0x47534931（"GSI1"） |
| 0x06C・0x070・0x07C | （予約） | — | 0 |
| 0x074・0x078 | DIAG_CP_PC・DIAG_CP_STATUS | RO | |
| 0x080 | EXT_CAP_FMT0 | RO | T2 |
| 0x084 | EXT_CAP_FEATURE | RO | T2 |
| 0x088〜0x0FC | （予約） | — | 0 |

- `include/ghci_regs.h`・`ghci_regs.vh` に NATIVE_ISA_ID（0x068）の定数を足す。

### T2. 能力の 3 つの register を §3.2.1・§3.2.2 どおりに（A2〜A4）

`rtl/gpu/cp/atg_cp_regs.v` の `ATG_DEF_DEVICE_CAPS`・`ATG_DEF_EXT_CAP_FEATURE`・`ATG_DEF_EXT_CAP_FMT0` を、仕様の bit の並びで、構成から作り直す。

**DEVICE_CAPS（0x008）**: [0] UMA（board、KC705 は 0）、[1] IOMMU 0、[2] HW_WALKER（`CONFIG_CP_AUTONOMOUS_MMU`）、[3] INDIRECT_DRAW・[4] SECONDARY_RING（`CONFIG_CP_USE_RV32_CORE`）、[5] DUAL_SOURCE、[6] FP16、[7] GSI1 = 1、[15:8] PHYS_LANES、[23:16] NUM_SU（**固定の 1 でなく実際の SU の数**）、[24] PROT（`CONFIG_MC_ENABLE_PROT`、今は SA）、[25] TEX_FFILT（`CONFIG_TU_ENABLE_FFILT`、今は TRILINEAR）、[26] ROP_PACK（`CONFIG_ROP_ENABLE_PACK`、今は 1 固定）、[27] TASK・[28] MESH（`CONFIG_SU_ENABLE_MESH`）、[29] BLT（`CONFIG_HAS_BLT`）、[30] DISP（**表示の装置の有無、今は 1 固定**）、[31] MSAA。

**EXT_CAP_FEATURE（0x084）**: [3:0] VA_BITS − 30（`CONFIG_GPU_VA_BITS`）、[7:4] PA_BITS − 40（`CONFIG_HOST_PA_BITS`）、[8] DUAL_SOURCE、[9] COARSE_SCAN、[10] FAST_CLEAR、[11] LOGIC_OP、[12] EARLY_Z、[13] HW_CURSOR、[14] COLOR_LUT・[15] MRT_BLEND（対象外の B8。**今の実装に有れば 1、無ければ 0**、判断が付かなければ 0 にして報告）、[19:16] MAX_MRT（`CONFIG_ROP_MRT_COUNT`）、[27:20] LDS_KB（`CONFIG_SU_LDS_KB`）、[28] FP16、[29] SUBGROUP、[30] CLIP_POLY、[31] BLT_SCALE。

**EXT_CAP_FMT0（0x080）**: [6:0] BC1〜BC7、[7] ETC2、[8] 0、[9] SRGB（`CONFIG_FMT_ENABLE_SRGB`、今は 0 固定）、[10] FFILT_FP16・[11] FFILT_FP32、[15:12] は**今の値のまま**（対象外の B5）、[16] TU_BILINEAR、[17] BLT_BLEND、[18] BLT_TILE、[31:19] 0。

- `include/ghci_regs.h`・`ghci_regs.vh` の bit の定義も同じ並びに（.h は TASK・MESH の位置が誤り、PROT・TEX_FFILT・ROP_PACK・BLT・DISP・MSAA が無い。EXT_CAP_FEATURE の `GHCI_EXT_FEAT_*` は古い並び）。使う所（tb・fw・tools・`gsi1_isa.h`）も追従。
- `` `elsif defined(...) ``（`atg_cp_regs.v:50,94`）を Verilog-2005 の正しい前処理に直す（A11）。

**minimal（KC705）で期待する値**: DEVICE_CAPS = **0x40010480**、EXT_CAP_FEATURE = **0x00110000**、EXT_CAP_FMT0 = **0x0000x200**（x は B5 で未決の [15:12]、今の値のまま）。

### T3. extended capability の窓（0x100〜0x3FF）を §4・§4.1 どおりに（A6・A7・A10）

- **書き込み**: 窓の RW の register（DISP2 の全部、MEM の WINDOW_CTRL、FMT_INDEX、PROT、CPFW）に host が書けるようにする（今は page 0 の 11 個だけが書ける）。
- **list の見出し**: `0x100`〜`0x13F` に連続で並べる（各 8 byte: word 0 = [7:0] ID・[15:8] version・[31:16] 次までの dword 数（最後は 0）、word 1 = register の byte offset）。今は 3 つ目以降が 0x180 から置かれ VEND の窓と重なる。
- **register の窓**（重ならない）: FMT `0x140`、PERF・CPFW `0x160`〜`0x17F`、VEND `0x180`（VENDOR_ID・DEVICE_ID・SUBSYSTEM_VENDOR・SUBSYSTEM_DEVICE・GCLK_HZ）、PROT `0x1D0`、MEM `0x1E0`、DISP2 `0x200`〜`0x2BC`。
- **MEM（0x1E0）が答えるように**（今は全部 0 に読める見込み）。KC705 の期待: LOCAL_BASE 0x80000000、LOCAL_SIZE 0x40000000、WINDOW_SIZE 0x04000000、HOST_LIMIT 0x80000000、WINDOW_CTRL のリセット値 1（RW）、MEM_CACHE_CFG は §11 の式。
- KC705・minimal の list に載る物: 今の XCAP（DBG・PERF・CPFW・MEM・DISP2）のままでよい（FMT を載せるかは対象外の B6）。

### T4. `config.mk.template` の欠け（A5）

`CONFIG_TU_L1_CACHE_KB`・`CONFIG_MC_L2_WAYS`・`CONFIG_VF_CACHE_LINES` を足す（値は `config/balanced.mk` と同じ）。

## 受け入れ（Gemini が流して結果を報告）

1. **atg_top を通した tb**（atg_cp_regs の単独の tb では A8 の混ざりが見えないので、全部の register の module が rdata を出す状態で）を作り、minimal と balanced の 2 つの preset で:
   - T1 の表の全 offset を読み、期待値・RO/RW/W1C/W1S の動きを照合（SCRATCH に書いて読む、FAULT_INFO の W1C、TIMESTAMP_HI が進む、など）。
   - T2 の 3 つの register の値を、preset ごとの期待値と照合（期待値は仕様の式で tb の中に書く）。
   - T3: EXTCAP_PTR から list を辿り、ID・version・offset が §4.1 と一致。MEM の値。DISP2 の 1 つの RW の register と WINDOW_CTRL に書いて読める。
2. 既存の tb（`sim/run.sh` の cp・ghci 系）、`tools/lint.sh`、`tools/arith-lint.py` が 0 件・PASS。
3. minimal の KC705 の合成で WNS ≥ 0（200 MHz）。

## 報告してほしい物

- commit の SHA、変えた file、流した試験のコマンドと結果（件数）、minimal と balanced の 3 つの register と MEM の 16 進の値、WNS。
- 仕様が黙っていて決められなかった所。

## 対象外（今回触らない。ユーザーの判断待ち）

- B4 CP_HEAP_BASE・LIMIT の行き先、B5 EXT_CAP_FMT0[15:12]、B6 KC705 の FMT capability、B7 INDIRECT・SECONDARY の報告、B8 COLOR_LUT・MRT_BLEND の構成、B9 仕様の GCLK_HZ の例、B10 `spec/gpu/cp/*.md` の説明書き、B1 BLT_SCALE の揃え、B2 flat の VA、B3 LDS の文。
- SFU（minimal はひとまず無しのまま、ユーザーの決定）。
