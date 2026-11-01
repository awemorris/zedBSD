<!-- awesome-plan project=zedbsd record=ws322-p004 -->

# ws322-p004 H.264 decode の道: engine の口、Wave5 の decoder、parameter set の書き出し、executor、Video Player の GPU の道

- WS: [WS322](../ws.md)。状態: cleared（2026-11-02 01:53 P2 q1869-i01。Q1 の受け入れ待ち）。担当: P2（q1869-i01、worktree p2-ws322、branch agent/p2-ws322）。
- 設計: [design.md](../design.md) 第 3 版の §7（Wave5 の decoder）・§8（executor）・§9（memory）・§11（失敗）・§14.3・§15 の p004。

## 目的

VF2 の Wave511 で H.264 を Vulkan Video の decode として出し、libmedia と Video Player の GPU の道（WS243 の `VP_GPU_PRESENT`）に載せる。p002 の decoder の core と p003 の NV12・export・import・query の上に、engine の口、Wave5 の decoder の session（ring・sequence・pool・index の厳密な選び）、SPS・PPS の書き出し、executor の session・parameters・capabilities・format の報告・記録・実行（pool から GPU の transfer で宛先へ）を足す。

## 範囲

1. engine の口（`powervr.h`）: `drv_powervr_video_ops` を open・decode・keep・reset・close に広げ、pool の callback（executor が render node の buffer で作る）、decode の job（bitstream、SPS・PPS、IDR・intra の印、POC、setup・参照・active の slot）と result（status、pool の index、大きさ、時間）。`video-state.c` に登録された engine を引く口。
2. `src/drivers/vpu/vpu-codec.h`・`vpu-h264-ps.c`: StdVideo の値から SPS・PPS の NAL を書く（VUI 無し、no-reorder の VUI の選択肢つき）。`vpu-slots.c`: DPB の slot と frame buffer の結び、生きた slot に結ばれない frame buffer を LRU で選ぶ。
3. `wave5-decode.c`: ring を続けて使い、終わりで折り返して書く（`drv_wave5_append_pieces`）、断られた絵の byte を取り戻す（`drv_wave5_unread`）。
4. `src/drivers/vpu/wave5-video.c`・`.h`: engine の実装（stream ＝ session、最初の I の絵で instance を開く、parameter set を新しい時か IDR で前に置く、INIT_SEQ で pool を作らせ登録、frame buffer の選びと display flag、decode と結果の読み、断り・skip・不一致・timeout の扱い、大きさの変化と reset で instance を作り直す）。`vf2-vpu.c` の boot の thread で登録。
5. executor `vk/vk-video.c`: capabilities（COINCIDE、SEPARATE_REFERENCE_IMAGES、16×16、64×64〜4096×2304、slot 17・参照 16、level 5.1）、format properties（NV12、MUTABLE | EXTENDED、DST | DPB | SAMPLED | TRANSFER_SRC）、session（engine の stream、binding 1 つ 4 KiB）、parameters（SPS 32・PPS 256、全部の field）、BEGIN・END・CONTROL・DECODE の記録と実行、query への報告。`vk-transfer.c` に 2 つの pitch の行の写し、`vk-queue.c`・`vk-command.c`・`vk-stream.c` の hook。
6. riscv64 の Video Player の GPU の道: `VP_GPU_PRESENT` は全 arch で既に 1（`videoplayer.h:57`、arch の条件は無い）。libmedia が PowerVR で GPU の道を選ぶか（external memory、COINCIDE、同じ family、SAMPLED の NV12 と plane の flag）を実機で確かめる。
- 範囲の外: H.265（p005・p006）、非同期・SLC の見直し・命令の queue（p007）、encode（p008〜）、core の reset の line の保持と firmware の boot し直し（§11 の timeout の道、下の「未実施」）。

## UAPI・HAL

- HAL の API は変えない。`include/uapi/` は変えない（使う opcode・wire は全部既にある。capset の bit は p003 の H.264 decode の bit 0 だけ）。

