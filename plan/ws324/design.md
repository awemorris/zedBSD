<!-- awesome-plan project=zedbsd record=ws324-design -->

# WS324 設計: RTX 2070（nvrtx、TU106）の NVDEC（H.265 decode）と NVENC（H.264・H.265 encode）を nvrtx の Vulkan の executor の Vulkan Video として

Parent: [WS324](ws.md)。Phase: [ws324-p001](phase001/phase.md)（この文書を書いた Phase）。
版: 第 1 版（2026-11-01、P1、q1854）。実機・QEMU の観測は無い。値と手順は全て source と公開の文書を読んだだけの物（§5 の出典）。main `b43b0c1f03`（worktree の基点。`efd4943052` との差は plan だけ）の code を読んだ。

## 0. 要約

- 見せ方（ユーザーの決定、Vulkan Video が OS の API）: nvrtx の executor（`src/drivers/gpu/nvrtx/vk/`）の queue family 0 に、今の `VK_QUEUE_VIDEO_DECODE_BIT_KHR`（H.264、WS243）へ H.265 decode と `VK_QUEUE_VIDEO_ENCODE_BIT_KHR`（H.264・H.265）を足す。新しい cdev・UAPI の ioctl は作らない。足すのは GPU の命令の番号（`include/uapi/gpu-op.h` の video の続き）と capset の bit だけ（§10、[proposed/](proposed/)）。
- **H.265 decode** は WS243 の NVDEC の channel をそのまま使う。NVDEC は stateless で、HEVC の picture の構造（`nvdec_hevc_pic_s`）と作業の buffer の大きさの式は NVIDIA の open-gpu-doc（MIT）に公開されている。WS243 の H.264 と同じ形（同期の decode、COINCIDE、block linear の NV12）で足りる見込み。範囲は Main（8 bit、4:2:0）だけ（WS318 Q318-8 の Q1 の判断）。
- **NVENC** は open-gpu-doc に class の method（`clc5b7.h`・`clc7b7.h`・`clc9b7.h`）と driver との interface（`nvenc_drv.h`、3830 行、H.264・H.265 の picture の設定・rate control・状態の構造）が公開されている。TU106 の class は `0xC4B7`（open-gpu-kernel-modules 570.144 の class の表）。**ただし、その interface を自分で駆動する公開の driver は見つからない**（nouveau・NVK は decode だけ、NVK の encode は無い）。zedBSD が最初の公開の実装になる。最大の危険は (1) Turing の firmware が読む構造の版（公開の 2 版で末尾の配置が 4 byte ずれる、§3.3）、(2) 動きの探索・mode の判定・量子化の制御の構造（`me_control` 192 byte ほか）の既定の値が文書に無いこと。→ 実装の最初に**関門の Phase**（最小の I 絵の CQP の encode が host の ffmpeg で decode できるか、時間の上限つき）を置く（§15、Q324-4）。
- encode の範囲: IDR と P だけ（B 無し、WS318 Q318-9）、参照 1 枚、1 絵 1 slice、rate control は DISABLED（CQP）から始め、CBR・VBR は後の Phase。入力は app の NV12（block linear）の image、参照・再構成は driver が 16x16 の tile の形で持つ DPB の image（NVENC の制約、§3.4）。
- **libvulkan**: H.265 decode と encode の転送（wire）、拡張の表、capset の bit。WS322 の design（Q322-4）は「先に着手する方が持つ」なので WS324 が持つ案（Q324-3）。
- **Vulkan の header**: `vulkan_video.h` は既に Debian の Vulkan-Headers 1.4.309 の入力から選んでいる（`API-PROVENANCE.md`）。H.265 decode も KHR の encode も同じ入力にある。WS318 Q318-3 のユーザーの答え（2 段）に従い、1 段目で H.265 decode を選び（`vulkan_core.h` の 1.3.269 は変えない）、2 段目で KHR の encode を選ぶ。2 段目は `vulkan_core.h` の BETA の囲みの中の encode の値に KHR の名前を `vulkan_video.h` で与える案（1.3.269 の BETA の 51 個の値は 1.4.309 の KHR と全部同じと host で照合した）。sysroot の作り直しは toolchain の扱いで **Q1 の許可が要る**（Q324-2）。
- **libmedia**: WS318 を始めずに済む最小は「libmedia を変えない」。driver の確かめは Vulkan を直に使う試験の道具（`vkvideo-probe` の H.265、新しい `vkencode-probe`）で行う。H.265 の解析（VPS・SPS・PPS・slice header・POC・RPS → StdVideoH265）は道具に要るので、WS318 p016〜p019 の範囲を libmedia の独立の module として先に作り、道具がそれを直に compile する案を推す（runtime には繋がない。Q324-1）。Video Player の H.265 の再生と libmedia の encoder は WS318（p020・p021・p022）のまま。
- HAL: 変えない（§19）。

## 1. 目的・範囲・範囲の外

- 目的（ws.md）: RTX 2070 の video の hardware のうち zedBSD で未実装の H.265 decode、H.264 encode、H.265 encode を Vulkan Video として出し、libmedia から使えるようにする（libmedia の側の口は WS318）。
- 範囲:
  - nvrtx: NVDEC の HEVC、NVENC の channel と H.264・H.265 の encode、executor の Vulkan Video（session・parameters・記録・実行・query）、capset、family の flag、NV12 の encode の入力と DPB の image。
  - libvulkan: H.265 decode と encode の転送、拡張の表、codec の操作の許可。
  - Vulkan の header の選び直し（Q1 の許可の後）。
  - 試験の道具: `vkvideo-probe` の H.265、`vkencode-probe`（新）、host の試験。
  - 実機（10.0.10.3 の RTX 2070）の確かめ。
- 範囲の外: H.265 Main10・Main12・4:4:4（hardware にはあるが libmedia が出さない、Q318-8）、H.264 の 10 bit（hardware に無い）、B-frame（Q318-9）、AV1（Turing に無い）、多 slice の encode・lookahead・2 pass・AQ・intra refresh、libmedia の encoder の口と Video Player の H.265（WS318）、RTX 30/40（WS242 と同じく class の番号だけを用意し、実機の確かめは無い）。

## 2. 今の姿（main で読んだ事実）

