<!-- awesome-plan project=zedbsd record=ws324-design -->

# WS324 設計: RTX 2070（nvrtx、TU106）の NVDEC（H.265 decode）と NVENC（H.264・H.265 encode）を nvrtx の Vulkan の executor の Vulkan Video として

Parent: [WS324](ws.md)。Phase: [ws324-p001](phase001/phase.md)（この文書を書いた Phase）。
版: 第 2 版（2026-11-01、P1、q1854）。第 1 版に design-reviewer の review 1（[design-review.md](design-review.md)、31 件）の指摘を反映した。実機・QEMU の観測は無い。値と手順は全て source と公開の文書を読んだだけの物（§5 の出典）。main `b43b0c1f03`（worktree の基点。`efd4943052` との差は plan だけ）の code を読んだ。

## 0. 要約

- 見せ方（ユーザーの決定、Vulkan Video が OS の API）: nvrtx の executor（`src/drivers/gpu/nvrtx/vk/`）の queue family 0 に、今の `VK_QUEUE_VIDEO_DECODE_BIT_KHR`（H.264、WS243）へ H.265 decode と `VK_QUEUE_VIDEO_ENCODE_BIT_KHR`（H.264・H.265）を足す。新しい cdev・UAPI の ioctl は作らない。足すのは GPU の命令の番号（`include/uapi/gpu-op.h` の video の続き）と capset の bit だけ（§10、[proposed/](proposed/gpu-op-video-encode.md)）。
- **H.265 decode** は WS243 の NVDEC の channel をそのまま使う。NVDEC は stateless で、HEVC の picture の構造（`nvdec_hevc_pic_s`）と作業の buffer の大きさの式は NVIDIA の open-gpu-doc（MIT）に公開されている。WS243 の H.264 と同じ形（同期の decode、COINCIDE、block linear の NV12）に、HEVC の作業の buffer・picture の番号の表・行の揃え（64）を足す。範囲は Main（8 bit、4:2:0）だけ（WS318 Q318-8 の Q1 の判断）。
- **NVENC** は open-gpu-doc に class の method（`clc5b7.h`・`clc7b7.h`・`clc9b7.h`）と driver との interface（`nvenc_drv.h`、H.264・H.265 の picture の設定・rate control・状態の構造）が公開されている。TU106 の class は `0xC4B7`（open-gpu-kernel-modules 570.144 の class の表）。**ただし、その interface を自分で駆動する公開の driver は見つからない**（nouveau・NVK は decode だけ）。zedBSD が最初の公開の実装になる。最大の危険は (1) Turing の firmware が読む構造の版（公開の 2 版で H.264・H.265 とも後半の field の位置が 4 byte ずれ、rate control の中の 4 byte の並びも違う、§3.3）、(2) 動きの探索・mode の判定・量子化の制御の構造の既定の値が文書に無いこと。→ 実装の最初に**関門の Phase**（channel の確かめと、最小の I・P 絵の CQP の encode が host の ffmpeg で decode できるか、時間の上限つき）を置く（§15、Q324-4）。
- encode の範囲: IDR と P だけ（B 無し、WS318 Q318-9）、参照 1 枚、1 絵 1 slice、rate control は DISABLED（CQP）と DEFAULT（CQP 26 と定義）から始め、CBR・VBR は後の Phase。入力は app の NV12（block linear）の image、参照・再構成は driver が 16x16 の tile の形で持つ DPB の image（NVENC の制約、§3.4）。
- **libvulkan**: H.265 decode と encode の転送（wire）、拡張の表、capset の bit。WS322 の design（Q322-4）は「先に着手する方が持つ」、WS318 §10.3 は WS319 を想定。WS324 が持つ案（Q324-3）。
- **Vulkan の header**: ユーザーの決定（WS318 Q318-3 の A: 2 段の更新）どおり、段 1 で今の版のまま H.265 decode を選び、段 2 で `vulkan_core.h` を KHR の encode のある固定の版（Debian の Vulkan-Headers 1.4.309、`vulkan_video.h` の入力として既に SHA-256 を記録済み）で `maintain-api.noct` から作り直す。sysroot の作り直しは toolchain の扱いで **Q1 の許可が要る**（Q324-2）。
- **libmedia**: WS318 を始めずに済む最小は「libmedia の runtime を変えない」。driver の確かめは Vulkan を直に使う試験の道具（`vkvideo-probe` の H.265、新しい `vkencode-probe`）で行う。H.265 の解析（VPS・SPS・PPS・slice header・POC・RPS → StdVideoH265）は道具に要るので、WS318 p016〜p019 の範囲を標準廉価実装手順で先に作る案を推す（Q324-1）。Video Player の H.265 の再生と libmedia の encoder の道（WS318 p020・p021・p022）を WS324 の完了の条件に残すかはユーザーに聞く（Q324-6）。
- HAL: 変えない（§19）。

## 1. 目的・範囲・範囲の外

- 目的（ws.md）: RTX 2070 の video の hardware のうち zedBSD で未実装の H.265 decode、H.264 encode、H.265 encode を Vulkan Video として出し、libmedia から使えるようにする（libmedia の側の口は WS318）。
- 範囲:
  - nvrtx: NVDEC の HEVC、NVENC の channel と H.264・H.265 の encode、executor の Vulkan Video（session・parameters・記録・実行・query）、capset、family の flag、NV12 の encode の入力と DPB の image。
  - libvulkan: H.265 decode と encode の転送、拡張の表、codec の操作の許可。
  - Vulkan の header の選び直しと作り直し（Q1 の許可の後）。
  - 試験の道具: `vkvideo-probe` の H.265、`vkencode-probe`（新）、host の試験。
  - 実機（10.0.10.3 の RTX 2070）の確かめ。
- 範囲の外: H.265 Main10・Main12・4:4:4（hardware にはあるが libmedia が出さない、Q318-8）、H.264 の 10 bit（hardware に無い）、B-frame（Q318-9）、AV1（Turing に無い）、多 slice の encode・lookahead・2 pass・AQ・intra refresh・H.265 encode の temporal MVP、libmedia の encoder の口と Video Player の H.265 の code（WS318）、RTX 30/40（WS242 と同じく class の番号だけを用意し、実機の確かめは無い）。

## 2. 今の姿（main で読んだ事実）

| 物 | 事実 | 場所 |
| --- | --- | --- |
| NVDEC の channel | 段 `nvdec-channel`（boot の `nvrtx.video=1` の時だけ）。work channel の address の空間、chid 48（`NVRTX_WORK_SESSION_CHID_FIRST` 16 + 32 本）、handle `0xf1f00030`・`0xc4b00030`。1 絵ずつ push を出し fence を待つ（`nvrtx_nvdec_run`）。timeout では `ready = 0` にするだけで、engine がまだ書いているかもしれない memory の扱い（WS243 R7）は実装されていない | `nvdec.c:1-80,379-440`、`gsp.c:361-376`、`stage.c:121` |
| NVDEC の class と engine | TU10x は `0xC4B0`。NV2080 の engine type NVDEC0 = 0x13、devinfo（RM の内部の番号）は 0x1d | `nvrtx-nvdec.h`、`objects.c:87-96,436-480` |
| H.264 の picture と push | `nvrtx_nvdec_h264_pic()` が 764 byte の構造を byte・bit の位置で書く。`nvrtx_nvdec_frame` と `nvrtx_nvdec_push_decode` は H.264 の MBHIST を含む | `nvdec-records.c:68,348`、`nvrtx-nvdec.h`、host 試験 `plan/tools/rtx-video/nvdec-host-test.sh` |
| executor の video | `vk/vk-video.c`（2952 行）: capabilities・形式・family の問い・session・parameters・result status の query pool・4 つの記録（begin・end・control・decode）。decode は同期（GR を流して待ち → NVDEC → 待つ）。session の binding は 3 本で固定（`VIDEO_BINDINGS`）、session の run は 8192 byte（pic 0・slice 1024・status 2048・push 6144） | `vk/vk-video.c:1-35,141-158,202-221` |
| query pool の wire | libvulkan は `CREATE_QUERY_POOL` の chain の presence を常に 0 で送る。結果の語は query ごとに 1（pipeline statistics は flag の数）。nvrtx は chain を読み捨てる | `libvulkan/query.c:60-104`、`vk/vk-video.c:1325-1345` |
| family | 1 つ（GRAPHICS・COMPUTE・TRANSFER、video の decoder があれば VIDEO_DECODE）、queueCount 1 | `vk/vk-device.c:607-613` |
| NV12 の image | optimal（block linear、GOB 2 段）だけ。行は 16 に揃える。features は DECODE_OUTPUT・DECODE_DPB・TRANSFER_SRC・SAMPLED。buffer → image の copy は無い（image → buffer は copy engine） | `vk/vk-device.c:657-667`、`vk/exec-copy.c`、WS243 design §4・§7 |
| memory type | 1 つ（VRAM、BAR1 越しに host visible の WC）。CPU の読みは約 12 MB/s（WS243 p004） | `vk/vk-device.c:560-564`、WS243 ws.md |
| capset | 176 byte、byte 168 に `0x5a4e4154`、172 に flags（bit 0 = H.264 decode、bit 1 = sync command） | `runtime.c:83-89,445-451`、`libvulkan/context.c:38-41,221-235` |
| libvulkan の video | H.264 decode だけ。video の門は `context->video_h264` の 1 つで、無い時に video の flag と format の feature を隠す。family の codec の操作は DECODE_H264 だけを残す。拡張の表は video_queue・decode_queue・decode_h264。sType は `case` で分ける | `libvulkan/video.c:1-80,754-765`、`instance.c:353,1314,1369,1392-1410`、`device.c:353-371,433-443`、`context.c:229-232` |
| GPU の命令の番号 | video は `0x10000`〜`0x10009`（reply 付き）と `0x1000a`〜`0x1000d`（記録）。i915・bcm2711 は番号の範囲で振り分ける。protocol version 2、video の capset の時だけ送る | `include/uapi/gpu-op.h:134-140,1893-2034`、i915 `vk-dispatch.c:174`・`vk-command.c:316`、bcm2711 `vulkan-video.c:629,742` |
| Vulkan の header | `vulkan_core.h` は 1.3.269（`VK_HEADER_VERSION 269`）。encode の値（sType・layout・query type・usage・format feature・queue flag・`VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR`）は `VK_ENABLE_BETA_EXTENSIONS` の囲みの中（H.264・H.265 の encode の sType は `_EXT` の名前、他の 23 個は既に `_KHR` の名前の enumerator）。H.265 decode の sType（1000187000〜5）は囲みの外。`vulkan_video.h` は Debian の 1.4.309（`libvulkan-dev 1.4.309.0-1`、SHA-256 `39f358bb…`）から `tools/maintain-video.noct` で video_queue・decode_queue・decode_h264・synchronization2 を選んだ物（4 block・19 command を照合）。`vk_video/` は H.264 の std と decode だけ | `include/libc/vulkan/vulkan_core.h:39,174-175,461-543,728-733,862,1359-1365,2511`、`API-PROVENANCE.md:159-210`、`libvulkan/tools/maintain-video.noct:1-80` |
| host の Vulkan-Headers | `/usr/include/vulkan/vulkan_core.h` の SHA-256 は provenance の値と同じ（1.4.309）。`/usr/include/vk_video/` に h265std・h265std_decode・h264std_encode・h265std_encode がある | host（2026-11-01 に `sha256sum` で照合） |
| libmedia | Vulkan Video の道は H.264 だけ（`vkvideo.c:130` が H.264 以外を断る）。H.264 の Vulkan 用の解析は `h264.c`・`h264-dpb.c`。共通の bit reader・NAL は `vcodec-bitreader.c`・`vcodec-nal.c`。mp4・mkv・avi・ts の demux は HEVC を知る | `userland/desktop/libmedia/`、`mediafile/` |
| 試験の道具 | `vkvideo-probe`（WS083）は自分の H.264 の解析（`h264.c` 1044 行）と DPB（`dpb.c`）を持ち、Vulkan を直に使う。amd64・rpi4 の package | `userland/tests/vkvideo-probe/` |
| stage の log | 1 行 79 桁。実機の log は SSH で取る（`plan/tools/rtx-video/rtx-notes-check.sh` が kernel の nvrtx の行を集める） | `nvrtx-private.h:87` |
| H.265 | src/drivers/gpu・libmedia・libvulkan に H.265 の code は無い（grep で 0 件） | — |