## 手順

1. 上の 1〜5 を実装。
2. host 試験 `plan/ws322/tests/p004/run.sh`: ps-slots（rpi4-h264.c の書き出しと byte で比べる 2000 回、固定の 1080p の SPS、範囲の外の拒否、slot の選び）、engine（偽の core で全ての結果の道）、wire（libvulkan の video の encoder の bytes を vk-video.c の reader で読み直す、`plan/tools/rtx-video/wire-encode.c`）、p003 の state・executor・p009。
3. build: VF2 の既定の config と `plan/ws322/tests/config-vf2-video.mk`（vf2-regress の build に `CONFIG_DRIVER_VF2_VPU=y` と media-probe・vkvideo-probe）で warning 0。
4. 実機: `plan/ws322/tests/p004/make-clips.sh`（流れと参照）、`run-vf2.sh`（firmware を置く、vf2-regress で起動、V0〜V3、元に戻す）。

## 受け入れ

| 項目 | 確かめ方 |
| --- | --- |
| host 試験 | `run.sh` が全部 0 failed |
| build | VF2 の既定の config と試験の config の vmunix・program が warning 0 |
| decode の hash | VF2 で vkvideo-probe --expect: ws083 の 7 本（64×64 Baseline、352×288 の Main・High の B pyramid・slices・scaling matrix）と p002 の 1080p High 10 frame が host の ffmpeg の NV12 の SHA-256 と全 frame 一致、--sampled（GPU の plane の copy）も一致 |
| libmedia の道 | media-probe --video-hash で 1080p30 High 20 s（600 frame、ring が数回折り返す）が host の ffmpeg と全 frame 一致 |
| Video Player の GPU の道 | Video Player が 1080p30 を再生: `PRESENT own=1 gpu_pictures=1`、libmedia の「pictures the GPU holds」、FRAMES の shown と late、画面の PNG |
| 非退行 | vf2-regress（vkmatrix・vkcoherency・vkcompute・desktop の PNG）が p003 と同じ形 |

## 結果（2026-11-02 P2、q1869-i01）

### commit（基点 main 160442a06）

- 007b218ea（実装と host 試験）、d89f9d6f1（実機で分かった直し: NV12 の全体の view、firmware に FB を選ばせる、UPDATE_BS、no-reorder の VUI、task buffer を 2 倍、診断の行、板の試験の道具）、最後の commit（この phase.md・ws.md、Q1 への報告に SHA）。

### 確かめた事（host・build）

| 項目 | 結果 |
| --- | --- |
| host 試験 | `sh plan/ws322/tests/p004/run.sh build/tmp/p004-host6`: ps-slots 4094 passed・engine 48・wire 33・p003 state 69・executor 505（p009）・nv12 752、全部 0 failed。`plan/tools/powervr/p009/run.sh` 505・`p010/run.sh` 全部 0 failed（007b218ea の時点） |
| build | `config/ci/config-visionfive2.mk`（build/ws322-p004d）、`plan/ws322/tests/config-visionfive2-vpu-probe.mk`（build/ws322-p004p）、`plan/ws322/tests/config-vf2-video.mk` の vmunix と wayland・files・terminal・keiland-shot・vkmatrix・vkcoherency・vkcompute・media-probe・vkvideo-probe・videoplayer（build/ws322-p004v）が warning 0 |

- 固定の 1080p の SPS の期待の byte は 7.3.2.1.1 の syntax から手で書いた物（最初の手の計算の誤りを直した）。

### 確かめた事（VF2 実機 10.0.30.2、最後の通し 01:46〜01:52、`build/tmp/p004-vf2-final`）

`VF2_HOST=kei@10.0.30.2 VF2_PLUG=AC276E432AE6 sh plan/ws322/tests/p004/run-vf2.sh build/tmp/p004-vf2-final build/ws322-p004v build/tmp/p004-clips2 build/tmp/vpu-fw`（kernel cksum 4269587702）。

