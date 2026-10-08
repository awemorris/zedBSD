<!-- awesome-plan project=zedbsd record=ws083-p004 -->

# ws083-p004: MFX AVC の I frame の builder、NV12 Tile Y、genxml の独立の decoder、試験の stream、vkvideo-probe

Status: in-progress（2026-10-08 q902 P1 の照合: §8.2 の QEMU 回帰は T1-371 PASS（Q1 判定、ws.md の表）。review の R-S1 の直し（q897）は host のみ。実機の hash は p005（T1-435、未実行））（旧: in-progress（q857-i01、P2。2026-10-08 実装と host 試験は済み、§8.2 の QEMU 回帰は T1 待ち））
Disposition: normal
Parent: [WS083](../ws.md)

## 範囲（Q1 の ACK 2026-10-08）

[design.md](../design.md) 第 3.1 版 §9 の p004: `intel/genxml-video.h`（H4）、NV12 の Tile Y の image、§6.3 の MFX の命令の builder、`genxml-decode.py`（D24）、試験の stream と `make-streams.sh`（H5・HD4 (a)、host の encoder で合成の絵から作る物だけ）、`userland/tests/vkvideo-probe`（host で build、§8.2 の `--list`）。受け入れは §8.1 の 4・5 行目（I frame の 2 本以上）、build warning 0、T1 の §8.2。Q1 の追加の許可: `platform/amd64/vmunix.mk`（source と link 規則）、`plan/ws031/tests/i915-vk-render-stubs.inc`（既定で無効の stub）。

## 実装

- `src/drivers/gpu/i915/intel/genxml-video.h`（新）: MI_FLUSH_DW・MI_FORCE_WAKEUP・MFX_WAIT と MFX/MFD の 12 命令の opcode・長さ・field の dword と bit（Mesa 25.0.7 の gen120 → gen110 → gen90 → gen80 → gen75 の解決、出典と SHA-256）。
- `render/video-mfx.[ch]`（新）: 解決済みの decode（`struct i915_video_mfx_decode`）から §6.3 の命令列を batch に書く純粋な builder と、scaling list の導出（Table 7-2 の fall-back A・B、useDefaultScalingMatrixFlag、無ければ Flat_16）。scan 順 → raster 順の並べ替え（B2）、PICID の 0xffff、使わない参照は出力の picture と書き込みの MV buffer（D21）、bitstream の upper bound は buffer の bind の範囲の終わり。H.264 の SPS・PPS の struct は video.c からここへ移した。`render/video-h264-tables.c`（新）: zig-zag（Table 8-12・8-13）と default の list（Table 7-3・7-4）。
- `render/video.c`: 検べを通った decode を解決（出力・参照の image、row store・MV の binding、bitstream の page・skew・終わり、slice の NAL の先頭と終わり）して builder で session の video batch（32 KiB、最初の decode で作る）に書き、VCS0 で run。batch を作れない時は submit が `VK_ERROR_OUT_OF_DEVICE_MEMORY`。D17 の 6 に CbCr の行の一致を追加、NV12 の検べは `planar` も見る。
- `render/image.c`・`gfx.h`: NV12 = Tile Y の picture（video の device だけ、2D・1 level・1 layer・1 sample・OPTIMAL・flags 0・4096 以下・usage は DST/DPB だけ）。layout: extent を MB に切り上げ、pitch は 128 B の倍数、Y の行は 32 の倍数、CbCr はその下、全体は page の倍数。`vkGetImageSubresourceLayout` の PLANE_0/1（N3）。`drv_i915_gfx_image_slice` は NV12 を拒む（copy・blit・clear・sampling・描画に出さない）。
- `render/instance.c`: NV12 の format feature（video の device で OPTIMAL に DECODE_OUTPUT・DPB、D23）、NV12 の image format properties（2D・OPTIMAL・video の usage だけ、4096・1 level・1 layer・1 sample）。
- `platform/amd64/vmunix.mk`: 2 file、vkvideo-probe の link 規則（vkdemo と同じ形）。
- `userland/tests/vkvideo-probe/`（新）: `--list`（family の flags・codec、video の拡張、要約の行 `video families N, video extensions M`）と decode（`h264.c` の Annex B の reader: SPS・PPS（scaling list を含む）・slice header の frame_num・idr_pic_id・POC type 0/2、`frame.c` の Tile Y の de-tile・crop・SHA-256、`--expect=FILE` で参照と比べ exit status で合否）。I の IDR の picture だけ（P・B は p006）、picture ごとに RESET して slot 0 に setup。Linux の loader は拡張の関数を export しないので host の Linux では link しない（zedBSD の libvulkan は export する）。
- 試験の stream（`plan/ws083/tests/streams/`、合計 約 30 KiB）: `make-streams.sh` が ffmpeg 7.1.5-0+deb13u1・libx264 0.164.3108+git31e19f9 で testsrc・mandelbrot から作る: i-baseline-64（64x64、CAVLC、1 slice）、i-main-352-slices（352x288、CABAC、4 slice）、i-high-352-cqm（352x288、8x8 変換、PPS の非対称の scaling matrix、list 2・5 は fall-back）。各 3 frame、全 IDR、参照は ffmpeg の NV12 の frame ごとの SHA-256。cqm の file も一緒に置く。
- 試験の道具: `genxml-decode.py`（D24、Mesa の genxml を SHA で固定して data として読み、import を解決、batch を命令と field に戻して期待と比べる、`--self-test` で解決の事実を検べる）、`host-mfx-avc.c`・`run-host-mfx-avc.sh`、`host-vkvideo-probe.c`・`run-host-vkvideo-probe.sh`、`host-video-executor.c` に batch の取り出しと NV12 の layout、`config-video-qemu.mk`（§8.2 の image）、`config-video-hw.mk` に vkvideo-probe。
- 他の WS の host 試験（ws031 `run-vk-host-tests.sh`、ws075 `run-host-layered.sh`、ws101 `host/run.sh`）の executor の source の並びに video-mfx・video-h264-tables を足した（無いと link できない）。