## 3. hardware

### 3.1 TU106 の video の engine（公開の資料）

| 物 | 値 | 出典 |
| --- | --- | --- |
| NVDEC | 1 基、4 世代目。H.264 8 bit 4:2:0、HEVC 8・10・12 bit の 4:2:0 と 4:4:4 | NVIDIA Video Encode and Decode GPU Support Matrix（GeForce RTX 2070 の行） |
| NVENC | 1 基、7 世代目、同時 session 12。H.264（4:4:4・lossless を含む）、HEVC（8K、10 bit、B-frame、4:4:4、lossless）、4:2:2 と AV1 は無い | 同上（RTX 2060 / 2070 の行） |
| NVENC の class | TU106 は `NVC4B7_VIDEO_ENCODER`（0xC4B7）を `ENG_NVENC(0)` に持つ。NVDEC は `NVC4B0`（表は NVDEC 0〜2 を並べるが、実の数は devinfo で決まる） | open-gpu-kernel-modules 570.144 `src/nvidia/generated/g_gpu_class_list.c`（TU106 の表）、`clc4b7.h` |
| NVENC の engine type | NV2080 の番号: NVENC0 = MSENC = 0x1b。RM の内部の番号（devinfo）: NVENC0 = 0x25 | 570.144 `cl2080_notification.h`、`src/nvidia/inc/kernel/gpu/gpu_engine_type.h` |
| NVENC の object の確保 | `NV_MSENC_ALLOCATION_PARAMETERS`（size、prohibitMultipleInstances、engineInstance の 3 語、12 byte。NVDEC の BSP と同じ形） | 570.144 `nvos.h` |
| 解像度 | NVENC の surface の設定の幅・高さは 0〜4095（`frame_width_minus1` の範囲の注記）。NVDEC の HEVC の幅は 16384 まで（注記） | open-gpu-doc `nvenc_drv.h`・`nvdec_drv.h` |

- 能力の報告は確かめた範囲に絞る: decode H.265 は 4096×4096 まで（8K は後）、encode は 4096×4096 まで。encode の最小の大きさは文書に無い（NVIDIA の SDK の caps の値を出典として持っていないので仮に 256×128 から報告し、p011・p012 で小さい方へ測って直す）。

### 3.2 NVDEC の HEVC（open-gpu-doc `nvdec_drv.h`・`clc5b0.h`）

- method（C5B0 の値、WS243 が使う C4B0 と同じ並び）: APPLICATION_ID の HEVC = 7（HEVC_PARSER 0x0C は使わない）、CONTROL_PARAMS の codec HEVC = 7、`HEVC_SET_SCALING_LIST_OFFSET` 0x580、`_TILE_SIZES_OFFSET` 0x584、`_FILTER_BUFFER_OFFSET` 0x588、`_SAO_BUFFER_OFFSET` 0x58C、`_SLICE_INFO_BUFFER_OFFSET` 0x590、`_SLICE_GROUP_INDEX` 0x594。DRV_PIC_SETUP・IN_BUF_BASE・PICTURE_INDEX・SLICE_OFFSETS・COLOC・NVDEC_STATUS・LUMA/CHROMA_OFFSET0〜16・EXECUTE は H.264 と同じ。HISTORY・H264_MBHIST は HEVC では出さない（p005 で要否を確かめる）。
- picture の構造 `nvdec_hevc_pic_s`（276 byte、2 版で配置が同じ）: 鍵の 12 語、stream_len、gptimer、tileformat・gob_height、`sw_start_code_e`（start code を含む stream）、framestride[2]、colMvBuffersize（256 byte 単位）、SAO・BSD の offset（filter buffer に対する 256 byte 単位）、SPS の値、PPS の値（tile の数を含む）、参照の値（num_ref_frames、longtermflag、initreflistidxl0/l1[16]、RefDiffPicOrderCnts[16]）、IDR・RAP の flag、curr_pic_idx、`sw_hdr_skip_length`、`num_bits_short_term_ref_pics_in_slice`、`num_delta_pocs_of_rps_idx`、拡張の v1（`HevcFltAboveOffset`・`HevcSaoAboveOffset` ほか）〜v3（`HevcSliceEdgeOffset` ほか）。
- 作業の buffer の大きさの式（`nvdec_drv.h` の GIP_ASIC_* と HEVC_* の macro）: 縦の filter 16 byte/pixel（tile の境ごと）、BSD の制御 4 byte/行、scaling list 1024 byte、縦の SAO 16 byte/pixel（tile の境ごと）、tile の大きさ `(20*22*2*2+16+15) & ~0xF`（= 1776）、deblock の上 `(align(w,16)/16+2)*align(632,128)`、左 `(align(h,16)/16+2)*align(506,128)`、SAO の左 `(align(h,16)/16+2)*align(713,128)`（いずれも 256 byte に揃える）。
- 版: 公開の 2 版（2021 の `97c510e` と 2024 の `9fdf5c40`）で `nvdec_hevc_pic_s` の配置は同じ（予約の語に名前が付いただけ。host で正規化して比べ、review 1 が pahole で全 field の位置を確かめた）。WS243 の H.264 は 2024 版で TU106 で動いた（hash が一致）。
- 文書に無い物（危険 R4）: filter の buffer の中の並び（deblock・SAO・縦の filter・BSD・slice edge を filter の buffer の中にどう置くか。offset の field と別の method `HEVC_SET_SAO_BUFFER_OFFSET` の関係）、scaling list の buffer の中の並び（合計 1024 byte と内訳の注記だけ）、tile の大きさの区画の並び、`sw_hdr_skip_length` の意味の細部。
- 危険: Mesa の NVK の H.265 は 2026-11 の公開の版（26.3.0-rc3）に無い。報道では NVK の H.265 の作業が GSP を止めた（wedge）とある（原因は不明）。zedBSD でも GSP・NVDEC の止まりを想定する（§13 の隔離、R3）。

### 3.3 NVENC の interface（open-gpu-doc `nvenc_drv.h`・`clc5b7.h`）

- method（C5B7。C7B7・C9B7 も同じ番号で、Ampere 以降は semaphore と predication の method が増える）: APPLICATION_ID 0x200（H264 = 1、H265 = 3）、EXECUTE 0x300、`SET_IN_REF_PIC0〜15_LUMA` 0x400〜、`_CHROMA` 0x440〜、`SET_CONTROL_PARAMS` 0x700（codec、FORCE_OUT_PIC、FORCE_OUT_COL、SLICE_STAT_ON、GPTIMER_ON、MPEC_STAT_ON、SUBFRAME_MODE、RCSTAT、RCMODE 31:24）、PICTURE_INDEX 0x704、`SET_IN_RCDATA` 0x70C、`SET_IN_DRV_PIC_SETUP` 0x710、`SET_OUT_ENC_STATUS` 0x718、`SET_OUT_BITSTREAM` 0x71C、`SET_IOHISTORY` 0x720、`SET_IO_RC_PROCESS` 0x724、`SET_IN_COLOC_DATA`・`SET_OUT_COLOC_DATA` 0x728・0x72C、`SET_OUT_REF_PIC_LUMA` 0x730、`SET_IN_CUR_PIC` 0x734、`SET_OUT_REF_PIC_CHROMA` 0x74C、`SET_IN_CUR_PIC_CHROMA_U` 0x740 ほか。
- 1 絵の設定 `nvenc_h264_drv_pic_setup_s`（512 byte、256 byte に揃える）: magic、参照・入力・再構成の surface の設定（各 32 byte）、SPS（4 byte）・PPS（8 byte）の値、rate control（88 byte）、picture の制御（2024 版で 276 byte）、半分の大きさの再構成の設定、gptimer。picture の制御が slice・動きの探索（me）・mode の判定（md）・量子化（q）の制御の配列の offset と数を持ち、配列は同じ buffer の後ろに置く。H.265 は `nvenc_h265_drv_pic_setup_s`（1024 byte）。
- magic: 上位 16 bit が class、15..8 が版、7..0 が改訂。TU10x（`NV_NVENC_7_2`）は `0xC4B70006`。firmware は上位 24 bit を照合し、違えば「BadMagic」で止まる（文書の注記）。
- **版の食い違い（危険 R1）**: 公開の 2 版（2021 の `97c510e`、2024 の `9fdf5c40`）で、magic はどちらも `0xC4B70006` なのに配置が違う（両版を host で compile して測り、review 1 が pahole で全 field の bit 位置を比べた）:
  - H.264: `pic_control` が 272 → 276 byte（末尾に `stats_fifo_offset`）。512 byte の中で半分の大きさの再構成の設定が 472 → 476、gptimer が 504 → 508。rate control の中の byte 185〜188（drv_pic_setup の中の位置）の 4 つの 1 byte の field の並びが違う（2021: `two_pass_rc`・`rc_class`・`first_pass_source_half_scaled`・`single_frame_VBV`、2024: `single_frame_VBV`・`two_pass_rc`・`rc_class`・`first_pass_source_half_scaled`）。
  - H.265: `pic_control` が 532 → 536 byte。1024 byte の中で半分の大きさの再構成の設定が 744 → 748、gptimer が 776 → 780（予約が 4 byte 縮む）。
  - 対処: 版でずれる範囲（H.264 の 472〜511、H.265 の 744〜783）と rate control の byte 185〜188 を 0 にして使わない（半分の大きさの出力・stats fifo・2 pass を使わず、gptimer は 0 = firmware の既定）。0 ならどちらの版でも同じ意味になる。CBR・VBR（p013）でこの 4 byte に値を入れる時は、p009 で決めた版に従う。
  - 版の見込み: 2021 版の magic の表は C7B7（Ampere）が最新で、Turing の時代に近い。2024 版は Ada を足した版。版の決めが要る時（p013 など）は 2021 版の配置を先に試す。
