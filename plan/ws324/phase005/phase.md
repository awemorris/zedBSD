<!-- awesome-plan project=zedbsd record=ws324-p005 -->

# ws324-p005 nvrtx の H.265 decode: NVDEC の HEVC、executor、行の揃え 64、timeout の後の隔離

Status: in-progress（2026-11-02 08:20 P1、q1917-i01: 実装・host 試験・build・実機の H.264 の非退行まで。受け入れの判定と cleared は Q1。H.265 の正しい絵は p007）

- WS: [WS324](../ws.md)。設計: [design.md](../design.md) §3.2・§3.4・§8.1〜§8.6・§9・§10・§12 R3・R4・R12・§13・§14.1 T1・T2・T4・T7・§15 の p005。
- 基点: main `5feabb6d5`、途中で main `12f6cb1e1`（P2 の libvulkan の H.265 の転送）へ fast-forward、後に main `245fa7cb9`（vkvideo-probe の H.265）を merge（`e534413ad`）。worktree `/home/awe/zedBSD-worktrees/p1-ws324`、branch `agent/p1-ws324`。
- 並行: libvulkan の H.265 decode の転送（WS324 p004 の中身）は P2 が ws322-p005（branch `agent/p2-ws322`）で作る。この Phase は nvrtx の側（wire の reader から NVDEC の push まで）だけを持ち、libvulkan は変えない。H.265 の Vulkan の header は P2 の commit（`bafa50879`）が入れる。nvrtx の kernel の code は Vulkan の H.265 の header を include せず（sType・enum の値を自分の名前で持つ、今の H.264 と同じ）、この Phase は header を入れない。

## 1. 目的

nvrtx の executor（`src/drivers/gpu/nvrtx/vk/`）の queue family 0 に H.265 Main（8 bit、4:2:0、progressive）の decode を足し、NVDEC の HEVC で 1 絵ずつ同期に decode する形を作る。実機で H.265 の絵を正しく出すことの確かめ（適合の stream、hash）は p007（vkvideo-probe の H.265、p006i の後）。この Phase の受け入れは host 試験と build と、実機の H.264 の非退行。

## 2. 範囲

1. **wire の reader**（`vk/vk-video.c`）: H.265 の profile（1000187003）・session parameters の create（1000187001）と add（1000187002）・picture（1000187004）・DPB slot（1000187005）・capabilities（1000187000）。形は P2 の libvulkan の encoder（ws322-p005、WS324 design §10: 構造は sType・chain・field の宣言の順、1 bit の flag は名前の順に 1 語、幅のある field は 1 語ずつ、固定の配列は 64 bit の数と要素（u8 は bytes、u16・u32 は要素ごとに u32）、pointer は有無の語と中身、VUI・HRD・sub-layer・PTL・DPB の管理・palette も送る）。reader は decode に要らない field も全部読んで捨てる。
2. **報告**: capabilities（H.265: maxDpbSlots 16、maxActiveReferencePictures 15、granularity 64×64、最小 64×64、最大 4096×4096、bitstream の整列 256、DPB_AND_OUTPUT_COINCIDE、level 5.1、std は `VK_STD_vulkan_video_codec_h265_decode` 1.0.0）、format（H.265 の profile も NV12）、family の codec の操作（DECODE_H264 | DECODE_H265）、capset の bit 2（`runtime.c`）。
3. **session**: codec ごとの memory の binding（H.264: 今の 3 本、H.265: 0 = co-located の MV（1 絵 align64(w)×align64(h)/16 byte × 17）、1 = filter（縦の filter・縦の SAO・BSD の制御の行ごとの区画と、上の deblock・上の SAO の区画））。session の run を 16 KiB にし、区画を picture の構造 0（2 KiB）・slice の表 2048（1 KiB）・状態 3072（4 KiB）・scaling list 7168（1 KiB）・tile の大きさ 8192（4 KiB）・push 12288 にする（H.264 も同じ表）。
4. **parameters**（`vk/vk-video-h265.c` に値、`vk-video.c` に保管）: VPS（id 16、有無だけ）、SPS（(VPS, SPS) 16×16）、PPS（(VPS, SPS, PPS) 16×16×64）。保つのは NVDEC の picture の構造・scaling list・tile・slice header の bit 数に要る値だけ。
5. **picture の構造**（`nvdec-records.c`・`nvrtx-nvdec.h`）: `nvdec_hevc_pic_s`（276 byte）を byte と bit の位置で書く。HEVC の push（APPLICATION_ID 7、CONTROL の codec 7・GPTIMER・ERR_CONCEAL、PIC_SETUP・IN_BUF・PICTURE_INDEX・STATUS・HEVC の SCALING_LIST 0x580・TILE_SIZES 0x584・FILTER_BUFFER 0x588・COLOC、LUMA・CHROMA の 16 面、EXECUTE。SLICE_OFFSETS・HISTORY・MBHIST は出さない）。作業の buffer の大きさの式。scaling list の区画（DC 8 byte・空き 8・4x4・8x8・16x16・32x32、raster の順。Vulkan の対角の順から並べ替え、無い時は規格の既定）。tile の区画（tile ごとの幅・高さ（CTB）、byte 0x700 から列と行の累積の境（16 画素の単位））。
6. **slot と picidx**: picidx = Vulkan の DPB の slot（0〜15）。setup の slot が無い絵は参照に使われていない番号。参照に無い picidx の面と `RefDiffPicOrderCnts` は「代わりの参照」（今の絵の RPS の最初の参照、無ければ今の絵）で埋める。`RefDiffPicOrderCnts` は int8 に切り詰め、`longtermflag` の bit (15 − i)、`num_ref_frames` = NumPicTotalCurr、初期の参照の list は L0 = Before → After → Lt、L1 = After → Before → Lt を 16 まで繰り返す。
7. **slice header**: 既定は「driver が数えて飛ばす」形: 最初の slice segment の header を bitstream から CPU で読み（emulation prevention を外す）、slice_type の後から long-term の RPS の終わりまでの bit 数を `sw_hdr_skip_length` に入れる（short-term の RPS は Vulkan の `NumBitsForSTRefPicSetInSlice` で飛ばす）。boot の引数 `nvrtx.hevc.dxva=1` で「hardware が読む」形（skip 0、DXVA の欄 `num_bits_short_term_ref_pics_in_slice` ほかを埋める）に替える（p007 で確かめる、R4）。
8. **行の揃え**: NV12 の video image の luma の行を 64、chroma の行を 32 に揃える（今は 16、H.264 にも当てる、R12）。
9. **timeout の後の隔離**（design §9、D13、H.264 にも当てる）: decoder の待ちが timeout した時、その decode の記録が hold する object（session・parameters・bitstream・出力と参照の view）を driver の隔離の表に移して boot の間返さない（view は image を、image と buffer は memory を、session は binding の memory と run を hold するので、それらの VRAM は再利用されない）。数と大きさを log に出す。`nvrtx_vk_video_decode()` は timeout を ETIMEDOUT、session の喪失を EIO で返し分ける。
10. **状態**: H.265 は `mbs_in_error`・`error_status`・`slice_header_error_code` のどれかが 0 でなければ ERROR（H.264 は今のまま）。

