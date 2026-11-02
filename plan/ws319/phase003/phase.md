<!-- awesome-plan project=zedbsd record=ws319-p003 -->

# ws319-p003 H.265 decode: i915 の HCP（long format）を Vulkan Video として

- 親: [WS319](../ws.md)。
- Status: cleared（2026-11-02 P4 q1926-i01、Q1 の受け入れ待ち。worktree p4-ws319、branch agent/p4-ws319、基点 main 373e3480b）
- 依存: WS322 p005（libvulkan の H.265 decode の wire、capset の bit 2）、WS324 の libmedia の H.265 の解析と WS322 p006 の Vulkan Video の H.265 の back end（vkvideo-h265.c）、vkvideo-probe の H.265 と --copy（main）。

## 目的

5330（Alder Lake-P、Gen12）の VCS0 の HEVC の decoder（HCP）で H.265（Main、8 bit 4:2:0、progressive）を Vulkan Video の decode として出す。H.264（WS083、MFX の short format）と同じ executor（`src/drivers/gpu/i915/vk/`）、同じ wire。

## 設計

### 方式: long format、HuC を使わない

- Gen12 の HCP は slice の header を自分で読まない long format と、HuC の firmware（S2L）が header を読む short format を持つ。short format は HuC の load と認証（GuC）が要るので採らない。long format では executor が各 slice segment の header を bitstream から読み、HCP_SLICE_STATE・HCP_REF_IDX_STATE・HCP_WEIGHTOFFSET_STATE に写す。
- Vulkan は bitstream を record の後に書いてよいので、header は submit の時に読む（executor の実行は submit の時なので、record の時に読む Mesa 25 の ANV の問題は無い）。読みは `drv_i915_gfx_memory_cpu` の CPU の view で、segment の範囲の中だけ。
- 出典（guardrail「MIT・GPL の出典のデータの引用」）: 命令の field の位置と長さは Mesa 25.0.7 `src/intel/genxml/gen90.xml`・`gen110.xml`・`gen120.xml`（MIT）、命令の順と手順は Mesa 25.0.7 `src/intel/vulkan/genX_cmd_video.c`（anv_h265_decode_video、long format の最後の版）と Mesa 26.3.0-rc3 の同じ関数（Log2MaxTransformSkipSize）、line buffer と MV の大きさは Mesa 25.0.7 `anv_video.c`。block ごとに「Value(s)/Procedure referenced:」。slice header・参照の list・重み・tile・scaling list は ITU-T H.265（7.3.6、7.4.7、8.3.4、6.5.1、6.5.3、7.3.4・Table 7-6）。slice header の読みの構造は P3 の `src/drivers/gpu/bcm2711/vk/vulkan-video-hevc-slice.c`（同じ project の Zlib）を写して i915 の形にした（Q1 の了承）。

### 1 picture の command

MI_FLUSH_DW（video の cache の無効化）、MI_FORCE_WAKEUP（HEVC の power well）、VD_CONTROL_STATE（pipeline の初期化）、MFX_WAIT、HCP_PIPE_MODE_SELECT（HEVC decode）、MFX_WAIT、HCP_SURFACE_STATE（出力と参照、Gen12 は両方）、HCP_PIPE_BUF_ADDR_STATE（121 dw: 出力、9 本の line buffer、この絵の MV、8 枚の参照と 8 本の co-located の MV）、HCP_IND_OBJ_BASE_ADDR_STATE（bitstream、上限は buffer の終わり）、HCP_QM_STATE ×20（raster、16x16・32x32 は DC）、HCP_PIC_STATE（41 dw）、HCP_TILE_STATE（tile の時）、slice segment ごとに HCP_SLICE_STATE（13 dw）・HCP_REF_IDX_STATE（L0、B は L1）・HCP_WEIGHTOFFSET_STATE（重みの時）・HCP_BSD_OBJECT（NAL の header から segment の終わり）、VD_CONTROL_STATE（memory の implicit flush）、VD_PIPELINE_FLUSH、MI_FLUSH_DW。上限は 256 segment で 35436 dw、session の batch は 144 KiB。

### 値の決め方（ANV から直した所を含む）