- 文書が決めている物: 参照の絵は 16x16 の tile の形が必須（`tiled_16x16 = 1`）、入力は block linear か 16x16 の tile（pitch linear は不可）、history の buffer（地の文の式は `(((frame_width_minus1+16)/16)+16)*128`、ただし同じ header の `H264_HIST_BLOCK_SIZE`・`H265_HIST_BLOCK_SIZE` は 224 byte）、co-located の motion の buffer の形（macroblock ごとに 64 byte）、rate control の持続の状態（256 byte、`(rc_mode & 3) != 0` の時だけ）、bitstream の出力は slice header 以下（SPS・PPS は driver が書く、`disable_slice_header = 0` で HW が slice header を書く）、状態（`nvenc_pic_stat_s` 128 byte、`total_bit_count`・`bitstream_start_pos`・`error_status`・`ucode_error_status`）。
- 文書が決めていない物（危険 R2）: me・md・q の制御の「安全な既定」、picture の制御の中の `codec`・`e4byteStartCode`、H.265 の `nal_unit_type`・`sao_*_mode`・`ceil_log2_pic_size_in_ctbs` の値の決め方、`SET_IN_RCDATA` の中身（CQP で要るか）、`SET_UCODE_STATE`・task status の要否、`block_height` の単位、FORCE_OUT_* の要否、CONTROL_PARAMS の codec の H.265 の値（H264 = 3 だけが定義）。→ 関門の Phase（p009b）で、文書の注記どおりの最小の設定（動きの探索は整数の探索だけ、sub-pel は既定、intra の 4x4・8x8・16x16 を全部、量子化は flat、deblock on）から 1 つずつ確かめる。
- firmware: NVDEC と同じく GSP-RM（570.144）が NVENC の falcon の ucode を持つ見込み（WS243 の NVDEC で driver は ucode を load していない）。**未確認**。p009a で確かめる。別の file が要れば Q1 に返す。
- falcon の失敗（BadMagic など）は GSP の RC（robust channel）の通知で来る見込み。p009a で GSP の event の記録の道（NVDEC にも無い、WS243 R8）を作り、channel の RC の通知を stage の log に出す。

### 3.4 surface の形

| 用途 | 形 | 理由 |
| --- | --- | --- |
| decode の出力・DPB（H.264・H.265） | NV12 の block linear（GOB 2 段、pitch = align(w,64)）。**行の揃えを luma 64・chroma 32 に変える**（今は 16） | HEVC の CTB は最大 64。NVDEC が CTB の端まで書くかは文書に無いので、書いても plane と確保を越えない大きさにする（R12）。H.264 にも同じ揃えを使い、配置を 1 つに保つ（WS243 の H.264 の hash と plane の試験で非退行を見る） |
| encode の入力（ENCODE_SRC） | 上と同じ NV12 の block linear | decode の出力・描画の結果を写さずに encode できる。`input_bl_mode = 0`（GPU の BL）、`block_height` は GOB 2 段の値（単位は p011 で確かめる、R9）。H.265 の入力の粒度 32 も行の揃え 64 の中に収まる |
| encode の参照・再構成（ENCODE_DPB） | 新しい NV12 の「16x16 の tile」の配置（luma は align(w,32)×align(h,32)、chroma は luma の半分の行。chroma の tile の形（16×8 か 16×16 か）と CTB 32 の書き方は p009b・p012 で確かめ、大きさは余裕を取る） | NVENC の制約（参照は 16x16 の tile が必須）。usage に ENCODE_DPB がある image だけこの配置にし、他の usage（sample・copy・decode）と組ませない |

- p007・p012 に、plane と確保の後ろに印の pattern を置き、engine が書き越さないことを確かめる試験を入れる。

## 4. firmware と license

- 新しい firmware の file は要らない見込み（§3.3）。GSP の firmware（`userland/firmware/nvidia-gsp/`、570.144）は今の package のまま。
- zedBSD の code は Zlib。

## 5. 出典と引用の扱い（guardrail「MIT・GPL の出典のデータの引用」）

| 出典 | license | 使い方 |
| --- | --- | --- |
| NVIDIA open-gpu-doc commit `9fdf5c4062007929d9f4e6cbad9c9771fe61b880`（2026-06-10、WS243 と同じ）: `classes/video/nvenc_drv.h`（SHA-256 `04ba0a17…`）、`clc5b7.h`（`ac4dcab9…`）、`clc7b7.h`、`clc9b7.h`、`nvdec_drv.h`（`6211a3cf…`）、`clc5b0.h`（`aba8a0ff…`） | MIT | method の番号・field の byte と bit の位置・buffer の大きさの式（値）。block ごとに「Value(s) referenced:」。手順（method の並び）は「Procedure referenced:」 |
| open-gpu-doc commit `97c510e`（2021-02-13）の `nvenc_drv.h`（`c17cc684…`）・`nvdec_drv.h` | MIT | 版の照合（§3.3 R1）。値は 2 版で同じ所だけを使う（版を決めた後はその版） |
| NVIDIA open-gpu-kernel-modules 570.144: `src/nvidia/generated/g_gpu_class_list.c`、`src/common/sdk/nvidia/inc/class/clc4b7.h`、`nvos.h`、`cl2080_notification.h`、`src/nvidia/inc/kernel/gpu/gpu_engine_type.h` | MIT | class の番号・engine type・確保の params の形（値） |
| Mesa 26.3.0-rc3（archive.mesa3d.org の tarball、SHA-256 `3600c01f11439628d0a85de5ed9cb76f297d13e9783cdeecb91aa685055c9942`）: `src/nouveau/vulkan/nvk_video_session.c`・`nvk_cmd_video.c`・`rust/video/decode/h264.rs` | MIT | WS243 と同じ参照（H.265・encode は無い）。H.265 の session の memory の式は §3.2 の open-gpu-doc の式を使う |
| Linux v6.19 nouveau（`nvkm/subdev/gsp/rm/r535/` の fifo・nvenc の作り方） | GPL-2.0 | 読まなくても済む見込み（NVENC の channel は NVDEC と同じ手順を zedBSD の `nvdec.c` が既に持つ）。読む時は手順と値だけ（Procedure referenced）、code・comment・構造・名前を写さない |
| ITU-T H.264・H.265（Annex B、7.3 の構文、8.3.4 の参照の list、Table 7-5・7-6 の既定の scaling list など） | 規格 | parameter set の書き出しと解析・参照の list の組み立て |
| Khronos Vulkan-Headers 1.4.309（Debian `libvulkan-dev 1.4.309.0-1`）の `vulkan_core.h`・`vk_video/*.h` | Apache-2.0 | 宣言の選び出し（今の `vulkan_video.h` と同じ扱い） |
| Vulkan の仕様の refpage（`vkGetEncodedVideoSessionParametersKHR`、`VkQueueFamilyVideoPropertiesKHR` ほか、registry.khronos.org） | 文書 | API の規則 |
| JCT-VC の HEVC の適合の bitstream（HEVC_v1、ITU の公開の site） | 試験の入力 | git に入れない（WS318 §13 と同じ `build/sources/h265-conformance/` に取得し、md5 を照合） |
| NVIDIA Video Encode and Decode GPU Support Matrix（公開の web） | 文書 | 能力の目安だけ |

- NVIDIA の proprietary の driver の振る舞いを観測して値を取る（push の dump など）ことはしない（EULA の逆解析の禁止に当たる恐れ、§17 D11）。
- source の中の出典は該当の block に書き、file の先頭に書かない。driver の中の probe（p009）の comment にも試験の進め方（どの config・どの段・実機での扱い）を書かず、「driver の自己診断」として振る舞いだけを書く（guardrail「source の中の開発の管理情報」）。
- 2026-11-01 の取得: GitHub の raw（open-gpu-doc・open-gpu-kernel-modules、パラメータの無い GET）、archive.mesa3d.org の tarball、Khronos の refpage（review 1）。gitlab.freedesktop.org は bot の確認（Anubis）に当たったので 1 回で止めた（Q1 に報告済み）。取得物は scratchpad に置き、repo に入れていない。

## 6. 構成（責務と境界）

```
 vkvideo-probe / vkencode-probe / （後に libmedia、WS318）
        │ Vulkan Video（KHR の decode・encode）
 libvulkan（wire。H.265 decode と encode の構造を送る）
        │ GPU_OP_*（video の番号の続き）、capset の flag
 nvrtx の executor（vk/）: vk-video.c（object・wire・記録・実行）
        ├ vk-video-h264.c（今）・vk-video-h265.c（新、decode の値の変換）
        ├ vk-encode.c（新、encode の object・記録・実行・feedback）
        ├ vk-encode-h264.c・vk-encode-h265.c（新、1 絵の設定の値、host 試験）
        └ video-ps.c（新、H.264・H.265 の parameter set の書き出し、host 試験）
        │ node の口（vk-node.c）
 nvrtx の engine: nvdec.c（今）・nvdec-records.c（HEVC の picture・push を足す）
                  nvenc.c（新、channel）・nvenc-records.c（新、push と 1 絵の設定の組み立て、host 試験）
                  video-quarantine（nvdec.c と nvenc.c が使う隔離の約束、§9）
```

| 部品 | 責務 | 知らない物 |
| --- | --- | --- |
| `nvenc.c` | NVENC の channel（devinfo・falcon の context・instance block・ring・promote・object・最初の push・停止・解放）、1 回の push の submit と待ち、RC の通知の記録 | Vulkan、codec |
| `nvenc-records.c`・`nvrtx-nvenc.h` | RM の params、1 絵の push（method の並び）、`drv_pic_setup` と制御の配列の byte の組み立て、状態の読み | hardware（host 試験で compile） |
| `nvdec-records.c` | HEVC の picture の構造・tile の大きさ・scaling list の区画・HEVC の push（0x580〜0x594 の method、MBHIST 無し）を足す | 同上 |
| `vk/vk-video-h265.c` | H.265 の parameters（VPS・SPS・PPS）の保管の形、slot と picture の番号の表、参照の list、Vulkan の decode の値 → NVDEC の値の変換 | hardware |
| `vk/vk-encode*.c` | encode の session・parameters（取り出しを含む）・quality level・rate control の状態・feedback の query・記録と実行、値の変換 | NVENC の channel の中身 |
| `vk/video-ps.c` | StdVideo の値（encode の session parameters）から SPS・PPS（H.265 は VPS も）の RBSP を書き、emulation prevention と start code を付ける | hardware |

- 場所の理由: 今の nvrtx の video は `vk/` に Vulkan の側、top に engine の側を置く（WS243）。同じ分け方を保つ。parameter set の書き出しは WS322 が `src/drivers/vpu/vpu-h26x-ps.c` を計画しているが、master-design-policy §2.6（遅い抽象化）に従い共有しない（§17 D7）。

## 7. NVENC の動かし方

### 7.1 channel（`nvenc.c`、段 `nvenc-channel`）