| 物 | 事実 | 場所 |
| --- | --- | --- |
| NVDEC の channel | 段 `nvdec-channel`（boot の `nvrtx.video=1` の時だけ）。work channel の address の空間、chid 48（`NVRTX_WORK_SESSION_CHID_FIRST` 16 + 32 本）、handle `0xf1f00030`・`0xc4b00030`。1 絵ずつ push を出し fence を待つ（`nvrtx_nvdec_run`）。timeout で decoder を使わなくする | `nvdec.c:1-80,379-440`、`gsp.c:361-376`、`stage.c:121` |
| NVDEC の class と engine | TU10x は `0xC4B0`。NV2080 の engine type NVDEC0 = 0x13、devinfo（RM の内部の番号）は 0x1d | `nvrtx-nvdec.h`、`objects.c:87-96,436-480` |
| H.264 の picture の組み立て | `nvrtx_nvdec_h264_pic()` が 764 byte の構造を byte・bit の位置で書く（host 試験 `plan/tools/rtx-video/nvdec-host-test.sh`） | `nvdec-records.c`、`nvrtx-nvdec.h` |
| executor の video | `vk/vk-video.c`（2952 行）: capabilities・形式・family の問い・session・parameters・result status の query pool・4 つの記録（begin・end・control・decode）。decode は同期（GR を流して待ち → NVDEC → 待つ）。H.264 の値の変換は `vk/vk-video-h264.c`（host 試験あり） | `vk/vk-video.c:1-35,56-165` |
| family | 1 つ（GRAPHICS・COMPUTE・TRANSFER、video の decoder があれば VIDEO_DECODE）、queueCount 1 | `vk/vk-device.c:607-613` |
| NV12 の image | optimal（block linear、GOB 2 段）だけ。features は DECODE_OUTPUT・DECODE_DPB・TRANSFER_SRC・SAMPLED。buffer → image の copy は無い（image → buffer は copy engine） | `vk/vk-device.c:657-667`、`vk/exec-copy.c`、WS243 design §7 |
| memory type | 1 つ（VRAM、BAR1 越しに host visible の WC）。CPU の読みは約 12 MB/s（WS243 p004） | `vk/vk-device.c:560-564`、WS243 ws.md |
| capset | 176 byte、byte 168 に `0x5a4e4154`、172 に flags（bit 0 = H.264 decode、bit 1 = sync command） | `runtime.c:83-89,445-451`、`libvulkan/context.c:38-41,221-235` |
| libvulkan の video | H.264 decode だけ。family の codec の操作は DECODE_H264 だけを残す。拡張の表は video_queue・decode_queue・decode_h264 | `libvulkan/video.c:1-80`、`instance.c:1392-1410`、`device.c:353-371,433-443` |
| GPU の命令の番号 | video は `0x10000`〜`0x1000d`（protocol version 2、video の capset の時だけ送る）。encode の番号は無い | `include/uapi/gpu-op.h:134-140,1893-2034` |
| Vulkan の header | `vulkan_core.h` は 1.3.269（`VK_HEADER_VERSION 269`）。encode の値（sType・layout・query type・usage・format feature）は `VK_ENABLE_BETA_EXTENSIONS` の囲みの中（H.264・H.265 の encode は `_EXT` の名前）。H.265 decode の sType（1000187000〜5）は囲みの外にある。`vulkan_video.h` は Debian の 1.4.309（`libvulkan-dev 1.4.309.0-1`、SHA-256 `39f358bb…`）から `tools/maintain-video.noct` で video_queue・decode_queue・decode_h264・synchronization2 を選んだ物。`vk_video/` は H.264 の std と decode だけ | `include/libc/vulkan/vulkan_core.h:39,461-543,728-733,1359-1365`、`API-PROVENANCE.md:159-210`、`libvulkan/tools/maintain-video.noct:1-80` |
| host の Vulkan-Headers | `/usr/include/vulkan/vulkan_core.h` の SHA-256 は provenance の値と同じ（1.4.309）。`/usr/include/vk_video/` に h265std・h265std_decode・h264std_encode・h265std_encode がある | host（2026-11-01 に `sha256sum` で照合） |
| libmedia | Vulkan Video の道は H.264 だけ（`vkvideo.c:130` が H.264 以外を断る）。H.264 の Vulkan 用の解析は `h264.c`・`h264-dpb.c`。共通の bit reader・NAL は `vcodec-bitreader.c`・`vcodec-nal.c`。mp4・mkv・avi・ts の demux は HEVC を知る | `userland/desktop/libmedia/`、`mediafile/` |
| 試験の道具 | `vkvideo-probe`（WS083）は自分の H.264 の解析（`h264.c` 1044 行）と DPB（`dpb.c`）を持ち、Vulkan を直に使う。amd64・rpi4 の package | `userland/tests/vkvideo-probe/` |
| H.265 | src/drivers/gpu・libmedia・libvulkan に H.265 の code は無い（grep で 0 件） | — |

## 3. hardware

### 3.1 TU106 の video の engine（公開の資料）

| 物 | 値 | 出典 |
| --- | --- | --- |
| NVDEC | 1 基、4 世代目。H.264 8 bit 4:2:0、HEVC 8・10・12 bit の 4:2:0 と 4:4:4 | NVIDIA Video Encode and Decode GPU Support Matrix（GeForce RTX 2070 の行） |
| NVENC | 1 基、7 世代目、同時 session 12。H.264（4:4:4・lossless を含む）、HEVC（8K、10 bit、B-frame、4:4:4、lossless）、4:2:2 と AV1 は無い | 同上（RTX 2060 / 2070 の行） |
| NVENC の class | TU106 は `NVC4B7_VIDEO_ENCODER`（0xC4B7）を `ENG_NVENC(0)` に持つ。NVDEC は `NVC4B0`（表は NVDEC 0〜2 を並べるが、実の数は devinfo で決まる） | open-gpu-kernel-modules 570.144 `src/nvidia/generated/g_gpu_class_list.c`（TU106 の表） |
| NVENC の engine type | NV2080 の番号: NVENC0 = MSENC = 0x1b。RM の内部の番号（devinfo）: NVENC0 = 0x25 | 570.144 `cl2080_notification.h`、`src/nvidia/inc/kernel/gpu/gpu_engine_type.h` |
| NVENC の object の確保 | `NV_MSENC_ALLOCATION_PARAMETERS`（size、prohibitMultipleInstances、engineInstance の 3 語、12 byte。NVDEC の BSP と同じ形） | 570.144 `nvos.h` |
| 解像度 | NVENC の surface の設定の幅・高さは 0〜4095（`frame_width_minus1` の範囲の注記）。NVDEC の HEVC の幅は 16384 まで（注記） | open-gpu-doc `nvenc_drv.h`・`nvdec_drv.h` |

- 能力の報告は確かめた範囲に絞る: decode H.265 は 4096×4096 まで（H.264 と同じ、8K は後）、encode は 4096×4096 まで。最小の大きさは encode が未確認（関門の Phase で測る、仮に 128×64）。

### 3.2 NVDEC の HEVC（open-gpu-doc `nvdec_drv.h`・`clc5b0.h`）

- method（C5B0 の値、WS243 が使う C4B0 と同じ並び）: APPLICATION_ID の HEVC = 7（HEVC_PARSER 0x0C は使わない）、CONTROL_PARAMS の codec HEVC = 7、`HEVC_SET_SCALING_LIST_OFFSET` 0x580、`_TILE_SIZES_OFFSET` 0x584、`_FILTER_BUFFER_OFFSET` 0x588、`_SAO_BUFFER_OFFSET` 0x58C、`_SLICE_INFO_BUFFER_OFFSET` 0x590、`_SLICE_GROUP_INDEX` 0x594。他（DRV_PIC_SETUP・IN_BUF_BASE・PICTURE_INDEX・SLICE_OFFSETS・COLOC・NVDEC_STATUS・LUMA/CHROMA_OFFSET0〜16・EXECUTE）は H.264 と同じ。
- picture の構造 `nvdec_hevc_pic_s`: 鍵の 12 語、stream_len、gptimer、tileformat・gob_height、`sw_start_code_e`（start code を含む stream）、framestride[2]、colMvBuffersize（256 byte 単位）、SAO・BSD の offset（filter buffer に対する 256 byte 単位）、SPS の値、PPS の値（tile の数を含む）、参照の値（num_ref_frames、longtermflag、initreflistidxl0/l1[16]、RefDiffPicOrderCnts[16]）、IDR・RAP の flag、curr_pic_idx、`sw_hdr_skip_length`、`num_bits_short_term_ref_pics_in_slice`、`num_delta_pocs_of_rps_idx`、拡張の v1〜v3。Vulkan の `StdVideoDecodeH265PictureInfo` の `NumBitsForSTRefPicSetInSlice`・`NumDeltaPocsOfRefRpsIdx`・`RefPicSetStCurrBefore/After/LtCurr` から作れる形（NVIDIA の DXVA と同じ考え）。
- 作業の buffer の大きさの式（`nvdec_drv.h` の GIP_ASIC_* と HEVC_* の macro）: 縦の filter 16 byte/pixel、BSD の制御 4 byte/行、scaling list 1024 byte、縦の SAO 16 byte/pixel、tile の大きさ `(20*22*2*2+16+15) & ~0xF`、deblock の上 `(align(w,16)/16+2)*align(632,128)`、左 `(align(h,16)/16+2)*align(506,128)`、SAO の左 `(align(h,16)/16+2)*align(713,128)`（いずれも 256 byte に揃える）。式を session の memory の binding にする（§8.4）。
- 版: 公開の 2 版（2021 の `97c510e` と 2024 の `9fdf5c40`）で `nvdec_hevc_pic_s` の配置は同じ（予約の語に名前が付いただけ、host で正規化して比べた）。WS243 の H.264 は 2024 版で TU106 で動いた（hash が一致）。
- 危険: Mesa の NVK の H.265 は 2026-11 の公開の版（26.3.0-rc3）に無い。報道では NVK の H.265 の作業が GSP を止めた（wedge）とある（原因は不明）。zedBSD でも GSP・NVDEC の止まりを想定し、WS243 の timeout の道（decoder を使わなくする）を使う（§12 R3）。

### 3.3 NVENC の interface（open-gpu-doc `nvenc_drv.h`・`clc5b7.h`）