範囲の外: libvulkan（P2）、vkvideo-probe の H.265（p006i）、実機の H.265 の debug（p007）、Main10・4:4:4・SCC・field、slice 単位の decode（v3）、encode、WS250 の回復との組み合わせ（回復は channel を止めるが video の engine と address の空間を作り直さないので、この Phase では変えない。p007 で hang の試験と一緒に確かめる）。

## 3. 出典

- NVIDIA open-gpu-doc `9fdf5c40` `nvdec_drv.h`（`nvdec_hevc_pic_s` を host の compiler で並べた byte と bit の位置、`GIP_ASIC_*`・`HEVC_DBLK_*`・`HEVC_SAO_LEFT_*`）・`clc5b0.h`（method）。MIT。値として block ごとに引用。
- averne の FFmpeg の nvtegra（GitHub `averne/FFmpeg` branch `nvtegra`、`libavcodec/nvtegra_hevc.c` SHA-256 `72a9bdaa…`、`libavcodec/hevc/hevcdec.c`（`nvidia_skip_length`）、`libavutil/nvdec_drv.h`（`nvdec_hevc_scaling_list_s`）。GPL-2.0+）: Tegra の NVDEC で動いている HEVC の手順と値（filter の区画の行ごとの byte（480・3840・60）、co-located の大きさ、tile の区画の形と 0x380 語の位置、scaling list の並び、skip の bit の範囲、参照の list の埋め方、method の並び）。guardrail「MIT・GPL の出典のデータの引用」に従い値と手順だけを「Value(s) referenced:」「Procedure referenced:」で示し、code・comment・構造・名前は写さない。取得物は scratchpad（repo に入れない）。
- ITU-T H.265（7.3.6.1 slice segment header、7.3.4 scaling list、Table 7-5・7-6、6.5.3 対角の走査、6.5.1 tile の境、8.3.4 参照の list）。

## 4. 受け入れ

