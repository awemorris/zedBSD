<!-- awesome-plan project=zedbsd record=ws347-spec-diff -->

# GHCI の仕様の差と minimal の構成の実際の値（2026-11-11 Q1 の調べ）

- 比べた物: driver が基にした ghci.md d9252e4（2026-10-28）と amaterrace-gpu main bace7d66（ghci.md a7f5ca47、2026-11-06）。
  間の commit は 1b9837a9・a7f5ca47 など 7 個、docs/ghci.md の差は 51 行。amaterrace-gpu の作業ツリーの docs・include に未 commit の差は無い。
- 実際の値は bace7d66 の RTL（`rtl/gpu/cp/atg_cp_regs.v` の `ATG_DEF_*`）を `config/minimal.mk` の `include/atg_config.vh` で読んだ物。

## 1. 仕様の差（d9252e4 → a7f5ca47）

| 項目 | 中身 | driver の今 |
| --- | --- | --- |
| DEVICE_CAPS[29] CAP_BLT | bit は前からある。0 の時 blitter は合成されず、転送・clear は driver が shader で行う、と明記 | 対応済み（`has_blitter()` → vk-meta の compute の道） |
| EXT_CAP_FEATURE[30] CAP_CLIP_POLY（新） | 0 なら多角形の clip が無く guard band。near 面をまたぐ primitive は捨てるか driver の software の代替 | 未対応 |
| EXT_CAP_FEATURE[31] CAP_BLT_SCALE（新） | 0 なら CMD_BLIT は 1:1 だけ。拡大縮小は 3D の描画で | 未対応 |
| EXT_CAP_FMT0[16] CAP_TU_BILINEAR（新） | 0 なら全 format で FILTER_LINEAR を報告しない、sampling は nearest だけ | 未対応 |
| EXT_CAP_FMT0[17] CAP_BLT_BLEND（新） | 0 なら blitter は不透明の fill・copy だけ | 未対応 |
| EXT_CAP_FMT0[18] CAP_BLT_TILE（新） | 0 なら blitter は linear だけ、tiling の変換は shader で | 未対応 |
| CTRL.RESET の後（N80） | RESET の自己解除の後に STATUS.MEM_READY=1 を待つ | 対応済み（ghci.c・recover.c） |
| occlusion query（N81） | 1 つの counter。BEGIN・END は session の切り替えを挟まず同じ流れに | 未確認 |
| attachment・MSAA の row pitch（N79） | `row_pitch_bytes` をそのまま stride に。最小 P_min、64 byte に揃えるのを推す | 未確認 |
| VS の属性の register（§12.10.3） | `GHCI_VS_REG_ATTR` は連続の割り当て（base + comp）に。旧の loc×4 は `_LEGACY` | WS290 p006（R18、base_i）で対応済みの見込み、未確認 |
| FAULT_INFO の kind の別名・stage の番号 | ghci_regs.vh に別名（INVALID_ARG・RING_OOB・WATCHDOG ほか）と VS/FS/CS/MS の番号 | 値は同じ、影響なしの見込み |

## 2. minimal の実機の値と仕様の食い違い（RTL を読んだ物、実機では未確認）

DEVICE_CAPS（RTL の並び）: [31] MSAA 0、[30] 1 固定、[29] BLT 0、[28:27] MESH 0、**[26] 1 固定**、**[25] TRILINEAR**、**[24] SA**、[23:16] NUM_SU 1、[15:8] lanes 4、[7] 1、[6] FP16 0、[5] DUAL 0、[4:3] RV32 0、[2] HW_WALKER 0、[1:0] 0。

| # | 食い違い | minimal での害 |
| --- | --- | --- |
| D1 | DEVICE_CAPS[26] は仕様で CAP_ROP_PACK、RTL は 1 固定（minimal は PACK 0） | driver が integer・float の render target を有ると誤る |
| D2 | DEVICE_CAPS[25] は仕様で CAP_TEX_FFILT、RTL は TRILINEAR。[24] は仕様で CAP_PROT、RTL は SA | minimal では両方 0 で偶然一致。他の構成では誤る |
| D3 | EXT_CAP_FEATURE の RTL の並びは古い形（[0] LOGICOP・[1] EARLY_Z・[2] CURSOR・[3] 1・[7:4] MRT 数・[8] DUAL・[9] COARSE・[10] FAST_CLEAR・[30] CLIP_POLY・[31] BLT_SCALE）。仕様は [3:0] VA_BITS_ENC・[7:4] PA_BITS_ENC・[19:16] MAX_MRT・[27:20] LDS_KB・[28] FP16・[29] SUBGROUP | minimal の値は 0x18。仕様どおりに読むと VA 40 bit・PA 41 bit・MAX_MRT 0・LDS 0 KiB（実際は VA 30・PA 40・MRT 1・LDS 1 KiB） |
| D4 | minimal の GPU の VA は 30 bit。仕様の VA_BITS_ENC は 32 + raw で 32 bit 未満を表せない | driver が VA の上限を誤る |
| D5 | EXT_CAP_FMT0[9] CAP_SRGB は RTL で 0 固定（minimal は sRGB を有効に構成） | sRGB を使わない（害は小さい） |
| D6 | SFU（rcp・rsq・exp2・log2・sin・cos・fract）は `CONFIG_SU_ENABLE_SFU=0` で外れ、CAP の bit が無い。RTL では xu_ready=0 のままで、その命令の warp は進まない見込み | shader の compiler が transcendental を出すと GPU が止まる。driver は外れたことを知る手段が無い |
| D7 | LDS は minimal で 1 KiB（仕様の文は KC705 で 4 KiB と書く）。Vulkan の最小は 16 KiB | compute の shared memory の報告。D3 で読めない |
| D8 | CONFIG_SU_ENABLE_SA・SUBGROUP・MRT 数・FP16 の format・early Z・cursor などの CAP は仕様にあるが、D3 の並びの違いで読めない物がある | D3 と同じ |

- 仕様に CAP の bit が無いまま外れる物（D6 の SFU、D7 の LDS の値）は、仕様に bit を足すか driver の側の仮定にするかを決める要がある。