- method（C5B7。C7B7・C9B7 も同じ番号で、Ampere 以降は semaphore と predication の method が増える）: APPLICATION_ID 0x200（H264 = 1、H265 = 3）、EXECUTE 0x300、`SET_IN_REF_PIC0〜15_LUMA` 0x400〜、`_CHROMA` 0x440〜、`SET_CONTROL_PARAMS` 0x700（codec、FORCE_OUT_PIC、FORCE_OUT_COL、SLICE_STAT_ON、GPTIMER_ON、MPEC_STAT_ON、SUBFRAME_MODE、RCSTAT、RCMODE 31:24）、PICTURE_INDEX 0x704、`SET_IN_RCDATA` 0x70C、`SET_IN_DRV_PIC_SETUP` 0x710、`SET_OUT_ENC_STATUS` 0x718、`SET_OUT_BITSTREAM` 0x71C、`SET_IOHISTORY` 0x720、`SET_IO_RC_PROCESS` 0x724、`SET_IN_COLOC_DATA`・`SET_OUT_COLOC_DATA` 0x728・0x72C、`SET_OUT_REF_PIC_LUMA` 0x730、`SET_IN_CUR_PIC` 0x734、`SET_OUT_REF_PIC_CHROMA` 0x74C、`SET_IN_CUR_PIC_CHROMA_U` 0x740 ほか。
- 1 絵の設定 `nvenc_h264_drv_pic_setup_s`（512 byte、256 byte に揃える）: magic、参照・入力・再構成の surface の設定（各 32 byte）、SPS（4 byte）・PPS（8 byte）の値、rate control（88 byte）、picture の制御（2024 版で 276 byte）、半分の大きさの再構成の設定、gptimer。picture の制御が slice の制御・動きの探索（me）・mode の判定（md）・量子化（q）の制御の配列の offset と数を持ち、配列は同じ buffer の後ろに置く。H.265 は `nvenc_h265_drv_pic_setup_s`（1024 byte）。
- magic: 上位 16 bit が class、15..8 が版、7..0 が改訂。TU10x（`NV_NVENC_7_2`）は `0xC4B70006`。firmware は上位 24 bit を照合し、違えば「BadMagic」で止まる（文書の注記）。
- **版の食い違い（危険 R1）**: 公開の 2 版で H.264 の `pic_control` が 272 → 276 byte（末尾に `stats_fifo_offset`）、H.265 は 532 → 536 byte。H.264 の 512 byte の中では半分の大きさの再構成の設定が 472 → 476、gptimer が 504 → 508 にずれる（両版を host で compile して `sizeof`・`offsetof` を測った）。magic は両版とも `0xC4B70006`。Turing の firmware がどちらの配置を読むかは文書から決まらない。対処: ずれる末尾（472〜511）を 0 で埋め、半分の大きさの出力と stats fifo を使わず、gptimer は 0（firmware の既定）にすると、どちらの配置でも同じ意味になる（§7.4）。H.265 の 1024 byte は両版で同じ大きさで、ずれは `pic_control` の末尾の 4 byte の後ろの予約の中に収まる（p012 で同じ照合を行う）。
- 文書が決めている物: 参照の絵は 16x16 の tile の形が必須（`tiled_16x16 = 1`）、入力は block linear か 16x16 の tile（pitch linear は不可）、history の buffer の大きさ `(((frame_width_minus1+16)/16)+16)*128` 以上、co-located の motion の buffer の形（macroblock ごとに 64 byte）、rate control の持続の状態（256 byte、`(rc_mode & 3) != 0` の時だけ）、bitstream の出力は slice header 以下（SPS・PPS は driver が書く、`disable_slice_header = 0` で HW が slice header を書く）、状態（`nvenc_pic_stat_s` 128 byte、`total_bit_count`・`bitstream_start_pos`・`error_status`）。
- 文書が決めていない物（危険 R2）: me・md・q の制御の「安全な既定」、`SET_IN_RCDATA` の中身（CQP で要るか）、`SET_UCODE_STATE`・task status の要否、`block_height` の単位、FORCE_OUT_* の要否、APPLICATION_ID と CONTROL_PARAMS の codec の関係（control の codec は H264 = 3 だけが定義、H.265 の値は未定義）。→ 関門の Phase（p009）で、文書の注記どおりの最小の設定（動きの探索は整数の探索だけ、sub-pel は既定、intra の 4x4・8x8・16x16 を全部、量子化は flat、deblock on）から 1 つずつ確かめる。
- firmware: NVDEC と同じく GSP-RM（570.144）が NVENC の falcon の ucode を持つ見込み（WS243 の NVDEC で driver は ucode を load していない）。**未確認**。p009 の最初の EXECUTE で確かめる。別の file が要れば Q1 に返す。

### 3.4 surface の形

| 用途 | 形 | 理由 |
| --- | --- | --- |
| decode の出力・DPB（H.264・H.265） | 今の NV12 の block linear（GOB 2 段、pitch = align(w,64)、各 plane の行は 16 に揃える） | WS243 と同じ。H.265 の CTB 64 の端は NVDEC が自分で扱う（framestride で行を渡す） |
| encode の入力（ENCODE_SRC） | 今の NV12 の block linear（同じ配置） | decode の出力・描画の結果を写さずに encode できる（transcode・camera の後の道）。`input_bl_mode = 0`（GPU の BL）、`block_height` は GOB 2 段の値（単位は p009 で確かめる） |
| encode の参照・再構成（ENCODE_DPB） | 新しい NV12 の「16x16 の tile」の配置（luma は align(w,16)×align(h,16)、chroma は半分の高さ、offset は 256 byte 単位） | NVENC の制約（参照は 16x16 の tile が必須）。usage に ENCODE_DPB がある image だけこの配置にし、他の usage（sample・copy・decode）と組ませない |

## 4. firmware と license

- 新しい firmware の file は要らない見込み（§3.3）。GSP の firmware（`userland/firmware/nvidia-gsp/`、570.144）は今の package のまま。
- zedBSD の code は Zlib。

## 5. 出典と引用の扱い（guardrail「MIT・GPL の出典のデータの引用」）

| 出典 | license | 使い方 |
| --- | --- | --- |
| NVIDIA open-gpu-doc commit `9fdf5c4062007929d9f4e6cbad9c9771fe61b880`（2026-06-10、WS243 と同じ）: `classes/video/nvenc_drv.h`（SHA-256 `04ba0a17…`）、`clc5b7.h`（`ac4dcab9…`）、`clc7b7.h`、`clc9b7.h`、`nvdec_drv.h`（`6211a3cf…`）、`clc5b0.h`（`aba8a0ff…`） | MIT | method の番号・field の byte と bit の位置・buffer の大きさの式（値）。block ごとに「Value(s) referenced:」。手順（method の並び）は「Procedure referenced:」 |
| open-gpu-doc commit `97c510e`（2021-02-13）の `nvenc_drv.h`（`c17cc684…`）・`nvdec_drv.h` | MIT | 版の照合だけ（§3.3 R1）。値は 2 版で同じ所だけを使う |
| NVIDIA open-gpu-kernel-modules 570.144: `src/nvidia/generated/g_gpu_class_list.c`、`src/common/sdk/nvidia/inc/class/clc4b7.h`、`nvos.h`、`cl2080_notification.h`、`src/nvidia/inc/kernel/gpu/gpu_engine_type.h` | MIT | class の番号・engine type・確保の params の形（値） |
| Mesa 26.3.0-rc3（archive.mesa3d.org の tarball、SHA-256 `3600c01f11439628d0a85de5ed9cb76f297d13e9783cdeecb91aa685055c9942`）: `src/nouveau/vulkan/nvk_video_session.c`・`nvk_cmd_video.c`・`rust/video/decode/h264.rs` | MIT | WS243 と同じ参照（H.265・encode は無い）。H.265 の session の memory の式を NVK から取れないので、§3.2 の open-gpu-doc の式を使う |
| Linux v6.19 nouveau（`nvkm/subdev/gsp/rm/r535/` の fifo・nvenc の作り方） | GPL-2.0 | 読まなくても済む見込み（NVENC の channel は NVDEC と同じ手順を zedBSD の `nvdec.c` が既に持つ）。読む時は手順と値だけ（Procedure referenced）、code・comment・構造・名前を写さない |
| ITU-T H.264・H.265（Annex B、7.3 の構文、Table 7-5・7-6 の既定の scaling list など） | 規格 | parameter set の書き出しと解析の表 |
| Khronos Vulkan-Headers 1.4.309（Debian `libvulkan-dev 1.4.309.0-1`）の `vulkan_core.h`・`vk_video/*.h` | Apache-2.0 | 宣言の選び出し（今の `vulkan_video.h` と同じ扱い） |
| JCT-VC の HEVC の適合の bitstream（HEVC_v1、ITU の公開の site） | 試験の入力 | git に入れない（`build/ws324/` に取得し、md5 を照合） |
| NVIDIA Video Encode and Decode GPU Support Matrix（公開の web） | 文書 | 能力の目安だけ |