| 項目 | 確かめ方 |
| --- | --- |
| build | `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/ws324-p005/k build/ws324-p005/k/vmunix` が warning 0 |
| T1・T2・T4（host） | `sh plan/ws324/tests/hevc-host-test.sh`: picture の構造の各 field の byte と bit（open-gpu-doc の header から測った表）、作業の buffer の大きさ（720p・1080p・4K、tile 最悪）、HEVC の push の語、scaling list の区画（対角 → raster、既定、DC）、tile の区画（一様・一様でない・無し）、slot と picidx・POC の差・long-term・初期の参照の list、slice header の bit 数（IDR・非 IDR・SPS の RPS の番号・slice の RPS・long-term・emulation prevention） |
| T7（host） | `sh plan/ws324/tests/h265-wire-host-test.sh`: P2 の encoder の順で書いた H.265 の parameters の create・update・decode・profile・capabilities の wire を `vk-video.c` の reader が全部の byte と要る field を読む。P2 の libvulkan が main に入った後は、libvulkan の encoder → nvrtx の reader の往復の試験に替える（`plan/tools/rtx-video/wire-host-test.sh` と同じ形） |
| 非退行（host） | `plan/tools/rtx-video/nvdec-host-test.sh`・`wire-host-test.sh`・`plane-header-host-test.sh`（行の揃えと run の配置の変更に追従させる。plan/tools の file の変更は Q1 の許可の後）・`plan/ws324/tests/nvenc-host-test.sh` |
| 非退行（実機、10.0.10.3） | H.264 の decode の 4 本の hash（`plan/tools/rtx-video/rtx-p004.sh … video`）が今と同じ（行の揃え 64・run の配置の変更の後）、`nvdec-channel ok`。H.265 の無い時の報告（capset の bit 0 だけの時）は今と同じ。機械は終わりに Debian に戻し cksum の一致 |

## 5. 判断を待つ点

- （無し。範囲の中の技術の判断は §2 に書いた。Tegra の手順を Turing に当てる所（filter の区画、上の deblock・SAO の offset、skip と DXVA）は p007 の実機で確かめる。）

## 6. 結果

### 6.1 commit

- `a137d2757`（基点 `12f6cb1e1`）: 実装と host 試験。`e534413ad`: main `245fa7cb9` の merge。この後の commit（bitstream を CPU の窓へ、H.265 の誤りの状態の log、実機の script、design・phase・ws の記録）は Q1 への報告に SHA を書く。
- 変えた file: `src/drivers/gpu/nvrtx/vk/vk-video-h265.c`（新）、`vk/vk-video.c`・`vk/nvrtx-video.h`・`vk/nvrtx-vk.h`・`vk/vk-video-h264.c`、`nvdec-records.c`・`nvrtx-nvdec.h`、`runtime.c`（capset の bit 2）、`vk-node.c`（ETIMEDOUT、`nvrtx_vk_video_hevc_dxva`）、`stage.c`・`nvrtx-private.h`（`nvrtx.hevc.dxva=1`）、`platform/amd64/vmunix.mk`、`plan/tools/rtx-video/nvdec-host-test.c`（行の揃えの期待値、Q1 の許可）。試験: `plan/ws324/tests/hevc-host-test.{c,sh}`、`h265-wire-{encode,decode}.c`・`h265-wire-test.h`・`h265-wire-host-test.sh`、`rtx-video324.sh`。
- HAL・`include/uapi/`・toolchain・libvulkan は変えていない。

### 6.2 設計の判断（範囲の中の技術、design.md §8.4・§8.6 に反映）

- 出典: NVDEC の HEVC を自分で駆動する公開の実装は averne の FFmpeg の nvtegra（Tegra X1 の NVDEC、GPL-2.0+）だけ。値と手順だけを block ごとに引用した（filter の区画の行ごとの byte、co-located の大きさ、tile の区画の形と byte 0x700 の境、scaling list の並び（raster、FFmpeg の `sl` が raster なので）、skip の bit の範囲、参照の list の埋め方、method の並び）。Turing（NVDEC 4）は v1 の拡張を読むので、上の deblock・上の SAO の offset に別の区画を与えた（Tegra は 0）。
- slice header: 既定は driver が数えて skip（Tegra の手順と同じ）、`nvrtx.hevc.dxva=1` で DXVA の欄。
- 参照: picidx = slot、参照に無い面は今の絵の最初の参照（無ければ今の絵）、差は int8 に切り詰め。
- session: H.265 の binding は 2（co-located 17 絵分、filter）、run 16 KiB（pic 0・slices 2048・status 3072・scaling 7168・tiles 8192・push 12288）。H.265 の能力: slot 16、参照 15、粒度 64×64、64×64〜4096×4096、level 5.1。
- timeout の後の隔離: decode の記録が hold する object を 1 つずつ retain して離さない（view → image → memory、buffer → memory、session → binding の memory と run）。`nvrtx_vk_video_decode()` は timeout を ETIMEDOUT で返す。

### 6.3 host 試験（2026-11-02、gcc、ASan・UBSan）