- 条件: `nvrtx.video=1`、work channel が動いた後、NVDEC の channel の段の後（独立: NVDEC が無くても試す）。失敗しても boot は続ける（encode の無い device になる）。
- 手順は `nvdec.c` と同じ（1. devinfo で RM の engine type 0x25 の entry の runlist と ENG_DESC、2. `GET_CONSTRUCTED_FALCON_INFO` で context の大きさ、3. VRAM の instance block・USERD と context の buffer、4. system memory の ring・semaphore・push、5. channel の確保（engine 0x1b）・BIND・SCHEDULE、6. `PROMOTE_CTX`（engine 0x1b）、7. object（`0xC4B7`、`NV_MSENC_ALLOCATION_PARAMETERS`、engineInstance 0）、8. 最初の push（subchannel 4 に SET_OBJECT と NOP と host の semaphore、2 s））。
- chid **49**（NVDEC の 48 の次。handle `0xf1f00031`・`0xc4b70031`）。`NVRTX_WORK_SESSION_CHID_FIRST + NVRTX_WORK_SESSION_CHANNELS + 1` で書き、48 と重ならないことを host 試験で見る。
- `nvrtx_engine_pick` に nvenc の 3 欄（found・runlist・descriptor）を足し、`nvrtx_devinfo_pick` で 0x25 を拾う。
- 停止と解放は NVDEC と同じ位置（`gsp.c` の stop・release）に 1 行ずつ、NVDEC の前。
- `nvdec.c` と `nvenc.c` の channel の code は似るが、master-design-policy §2.6 に従い今は共通の部品にしない（§17 D6）。

### 7.2 1 回の encode の push（`nvenc-records.c`）

APPLICATION_ID（H264 = 1・H265 = 3）→ SET_CONTROL_PARAMS（codec、RCMODE、GPTIMER_ON 0、FORCE_OUT_PIC 1（参照にする絵）、FORCE_OUT_COL は参照にする H.264 の絵）→ PICTURE_INDEX（session の通し番号）→ SET_IN_DRV_PIC_SETUP → SET_OUT_ENC_STATUS → SET_OUT_BITSTREAM → SET_IOHISTORY → SET_IN_CUR_PIC（luma）・SET_IN_CUR_PIC_CHROMA_U（NV12 の CbCr）→ SET_OUT_REF_PIC_LUMA・_CHROMA（setup の slot の image）→ SET_IN_REF_PIC0_LUMA・_CHROMA（P の参照）→ SET_OUT_COLOC_DATA（H.264 の参照にする絵）→ EXECUTE（NOTIFY 無し、awaken 無し）。subchannel 4。address は GPU の仮想 address を 8 bit 右へ（256 byte 整列）。method の並びの細部は p009b で確かめる（Procedure は文書の method の説明から自分で組む）。H.265 は temporal MVP を使わない（§7.5）ので coloc の method を出さない。

### 7.3 状態と feedback

- 状態の区画（`nvenc_pic_stat_s` 128 byte）を session の executor の system memory の run に置く。実行の後に `error_status`・`ucode_error_status` を見て、0 でなければ絵を ERROR にする。
- `VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR`: pool の作成の時の `encodeFeedbackFlags` の順に `BITSTREAM_BUFFER_OFFSET`（`bitstream_start_pos` に置いた値）と `BITSTREAM_BYTES_WRITTEN`（`total_bit_count / 8`）、result status を付けた pool なら最後に COMPLETE・ERROR。1 つの query が返す値の数は flag から数える（libvulkan の `result_count`、§10）。結果は実行の時に決める（decode の status と同じ形、`video_pool` を拡げる）。
- bitstream の形式（start code の有無と長さ、`e4byteStartCode`、parameter set の出力の形）は p008 の最初に Vulkan の仕様（encode の bitstream と `vkGetEncodedVideoSessionParametersKHR`）と照らして決める（未照合。見込みは Annex B の byte stream で start code 付き）。

### 7.4 1 絵の設定（H.264、`vk-encode-h264.c` → `nvenc_h264_drv_pic_setup_s`）

- magic `0xC4B70006`（Ampere・Ada は class から）。
- surface: 入力（block linear、pitch = image の pitch、luma・chroma の offset は image の中の plane の offset の 256 byte 単位）、参照と再構成（16x16 の tile、同じ式）。`frame_width_minus1`・`frame_height_minus1` は Vulkan の `codedExtent` から（NVENC は右と下を crop する、注記）。
- SPS・PPS の値: encode の session parameters の StdVideoH264 から（profile_idc、level_idc、chroma 1、poc_type、log2 の値、frame_mbs_only 1、entropy、num_ref_idx、qp の初期値、chroma qp offset、deblock の制御、transform_8x8）。
- picture の制御: l0[0] = 参照の slot、frame_num、pic_order_cnt_lsb、idr_pic_id、pic_type（IDR 3・I 2・P 0）、ref_pic_flag、slice_mode 1（静的、1 slice）、slice・me・md・q の制御の配列の offset と数（各 1 つ）、hist_buf_size（確保した大きさ）、bitstream_buf_size（dstBufferRange）、bitstream_start_pos 0。
- rate control: mode は実行の時の session の状態（§8.6）。DISABLED は rc_mode 0 で QP[0..2] = slice の `constantQp`（1 slice なので絵の値）。**DEFAULT は CQP の QP 26 と定義する**（reset の後の mode。Vulkan の規則で DISABLED でない時の constantQp は 0 なので、その値を使わない）。CBR・VBR は p013（rc_mode 2・1、持続の状態の 256 byte を session の memory に）。
- **0 に固定する所（R1）**: 472〜511 byte（版でずれる半分の大きさの再構成の設定と gptimer）、rate control の byte 185〜188。`stats_fifo_enable`・半分の大きさの出力を使わない。
- me・md・q の制御の値は p009b の関門で決め、`vk-encode-h264.c` の表にする（実機で確かめた経緯は plan に、source には値の意味だけ）。

### 7.5 1 絵の設定（H.265、`vk-encode-h265.c` → `nvenc_h265_drv_pic_setup_s`）

- 同じ形。SPS の値は min・max の CU と TU の大きさ（2 bit の符号）、SAO、AMP、short-term RPS の数。CTB は 32×32 から始める（p012 で 64 を試す）。RPS は slice header の中（`short_term_ref_pic_set_sps_flag = 0`）、P の参照 1 枚。
- **temporal MVP は使わない**（`sps_temporal_mvp_enabled_flag = 0`。app が 1 を求めたら override で 0 に直して feedback で返す）。H.265 の co-located の buffer と method は作らない（後の拡張）。
- **0 に固定する所（R1）**: 744〜783 byte。
- VPS・SPS・PPS の値は session parameters の StdVideoH265 から。NVENC が対応しない値（tile、PCM、明示の scaling list、temporal MVP、long-term）は `vkGetEncodedVideoSessionParametersKHR` の feedback（`hasStdSPSOverrides`・`hasStdPPSOverrides`）で「直した」と返し、書き出しも直した値で行う。

### 7.6 parameter set の書き出し（`video-ps.c`）

- `vkGetEncodedVideoSessionParametersKHR` は最初の encode の前にも呼ばれる（container の header のため）ので、hardware に頼らず software で書く。入力は session parameters の StdVideo の値（NVENC に合わせて直した後の値）。VUI は Vulkan の値を書く（timing・aspect・colour）。HRD は書かない（CBR の時の扱いは p013）。
- 呼び出しの形（refpage）: pData が NULL なら大きさだけ、pData があって大きさが足りなければ何も書かず 0 を書いて `VK_INCOMPLETE`。wire はこの 2 つを区別して送る（§10）。
- host 試験: 書いた RBSP を host の ffmpeg（`-bsf:v trace_headers`）で読み、値が一致する。

## 8. executor の Vulkan Video

### 8.1 報告

- family 0: 今の flag に `VK_QUEUE_VIDEO_ENCODE_BIT_KHR` を足す（encoder の channel がある時だけ）。queue family の video properties は decode と encode の codec の操作の和。Vulkan の refpage（`VkQueueFamilyVideoPropertiesKHR`）に 1 つの family に decode と encode を置くことを禁じる文は無い（review 1 で確かめた）。p008 で仕様の他の箇所（queue の video の coding scope の規則）も読み、問題が無ければこのまま（§17 D3、R6 は確率の低い退路）。
- capset の flags: bit 2 = H.265 decode、bit 3 = H.264 encode、bit 4 = H.265 encode（[proposed/gpu-op-video-encode.md](proposed/gpu-op-video-encode.md)）。
- 拡張: `VK_KHR_video_decode_h265`、`VK_KHR_video_encode_queue`、`VK_KHR_video_encode_h264`、`VK_KHR_video_encode_h265`（libvulkan の表、capset の bit がある時だけ）。
- `vkGetPhysicalDeviceFormatProperties(NV12)` の optimal に `VIDEO_ENCODE_INPUT`・`VIDEO_ENCODE_DPB`・`TRANSFER_DST`（encode の入力を CPU から作るため、§8.5）を足す。video の format の問い（`vkGetPhysicalDeviceVideoFormatPropertiesKHR`）は usage ごとに: ENCODE_SRC は block linear、ENCODE_DPB は 16x16 の tile の image を作る（usage の組で区別）。

### 8.2 capabilities（案、p007・p011・p012 で直す）

| 物 | H.265 decode | H.264 encode | H.265 encode |
| --- | --- | --- | --- |
| profile | Main（1）、8 bit、4:2:0、progressive | Baseline・Main・High、8 bit、4:2:0 | Main、8 bit、4:2:0 |
| min / max の大きさ | 64×64（未確認）/ 4096×4096 | 256×128（仮、§3.1）/ 4096×4096 | 同左 |
| maxDpbSlots / maxActiveReferencePictures | **16 / 15**（HEVC の DPB は current を含め 16、§8.6） | 2 / 1 | 2 / 1 |
| capability の flag | SEPARATE_REFERENCE_IMAGES（slot ごとに別の image） | SEPARATE_REFERENCE_IMAGES | 同左 |
| bitstream の offset・大きさの整列 | 256 / 256 | 256 / 256 | 256 / 256 |
| level | 5.1（main tier） | 5.1 | 5.1（main tier） |
| decode・encode の flag | DPB_AND_OUTPUT_COINCIDE | — | — |
| rate control | — | DISABLED・DEFAULT（p013 で CBR・VBR） | 同左 |
| QP の範囲 | — | minQp 0・maxQp 51 | 同左 |
| maxQualityLevels / maxBitrate | — | 1 / 100 Mbit/s（目安、未確認） | 同左 |
| quality level の properties | — | preferredRateControlMode DISABLED、preferredConstantQp 26、preferredGopFrameCount 30、preferredIdrPeriod 30、preferredMaxL0ReferenceCount 1、B 0 | 同左 |
| requiresGopRemainingFrames | — | FALSE | FALSE |
| feedback | — | BITSTREAM_BUFFER_OFFSET・BYTES_WRITTEN | 同左 |
| encodeInputPictureGranularity | — | 16×16 | 32×32（CTB 32） |
| slice | — | maxSliceCount 1、P の L0 の参照 1、B 無し、slice ごとの constantQp（1 slice なので絵の値） | 同左 |
| ctb・transform（H.265） | — | — | ctbSizes 32、transformBlockSizes 4〜32 |