- NVIDIA の proprietary driver の振る舞いを観測して値を取る（push の dump など）ことはしない（EULA の逆解析の禁止に当たる恐れ、§17 D11）。
- 2026-11-01 の取得: GitHub の raw（open-gpu-doc・open-gpu-kernel-modules、パラメータの無い GET）、archive.mesa3d.org の tarball。gitlab.freedesktop.org は bot の確認（Anubis）に当たったので 1 回で止めた（Q1 に報告済み）。取得物は scratchpad に置き、repo に入れていない。

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
 nvrtx の engine: nvdec.c（今）・nvdec-records.c（HEVC の picture を足す）
                  nvenc.c（新、channel）・nvenc-records.c（新、push と 1 絵の設定の組み立て、host 試験）
```

| 部品 | 責務 | 知らない物 |
| --- | --- | --- |
| `nvenc.c` | NVENC の channel（devinfo・falcon の context・instance block・ring・promote・object・最初の push・停止・解放）、1 回の push の submit と待ち | Vulkan、codec |
| `nvenc-records.c`・`nvrtx-nvenc.h` | RM の params、1 絵の push（method の並び）、`drv_pic_setup` と制御の配列の byte の組み立て、状態の読み | hardware（host 試験で compile） |
| `nvdec-records.c` | HEVC の picture の構造・tile の大きさ・scaling list の buffer の組み立てを足す | 同上 |
| `vk/vk-video-h265.c` | H.265 の parameters（VPS・SPS・PPS）の保管の形、Vulkan の decode の値 → NVDEC の値の変換 | hardware |
| `vk/vk-encode*.c` | encode の session・parameters（取り出しを含む）・quality level・rate control の状態・feedback の query・記録と実行、値の変換 | NVENC の channel の中身 |
| `vk/video-ps.c` | StdVideo の値（encode の session parameters）から SPS・PPS（H.265 は VPS も）の RBSP を書き、emulation prevention と start code を付ける | hardware |

- 場所の理由: 今の nvrtx の video は `vk/` に Vulkan の側、top に engine の側を置く（WS243）。同じ分け方を保つ。parameter set の書き出しは WS322 が `src/drivers/vpu/vpu-h26x-ps.c` を計画しているが、master-design-policy §2.6（遅い抽象化）に従い共有しない（§17 D7）。

## 7. NVENC の動かし方

### 7.1 channel（`nvenc.c`、段 `nvenc-channel`）

- 条件: `nvrtx.video=1`、work channel が動いた後、NVDEC の channel の段の後（独立: NVDEC が無くても試す）。失敗しても boot は続ける（encode の無い device になる）。
- 手順は `nvdec.c` と同じ（1. devinfo で RM の engine type 0x25 の entry の runlist と ENG_DESC、2. `GET_CONSTRUCTED_FALCON_INFO` で context の大きさ、3. VRAM の instance block・USERD と context の buffer、4. system memory の ring・semaphore・push、5. channel の確保（engine 0x1b）・BIND・SCHEDULE、6. `PROMOTE_CTX`（engine 0x1b）、7. object（`0xC4B7`、`NV_MSENC_ALLOCATION_PARAMETERS`、engineInstance 0）、8. 最初の push（subchannel 4 に SET_OBJECT と fence、2 s））。
- chid **49**（NVDEC の 48 の次。handle `0xf1f00031`・`0xc4b70031`）。`NVRTX_WORK_SESSION_CHID_FIRST + NVRTX_WORK_SESSION_CHANNELS + 1` で書き、48 と重ならないことを host 試験で見る。
- `nvrtx_engine_pick` に nvenc の 3 欄（found・runlist・descriptor）を足し、`nvrtx_devinfo_pick` で 0x25 を拾う。
- 停止と解放は NVDEC と同じ位置（`gsp.c` の stop・release）に 1 行ずつ、NVDEC の前。
- `nvdec.c` と `nvenc.c` の channel の code は似るが、master-design-policy §2.6 に従い今は共通の部品にしない（2 つ目で形が同じと分かった。3 つ目（OFA など）が来たら共通化を考える。§17 D6）。

### 7.2 1 回の encode の push（`nvenc-records.c`）

APPLICATION_ID（H264 = 1・H265 = 3）→ SET_CONTROL_PARAMS（codec、RCMODE、GPTIMER_ON 0、FORCE_OUT_PIC 1（参照にする絵）、FORCE_OUT_COL は参照にする H.264 の絵）→ PICTURE_INDEX（session の通し番号）→ SET_IN_DRV_PIC_SETUP → SET_OUT_ENC_STATUS → SET_OUT_BITSTREAM → SET_IOHISTORY → SET_IN_CUR_PIC（luma）・SET_IN_CUR_PIC_CHROMA_U（NV12 の CbCr）→ SET_OUT_REF_PIC_LUMA・_CHROMA（setup の slot の image）→ SET_IN_REF_PIC0_LUMA・_CHROMA（P の参照）→ SET_OUT_COLOC_DATA（H.264 の参照にする絵）→ EXECUTE（NOTIFY 無し、awaken 無し）。subchannel 4。address は GPU の仮想 address を 8 bit 右へ（256 byte 整列）。method の並びの細部は p009 で確かめる（Procedure は文書の method の説明から自分で組む）。

### 7.3 状態と feedback

- 状態の区画（`nvenc_pic_stat_s` 128 byte）を session の executor の system memory の run に置く。実行の後に `error_status`・`ucode_error_status` を見て、0 でなければ絵を ERROR にする。
- `VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR`: `BITSTREAM_BUFFER_OFFSET`（`bitstream_start_pos` に置いた値、= dstBufferOffset の中の書き始め）と `BITSTREAM_BYTES_WRITTEN`（`total_bit_count / 8`）、result status を付ける時は COMPLETE・ERROR。query の結果は実行の時に決める（decode の status と同じ形、`video_pool` を拡げる）。

### 7.4 1 絵の設定（H.264、`vk-encode-h264.c` → `nvenc_h264_drv_pic_setup_s`）

- magic `0xC4B70006`（Ampere・Ada は class から）。
- surface: 入力（block linear、pitch = image の pitch、luma・chroma の offset は image の中の plane の offset の 256 byte 単位）、参照と再構成（16x16 の tile、同じ式）。`frame_width_minus1`・`frame_height_minus1` は Vulkan の `codedExtent` から（NVENC は右と下を crop する、注記）。
- SPS・PPS の値: encode の session parameters の StdVideoH264 から（profile_idc、level_idc、chroma 1、poc_type、log2 の値、frame_mbs_only 1、entropy、num_ref_idx、qp の初期値、chroma qp offset、deblock の制御、transform_8x8）。
- picture の制御: l0[0] = 参照の slot、frame_num、pic_order_cnt_lsb、idr_pic_id、pic_type（IDR 3・I 2・P 0）、ref_pic_flag、slice_mode 1（静的、1 slice）、slice・me・md・q の制御の配列の offset と数（各 1 つ）、hist_buf_size、bitstream_buf_size（dstBufferRange）、bitstream_start_pos 0。
- rate control: DISABLED の時は rc_mode 0 で QP[0..2] に picture の constantQp。CBR・VBR は p013（rc_mode 2・1、持続の状態の 256 byte を session の memory に）。
- **末尾の扱い（R1）**: 472〜511 byte（半分の大きさの再構成の設定と gptimer、版でずれる所）を 0 にし、`stats_fifo_enable`・半分の大きさの出力を使わない。
- me・md・q の制御の値は p009 の関門で決め、`vk-encode-h264.c` の表（block ごとに出典と「p009 で実機で確かめた値」の注記は plan に置き、source には値の意味だけを書く）にする。

### 7.5 1 絵の設定（H.265、`vk-encode-h265.c` → `nvenc_h265_drv_pic_setup_s`）

- 同じ形。SPS の値は min・max の CU と TU の大きさ（2 bit の符号）、SAO、AMP、temporal MVP、short-term RPS の数。CTB は 32×32 から始める（p012 で 64 を試す）。RPS は slice header の中（`short_term_ref_pic_set_sps_flag = 0`）、P の参照 1 枚。
- VPS・SPS・PPS の値は session parameters の StdVideoH265 から。NVENC が対応しない値（例: tile、PCM、scaling list の明示）は `vkGetEncodedVideoSessionParametersKHR` の feedback（`hasStdSPSOverrides` など）で「直した」と返し、書き出しも直した値で行う。

### 7.6 parameter set の書き出し（`video-ps.c`）

- `vkGetEncodedVideoSessionParametersKHR` は最初の encode の前にも呼ばれる（container の header のため）ので、hardware に頼らず software で書く。入力は session parameters の StdVideo の値（NVENC に合わせて直した後の値）。VUI は Vulkan の値を書く（timing・aspect・colour）。HRD は書かない（CBR の時の扱いは p013）。
- host 試験: 書いた RBSP を host の ffmpeg（`-bsf:v trace_headers`）で読み、値が一致する。

## 8. executor の Vulkan Video

### 8.1 報告

- family 0: 今の flag に `VK_QUEUE_VIDEO_ENCODE_BIT_KHR` を足す（encoder の channel がある時だけ）。queue family の video properties は decode と encode の codec の操作の和（H.264 decode、H.265 decode、H.264 encode、H.265 encode のうち使える物）。同じ family に decode と encode を置くのは Vulkan の規則で禁止されていない見込み（**未照合**、p008 の最初に仕様を読む。駄目なら family 1 を encode に分ける、§17 D3）。
- capset の flags: bit 2 = H.265 decode、bit 3 = H.264 encode、bit 4 = H.265 encode（[proposed/gpu-op-video-encode.md](proposed/gpu-op-video-encode.md)）。
- 拡張: `VK_KHR_video_decode_h265`、`VK_KHR_video_encode_queue`、`VK_KHR_video_encode_h264`、`VK_KHR_video_encode_h265`（libvulkan の表、capset の bit がある時だけ）。
- `vkGetPhysicalDeviceFormatProperties(NV12)` の optimal に `VIDEO_ENCODE_INPUT`・`VIDEO_ENCODE_DPB`・`TRANSFER_DST`（encode の入力を CPU から作るため、§8.5）を足す。video の format の問い（`vkGetPhysicalDeviceVideoFormatPropertiesKHR`）は usage ごとに: ENCODE_SRC は block linear、ENCODE_DPB は 16x16 の tile の image を作る（imageCreateFlags・usage の組で区別）。

### 8.2 capabilities（案、p009〜p012 で直す）

| 物 | H.265 decode | H.264 encode | H.265 encode |
| --- | --- | --- | --- |
| profile | Main（1）、8 bit、4:2:0、progressive | Baseline・Main・High、8 bit、4:2:0 | Main、8 bit、4:2:0 |
| min / max の大きさ | 64×64（未確認）/ 4096×4096 | 128×64（未確認）/ 4096×4096 | 同左 |
| maxDpbSlots / maxActiveReferencePictures | 17 / 16（H.264 と同じ） | 2 / 1 | 2 / 1 |
| bitstream の offset・大きさの整列 | 256 / 256 | 256 / 256 | 256 / 256 |
| level | 5.1（main tier） | 5.1 | 5.1（main tier） |
| decode・encode の flag | DPB_AND_OUTPUT_COINCIDE | — | — |
| rate control | — | DISABLED（p013 で CBR・VBR） | 同左 |
| maxQualityLevels / maxBitrate | — | 1 / 100 Mbit/s（目安、未確認） | 同左 |
| feedback | — | BITSTREAM_BUFFER_OFFSET・BYTES_WRITTEN | 同左 |
| encodeInputPictureGranularity | — | 16×16 | 32×32（CTB 32） |
| slice | — | maxSliceCount 1、P の L0 の参照 1、B 無し | 同左 |
| ctb・transform（H.265） | — | — | ctbSizes 32、transformBlockSizes 4〜32 |

- stdSyntaxFlags（encode）は 0 から始める（app の値を全部は受けず、直した値を feedback で返す）。

### 8.3 parameters

- H.265 decode: VPS 16・SPS 16・PPS 64 の id。保つ値は NVDEC の picture の構造に要る物だけ（SPS の大きさ・bit depth・CU・TU・PCM・AMP・SAO・temporal MVP・strong intra・scaling list・short-term RPS の数・long-term の有無と数・log2_max_poc_lsb、PPS の flag・qp・offset・tile の列と行の幅・scaling list・parallel merge・extra slice header bits。VPS は有無の確かめだけ）。update は鍵が重ならない物の追加（libvulkan が先に断る）と updateSequenceCount の +1。
- encode: H.264 は SPS・PPS、H.265 は VPS・SPS・PPS を保ち、NVENC に合わせて直した写しも作る（§7.5）。`vkGetEncodedVideoSessionParametersKHR` は直した写しを `video-ps.c` で書き、feedback を返す。

### 8.4 session の memory

- H.265 decode（binding 4 本、256 byte 整列、memory type bit 0）: coloc（`colMvBuffersize` × (maxDpbSlots + 1)、1 絵の大きさは CTB 16 ごとの MV の量、式は p005 で NVIDIA の文書の注記から決め、足りない時は余裕を取る）、filter（deblock の上・左、SAO の左、縦の filter と SAO、BSD の制御の和、§3.2 の式）、scaling list と tile の大きさ（1024 + tile の区画）は executor の system memory の run（decode ごとに CPU が書く）。
- encode（binding 2〜3 本）: history（§3.3 の式）、coloc（H.264 の参照の slot ごとに macroblock 数 × 64 byte）、rate control の持続の状態（256 byte、p013）。`drv_pic_setup` と制御の配列・状態の区画・push は executor の system memory の run（session ごとに作成の時に取る、WS243 と同じ）。

### 8.5 image

- NV12 の encode の入力: 今の block linear の配置。CPU から作るために `vkCmdCopyBufferToImage`（PLANE_0・PLANE_1）を copy engine で pitch → block linear に（今の image → buffer の逆、`exec-copy.c` の method の向きを変える）。描画（plane の view を color attachment に）は今は出さない。
- NV12 の encode の DPB: 16x16 の tile の配置。usage は ENCODE_DPB だけ（TRANSFER・SAMPLED・decode と組ませない）。memory の要求の大きさは luma + chroma、4 KiB 整列。

### 8.6 記録と実行

- H.265 decode: 今の `DecodeVideo` の記録に H.265 の picture info（`StdVideoDecodeH265PictureInfo`、slice segment の offset の列）と参照の slot の H.265 の値（PicOrderCntVal、long-term）を足す。実行は H.264 と同じ同期の道（drain → NVDEC → 待つ）。
- encode: `CmdEncodeVideoKHR` の記録（bitstream の buffer・offset・range、入力の view、setup の slot、参照の slot、rate control の状態、H.264・H.265 の picture info と slice の情報、quality level）。実行は decode と同じく同期（drain → NVENC → 待つ）、待ちの上限 2 s、timeout で encoder を使わなくする。
- `ControlVideoCoding` の reset で通し番号・rate control の状態を戻し、encode の rate control の設定（`VkVideoEncodeRateControlInfoKHR`）と quality level を session に保つ。BeginVideoCoding の chain の rate control が session の状態と違えば記録を失敗にする（Vulkan の規則）。

### 8.7 encode の入力の CPU の道と guardrail

- 試験の道具は合成の絵（決まった式の模様）を CPU で buffer に書き、copy engine で image へ写す。画素を CPU で変換しない（guardrail「画像の処理は GPU で」）。CPU が書くのは元の画素だけで、block linear への並べ替えは copy engine（GPU）。

## 9. memory と所有

| 物 | 持ち主 | 寿命 |
| --- | --- | --- |
| NVENC の channel の VRAM・system memory・context | `struct nvrtx_nvenc_state`（`gpu->nvenc`） | 段の始めから GSP の release まで |
| session の executor の system memory の run（設定・制御の配列・状態・push） | `video_session` / `encode_session` | session の作成から破棄まで |
| session の memory の binding（history・coloc・filter） | app の VkDeviceMemory（session が object を hold） | bind から session の破棄まで |
| DPB・入力・出力の image、bitstream の buffer | app。記録が実行の終わりまで hold | 記録の drop まで |

- timeout の後に engine がまだ書いているかもしれない memory（WS243 R7）: encoder を使わなくした後は、その session の run と記録が hold する物を session の破棄まで返さない（返すのは GSP の release の後）。

## 10. libvulkan・UAPI の変更

- `gpu-op.h`: `0x1000e` `GET_PHYSICAL_DEVICE_VIDEO_ENCODE_QUALITY_LEVEL_PROPERTIES`、`0x1000f` `GET_ENCODED_VIDEO_SESSION_PARAMETERS`、`0x10010` `CMD_ENCODE_VIDEO`。query pool の作成は今の `CREATE_QUERY_POOL` の chain（`VkQueryPoolVideoEncodeFeedbackCreateInfoKHR` と profile）。protocol の版は上げない（capset の bit が無い backend には送らない、今の video と同じ規則）。差分の案は [proposed/gpu-op-video-encode.md](proposed/gpu-op-video-encode.md)。UAPI の追加で HAL ではない。i915・PowerVR・bcm2711 の executor は知らない番号を今どおり断る。
- `libvulkan/video.c`: H.265 の StdVideo（VPS・SPS・PPS、profile tier level、DPB の管理、scaling list、short-term RPS の配列、long-term、VUI と HRD、picture info、reference info）の転送、encode の構造（capabilities・quality level・session の作成・parameters の add と取り出しと feedback・rate control と layer・encode の info・H.264 と H.265 の picture info・slice の情報・参照の情報・GOP の残り・DPB の slot）の転送。bit field の flag は今と同じ「名前で 1 語に詰める」規則。
- `instance.c`: family の codec の操作の許可に H.265 decode・H.264 encode・H.265 encode を足す（capset の bit がある時だけ）。encode の flag を持つ family の video の flag を残す。
- `device.c`: 拡張の表と依存（decode_h265 → decode_queue、encode_h26x → encode_queue → video_queue）。
- `context.c`: capset の flag の bit 2〜4。
- `query.c`・`external-properties.c`: feedback の query の型、result status を encode の family にも。
- 試験: host の wire の往復（libvulkan の `video.c` の符号化と nvrtx の reader を 1 本で、WS243 T7 の形、`plan/tools/rtx-video/wire-host-test.sh` を拡げる）。i915・bcm2711 の H.264 の今の wire が変わらないこと（同じ試験の H.264 の項目）。
- WS322 との関係: WS322 p005・p010（PowerVR の側の H.265・encode の転送）は WS324 が先なら libvulkan の部分を取り下げ、PowerVR の reader だけになる（Q324-3、Q1）。WS322 の design §8.2 の capset の bit の案（bit 2 H.265 decode、bit 3 H.265 encode）とは bit 3 が違うので、WS324 の案で揃えるよう Q1 に頼む。

## 11. Vulkan の header（Q1 の許可の項目、Q324-2）

- 段 1（p002）: `tools/maintain-video.noct` の選びに `VK_KHR_video_decode_h265` を足し、`vk_video/vulkan_video_codec_h265std.h`・`_h265std_decode.h` を Debian の package から変えずに写す。`vulkan_core.h` は変えない（H.265 decode の sType は既にある）。`API-PROVENANCE.md` に SHA-256 を足す。
- 段 2（p003）: 同じ入力から `VK_KHR_video_encode_queue`・`_encode_h264`・`_encode_h265` と、encode の std（`_h264std_encode.h`・`_h265std_encode.h`）を選ぶ。`vulkan_core.h`（1.3.269）の BETA の囲みの中の encode の値（sType・layout・query type・usage・format feature・queue flag、51 個）は、1.4.309 の KHR の名前と値で `vulkan_video.h` に static const として置く（今の synchronization2 の stage の bit と同じ手法）。2026-11-01 に host で 51 個の値が 1.4.309 と全部一致することを照合した（名前の接尾辞を除いて）。
- 選ばなかった案: `vulkan_core.h` を 1.4.309 で作り直す（`maintain-api.noct` の 1.0 の選びと ABI の照合を全部やり直し、encode を使わない全部の code に差が出る）。
- どちらの段も target の build は共有の sysroot（`build/amd64/sysroot` ほか）の header を読むので、sysroot の作り直しは Q1（toolchain・共有の `build/` の規則）。担当は自分の worktree の `build/` の sysroot で build を確かめ、作り直しを Q1 に頼む。
- 受け入れ: 全 arch の libvulkan・libmedia・GPU の driver の build が warning 0（target の C は C99/GNU89 で line comment を許す今の条件のまま）、`maintain-dispatch.noct` の command の数の照合。

## 12. 危険

| ID | 危険 | 見込み・対処 |
| --- | --- | --- |
| R1 | NVENC の構造の版（§3.3）。Turing の firmware が 2021 版・2024 版のどちらの配置を読むか不明 | 版でずれる末尾を 0 にして使わない。それでも BadMagic・誤りが出たら、2021 版の配置で試す。両方駄目なら p009 で止まって Q1（Q324-4 の時間の上限） |
| R2 | me・md・q の制御の既定が無い。値の組み合わせで firmware が止まる・bitstream が壊れる | p009 で注記に沿った最小の設定から 1 項目ずつ。CQP の I 絵 → P 絵の順。status の誤りの語と host の ffmpeg の decode の誤りで判定 |
| R3 | H.265 decode で GSP・NVDEC が止まる（NVK の報道） | timeout で decoder を使わなくする今の道。session の buffer は式より大きく取って始める（2 倍）、止まる絵を最小の stream に絞る（適合の stream の 1 本ずつ） |
| R4 | HEVC の scaling list の buffer の中の並び（文書は合計 1024 byte と内訳の注記だけ） | scaling list の無い stream を先に。適合の SLIST_A〜D で並びを確かめる |
| R5 | NVENC の ucode が GSP-RM に無い（別の file が要る） | p009 の最初の EXECUTE で分かる。要れば Q1（firmware の package の判断） |
| R6 | 1 つの family に decode と encode を置けない（仕様） | family 1 を encode に（executor の queue の作り直し、+8 LW） |
| R7 | NVENC の encode の速さ（同期、node の lock の下）で compositor が待つ（WS243 R9 と同じ） | 1080p の 1 絵は数 ms の見込み。p014 で compositor の frame の時間を測る。駄目なら非同期（GR が semaphore を待つ形）を後の Phase に |
| R8 | 出力の bitstream が VRAM で、CPU の読みが遅い（約 12 MB/s） | 1080p60 の 20 Mbit/s で約 2.5 MB/s なので足りる。4K の高い bitrate は記録だけ（host cached な memory type は WS243 の外の提案のまま） |
| R9 | `block_height` の単位・入力の BL の版（`input_bl_mode` の 2 版で bit 幅が違う、1 → 2 bit。値 0 は同じ） | 値 0（GPU の BL）だけを使う。block_height は 1（GOB 2 段）から試し、ずれた絵で単位を決める |
| R10 | encode の能力の報告（最小の大きさ・bitrate の上限）が実と違う | p011・p012 で測って直す |
| R11 | libmedia の H.265 の解析（Q324-1 が A の時）を WS318 が別の形で作り直す | WS318 p016 の設計の前に WS324 の module を作るので、WS318 の design の H.265 の節を「WS324 の module を使う」に直す（Q1 が WS318 に記録） |

## 13. 失敗と回復

- API の規則違反（範囲の外の slot、coding scope の外、bind されていない session の memory、rate control の食い違い）は記録を失敗にする（WS243 と同じ）。
- bitstream・値が hardware に合わない（未対応の profile、tile が多すぎる、field、Main10）は絵を飛ばし、status の query を ERROR にする（decode）。encode で直せない値は session parameters の作成で `VK_ERROR_VIDEO_STD_VERSION_NOT_SUPPORTED_KHR` か、作成の時に直して feedback。
- engine の timeout: その engine を boot まで使わなくし（以後の decode・encode は即 EIO → `VK_ERROR_DEVICE_LOST`）、段の log に fence と semaphore の値を出す（今の NVDEC と同じ）。channel の作り直しは WS250 の回復にも無く、この WS でも作らない（§16 の制限）。
- status の error（NVDEC の `mbs_in_error`、NVENC の `error_status`）: 絵を ERROR に。encode の ERROR の絵の bitstream の bytes は 0 を返す。

## 14. 試験の計画

### 14.1 host（各 Phase、gcc で driver の file をそのまま compile、`plan/ws324/tests/`）

- T1 HEVC の picture: 既知の VPS・SPS・PPS・picture info から `nvdec_hevc_pic_s` を組み、byte・bit の位置（open-gpu-doc の header を host の temp で compile して測った表、表は試験に値として持つ）に期待の値。tile の大きさの区画、scaling list の区画。
- T2 HEVC の作業の buffer の大きさの式（720p・1080p・4K）。
- T3 NVENC の RM の params（BSP と同じ形の MSENC、promote、devinfo の pick の 0x25）と chid・handle の重なり。
- T4 NVENC の 1 回の push（method の並び・値・address の >> 8）。
- T5 `nvenc_h264_drv_pic_setup_s`・`nvenc_h265_drv_pic_setup_s` の組み立て（両版の配置で、版でずれない所の値が同じこと、ずれる所が 0 であること）。
- T6 parameter set の書き出し（`video-ps.c`）: 書いた SPS・PPS・VPS を host の ffmpeg の trace_headers で読み、値が一致。emulation prevention の境界。
- T7 wire の往復: libvulkan の `video.c` の符号化と nvrtx の reader（H.265 decode、encode の全構造）。H.264 decode の今の往復が変わらない。
- T8（Q324-1 が A の時）libmedia の H.265 の解析: WS318 の standard 廉価の形（突然変異の確かめ、適合の stream の SPS・PPS・slice header の値を host の ffmpeg の trace_headers と比べる）。
- build: amd64 の `vmunix`（warning 0）、libvulkan（amd64・arm64・riscv64）、venus-reg。

### 14.2 QEMU

- NVDEC・NVENC は QEMU に無い。QEMU（Venus、amd64）では非退行だけ: video の capset の無い device で libvulkan が H.265・encode の拡張を出さず、Video Player が今どおり（`plan/tools/rtx-video/venus-videoplayer.sh`）。

### 14.3 RTX 2070 の実機（10.0.10.3。Q1 が割り当てた時だけ、1 本の script: 配置 → 起動 → 実行 → 回収 → 要約）

- 道具: `plan/ws324/tests/rtx-video324.sh`（`plan/tools/rtx-video/rtx-p004.sh` の形を写して直す）。image は `plan/ws324/tests/config-*.mk` で build し、`nvrtx.video=1`。
- H.265 decode: `vkvideo-probe` の H.265 で適合の stream（HEVC_v1 の Main の progressive・8 bit・4:2:0 の物、まず 10 本、後に全部）の各 frame の hash が host の ffmpeg の decode（`nv12-hashes.py` の形）と一致。host の ffmpeg（libx265）で作った 1080p30・1080p60・4K30 の clip の hash も一致、1 絵の decode の時間（`--time`）。
- encode: `vkencode-probe`（新）が決まった模様の絵の列（と、decode した clip の絵）を encode し、Annex B の file（SPS・PPS は `vkGetEncodedVideoSessionParametersKHR`）を書く。host で `ffmpeg -xerror` が誤り無く decode し、元の絵との PSNR が目安以上（CQP 26 で 35 dB 以上を目安、p011 で直す）、`ffprobe` の profile・level・大きさ・frame の数が設定どおり。zedBSD の NVDEC（`vkvideo-probe`）で同じ stream を decode した hash が host の ffmpeg と一致（decode 側の適合の確かめを兼ねる）。1 絵の encode の時間。
- 非退行: H.264 decode の 4 本の hash（WS243 の `rtx-p004.sh … video`）、hang の確かめ（`plan/ws250/tests/rtx-hang-check.sh`）、desktop の起動（`boot-test.sh` の画面）。
- 失敗の道: 壊した stream（途中を切る、SPS の値を壊す）で ERROR が返り、次の正しい stream が decode できる。timeout の注入（`nvrtx.clear.fault` と同じ形の `nvrtx.video.fault=1` を考える。要るかは p007 で）。

## 15. Phase の分け方と受け入れ（案、LW は見積り）

型: O = Opus（kernel・driver は phase-runner-high）、S = Sonnet 5.5 high（標準廉価実装手順）。

| Phase | 目的 | 型 | 依存 | 受け入れ | LW |
| --- | --- | --- | --- | --- | --- |
| p001 | 調べと設計（この文書）、review | O | — | design-reviewer の反映、問いを Q1 へ | — |
| p002 | Vulkan の header の段 1（H.265 decode の選び、std の写し、provenance） | O ＋ Q1 | Q324-2 | §11 の受け入れ、sysroot の作り直しは Q1 | 3 |
| p003 | Vulkan の header の段 2（KHR の encode、BETA の値の KHR の名前） | O ＋ Q1 | p002、Q324-2 | 同上、51 個の値の照合の試験 | 5 |
| p004 | libvulkan の H.265 decode の転送（構造・codec の操作・拡張の表・capset の bit 2） | O | p002、Q324-3 | T7（H.265 decode）、amd64・arm64・riscv64 の libvulkan の build、H.264 の今の往復が変わらない | 8 |
| p005 | nvrtx の H.265 decode（HEVC の picture・作業の buffer・tile・scaling list、`vk-video-h265.c`、capabilities・形式・family の報告、capset の bit 2） | O（high） | p004 | T1・T2、amd64 の vmunix の warning 0。H.265 の無い時の報告は今と同じ | 16 |
| p006 | H.265 の解析（Q324-1 の答えによる。A: libmedia の独立の module を標準廉価手順で、設計 4 と実装 3 本 S） ＋ `vkvideo-probe` の H.265 | O・S | p002（header）、Q324-1 | T8、probe の build、host の適合の stream の解析の誤り 0 | 24（A）/ 16（B） |
| p007 | H.265 decode の実機の debug（適合の stream、clip、時間、失敗の道） | O（high） | p005、p006、実機 | §14.3 の H.265 decode の項目 | 12 |
| p008 | libvulkan の encode の転送（全構造、命令の番号、feedback の query、quality level、取り出し、capset の bit 3・4） | O | p003、Q324-3 | T7（encode）、build | 16 |
| p009 | **NVENC の関門**: channel（段 `nvenc-channel`）、1 回の push、`nvenc-records.c` の H.264 の最小の設定。driver の中の probe（`CONFIG_DRIVER_NVRTX_NVENC_PROBE`、既定 n、試験の config でだけ y）が boot の後に 1 回、VRAM に決まった模様の小さな絵（64×64、copy engine の fill と CPU の書き込み）を CQP の I 絵（次に P 絵）で encode し、状態の値と bitstream の先頭（最大 4 KiB）を stage の log に hex で出す。host の script（試験だけの物）が同じ値の SPS・PPS を前に付け、ffmpeg で decode して元の模様と比べる（log は実機の SSH で取る） | O（high） | p001、実機 | 最初の EXECUTE が通る、status の誤り 0、host の ffmpeg が I 絵と P 絵を誤り無く decode し模様と一致（PSNR 40 dB 以上）、R1・R2・R5・R9 の答えの記録。**時間の上限（Q324-4）を越えたら止まって Q1** | 20 |
| p010 | encode の共通の executor（NV12 の buffer → image の copy、16x16 の tile の DPB、session と memory、feedback の query、quality level、`video-ps.c` の H.264、`vkGetEncodedVideoSessionParametersKHR`） | O（high） | p008、p009 | T6・T7、build | 14 |
| p011 | H.264 encode（`vk-encode-h264.c`、IDR と P、CQP、`vkencode-probe`、実機の debug） | O（high） | p010 | §14.3 の H.264 encode の項目（PSNR、ffprobe、NVDEC の decode の hash） | 18 |
| p012 | H.265 encode（`vk-encode-h265.c`、`video-ps.c` の H.265、実機） | O（high） | p011、p006（probe の NVDEC の確かめに H.265 の解析） | §14.3 の H.265 encode の項目 | 18 |
| p013 | rate control（CBR・VBR、持続の状態、HRD を書くか） | O | p011・p012 | 実機で 10 s の平均の bitrate が設定の ±10% 以内、PSNR | 10 |
| p014 | 実機の受け入れ（全項目の 1 本の script、非退行、compositor の frame の時間） | O | p007・p011・p012・p013 | §14.3 の全項目の PASS | 6 |
| p015 | 規約の全文の見直し | conformance-reviewer | 全部 | coding-style の全文で見直し、動作は不変 | 4 |

- 合計 約 174 LW（A の時。8 LW ≈ 2〜3 h なので 約 44〜65 h）。encode（p003・p008〜p013、約 101 LW）の幅が最も大きく、p009 の関門の結果で ±50%。
- 先に進められる物: p002・p003（Q1 の許可の後）、p009（実機の割り当ての後、他と独立）。p004・p005・p006 は p002 の後に並べられる（file が重ならない: libvulkan、nvrtx、libmedia・probe）。
- 標準廉価実装手順に合うのは H.265 の解析（規格で振る舞いが決まる）と parameter set の書き出し（同）。NVDEC・NVENC は hardware を探りながら作るので普通の Phase。
- 実装の前倒しは 2026-11-01 のユーザーの指示（RTX の H.264 encode・H.265 decode・H.265 encode の実装に移る）で明示されている。

## 16. 制限（今の設計で残す物）

- engine の作り直し（timeout の後）は無い。
- encode は 1 slice、参照 1 枚、B 無し、rate control は CQP（p013 の後 CBR・VBR）。lookahead・AQ・2 pass・intra refresh は無い。
- decode・encode とも同期（1 絵ずつ GR を流して待つ）。
- 4K を越える大きさ、Main10・4:4:4 は報告しない。
- RTX 30・40 は class の番号（`0xC7B7`・`0xC9B7`、magic は class から）だけを用意し、実機の確かめは無い。

## 17. 判断（根拠と、選ばなかった案）

| # | 決定 | 根拠 | 選ばなかった案と理由 |
| --- | --- | --- | --- |
| D1 | nvrtx の device の family 0 に H.265 decode と encode を足す | ユーザーの決定（Vulkan Video が OS の API）。WS243 の NVDEC の道と executor の memory・image・fence をそのまま使える | 別の VkPhysicalDevice・別の cdev（ユーザーの決定に反する） |
| D2 | NVENC は open-gpu-doc の公開の interface を自分で駆動する | NVIDIA が MIT で公開した driver 向けの文書で、正当な出典。他に道が無い | proprietary の driver の観測（D11）、encode を software だけにする（ユーザーの指示に反する。駄目な時の退路として Q324-4 の B） |
| D3 | encode も family 0 に（同じ queue） | libmedia（WS318）は queue の lock を役ごとに持つ設計で、family が 1 つの方が app の device の作成が簡単。executor の queue は 1 本 | family 1 に分ける（executor の queue の作り直し。仕様が許さない時だけ、R6） |
| D4 | encode の DPB は 16x16 の tile の別の配置、入力は今の block linear | NVENC の制約（参照は 16x16 の tile が必須）。入力を block linear にすると decode・描画の結果を写さずに encode できる | 入力も 16x16 の tile（decode・描画との共有ができず copy が要る） |
| D5 | decode・encode とも同期で始める | WS243 で正しさが分かりやすかった。1080p60 は WS243 で足りた | 最初から非同期（正しさの確かめの前に fence の形を変えない） |
| D6 | NVENC の channel は `nvdec.c` と別の file（似た code を共有しない） | master-design-policy §2.6（遅い抽象化）。2 つで形が同じと分かったので、3 つ目で共通化を考える | 今 `video-channel.c` に共通化（完了した WS243 の code の再試験が要る） |
| D7 | parameter set の書き出しは nvrtx の `vk/video-ps.c` | 同上。WS322 の `vpu-*-ps.c` と同じ物を共有する抽象は早い | `src/drivers/video/` の共通の module（3 つの driver の WS の調整が要る） |
| D8 | libmedia は変えない（試験の道具で確かめる）。H.265 の解析は libmedia の独立の module として先に作る案を推す（Q324-1） | ユーザーの指示（WS318 を始めずに済む最小）。解析は道具にも WS318 にも要るので、一度だけ書く | 道具の中に試験だけの解析（重複、約 2500 行）、WS318 を待つ（指示に反する） |
| D9 | Vulkan の header は `vulkan_video.h` の選びを拡げ、`vulkan_core.h` は 1.3.269 のまま | 入力（1.4.309）は既に pin されている。BETA の値は 1.4.309 と一致を照合した。影響が encode の宣言だけに閉じる | `vulkan_core.h` の作り直し（全部の code に差、ABI の照合のやり直し） |
| D10 | NVENC の構造の版でずれる末尾を 0 にして使わない | どちらの版でも同じ意味になる（R1） | 2024 版をそのまま（Turing の firmware が古い配置なら gptimer の位置がずれる） |
| D11 | proprietary の driver の振る舞いを観測しない | EULA の逆解析の禁止に当たる恐れ。出典が公開の MIT の文書で足りるうちは要らない | Debian の NVIDIA の driver で ffmpeg の hevc_nvenc を動かし push を観測（license の危険） |
| D12 | 能力の報告は確かめた範囲（4096、参照 1、slice 1） | 実機の確かめの前に大きく出さない（WS243 D3 と同じ考え） | hardware の公表の全部（8K・B・4:4:4） |

## 18. 問い（ユーザー・Q1 へ）

phase.md の「判断を待つ点」に背景・用語・選択肢・得失・推しを書いた。要約:

| ID | 問い | 推し | 誰が |
| --- | --- | --- | --- |
| Q324-1 | H.265 の解析（StdVideoH265 を作る物）をどこに作るか | A: WS318 p016〜p019 の範囲を libmedia の独立の module として WS324 で先に作り（runtime には繋がない）、`vkvideo-probe` が直に compile する。WS318 は後でそれを使う | Q1（WS 間の範囲の移し。WS318 の記録の直しも） |
| Q324-2 | Vulkan の header の選び直しと sysroot の作り直しを許可するか（toolchain） | A: `vulkan_video.h` の選びを 1.4.309 の同じ入力から 2 段で拡げ、`vulkan_core.h` は変えない | Q1（toolchain の許可） |
| Q324-3 | libvulkan の H.265 decode・encode の転送と capset の bit の割り当てを WS324 が持つか（Q322-4） | A: WS324 p004・p008 が持ち、bit 2 = H.265 decode、bit 3 = H.264 encode、bit 4 = H.265 encode。WS322 p005・p010 の libvulkan の部分は取り下げ | Q1 |
| Q324-4 | NVENC は公開の実装の前例が無い。関門の Phase（p009）に時間の上限を置き、越えたら encode を止めて software（WS329・WS330）に任せるか | A: 上限 20 LW（約 5〜8 h）で関門。越えたら Q1 に返し、ユーザーが続けるか決める | ユーザー（費用と危険の受け入れ） |
| Q324-5 | encode を decode と同じ queue family 0 に置くか | A: 同じ family（仕様が許さなければ family 1） | Q1（技術） |
| Q324-6 | WS324 の受け入れに Video Player の H.265 の再生を入れるか | A: 入れない（試験の道具で driver を確かめる。Video Player の H.265 は WS318 p020・p026） | Q1（ws.md の目標の「libmedia から使えるようにする」の読み） |

## 19. HAL

- 変えない。要る物は全部 nvrtx の driver と kernel の今の口で足りる: register の読み書き（nvrtx の `nvrtx_wr32` ほか、今の BAR の map）、VRAM・system memory の確保と GPU の page table（nvrtx の allocator・`nvrtx_work_map_*`）、GSP-RM の RPC（今の `rpc.c`）、待ちと時刻（`kern/clock.h`）、lock（`kern` の mutex）。NVENC は GSP-RM の向こうの engine で、HAL の責務（platform・arch の抽象）に関わる物が無い。
- 新しい code は HAL の header を include しない（guardrail「driver は HAL を知らない」）。

## 20. 既存の file への変更（所有の重なり）

| file | 変更 | 注意 |
| --- | --- | --- |
| `src/drivers/gpu/nvrtx/nvdec-records.c`・`nvrtx-nvdec.h` | HEVC の picture・buffer の大きさ・tile・scaling list | WS243 の完了した code。host 試験 `nvdec-host-test` を拡げる |
| `src/drivers/gpu/nvrtx/objects.c`・`nvrtx-gsp.h` | devinfo の pick に nvenc の欄 | P2（WS291）が触った所。WS291 はラップアップ中 |
| `src/drivers/gpu/nvrtx/gsp.c`・`stage.c`・`nvrtx-gpu.h` | 段 `nvenc-channel`、state の欄 | 同上 |
| `src/drivers/gpu/nvrtx/vk/vk-video.c`・`vk-device.c`・`vk-resource.c`・`vk-memory.c`・`vk-record.c`・`vk-execute.c`・`exec-copy.c`・`nvrtx-vk-objects.h`・`nvrtx-vk.h`、`vk-node.c`、`runtime.c` | §8 の hook | WS250（回復）・WS291（速さ）の後の main を基点に |
| `userland/desktop/libvulkan/`（video.c・instance.c・device.c・context.c・query.c・external-properties.c・tools/maintain-video.noct・api-commands.tsv など） | §10・§11 | WS322・WS319 と調整（Q324-3） |
| `include/libc/vulkan/vulkan_video.h`・`vk_video/`・`API-PROVENANCE.md` | §11 | toolchain の扱い（Q1） |
| `include/uapi/gpu-op.h` | encode の番号 3 つ | UAPI。全部の executor の番号の台帳 |
| `userland/tests/vkvideo-probe/`、新しい `userland/tests/vkencode-probe/` | H.265、encode | WS083 の道具 |
| `userland/desktop/libmedia/`（Q324-1 が A の時、新しい `h265*.c` だけ、`sources.mk` には入れない（libmedia の build に入れるのは WS318 p020）） | H.265 の解析 | WS318 の範囲の前借り |
| `platform/amd64/vmunix.mk` ほか | 新しい file | — |

新しい file: `src/drivers/gpu/nvrtx/nvenc.c`・`nvenc-records.c`・`nvrtx-nvenc.h`、`vk/vk-video-h265.c`・`vk/vk-encode.c`・`vk/vk-encode-h264.c`・`vk/vk-encode-h265.c`・`vk/video-ps.c`・`vk/nvrtx-encode.h`、試験は `plan/ws324/tests/`。

## 21. review

- review 1（design-reviewer）の指摘と反映は [design-review.md](design-review.md)。