| 項目 | 結果 |
| --- | --- |
| engine の登録 | dmesg に `vf2-vpu: WAVE 5110 firmware 0002d46a`、`vf2-vpu: video decoder is the render node's engine`。vkvideo-probe --list: family 0 flags 0x27、codecs 0x1、`VK_KHR_video_queue`・`_decode_queue`・`_decode_h264` |
| decode の hash（vkvideo-probe --expect） | ws083 の 7 本（i-baseline-64 3、p-baseline-64 10、i-high-352-cqm 3、i-main-352-slices 3、pb-high-352-pyramid 15、pb-main-352 15）と p002 の 1080p High 10 frame が全 frame 一致、mismatch 0・incomplete 0。--sampled（GPU の plane の copy）の pb-high-352-pyramid 15・h264-1080 10 も一致。1080p の 1 枚は vkvideo-probe の記録から fence まで平均 31 ms（最初の 1 枚は instance と pool の作成で 190 ms） |
| libmedia の道（media-probe --video-hash） | 1080p30 High 20 s（8 Mbit/s、B、IDR 60 枚ごと）の 600 frame が host の ffmpeg と全部一致（MATCH 600 of 600）、backend=vulkan-video、engine の誤り 0。12 枚ごとの平均 submit 約 10 ms |
| Video Player の GPU の道 | `PRESENT own=1 gpu_pictures=1`、`pictures the GPU holds, 14 slots`。16 s で shown 479・late 2（約 30 fps）、decode 60 枚／2 s、engine の誤り 0、compositor の誤り 0。`top` の CPU は約 20%。画面の PNG（`build/tmp/p004-vf2-final/v3.png`、縮小 `v3-small.png`）に testsrc2 の絵 |
| 非退行（vf2-regress、同じ起動） | vkmatrix 109/109、`-p 3` PASS、`-r 3` 327/327、vkcoherency PASS、vkcompute 4096/4096、desktop の compositor error 0・起動 1、keiland-shot の PNG cksum 518389155（p003 と同じ）、fault・lost の行なし |
| 板の後始末 | `/boot/boot0/vmunix` 3758577736（= vmunix.q1825orig）、`/etc/group` 4260090434、`zedbsd.cfg` 632416842、`/lib/firmware/wave511` は無い（試験の間だけ置いた）、/tmp は空 |

### 実機で分かった事と直し（r1〜r10、`build/tmp/p004-vf2-r1`〜`r10`）

| 回 | 観測 | 直し |
| --- | --- | --- |
| r1 | vkvideo-probe と libmedia が `vkCreateImageView` で −3（libmedia は software に落ちた） | Vulkan Video の picture resource は NV12 の全体の形式・COLOR の aspect の view。p003 は断っていた。decode の usage（DST・DPB）の image に限り、texture の語の無い view として許した（`vk-image.c` の `video_view`） |
| r2〜r9 | vkvideo-probe の短い流れは全部一致。長い 1080p の流れで、ある枚数（c-same 69、c-nob 57、c-clean 89、media-probe と Video Player も 69）で DEC_PIC が完了せず（500 ms、queue に 1 つ残る、BSBUF_EMPTY の割り込み無し）。ring 1 MiB（2 回折り返し）でも同じ枚数、FB を driver が選んでも firmware に選ばせても、no-reorder の VUI でも、各 append の後の UPDATE_BS でも同じ。止まるのは流れの bitstream が約 2.1〜2.4 MB を越えた所 | — |
| r10 | task buffer（VLC × 2 ＋ parameter × queue 深さ、vendor の式）を 2 倍にすると全部の流れが最後まで通る | task buffer を 2 倍（`wave5-decode.c` の `TASK_FACTOR`）。firmware の報告は vlc 6789444・parameter 985456 で、1 instance に約 31 MB |

- r2 以降も残した直し: firmware に生きた slot の FB 以外から選ばせる（生きた slot の FB にだけ display flag を立て、他は下ろす。firmware が持っている FB を driver が選ぶと断らずに待つ恐れがあるため）、各 append の後に UPDATE_BS（vendor の driver と同じ）、SPS に no-reorder の VUI（design §16 R1 の代わりの形、出力は変わらない）。どれも最後の通しで確かめた形。