- stdSyntaxFlags（encode）は 0 から始める（app の値を全部は受けず、直した値を feedback で返す）。

### 8.3 parameters

- H.265 decode: VPS 16・SPS 16・PPS 64 の id。保つ値は NVDEC の picture の構造に要る物だけ（SPS の大きさ・bit depth・CU・TU・PCM・AMP・SAO・temporal MVP・strong intra・scaling list・short-term RPS の数・long-term の有無と数・log2_max_poc_lsb、PPS の flag・qp・offset・tile の列と行の幅・scaling list・parallel merge・extra slice header bits。VPS は有無の確かめだけ）。update は鍵が重ならない物の追加（libvulkan が先に断る）と updateSequenceCount の +1。
- encode: H.264 は SPS・PPS、H.265 は VPS・SPS・PPS を保ち、NVENC に合わせて直した写しも作る（§7.5）。`vkGetEncodedVideoSessionParametersKHR` は直した写しを `video-ps.c` で書き、feedback を返す。

### 8.4 session の memory と run

session の memory の binding（256 byte 整列、memory type bit 0）は profile ごとに決め、`VIDEO_BINDINGS` を profile の最大（4）にする。大きさは session の `maxCodedExtent` と、**PPS が分からない時点の最悪の tile の数（20 列 × 22 行）**で計算する。

| profile | binding | 大きさ |
| --- | --- | --- |
| H.264 decode | coloc・mbhist・history（今のまま） | WS243 の式 |
| H.265 decode | 0: coloc（`colMvBuffersize` × 17。p005: 1 絵は CTB 64 に揃えた面積の 1/16 byte（Tegra の手順、averne の FFmpeg nvtegra）。2 倍の余裕は取らず、p007 の印の pattern で確かめる） | 同左 |
| | 1: filter（p005: 高さ（CTB 64 に揃えた）の行ごとに 縦の filter 480・SAO 3840・BSD 60 byte を順に（Tegra の手順の値）、その後に上の deblock（`HEVC_DBLK_TOP_*` の形、16 画素の列ごと 640 byte）と上の SAO（同 768 byte）。`HevcSaoBufferOffset`・`HevcBsdCtrlOffset`・v1 の `HevcFltAboveOffset`・`HevcSaoAboveOffset` をそれぞれの位置に。`HEVC_SET_SAO_BUFFER_OFFSET` と slice edge は使わない。並びが違えば R4、p007） | 左の和 |
| H.264 encode | 0: history（**1 macroblock 列あたり 224 byte**（`H264_HIST_BLOCK_SIZE`）× (幅の MB + 16)、文書の地の文の 128 より大きい方）、1: coloc（slot ごとに macroblock 数 × 64 byte × maxDpbSlots）、2: rate control の持続の状態（256 byte、p013 から） | 同左 |
| H.265 encode | 0: history（224 byte × (幅の 16 単位 + 16)）、1: rate control（p013 から） | 同左 |

- executor の system memory の run（session ごとに作成の時に取る、CPU が絵ごとに書く物）を 16 KiB にし、区画を: picture の構造 0（2 KiB、H.265 の 276 byte と NVENC の 1024 byte + 制御の配列が入る大きさ）、slice の offset 2048（1 KiB）、状態 3072（4 KiB）、scaling list 7168（1 KiB）、tile の大きさ 8192（4 KiB。p005 で Tegra の手順に合わせて 0x900 byte を使う: tile ごとの幅・高さと、byte 0x700 からの境）、push 12288 にする（p005 で実装）。H.264 decode の今の配置（8 KiB）も同じ表に移した。
- p005・p009b・p011 に、binding の後ろに印の pattern を置いて書き越しを確かめる試験を入れる。

### 8.5 image

- NV12 の行の揃えを luma 64・chroma 32 に変える（§3.4）。
- NV12 の encode の入力: block linear。CPU から作るために `vkCmdCopyBufferToImage`（PLANE_0・PLANE_1）を copy engine で pitch → block linear に（今の image → buffer の逆、`exec-copy.c` の method の向きを変える）。描画（plane の view を color attachment に）は今は出さない。
- NV12 の encode の DPB: 16x16 の tile の配置。usage は ENCODE_DPB だけ。memory の要求の大きさは luma + chroma、4 KiB 整列。SEPARATE_REFERENCE_IMAGES を報告するので slot ごとに別の image で、array の layer を扱わない。

### 8.6 記録と実行

- H.265 decode の記録: 今の `DecodeVideo` の記録に H.265 の picture info（`StdVideoDecodeH265PictureInfo`、slice segment の offset の列）と参照の slot の H.265 の値（PicOrderCntVal、long-term）を足す。
- **H.265 の slot と picture の番号**: NVDEC の picture の番号（picidx）0〜15 を Vulkan の DPB の slot の番号に一致させる（maxDpbSlots 16）。surface の番号も同じ（LUMA/CHROMA_OFFSETn、n = picidx）。`curr_pic_idx` = setup の slot。setup の slot が無い decode は、active な参照に使われていない番号を使う（H.264 の `nvrtx_video_free_surface` と同じ考え、参照は最大 15 なので必ず空きがある）。`RefDiffPicOrderCnts[i]` = 今の絵の PicOrderCntVal − slot i の絵の PicOrderCntVal（[-128, 127] に切り詰める、p005）、`longtermflag` の bit (15 − i) = slot i が long-term、`num_ref_frames` = 参照の数。
- **初期の参照の list**（`initreflistidxl0/l1`）: H.265 8.3.4 の規則で、`RefPicSetStCurrBefore`・`StCurrAfter`・`LtCurr`（Vulkan が slot の番号で渡す）を L0 は Before → After → Lt、L1 は After → Before → Lt の順に、`NumPicTotalCurr` で繰り返して 16 まで埋める（list の modification は hardware が slice header から読む前提）。slice header の参照の集合: p005 の既定は Tegra の手順と同じく driver が最初の slice segment の header を CPU で読み、slice_type の後から long-term の終わりまでの bit 数を `sw_hdr_skip_length` に入れる（slice header の short-term の集合は Vulkan の `NumBitsForSTRefPicSetInSlice` で飛ばす）。boot の引数 `nvrtx.hevc.dxva=1` で skip 0 と DXVA の欄（`num_bits_short_term_ref_pics_in_slice` ほか）の形に替える。どちらが Turing で正しいかは p007（R4）。参照に無い picidx の面と `RefDiffPicOrderCnts` は今の絵の最初の参照（無ければ今の絵）で埋め、差は int8 に切り詰める（Tegra の手順）。
- encode の記録: `CmdEncodeVideoKHR`（bitstream の buffer・offset・range、入力の view、setup の slot、参照の slot、H.264・H.265 の picture info と slice の情報、quality level）。
- 実行は decode・encode とも同期（drain → engine → 待つ）、待ちの上限 2 s、timeout で engine を使わなくし、§9 の隔離をする。
- **rate control の状態は実行の時に扱う**: `ControlVideoCoding`（reset・rate control・quality level）は実行の順で session の状態（rate control の mode と層、quality level、通し番号）を変える。BeginVideoCoding の chain の rate control と session の状態の比べも実行の時に行い、食い違い（API の違反）はその coding scope の encode を ERROR にする（記録の時には比べない。記録と実行の順は違ってよいため）。reset の後の mode は DEFAULT（§7.4）。

### 8.7 encode の入力の CPU の道と guardrail

- 試験の道具は合成の絵（決まった式の模様）を CPU で buffer に書き、copy engine で image へ写す。画素を CPU で変換しない（guardrail「画像の処理は GPU で」）。CPU が書くのは元の画素だけで、block linear への並べ替えは copy engine（GPU）。

## 9. memory と所有

| 物 | 持ち主 | 寿命 |
| --- | --- | --- |
| NVENC の channel の VRAM・system memory・context | `struct nvrtx_nvenc_state`（`gpu->nvenc`） | 段の始めから GSP の release まで |
| session の executor の system memory の run | `video_session` / `encode_session` | session の作成から破棄まで（隔離の時は boot の終わりまで） |
| session の memory の binding（history・coloc・filter） | app の VkDeviceMemory（session が object を hold） | bind から session の破棄まで（同上） |
| DPB・入力・出力の image、bitstream の buffer | app。記録が実行の終わりまで hold | 記録の drop まで（同上） |

- **timeout の後の隔離**（NVDEC の H.265 と NVENC に共通の約束、今の H.264 にも当てる）: engine の待ちが timeout したら、その時に実行していた記録と session が hold する全部の object（memory・image・buffer・session・run）の参照を、driver の「隔離の list」に移して boot の間返さない。app が `vkFreeMemory`・session の破棄をしても、隔離された object の VRAM は再利用されない（object の hold が残るので、今の executor の参照の数の仕組みで実の解放が起こらない）。隔離した object の数と大きさを段の log に出す。
- WS250 の回復が work channel（NVDEC・NVENC と address の空間を共有する）を止める時は、その前に NVDEC・NVENC の `ready` を 0 にして新しい submit を断り、実行中の engine の仕事の終わりを待つ（上限 2 s、越えたら隔離）。空間の作り直しは WS250 の範囲で、video の channel の作り直しは無い（§16）。

## 10. libvulkan・UAPI の変更

- `gpu-op.h`（[proposed/](proposed/gpu-op-video-encode.md)）: reply 付きの物を `0x10010`〜`0x1001f`、記録の物を `0x10020`〜`0x1002f` に分け、範囲の約束を comment に書く（今の i915・bcm2711 の範囲の振り分けと同じ考え）:
  - `0x10010` `GET_PHYSICAL_DEVICE_VIDEO_ENCODE_QUALITY_LEVEL_PROPERTIES`
  - `0x10011` `GET_ENCODED_VIDEO_SESSION_PARAMETERS`（pData の有無の語と受ける大きさを別に送る）
  - `0x10012` `CREATE_VIDEO_QUERY_POOL`（feedback の query pool の作成。profile と `VkQueryPoolVideoEncodeFeedbackCreateInfoKHR` を持つ。今の `CREATE_QUERY_POOL` は Venus の wire format 1 の番号で全部の executor が chain の presence 0 を前提に読むので、chain を送らない。result status の pool は今の番号のまま）
  - `0x10020` `CMD_ENCODE_VIDEO`
  - protocol の版は上げない（capset の bit が無い backend には送らない）。i915・PowerVR・bcm2711 の executor には送られない。