| 試験 | 結果 |
| --- | --- |
| `sh plan/ws324/tests/hevc-host-test.sh`（T1・T2・T4、scaling・tile・describe・参照・skip） | 157 checks, 0 failed。壊す確かめ 7 か所（bit の位置、tile の境の語、scaling の区画、filter の行の byte、対角の走査、skip の始まり、L1 の順）が全部試験の失敗で落ちた |
| `sh plan/ws324/tests/h265-wire-host-test.sh`（T7: P2 の libvulkan の encoder → nvrtx の reader、全 pointer の VPS・SPS・PPS、decode、profile） | 54 checks, 0 failed（全 byte を読む） |
| `plan/tools/rtx-video/nvdec-host-test.sh`（行の揃え 720 → 768・360 → 384 に追従） | 88 checks, 0 failed |
| `plan/tools/rtx-video/wire-host-test.sh`・`plane-header-host-test.sh`・`copy-clear-host-test.sh`、`plan/ws324/tests/nvenc-host-test.sh` | 32・40・8・61 checks, 0 failed |
| build `make ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/ws324-p005/k build/ws324-p005/k/vmunix` | rc 0、warning 0（最後の vmunix の cksum 1717610756） |

### 6.4 実機（RTX 2070、10.0.10.3。QEMU は使っていない。2026-11-02 07:26〜08:16、lock の中）

- 道具: `plan/ws324/tests/rtx-video324.sh OUT VMUNIX PROGRAMS CLIPS`（`H265_DIR`・`DXVA_PASS=1`）。programs は `build/ws324-p005/u`（media-probe・vkvideo-probe・libmedia・libvulkan・libc・libwayland-client を同じ tree で build、warning 0）。clip は P2 の p004 の H.264 の 5 本の写しと、`nv12-hashes.py` で作り直した参照（P2 の物と同じ）。H.265 は適合の 6 本（AMP_A_Samsung_7、CAINIT_A_SHARP_4、DBLK_A_SONY_3、POC_A_Bossen_3、SLIST_A_Sony_5、TILES_A_Cisco_2）と ffmpeg の framehash（nv12、sha256）。
- 各回の始めに NVMe の kernel（1463007375）と zedbsd.cfg（38198411）を読み戻し、終わりに戻して cksum が一致（run1・run3・run4・run5 とも MATCH、Debian に戻る、vfio-pci）。run2（07:30）は元の kernel のまま zedBSD が network に出ず（5 分 ping 無し、変更の前）、07:36 に plug 1 を入れ直して戻った。
- 起動（kernel 1126250319、commit `a137d2757`、`nvrtx.video=1`）: `nvdec-channel ok: class c4b0, channel 48`、work-channel ok 3、copy object 4、channel error 0、session の socket あり。fault の行は boot の vram-window retry の物だけ（p009b の回にもある）。
- **H.264 の非退行**（行の揃え 64・run の配置の変更の後）: h264-1080p30（High 1080p、600 絵）MATCH 600/600、h264-slices MATCH 3/3。h264-1080・h264-main・h264-pyramid は media-probe が先頭の余分な絵（負の時刻）を出すので数が合わず DIFFER と出たが、ffmpeg の framehash の全部（7・13・12 絵）が media-probe の hash の中にある（解析は `build/tmp/p005-run5`）。
- **H.265 の最初の観測**（受け入れの外、p007 の入力）: skip の形では全部の絵が「the bitstream is out of the CPU's view」で飛ばされた（bitstream の VRAM が BAR1 の窓に無い）→ `nvrtx_vk_buffer_cpu()` で窓に入れるよう直した（実機は未実施）。DXVA の形（`nvrtx.hevc.dxva=1`）では NVDEC が全部の絵を実行し（1 絵 1.6〜10 ms、timeout・hang・GSP の止まり無し）、状態は ERROR、framehash の一致 0。TILES_A（1080p 100 絵）は probe の 120 s の上限で止まった（読み戻しの時間、decoder の timeout の行は無い）。状態の語の log（CTB の誤り数、error、slice header の code、skip の bit、参照の数）を足した（実機は未実施）。
- 使った kernel: 実機で使ったのは 1126250319（`a137d2757`）。その後の窓と log の変更を入れた kernel（1717610756）は実機で未実施。

### 6.5 未実施・残り

- 受け入れの表の「H.265 の無い時の報告は今と同じ」は capset の bit 0 だけの renderer（i915・bcm2711）を変えていないことで、実機の確かめは未実施。
- H.265 の正しい絵（p007）: 窓の直しの後の skip の形、状態の語、filter の区画・上の offset・scaling・tile の確かめ（R4）。
- timeout の後の隔離は実機で起こしていない（timeout が起きなかった）。
- QEMU（Venus）の非退行（video の capset の無い device で H.265 の拡張が出ない）は未実施（libvulkan の門は P2 の範囲）。
