<!-- awesome-plan project=zedbsd record=ws347-p006 -->

# ws347-p006 Amaterrace GPU の RTL の CAPS を仕様（ghci.md）に合わせる

- 親: [WS347](../ws.md)。状態: planned。repo: `/home/awe/amaterrace-gpu`（2026-11-11 から Q1 の物、その repo の AGENTS.md に従う）。
- 根拠: [spec-diff.md](../spec-diff.md) の §2（D1〜D8）。2026-11-11 ユーザーから、仕様を正とし瑕疵が無ければ RTL を直す、という趣旨の指示を受けた。
- Q1 の判断（2026-11-11）: D1〜D8 はどれも仕様の瑕疵ではない。超越関数（rcp・rsq・exp2・log2・sin・cos・fract）は §12.5 で CAPS の bit を持たない必須の命令、
  VA_BITS は 32 か 40（§3.2.2、§11 の page table）。よって minimal の構成を仕様に合わせる。

## 範囲

1. `rtl/gpu/cp/atg_cp_regs.v` の `ATG_DEF_DEVICE_CAPS`・`ATG_DEF_EXT_CAP_FEATURE`・`ATG_DEF_EXT_CAP_FMT0` を ghci.md §3.2.1・§3.2.2 の bit の並びに直す。
   - DEVICE_CAPS: [0] UMA（KC705 は 0）、[1] IOMMU 0、[2] HW_WALKER、[3] INDIRECT_DRAW・[4] SECONDARY_RING（RV32）、[5] DUAL_SOURCE、[6] FP16、[7] GSI1、
     [15:8] PHYS_LANES、[23:16] NUM_SU、[24] PROT（`CFG_MC_ENABLE_PROT`）、[25] TEX_FFILT（`CFG_TU_ENABLE_FFILT`）、[26] ROP_PACK（`CFG_ROP_ENABLE_PACK`）、
     [27] TASK・[28] MESH、[29] BLT、[30] DISP（固定の 1 でなく表示の装置の有無）、[31] MSAA。
   - EXT_CAP_FEATURE: [3:0] VA_BITS − 32、[7:4] PA_BITS − 40、[8] DUAL_SOURCE、[9] COARSE_SCAN、[10] FAST_CLEAR、[11] LOGIC_OP、[12] EARLY_Z、[13] HW_CURSOR、
     [14] COLOR_LUT、[15] MRT_BLEND、[19:16] MAX_MRT、[27:20] LDS_KB、[28] FP16、[29] SUBGROUP、[30] CLIP_POLY、[31] BLT_SCALE。
   - EXT_CAP_FMT0: [6:0] BC1〜7、[7] ETC2、[8] 0、[9] SRGB（`CFG_FMT_ENABLE_SRGB`）、[10] FFILT_FP16・[11] FFILT_FP32、[15:12] MAX_COLOR_ATTACHMENTS（MAX_MRT と同じ）、
     [16] TU_BILINEAR、[17] BLT_BLEND、[18] BLT_TILE。
   - 実装の有無と食い違う固定値を残さない（config の macro か、atg_top・atg_cp の parameter から）。
2. `include/ghci_regs.h`・`include/ghci_regs.vh` の EXT_CAP_FEATURE・EXT_CAP_FMT0 の bit の定義を ghci.md に合わせる（ghci.md が絶対の正本）。使う所（sim の tb・fw・tools・gsi1_isa.h）を追従。
3. `config/minimal.mk`: `CONFIG_GPU_VA_BITS = 32`、`CONFIG_SU_ENABLE_SFU = 1`（超越関数の装置。無いと命令の warp が止まる）。HOST_PA_BITS 40 はそのまま。
   VA 32 で 30 を前提にした所（TLB の VPN の幅など）が壊れないか確かめる。
4. 試験: `sim/run.sh` の cp の tb（`sim/tb/atg_cp_tb.v`）と関係する tb で、minimal と既定（config.mk.template）の両方の構成の DEVICE_CAPS・EXT_CAP_FEATURE・EXT_CAP_FMT0 の値を仕様の式で照合。
   `tools/lint.sh` と `tools/arith-lint.py` の 0 件。SU の tb で SFU を有効にした minimal の構成が通る。
- Vivado は使わない（host の Vivado は Q1 の bitstream の build が lock を持つ）。bitstream の作り直しは p007（Q1）。

## 受け入れ

- 3 つの register の値が、minimal と既定の構成で ghci.md の式と一致（tb で）。lint・arith-lint 0、関係の tb が PASS。
- minimal で SFU と VA 32 の構成が elaborate・sim で通る。

## 範囲の訂正（2026-11-11 ユーザーの決定、Q1）

- ユーザーから、仕様の規則は「CAPS に無い命令は使わない」で、VA は 30 bit 以上（32・40 の決め打ちでない）、という趣旨の訂正を受けた。上の「Q1 の判断」の 2 文は誤り。
- SFU: 一度、超越関数を CAPS の bit（EXT_CAP_ISA）で外せる形に仕様を直した（amaterrace-gpu 21f78ddc）が、ユーザーの決定で SFU は仕様上必須に戻した（aab26bb6 で revert）。ただし、ひとまず RTL の SFU の部分は直さない（minimal は SFU 0 のまま、SFU 無しの時の命令の扱いも変えない）。
- VA: minimal は 30 bit のまま。EXT_CAP_FEATURE の VA_BITS_ENC（VA − 32）が 30 bit を表せない件は未決（仕様の符号化の変更の案: VA − 30）。
- よって範囲 3（minimal.mk の変更）は取り下げ。範囲 1・2・4 のうち VA_BITS_ENC を除く物を進める。