- `libvulkan/video.c`: H.265 の StdVideo（VPS・SPS・PPS、profile tier level、DPB の管理、scaling list、short-term RPS の配列、long-term、VUI と HRD と sub-layer、picture info、reference info）の転送、encode の構造（capabilities・quality level・session の作成・parameters の add と取り出しと feedback・rate control と layer・encode の info・H.264 と H.265 の picture info・slice の情報・参照の情報・GOP の残り・DPB の slot）の転送。bit field の flag は今と同じ「名前で 1 語に詰める」規則を 1 bit の flag に使い、幅のある field（`StdVideoH265HrdFlags` の 8 bit の 3 つなど）はそれぞれ 1 語で送る。
- `instance.c`・`context.c`: video の門を capset の bit ごとにする（`video_h264` の 1 つの門を、decode H.264・decode H.265・encode H.264・encode H.265 の 4 つに）。encode の bit が無ければ encode の queue の flag と format の encode の feature を隠し、decode の bit が全部無ければ decode の物を隠す。family の codec の操作は renderer の答えと bit の積。
- `device.c`: 拡張の表と依存（decode_h265 → decode_queue、encode_h26x → encode_queue → video_queue）。
- `query.c`: feedback の query pool を `CREATE_VIDEO_QUERY_POOL` で作り、結果の語の数を flag から数える（feedback の flag の数 + result status の有無）。result status だけの pool は今のまま。`external-properties.c`: result status を encode の family にも。
- 試験（T7）: libvulkan の側の往復（`video.c` の符号化を host で読み戻す）を p004・p008 で、nvrtx の reader との往復を p005・p010 で（`plan/tools/rtx-video/wire-host-test.sh` を拡げる）。i915・bcm2711 の H.264 の今の wire と `CREATE_QUERY_POOL` の形が変わらないこと。
- WS322 との関係: WS322 p005・p010（PowerVR の側の H.265・encode の転送）は WS324 が先なら libvulkan の部分を取り下げ、PowerVR の reader だけになる（Q324-3、Q1）。WS322 の design §8.2 の capset の bit の案（bit 2 H.265 decode、bit 3 H.265 encode）とは bit 3 が違うので、WS324 の案で揃えるよう Q1 に頼む。WS318 §10.3（転送は WS319 を想定）の記録も Q1 が直す。

## 11. Vulkan の header（Q1 の許可の項目、Q324-2）

- ユーザーの決定（WS318 Q318-3 の A）: 2 段で更新する。段 1 は版を変えずに H.265 decode を選び直し、段 2 は KHR の encode のある 1.3.274 以降の固定の版で作り直す（WS318 design §10.2 の p011a・p011b、`maintain-api.noct`）。WS324 はこれに従う。
- 段 1（p002）: `tools/maintain-video.noct` の選びに `VK_KHR_video_decode_h265` を足し（照合の block・command の数を直す）、`vk_video/vulkan_video_codec_h265std.h`・`_h265std_decode.h` を Debian の package から変えずに写す。`vulkan_core.h` は変えない（H.265 decode の sType は 1.3.269 に既にある）。`API-PROVENANCE.md` に SHA-256 を足す。
- 段 2（p003）: `vulkan_core.h` を固定の版 **1.4.309**（Debian `libvulkan-dev 1.4.309.0-1`、SHA-256 `39f358bb…`、`vulkan_video.h` の入力として既に provenance にある。1.3.269 の pin の入力は手元に無いと provenance に書かれている）で `maintain-api.noct` から作り直し、encode の宣言（`VK_KHR_video_encode_queue`・`_encode_h264`・`_encode_h265`）と encode の std（`_h264std_encode.h`・`_h265std_encode.h`）を `maintain-video.noct` で選ぶ。BETA の囲みは無くなる（encode が正式になった版）。dispatch の command の数は video の 3 つ（`vkGetPhysicalDeviceVideoEncodeQualityLevelPropertiesKHR`・`vkGetEncodedVideoSessionParametersKHR`・`vkCmdEncodeVideoKHR`）で 192 → 195。
  - 作り直しの前に、1.3.269 と 1.4.309 の `vulkan_core.h` の選んだ宣言の差（enum の新しい値、構造の新しい field の有無、1.0 の command の形）を一覧にし、ABI の照合（今の手順）を流す。encode を使う code は今の tree に無いので、EXT → KHR の名前の変化の使い手は無い（build で確かめる）。1.3.269 の `VK_ENABLE_BETA_EXTENSIONS` の囲みの中の enumerator 65 個（名前に VIDEO_ENCODE を含む 51 個と `VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR` ほか）は、`_EXT` を `_KHR` に読み替えると 1.4.309 の値と全部一致することを 2026-11-01 に host で照合した（食い違い 0）。
  - 選ばなかった案: 第 1 版の「`vulkan_core.h` を変えず、BETA の値に `vulkan_video.h` で KHR の名前を static const で与える」。BETA を定義した翻訳単位で再定義の error になり（23 個は既に同じ KHR の名前の enumerator）、static const は C の整数定数式でないので `case` の label に使えず（libvulkan は sType を `case` で分ける、`video.c:754-765`）、Khronos に無い宣言を zedBSD が作ることになり provenance の規則の外になる（review 1 の 1 番）。ユーザーの決定（作り直し）にも合わない（review 1 の 2 番）。
- どちらの段も target の build は共有の sysroot（`build/amd64/sysroot` ほか）の header を読むので、sysroot の作り直しは Q1（toolchain・共有の `build/` の規則）。担当は自分の worktree の `build/` の sysroot で build を確かめ、作り直しを Q1 に頼む。
- 受け入れ: 全 arch の libvulkan・libmedia・GPU の driver の build が warning 0（target の C は C99/GNU89 で line comment を許す今の条件のまま）、`maintain-dispatch.noct` の command の数の照合、encode の sType を `case` の label に使う host の compile の試験、ABI の照合。

## 12. 危険

| ID | 危険 | 見込み・対処 |
| --- | --- | --- |
| R1 | NVENC の構造の版（§3.3）。Turing の firmware が 2021 版・2024 版のどちらの配置を読むか不明 | 版でずれる範囲を 0 にして使わない。版の決めが要る時（BadMagic・誤り、p013 の rate control）は boot の引数（`nvrtx.nvenc.layout=2021|2024`、p009b の自己診断の時だけ）で切り替えて build のやり直しを避ける。2021 版を先に。両方駄目なら p009b で止まって Q1（Q324-4 の上限） |
| R2 | me・md・q の制御の既定が無い。値の組み合わせで firmware が止まる・bitstream が壊れる | p009b で注記に沿った最小の設定から 1 項目ずつ。CQP の I 絵 → P 絵の順。status の誤りの語と host の ffmpeg の decode の誤りで判定。失敗の多くは timeout で、engine の作り直しが無いので 1 回ごとに実機の再起動が要る（費用は §15） |
| R3 | H.265 decode で GSP・NVDEC が止まる（NVK の報道） | §9 の隔離。session の buffer は式より大きく取って始める（2 倍）、止まる絵を最小の stream に絞る（適合の stream の 1 本ずつ） |
| R4 | HEVC の filter の buffer・scaling list・tile の区画の中の並び、`HEVC_SET_SAO_BUFFER_OFFSET` と offset の field の関係、`sw_hdr_skip_length` の意味（文書は大きさと注記だけ） | scaling list の無い・tile の無い stream を先に。適合の stream（SLIST_A〜D、TILES_A・B、SAO_A〜H、DBLK_A〜G）で 1 つずつ確かめる |
| R5 | NVENC の ucode が GSP-RM に無い（別の file が要る） | p009a で分かる。要れば Q1（firmware の package の判断） |
| R6 | 1 つの family に decode と encode を置けない（仕様） | refpage に禁止は無い（低い）。駄目なら family 1 を encode に（executor の queue の作り直し、+8 LW） |
| R7 | 同期の engine の待ち（node の lock の下）で compositor が待つ（WS243 R9 と同じ） | 1080p の 1 絵は数 ms の見込み。p014 で compositor の frame の時間を測る。駄目なら非同期（GR が semaphore を待つ形）を後の Phase に |
| R8 | 出力の bitstream が VRAM で、CPU の読みが遅い（約 12 MB/s） | 1080p60 の 20 Mbit/s で約 2.5 MB/s なので足りる。4K の高い bitrate は記録だけ |
| R9 | `block_height` の単位・入力の BL の版（`input_bl_mode` の 2 版で bit 幅が違う、1 → 2 bit。値 0 は同じ） | 値 0（GPU の BL）だけを使う。block_height は 1（GOB 2 段）から試し、ずれた絵で単位を決める（p011、block linear の入力が出来た後） |
| R10 | encode の能力の報告（最小の大きさ・bitrate の上限）が実と違う | p011・p012 で測って直す |
| R11 | libmedia の H.265 の解析（Q324-1 が A の時）を WS318 が別の形で作り直す | WS318 p016 の設計を WS324 の中で WS318 の形のまま行うので、WS318 は p016〜p019 を「済」として受ける（Q1 が WS318 に記録） |
| R12 | NVDEC の HEVC が CTB の端（64 の倍数）まで書く、NVENC が CTB 32 で再構成を書く | 行の揃え 64・DPB の 32 の揃え（§3.4）、後ろの印の pattern の試験 |
| R13 | history の大きさが足りず engine が確保の外に書く | 224 byte/列で確保、後ろの印の pattern（§8.4） |

## 13. 失敗と回復

- API の規則違反（範囲の外の slot、coding scope の外、bind されていない session の memory）は記録を失敗にする（WS243 と同じ）。rate control の状態の食い違いは実行の時に見て encode を ERROR にする（§8.6）。
- bitstream・値が hardware に合わない（未対応の profile、tile が多すぎる、field、Main10、RefDiffPicOrderCnts の範囲の外）は絵を飛ばし、status の query を ERROR にする（decode）。encode で直せない値は session parameters の作成で `VK_ERROR_INVALID_VIDEO_STD_PARAMETERS_KHR`（std の版の不一致は `VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR`）、直せる値は作成の時に直して feedback。
- engine の timeout: その engine を boot まで使わなくし（以後の decode・encode は即 EIO → `VK_ERROR_DEVICE_LOST`）、§9 の隔離、段の log に fence と semaphore の値と GSP の RC の通知を出す。channel の作り直しは WS250 の回復にも無く、この WS でも作らない（§16）。
- status の error: NVDEC は `mbs_in_error`（HEVC では CTB の単位）・`error_status`・`slice_header_error_code`（`nvdec_status_s`）のどれかが 0 でなければ絵を ERROR に。NVENC は `error_status`・`ucode_error_status`。encode の ERROR の絵の bitstream の bytes は 0 を返す。

## 14. 試験の計画

### 14.1 host（各 Phase、gcc で driver の file をそのまま compile、`plan/ws324/tests/`）