## 確認

| コマンド | 結果 |
| --- | --- |
| `python3 -I plan/ws083/tests/genxml-decode.py --mesa build/mesa-tools/mesa-25.0.7 --self-test` | 0 failures（Gen12 の IMG_STATE は gen110 の 21 dword、DPB_STATE は gen90、DIRECTMODE は gen80、PICID は gen75、MFX_WAIT の subtype 1、3DSTATE_CPS の exclude） |
| `sh plan/ws083/tests/run-host-mfx-avc.sh` | 4 case（intra-cqm 548・references 406・flat 535・defaults 535 の check）とも 0 failures、plain・ASan/UBSan。builder の変異 4 種（pitch の shift、zig-zag の逆、D21 の 0、long term の bit）は全て検出 |
| `sh plan/ws083/tests/run-host-video-roundtrip.sh` | PASS（p003b の全 case ＋ IDR と P の batch 36 命令 89 checks を genxml で、NV12 の layout 16x16・1920x1080・4096x4096・100x50、NV12 は image_slice が拒む） |
| `sh plan/ws083/tests/run-host-vkvideo-probe.sh` | 3 本の stream とも、reader が全 picture（IDR・intra・slice の数）と cqm の PPS（list 0xdb）を読み、ffmpeg の frame を Tile Y にして de-tile・hash した値が参照と一致（plain・ASan/UBSan） |
| `sh plan/ws031/tests/run-vk-host-tests.sh`、`sh plan/ws075/tests/run-host-layered.sh`、`sh plan/ws101/tests/host/run.sh` | PASS |
| `make -j16 BUILD=build/p2-k ZEDBSD_CONFIG=config/ci/config-amd64.mk build/p2-k/vmunix` | 成功 warning 0（kernel include check・vmunix check PASS） |
| `make … ZEDBSD_USER_PROGRAMS="libvulkan vkvideo-probe" build/p2-k/bin/vkvideo-probe` | 成功 warning 0（-Wall -Wextra -Werror、check-dynamic-elf PASS） |

未実施: QEMU の回帰（§8.2、T1 に依頼）、実機（p005）。MFX の命令が実機で正しく decode するか（U1・U5・U16）、upper bound の意味（新 U18: 実機で確かめる）は p005。

## 残り・次

- §8.2 の T1 の結果で cleared の判定（Q1）。
- p005: 5330 の実機で VCS の bring-up と I frame の hash（`config-video-hw.mk`、`vkvideo-probe --expect`）。ユーザーの変換した `/home/awe/zedbsd-media/sample-h264-*.h264`（1920x1080、tree の外）は最初の IDR だけ（`--frames=1`、参照は host の ffmpeg で作る）を p005 で、全体は p006 で。

## T1-371 の判定（2026-10-08 Q1）

PASS（QEMU Venus、main b84e680a8）: boot-test、compositor（zdesktop-p054）、vkdemo（frames=23、offscreen の rgb_sha256 は T1-313 と同じ）、`vkvideo-probe --list` は `video families 0, video extensions 0`（family 0 だけ、sync2 の行無し）、存在しない file で exit 1。QEMU の回帰（§8.2）を満たす。実機は p005。