- 参照の place（HCP の 8 枚）: 絵の StCurrBefore・StCurrAfter・LtCurr の順に、decode の pReferenceSlots にある slot を 0〜7 に。8 を超える時は skip。Foll の絵は HCP に渡さない。Vulkan の maxActiveReferencePictures は 16（libmedia の DPB の大きさと合わせる）。
- RefPicList: 8.3.4 の一時の list を NumPicTotalCurr か num_ref_idx の大きい方まで繰り返し、list_entry の modification を当てる（ANV は modification を assert で拒む）。無い絵の場所は最初の在る絵。
- slice の値: SliceQpY、chroma の offset、deblocking は override が無い時 PPS の値（ANV は 0）、slice_loop_filter_across_slices は無い時 PPS の値（ANV は 0）、header extension は byte で読む（ANV は bit）、dependent segment は独立の segment の値を写す（ANV は 0 のまま）。Original Slice Start は独立の segment の address。
- SliceHeaderLength: NAL の header から byte_alignment までの byte 数、emulation prevention の byte を除いた数（Mesa と同じ）。
- Low Delay: L0・L1 のどの絵も今の絵の後でない（8.5.3.2.2）。tb は POC の差を -128〜127 に。long-term の印、重みを送った印。
- Log2MaxTransformSkipSize = 2（range extension が無い時の H.265 の推定値。0 だと VCS0 が止まった、下の結果）。
- scaling list: wire は対角（coded）の順、HCP には raster。SPS の flag が無ければ全部 16、PPS の list、SPS の list、既定（Table 7-6）の順。
- tile: uniform は 6-3・6-4 で計算（ANV は column_width_minus1 をそのまま使う）。各 tile の最初の CTB の下位 8 bit と上位 2 bit。
- 能力: profile Main（SPS は Main の道具: 8 bit 4:2:0、range・SCC の拡張無し）、16〜4096、level 5.1、slot 17・参照 16、粒度 64×64（絵の image が書かれる CTB を全部持つ）。
- session の binding: 9 本の line buffer（ANV の式を最小の CTB 16 で数えて 2 倍）、slot ごとの MV と予備 1 本。

## 範囲

- 範囲の外: Main10・4:2:2・interlaced、encode（WS319 の後の Phase）、4096 を超える絵（image の上限）、HuC。

## UAPI・HAL

- HAL は変えない。`include/uapi/` は変えない。toolchain は触らない。

## 受け入れ

| 項目 | 確かめ方 |
| --- | --- |
| host | `plan/ws319/tests/hcp/run.sh`（HCP の命令の長さ・順・field）、`plan/ws319/tests/hevc-slice/run.sh`（slice reader を HEVC_v1 全部で libmedia と照合、fuzz） |
| build | `config-5330.mk` の vmunix と userland が warning 0 |
| 実機 | ws319-p004 |

## 結果（2026-11-02 P4）

| commit | 中身 |
| --- | --- |
| 350551baad | `vk/vk-video-h265.c/.h`（wire の reader、Main の確かめ、slice segment の header、参照の list、重み、tile、scaling の raster）、`vk/vk-video-hcp.c/.h`（HCP の命令）、`vk/vk-video.c`（H.265 の profile・能力・session・parameters・record・確かめ・書き、binding 27）、`vk/vk.c`（capset の bit 2）、`intel/genxml-video.h`（HCP の値）、`worker.c`（video の hang で VCS0 の register を出す）、`platform/amd64/vmunix.mk`、`plan/ws319/tests/hcp/` |
| 01c0463444 | vkvideo-probe: --copy を transfer のできる family で（i915 の video の family は decode だけ）。2 family の時だけ queue・pool を足し、image を CONCURRENT に |
| f1e6e5b443 | `plan/ws319/tests/hevc-slice/`（P3 の slice の host 試験を i915 へ。fork の helper が作った） |

- host: `sh plan/ws319/tests/hcp/run.sh build/tmp/hcp` → 112 checks 0 failed（命令の長さ・順・field）。`HEVC_SLICE_FUZZ=100000 sh plan/ws319/tests/hevc-slice/run.sh` → `hevc-slice run: PASS (147 streams, 15 refused)`（132 本で libmedia と全 field 一致: header_bytes・list・重み・matrix、49158 segment、fuzz 100000 で contract の違反 0）。fuzz が見つけた POC の差の signed overflow（int64 に）と、tile の絵の segment の順を raster で比べていた誤り（tile scan で比べる）を直した。
- build: config-5330.mk の vmunix と userland、warning 0。
- 実機の最初の decode は VCS0 が止まった（ACTHD が batch の終わり、IPEHR は MI_FLUSH_DW: HCP が終わらない）。HCP_PIC_STATE の Log2MaxTransformSkipSize を 2 にして直った。engine の reset で reboot 無しに戻った。
- 実機の表と Video Player は [ws319-p004](../phase004/phase.md): HEVC_v1 は Main の 131 本が全部正しい（128 本は ffmpeg と、SAODBLK_A・B は zip の MD5 と、VPSSPSPPS_A は 1 枚ずつの参照と）、15 本は理由を出して断る。

## 判断（範囲の中の技術）

- HuC を使わず long format（HuC の firmware と GuC の認証が要らない）。
- SPS の profile_idc は見ず、Main の道具（8 bit 4:2:0、range・SCC の拡張無し）なら decode する。Vulkan の profile は Main（libvulkan の約束）。
- 参照の place は Curr の 3 つの set の順。Foll の絵は HCP に渡さない。maxActiveReferencePictures は 16。
- 粒度 64（絵の image が CTB を全部持つ）。最大 4096（NV12 の image の上限）。