- T1 HEVC の picture: 既知の VPS・SPS・PPS・picture info から `nvdec_hevc_pic_s` を組み、byte・bit の位置（open-gpu-doc の header を host の temp で compile して測った表、表は試験に値として持つ）に期待の値。tile の大きさの区画、scaling list の区画、初期の参照の list（規格の例を手で計算した値）、slot と picidx の表。
- T2 HEVC の作業の buffer と run の大きさの式（720p・1080p・4K、最悪の tile の数）。
- T3 NVENC の RM の params（BSP と同じ形の MSENC、promote、devinfo の pick の 0x25）と chid・handle の重なり。
- T4 NVENC の 1 回の push（method の並び・値・address の >> 8）、HEVC の decode の push（0x580〜0x594、MBHIST 無し）。
- T5 `nvenc_h264_drv_pic_setup_s`・`nvenc_h265_drv_pic_setup_s` の組み立て。2 版の全 field の bit 位置の差の一覧を値として持ち、使う field が差の外にあり、差の中が 0 であること。
- T6 parameter set の書き出し（`video-ps.c`）: 書いた SPS・PPS・VPS を host の ffmpeg の trace_headers で読み、値が一致。emulation prevention の境界。
- T7 wire の往復（§10）。H.264 decode の今の往復と `CREATE_QUERY_POOL` の形が変わらない。
- T8（Q324-1 が A の時）libmedia の H.265 の解析: 標準廉価実装手順の形（突然変異の確かめ、適合の stream の SPS・PPS・slice header の値を host の ffmpeg の trace_headers と比べる）。
- T9 header: encode の sType を `case` の label に使う C の compile、dispatch の数、ABI の照合（p003）。
- build: amd64 の `vmunix`（warning 0）、libvulkan（amd64・arm64・riscv64）、venus-reg。

### 14.2 QEMU

- NVDEC・NVENC は QEMU に無い。QEMU（Venus、amd64）では非退行だけ: video の capset の無い device で libvulkan が H.265・encode の拡張を出さず、Video Player が今どおり（`plan/tools/rtx-video/venus-videoplayer.sh`）。

### 14.3 RTX 2070 の実機（10.0.10.3。Q1 が割り当てた時だけ、1 本の script: 配置 → 起動 → 実行 → 回収 → 要約）

- 道具: `plan/ws324/tests/rtx-video324.sh`（`plan/tools/rtx-video/rtx-p004.sh` の形を写して直す）。image は `plan/ws324/tests/config-*.mk` で build し、`nvrtx.video=1`。kernel の log は SSH で取る（`rtx-notes-check.sh` と同じ）。
- H.265 decode: `vkvideo-probe` の H.265 で適合の stream（HEVC_v1 の Main の progressive・8 bit・4:2:0 の物、まず 10 本、後に全部）の各 frame の hash が host の ffmpeg の decode（`nv12-hashes.py` の形）と一致。host の ffmpeg（libx265）で作った 1080p30・1080p60・4K30 の clip の hash も一致、1 絵の decode の時間（`--time`）。plane の後ろの印の pattern が壊れない。
- encode: `vkencode-probe`（新）が決まった模様の絵の列（と、decode した clip の絵）を encode し、Annex B の file（SPS・PPS は `vkGetEncodedVideoSessionParametersKHR`）を書く。host で `ffmpeg -xerror` が誤り無く decode し、元の絵との PSNR が目安以上（CQP 26 で 35 dB 以上、p011 で測って直す）、`ffprobe` の profile・level・大きさ・frame の数が設定どおり。zedBSD の NVDEC（`vkvideo-probe`）で同じ stream を decode した hash が host の ffmpeg と一致（H.265 は p007 の後）。1 絵の encode の時間。
- 非退行: H.264 decode の 4 本の hash（WS243 の `rtx-p004.sh … video`、行の揃えの変更の後も）、hang の確かめ（`plan/ws250/tests/rtx-hang-check.sh`）、desktop の起動（`boot-test.sh` の画面）。
- WS318 §13.6 の項目（vkfake で確かめた session の作成・parameter・GOP・feedback・H.265 の Std の値、COINCIDE の decode の image の別名の約束）を、WS318 の該当の Phase が出来た後に p016 で実の device で確かめる（Q324-6 が A の時）。
- 失敗の道: 壊した stream（途中を切る、SPS の値を壊す）で ERROR が返り、次の正しい stream が decode できる。timeout の後の隔離（engine を止める注入の要否は p007 で決める）。

## 15. Phase の分け方と受け入れ（案、LW は見積り）

型: O = Opus（kernel・driver は phase-runner-high）、S = Sonnet 5.5 high（標準廉価実装手順、1 Phase = 1 起動）。

| Phase | 目的 | 型 | 依存 | 受け入れ | LW |
| --- | --- | --- | --- | --- | --- |
| p001 | 調べと設計（この文書）、review | O | — | design-reviewer の反映、問いを Q1 へ | — |
| p002 | Vulkan の header の段 1（H.265 decode の選び、std の写し、provenance） | O ＋ Q1 | Q324-2 | §11 の受け入れ、sysroot の作り直しは Q1 | 3 |
| p003 | Vulkan の header の段 2（`vulkan_core.h` を 1.4.309 で作り直し、KHR の encode の選び） | O ＋ Q1 | p002、Q324-2 | §11 の受け入れ（T9、差の一覧、ABI の照合） | 8 |
| p004 | libvulkan の H.265 decode の転送（構造・codec の操作・拡張の表・capset の bit 2、門を bit ごとに） | O | p002、Q324-3 | T7（libvulkan の側）、amd64・arm64・riscv64 の libvulkan の build、H.264 の今の往復が変わらない | 14 |
| p005 | nvrtx の H.265 decode（HEVC の picture・push・作業の buffer・tile・scaling list・slot と picidx・参照の list、run の配置の書き直し、行の揃え 64、`vk-video-h265.c`、報告、capset の bit 2、隔離の約束） | O（high） | p004 | T1・T2・T4・T7（nvrtx の reader）、amd64 の vmunix の warning 0。H.265 の無い時の報告は今と同じ | 20 |
| p006a | H.265 の解析の設計（標準廉価: module・header の骨格・仕様の置き場と SHA・適合の取得の script・phase.md） | O | p002、Q324-1 | design-reviewer、骨格の build、取得の script | 4 |
| p006b〜d | H.265 の解析（b: NAL・VPS・SPS、c: PPS・slice segment header、d: POC・RPS・DPB の marking と出力の順）。各 1 起動 | S | p006a | T8、run-unit、適合の stream の値 | 6 × 3 |
| p006e | `vkvideo-probe` の H.265（p006b〜d の module を使う） | O | p006d | probe の build、host で適合の stream を解析して誤り 0 | 4 |
| p007 | H.265 decode の実機の debug（適合の stream、clip、時間、失敗の道、印の pattern） | O（high） | p005、p006e、実機 | §14.3 の H.265 decode の項目 | 12 |
| p008a | libvulkan の encode の転送（queue と H.264: 構造、命令の番号、video の query pool、quality level、取り出し、capset の bit 3、bitstream の形式の照合） | O | p003、Q324-3 | T7（libvulkan の側）、build、i915・bcm2711 の非退行 | 14 |
| p008b | libvulkan の encode の転送（H.265、capset の bit 4） | O | p008a | 同上 | 10 |
| p009a | NVENC の channel（段 `nvenc-channel`、devinfo・promote・object・NOP と semaphore、GSP の RC の通知の記録） | O（high） | p001、実機 | 実機で channel が出来て最初の semaphore が返る、R5 の答え。host の T3 | 8 |
| p009b | **NVENC の関門**: `nvenc-records.c` の H.264 の最小の設定。driver の自己診断（`CONFIG_DRIVER_NVRTX_NVENC_PROBE`、既定 n）が boot の後に 1 回、VRAM に CPU で書いた 16x16 の tile の模様の絵（256×128、入力として許される形）を CQP（QP 26）の I 絵と P 絵で encode し、bitstream を小さな VRAM の buffer に残す。状態の値と bitstream（最大 8 KiB、1 行 32 byte、行ごとに offset と checksum、全体の SHA-256）を stage の log に出し、SSH で取る。host の script（試験だけの物）が同じ値の SPS・PPS を前に付け、ffmpeg で decode して元の模様と比べる。版・設定は boot の引数で選ぶ | O（high） | p009a | status の誤り 0、host の ffmpeg が I 絵と P 絵を誤り無く decode し、模様との PSNR が 35 dB 以上、R1・R2 の答えの記録。**時間の上限（Q324-4）を p009a の後から数え、越えたら止まって Q1** | 20 |
| p010 | encode の共通の executor（NV12 の buffer → image の copy、16x16 の tile の DPB、session と memory、video の query pool と feedback、quality level、`video-ps.c` の H.264、`vkGetEncodedVideoSessionParametersKHR`、rate control の状態を実行の時に） | O（high） | p008a、p009b | T6・T7（nvrtx の reader）、build | 16 |
| p011 | H.264 encode（`vk-encode-h264.c`、IDR と P、CQP・DEFAULT、`vkencode-probe`、block linear の入力、実機の debug） | O（high） | p010 | §14.3 の H.264 encode の項目（PSNR、ffprobe、NVDEC の decode の hash）、R9・R10 の答え | 18 |
| p012 | H.265 encode（`vk-encode-h265.c`、`video-ps.c` の H.265、実機） | O（high） | p011、p008b、p007 | §14.3 の H.265 encode の項目 | 18 |
| p013 | rate control（CBR・VBR、持続の状態、rc の byte 185〜188 の版、HRD を書くか） | O | p011・p012 | 実機で 10 s の平均の bitrate が設定の ±10% 以内、PSNR | 10 |
| p014 | 実機の受け入れ（全項目の 1 本の script、非退行、compositor の frame の時間） | O | p007・p011・p012・p013 | §14.3 の全項目の PASS | 6 |
| p015 | 規約の全文の見直し | conformance-reviewer | p014 | coding-style の全文で見直し、動作は不変 | 4 |
| p016 | （Q324-6 が A の時）libmedia の道の実機の確かめ: WS318 p020・p022 の後に Video Player の H.265 の再生と libmedia の encoder を RTX で、WS318 §13.6 の項目 | O | WS318 p020・p022、p014 | Video Player の H.265 の 1080p の再生（FRAMES・PNG）、libmedia の encode の往復 | 6 |

- 合計 約 213 LW（A の時。8 LW ≈ 2〜3 h なので 約 53〜80 h）。encode（p003・p008a〜p013、約 122 LW）の幅が最も大きく、p009b の関門の結果で ±50%。p009b の 1 回の試行は、失敗すると timeout の後に engine が使えなくなるので実機の再起動が要る（SwitchBot の入れ直しは Q1）。boot の引数で版・設定を選べるので build のやり直しは少ない。
- 先に進められる物: p002・p003（Q1 の許可の後）、p009a（実機の割り当ての後、他と独立）。p004・p005・p006a は p002 の後に並べられる（file が重ならない: libvulkan、nvrtx、libmedia・probe）。
- 標準廉価実装手順に合うのは H.265 の解析（p006b〜d、規格で振る舞いが決まる）。parameter set の書き出し（`video-ps.c`）も規格で決まるが量が小さく p010・p012 の中に置く。NVDEC・NVENC は hardware を探りながら作るので普通の Phase。
- 実装の前倒しは 2026-11-01 のユーザーの指示（RTX の H.264 encode・H.265 decode・H.265 encode の実装に移る）で明示されている。

## 16. 制限（今の設計で残す物）