## 判断（範囲の中の技術、design との差）

- task buffer は vendor の式の 2 倍。理由は上の r10。firmware がなぜ足りないかは分からない（firmware は解析しない）。memory が大きい（1 instance 約 31 MB、64×64 でも firmware は同じ vlc を報告する）ので、必要な最小を p007（速さ・memory、R8）で測る。

- RESET（CONTROL）と SPS の大きさの変化と INIT_SEQ の失敗は、instance を閉じて次の I の絵で作り直す（design §7.4 の FLUSH_INSTANCE で FBC を保つ形はとらない）。p002 で firmware の read の位置を戻せないと分かっており、FLUSH の後の位置の扱いを実機で確かめていないため、確かめ済みの道（CREATE・INIT_SEQ・SET_FB）だけにした。費用は seek ごとの再登録（1080p で FBC の組 7 枚の確保）。FLUSH は p007 で。
- frame buffer は生きた slot に結ばれた物にだけ display flag を立て、残りから firmware が選ぶ（design §7.5 の driver が 1 つを選ぶ形から変えた。`vpu_slots_pick` は空きが有るかの確かめに使う）。
- 断られた絵（decoded −1）は ring に入れた byte を取り戻す（firmware は読んでいないので、次の絵を同じ所に書く）。
- parameter set は前に送った物と byte で違う時か IDR の時に前に置く（bcm2711 と同じ）。SPS には no-reorder の VUI（max_num_reorder_frames 0、max_dec_frame_buffering は level の MaxDpbFrames）を書く（FORCE_FB_LATENCY_PLUS1 と合わせて出す順を decode の順に）。
- ring は 4 MiB、1 枚は ring − 1 KiB まで。試験の build は `-DWAVE5_VIDEO_RING_BYTES=` で小さくできる。
- `plan/tools/powervr/p009/run.sh` の file の一覧に `vk/vk-video` を足した（vk-stream.c・vk-command.c・vk-queue.c が vk-video.c を呼ぶので、足さないと link が落ちる）。

- 試験の道具: `plan/ws322/tests/p004/make-clips.sh`（ws083 の流れと参照、p002 の 1080p と ffmpeg の framehash、1080p30 の MP4 と nv12-hashes.py の参照）、`run-vf2.sh`（firmware を置く、vf2-regress、V0〜V3、元に戻す。`SKIP_REGRESS`・`NO_V1`・`KERNEL`）、`plan/ws322/tests/config-vf2-video.mk`。

## 未実施

- §11 の DEC_PIC の timeout の道の core の reset（reset の line の保持、firmware の boot し直し、3 回で止める）。v1 は timeout の stream を壊れたとし（以後 EIO）、pool を session の破棄の後も残す（VPU が書くかもしれないため）。
- §14.3 の失敗の道の実機の項目（fault の注入、2 session の同時の decode、途中の fd の close）。
- JVT の適合の stream（WS326 の software decoder との照合）。
- seek（CONTROL の RESET を流れの途中で）の実機の確かめ。vkvideo-probe・media-probe・Video Player の試験は最初の RESET だけを通る。
- 1080p60・4K の decode（p007）。
- `CONFIG_DRIVER_VF2_VPU` の既定は n のまま（試験の config だけで y）。既定を y にするかは Q1 の判断（下の問い）。

## 問い（Q1 へ）

- `CONFIG_DRIVER_VF2_VPU` を VF2 の既定の config で y にするか。design §20 は「p004 の実機の確かめまで n」。y にすると、`/lib/firmware/wave511/chagall.bin` が無い板では log 1 行で video 無しのまま（GPU は今どおり）、有る板では render node が H.264 decode を出し、libmedia が Vulkan Video を使う。firmware の package（`wave511-firmware`）を image に入れるかも合わせて。