- engine の作り直し（timeout の後）は無い。隔離した memory は boot の間返らない。
- encode は 1 slice、参照 1 枚、B 無し、temporal MVP 無し（H.265）、rate control は CQP（p013 の後 CBR・VBR）。lookahead・AQ・2 pass・intra refresh は無い。
- decode・encode とも同期（1 絵ずつ GR を流して待つ）。
- 4K を越える大きさ、Main10・4:4:4 は報告しない。
- RTX 30・40 は class の番号（`0xC7B7`・`0xC9B7`、magic は class から）だけを用意し、実機の確かめは無い。

## 17. 判断（根拠と、選ばなかった案）

| # | 決定 | 根拠 | 選ばなかった案と理由 |
| --- | --- | --- | --- |
| D1 | nvrtx の device の family 0 に H.265 decode と encode を足す | ユーザーの決定（Vulkan Video が OS の API）。WS243 の NVDEC の道と executor の memory・image・fence をそのまま使える | 別の VkPhysicalDevice・別の cdev（ユーザーの決定に反する） |
| D2 | NVENC は open-gpu-doc の公開の interface を自分で駆動する | NVIDIA が MIT で公開した driver 向けの文書で、正当な出典。他に道が無い | proprietary の driver の観測（D11）、encode を software だけにする（ユーザーの指示に反する。駄目な時の退路として Q324-4 の C） |
| D3 | encode も family 0 に（同じ queue） | libmedia（WS318）は queue の lock を役ごとに持つ設計で、family が 1 つの方が app の device の作成が簡単。executor の queue は 1 本。refpage に禁止は無い | family 1 に分ける（executor の queue の作り直し。仕様が許さない時だけ、R6） |
| D4 | encode の DPB は 16x16 の tile の別の配置、入力は block linear | NVENC の制約（参照は 16x16 の tile が必須）。入力を block linear にすると decode・描画の結果を写さずに encode できる | 入力も 16x16 の tile（decode・描画との共有ができず copy が要る。p009b の自己診断だけは CPU で書ける 16x16 の tile を使う） |
| D5 | decode・encode とも同期で始める | WS243 で正しさが分かりやすかった。1080p60 は WS243 で足りた | 最初から非同期（正しさの確かめの前に fence の形を変えない） |
| D6 | NVENC の channel は `nvdec.c` と別の file（似た code を共有しない） | master-design-policy §2.6（遅い抽象化）。2 つで形が同じと分かったので、3 つ目（OFA など）で共通化を考える | 今 `video-channel.c` に共通化（完了した WS243 の code の再試験が要る） |
| D7 | parameter set の書き出しは nvrtx の `vk/video-ps.c` | 同上。WS322 の `vpu-*-ps.c` と同じ物を共有する抽象は早い | `src/drivers/video/` の共通の module（3 つの driver の WS の調整が要る） |
| D8 | libmedia の runtime は変えない（試験の道具で確かめる）。H.265 の解析は WS318 の範囲を先に作る案を推す（Q324-1） | ユーザーの指示（WS318 を始めずに済む最小）。解析は道具にも WS318 にも要るので、一度だけ書く | 道具の中に試験だけの解析（重複、約 2500 行）、WS318 を待つ（指示に反する） |
| D9 | Vulkan の header はユーザーの決定どおり 2 段（段 2 で `vulkan_core.h` を 1.4.309 で作り直す） | ユーザーの決定（Q318-3 の A）。1.4.309 の入力は既に pin されている | 第 1 版の「`vulkan_core.h` を変えず BETA の値に static const の別名」（C として成り立たず、provenance の外、review 1 の 1・2 番） |
| D10 | NVENC の構造の版でずれる範囲を 0 にして使わない | どちらの版でも同じ意味になる（R1） | 2024 版をそのまま（Turing の firmware が古い配置なら gptimer の位置がずれる） |
| D11 | proprietary の driver の振る舞いを観測しない | EULA の逆解析の禁止に当たる恐れ。出典が公開の MIT の文書で足りるうちは要らない | Debian の NVIDIA の driver で ffmpeg の hevc_nvenc を動かし push を観測（license の危険） |
| D12 | 能力の報告は確かめた範囲（4096、参照 1、slice 1） | 実機の確かめの前に大きく出さない（WS243 D3 と同じ考え） | hardware の公表の全部（8K・B・4:4:4） |
| D13 | timeout の後は関わる object を隔離して boot の間返さない | engine の作り直しが無く、止まった engine の書き込みが再利用された VRAM を壊すのを防ぐ最小の形 | 今のまま（`ready = 0` だけ。WS243 R7 が残る）、engine の reset と作り直し（GSP-RM の回復の手順が要り、WS250 の範囲を越える） |
| D14 | rate control の DEFAULT は CQP 26 | Vulkan の reset の後の mode。DISABLED でない時の constantQp は 0 なので、実装の定義が要る | DEFAULT を VBR（rate control の持続の状態の版 R1 が決まる前に使えない） |

## 18. 問い（ユーザー・Q1 へ）

phase.md の「判断を待つ点」に背景・用語・選択肢・得失・推しを書いた。要約:

| ID | 問い | 推し | 誰が |
| --- | --- | --- | --- |
| Q324-1 | H.265 の解析（StdVideoH265 を作る物）をどこに作るか | A: WS318 p016〜p019 の範囲を、標準廉価実装手順（設計 p006a と実装 3 本）で libmedia の独立の module として WS324 で先に作る（libmedia の build・runtime には繋がない）。`vkvideo-probe` が直に compile する。WS318 は後でそれを使う | Q1（WS 間の範囲の移し。WS318 の記録の直しも） |
| Q324-2 | Vulkan の header の選び直し・作り直しと sysroot の作り直しを許可するか（toolchain） | A: ユーザーの決定どおり 2 段。段 2 の固定の版は 1.4.309（既に pin 済みの入力） | Q1（toolchain の許可） |
| Q324-3 | libvulkan の H.265 decode・encode の転送と capset の bit・命令の番号を WS324 が持つか（Q322-4、WS318 §10.3） | A: WS324 p004・p008 が持ち、bit 2 = H.265 decode、bit 3 = H.264 encode、bit 4 = H.265 encode。WS322 p005・p010 の libvulkan の部分は取り下げ、WS318・WS319 の記録も直す | Q1 |
| Q324-4 | NVENC は公開の実装の前例が無い。関門の Phase（p009b）に時間の上限を置き、越えたら encode を止めて software（WS329・WS330）に任せるか | A: 上限 20 LW（約 5〜8 h、p009a の後から数える）。越えたら Q1 に返し、ユーザーが続けるか決める | ユーザー（費用と危険の受け入れ） |
| Q324-5 | encode を decode と同じ queue family 0 に置くか | A: 同じ family（refpage に禁止は無い。他の規則で駄目なら family 1） | Q1（技術） |
| Q324-6 | WS324 の完了の条件に libmedia の道（Video Player の H.265 の再生と libmedia の encoder の RTX での確かめ）を残すか | A: 残す（p016 を WS318 p020・p022 の後に置き、WS324 はそれまで incomplete）。ws.md の目標「libmedia から使えるようにする」を保つ | ユーザー（範囲） |

## 19. HAL

- 変えない。要る物は全部 nvrtx の driver と kernel の今の口で足りる: register の読み書き（nvrtx の `nvrtx_wr32` ほか、今の BAR の map）、VRAM・system memory の確保と GPU の page table（nvrtx の allocator・`nvrtx_work_map_*`）、GSP-RM の RPC（今の `rpc.c`）、待ちと時刻（`kern/clock.h`）、lock（`kern` の mutex）。NVENC は GSP-RM の向こうの engine で、HAL の責務（platform・arch の抽象）に関わる物が無い。
- 新しい code は HAL の header を include しない（guardrail「driver は HAL を知らない」）。

## 20. 既存の file への変更（所有の重なり）

| file | 変更 | 注意 |
| --- | --- | --- |
| `src/drivers/gpu/nvrtx/nvdec-records.c`・`nvrtx-nvdec.h`・`nvdec.c` | HEVC の picture・push・buffer の大きさ・tile・scaling list、隔離 | WS243 の完了した code。host 試験 `nvdec-host-test` を拡げる |
| `src/drivers/gpu/nvrtx/objects.c`・`nvrtx-gsp.h`・`rpc.c` | devinfo の pick に nvenc の欄、RC の通知の記録 | P2（WS291）が触った所。WS291 はラップアップ中 |
| `src/drivers/gpu/nvrtx/gsp.c`・`stage.c`・`nvrtx-gpu.h` | 段 `nvenc-channel`、state の欄、boot の引数 | 同上 |
| `src/drivers/gpu/nvrtx/vk/vk-video.c`・`vk-device.c`・`vk-resource.c`・`vk-memory.c`・`vk-record.c`・`vk-execute.c`・`exec-copy.c`・`nvrtx-vk-objects.h`・`nvrtx-vk.h`・`nvrtx-video.h`、`vk-node.c`、`runtime.c` | §8 の hook、run の配置、行の揃え | WS250（回復）・WS291（速さ）の後の main を基点に。`plan/tools/rtx-video/plane-header-host-test` を追従 |
| `userland/desktop/libvulkan/`（video.c・instance.c・device.c・context.c・query.c・external-properties.c・internal.h・tools/maintain-video.noct・tools/maintain-api.noct の入力・api-commands.tsv・dispatch-table.inc など） | §10・§11 | WS322・WS319 と調整（Q324-3） |
| `include/libc/vulkan/vulkan_core.h`・`vulkan_video.h`・`vk_video/`・`API-PROVENANCE.md` | §11 | toolchain の扱い（Q1） |
| `include/uapi/gpu-op.h` | encode の番号 4 つと範囲の約束 | UAPI。全部の executor の番号の台帳 |
| `userland/tests/vkvideo-probe/`、新しい `userland/tests/vkencode-probe/` | H.265、encode | WS083 の道具 |
| `userland/desktop/libmedia/`（Q324-1 が A の時、新しい `h265*.c` だけ。`sources.mk` には入れない（libmedia の build に入れるのは WS318 p020）） | H.265 の解析 | WS318 の範囲の前借り。probe が libmedia の内部の file（`vcodec-bitreader.c` ほか）を compile する形になるので Q1 が両方の WS に記録 |
| `platform/amd64/vmunix.mk`、`config/drivers/` | 新しい file、`CONFIG_DRIVER_NVRTX_NVENC_PROBE`（既定 n） | — |

新しい file: `src/drivers/gpu/nvrtx/nvenc.c`・`nvenc-records.c`・`nvrtx-nvenc.h`、`vk/vk-video-h265.c`・`vk/vk-encode.c`・`vk/vk-encode-h264.c`・`vk/vk-encode-h265.c`・`vk/video-ps.c`・`vk/nvrtx-encode.h`、試験は `plan/ws324/tests/`。

## 21. review

- review 1（design-reviewer、2026-11-01、31 件: blocker 1・major 14・minor 16）の指摘と反映は [design-review.md](design-review.md)。第 2 版はその反映の後の版。
