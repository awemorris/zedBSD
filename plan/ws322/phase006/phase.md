<!-- awesome-plan project=zedbsd record=ws322-p006 -->

# ws322-p006 H.265 decode: PowerVR の executor の H.265 の wire の reader と Wave5 の HEVC の道

- 親: [WS322](../ws.md)。設計: [design.md](../design.md) 第 3 版の §7（Wave5 の decoder）・§8（executor）・§14.1・§14.3・§15 の p006。
- Status: cleared（2026-11-02 P2 q1919-i01、Q1 の受け入れ待ち。worktree p2-ws322、branch agent/p2-ws322、基点 main 5d6afb993）
- 依存: p004（H.264 の道、engine の口）、p005（libvulkan の H.265 decode の転送、capset の bit 2。wire は main のまま変えない）。WS324 の libmedia の H.265 の解析（h265-stream・h265-dpb・h265-std・h265sw-*、merge 済み）。

## 目的

VF2 の Wave511 で H.265（Main、8 bit 4:2:0、progressive）を Vulkan Video の decode として出す。p005 が送る H.265 の wire を PowerVR の executor が読み、VPS・SPS・PPS を StdVideo の値から NAL に書き戻して slice の前に置き（stateful の firmware）、p004 の engine の道（ring、pool、生きた slot の frame buffer、pool から GPU の transfer）で decode する。

## 範囲

1. 試験の道具 vkvideo-probe の H.265（WS324 p006i の中身、2026-11-02 Q1 の判断 A で WS322 p006 が持つ。別の commit で先に出し、P1 の nvrtx の確かめにも使う）: libmedia の h265-stream・h265-dpb（17 slot、after_eos で DPB を flush しない: h265_dpb_begin に任せる）・h265-std で StdVideoH265 を作り、DPB の出す順に各 frame の NV12 の SHA-256 を出す（`--expect` は ffmpeg の framehash と比べる）。H.264 の道は変えない。
2. `src/drivers/vpu/vpu-codec.h`・`vpu-h265-ps.c`: StdVideoH265 の値（executor が写す自分の形）から VPS・SPS・PPS の NAL を書く（PTL、DecPicBufMgr、scaling list、short-term の set は明示の形、long-term、PCM、tile、deblocking、range extension の flag。VUI・HRD・timing は書かない: decode の値を変えない）。
3. `wave5-video.c`: stream の codec（H.264・H.265）、H.265 の parameter set の前置き（違う時か IRAP の時に VPS・SPS・PPS）、coded size は SPS の pic_width/height_in_luma_samples、instance は IRAP から、POC は PicOrderCntVal。engine の operations に H.265 decode。
4. executor `vk/vk-video.c`（と H.265 の読みを分けた `vk/vk-video-h265.c`）: H.265 の profile（Main、8 bit 4:2:0）・capabilities（H.265 の maxLevelIdc 5.1、64×64〜4096×2176（level 5.1 の MaxLumaPs）、17 slot・16 参照）・session（std header `VK_STD_vulkan_video_codec_h265_decode`）・parameters（VPS 16・SPS 16・PPS 64、鍵 (VPS, SPS, PPS)）・picture info と DPB slot info の読み、decode の job への写し。capset の bit 2 は engine が H.265 decode を出す時（`render-*.c` の capset、p003 の bit 0 と同じ所）。
5. host 試験 `plan/ws322/tests/p006/`: (T1) HEVC_v1 の 147 本の parameter set を h265sw-ps → h265-std → executor の形 → vpu-h265-ps → h265sw-ps で読み直して decode に効く field が全部一致、(T2) libvulkan の H.265 の encoder の bytes を vk-video の reader で読み直す（全 field）、(T3) engine の H.265 の道（偽の core）。p004・p005 の host 試験を流し直す。
6. 実機（VF2 10.0.30.2）: HEVC_v1 の Main の stream（class in の 136 本と派生 1 本のうち Wave511 の限り（Main、≤ 4096×2176、level ≤ 5.1）に入る物）を vkvideo-probe --expect で host の ffmpeg の NV12 の SHA-256 と照らす。1 本の script（配置 → 起動 → 実行 → 回収 → 要約）。p004・p007 の回帰を同じ板で流す。
7. （2026-11-02 Q1 の依頼で足した、ユーザーの範囲の拡げ）libmedia の Vulkan Video の H.265 の back end（`userland/desktop/libmedia/vkvideo-h265.c`、vkvideo-runtime.c を codec に依らない job にする）と、VF2 の Video Player の GPU の道（`VP_GPU_PRESENT`）での H.264・H.265 の再生（p012 の decode の部分）。device に依らない形（能力は device から、P1 の RTX・P3 の Pi 5 も使う）。
- 範囲の外: Main10・4:2:2・interlaced（field_seq）、encode（p008〜）、wire・libvulkan の変更（WS324 と共有、変える時は Q1 経由）、速さの測り（p012）。

## UAPI・HAL

- HAL は変えない。`include/uapi/` は変えない（opcode は H.264 と同じ、capset の bit 2 は p005 の libvulkan の約束）。toolchain は触らない（worktree の sysroot の写しは p005 で作った物）。

## 受け入れ

| 項目 | 確かめ方 |
| --- | --- |
| host 試験 | `plan/ws322/tests/p006/run.sh` が全部 0 failed（T1 は 147 本の全部の parameter set）。`p004/run.sh`・`p005/run.sh` が前と同じ |
| build | VF2 の既定の config と `config-vf2-video.mk`・`config-vf2-video-test.mk` の vmunix と vkvideo-probe ほかが warning 0。amd64 の vkvideo-probe も warning 0 |
| HEVC の適合 | VF2 で vkvideo-probe --list が `VK_KHR_video_decode_h265` と codecs 0x3。Wave511 の限りに入る Main の各 stream が全部の frame で ffmpeg と一致するか、一致しない物・断る物はそれぞれ原因（firmware の限り、stream の性質）を示す。stream ごとの表を下の結果に |
| 非退行 | p004・p007 の板の回帰（H.264 の hash、JVT、立て直し、vf2-regress）が前と同じ形 |
| 後始末 | 板の vmunix・設定を元に戻し cksum が一致 |

## 結果（2026-11-02 P2、q1919-i01）

### commit（基点 main 5d6afb993）

| SHA | 中身 |
| --- | --- |
| 52208da06 | vkvideo-probe の H.265（WS324 p006i の中身、Q1 の判断 A）: `h265.c`・`probe.h`、main.c は codec の分岐と共有の関数の公開だけ |
| 92501c821 | kernel: `vpu-bits.c/.h`（共有の bit writer、vpu-h264-ps.c をその上に）、`vpu-h265-ps.c`、`vpu-codec.h`、`wave5-video.c`（H.265 の stream）、`powervr.h`（job に VPS・SPS・PPS）、`video-state.c/.h`（capset bit 2）、`vk/vk-video.c`・`vk/vk-video-h265.c/.h`（executor の reader）、`platform/riscv64/vmunix.mk`、host 試験 `tests/p006/`、p003・p004 の試験の一覧、`plan/tools/powervr/p009/run.sh` の一覧に `vk/vk-video-h265` |
| 78456b0cb | 板の道具 `make-hevc.sh`・`run-vf2-hevc.sh`、vk-video.c の comment の折り返し |
| ed4083968 | libmedia の H.265 の back end `vkvideo-h265.c`、`vkvideo-runtime.c/.h` を codec に依らない job に（H.264 は今までの関数が job を作る）、`vkvideo.c` が H.265 の track を `media_vkvideo_ops` から渡す（decoder.c の表と共有の host 試験の stub は変えない）、`sources.mk`、host 試験 `libmedia-h265-host-test.c` |
| df9ad3a00 | kernel: H.265 の frame buffer の幅を 16 に丸める（`wave5-decode.c` の read_sequence） |
| 最後の commit | `make-play.sh`・`run-vf2-play.sh`・`split-pictures.py`、run-vf2-hevc.sh の拡げ（任意の directory、mp4 の media-probe）、この phase.md・ws.md |

### host 試験・build

| 項目 | 結果 |
| --- | --- |
| `sh plan/ws322/tests/p006/run.sh build/tmp/p006-t3` | `p006: PASS (147 streams)`、441 の run が全部 PASS: HEVC_v1 の 147 本それぞれで (1) VPS・SPS・PPS の往復（h265-std → libvulkan の wire → executor の reader → Wave5 の writer → libmedia の parse で読み直し、decode に効く field が全部一致。746 組、絵の記録 9235、128463 checks）、(2) libmedia の back end を 1 packet で、(3) access unit ごとの packet で（出す順が reader と DPB だけの順と一致、参照の slot がその POC の絵を持つ、setup の slot が使用中でない。16 本は断る: Main10 など 12、PICSIZE 4）。engine-h265 21 passed |
| 試験の効き | writer の 11 の改変が全部 corpus で落ちた（m0〜m10） |
| 回帰 | `p004/run.sh`: ps-slots 4094（rpi4 の writer と byte で一致、bit writer の共有化の後）・engine 56・wire 33・state 70・executor 505・nv12 752 全部 0 failed。`p005/run.sh`: 65375・26。`plan/tools/powervr/p009`: 505、`p010`: 0 failed |
| build | `config-visionfive2.mk`・`config-vf2-video.mk`・`config-vf2-video-test.mk` の vmunix と programs、amd64 の vkvideo-probe と libmedia.so が warning 0 |

### VF2 実機（10.0.30.2）

- 全部の run の後に板を元に戻し cksum を照らした: `/boot/boot0/vmunix` 3758577736（= vmunix.q1825orig）、`/etc/group` 4260090434、`zedbsd.cfg` 632416842、`/lib/firmware/wave511` は無い、/tmp は空。

| run | kernel | 結果 |
| --- | --- | --- |
| smoke（08:24、`build/tmp/p006-vf2-s1`） | 3794672705 | 5 本全部の frame が一致（AMP_A 2560x1600、SLIST_A、NUT_A、TSCL_A、WPP_A）、vkcompute PASS |
| 全 146 本（08:27、`build/tmp/p006-vf2-f1`、`run-vf2-hevc.sh`） | 3794672705 | 128 本が全 frame 一致、15 本は理由を出して断る（Main10 の 10 本・TSUNEQBD は ffmpeg が絵を出さず列に無い、IPRED_B は Main Still Picture、PICSIZE_A〜D は 4096x2176 より大きい）、3 本が不一致（下） |
| 再生と診断（09:01、`build/tmp/p006-vf2-p1`、`run-vf2-play.sh`） | 3355858240（df9ad3a00） | 下の表 |
| 大きさの変わる流れの診断（09:16、`build/tmp/p006-vf2-d1`） | 3355858240 | 下 |

不一致の 3 本:

| stream | 原因 | 結果 |
| --- | --- | --- |
| VPSSPSPPS_A_MainConcept_1（6 枚の I、全部大きさが違う） | 参照の誤り: ffmpeg は流れ全体を decode すると後の大きさの絵を最初の大きさに拡大縮小して hash する（`-autoscale` の無い版）。各絵を単独で decode した参照（`split-pictures.py`、parameter set を前に並べ替えた写し）とは 6/6 一致 | 一致 |
| SAODBLK_A・B_MainConcept_4（1016x760、500 枚） | 幅が 16 の倍数でない。frame buffer の幅を 16 に丸めた（df9ad3a00）後、x265 の 1016x760・1000x562 は 60/60 一致したが、SAODBLK は変わらず不一致。ffmpeg の decode の md5 が zip の md5（参照の YUV）とも違う（A: ffmpeg 23f03fff…、zip b9c98937…）ので ffmpeg の参照も疑わしい | 未決（下の「未実施」） |

再生（09:01〜09:14、`run-vf2-play.sh`、kernel 3355858240、libmedia ed4083968）:

| 項目 | 結果 |
| --- | --- |
| H.264 の非退行（libmedia の runtime を job にした後） | media-probe h264-1080p30.mp4 600/600 一致、JVT の MP4（CAPM3 300・CAWP1 90・MR2 300・CVSE3 278）が JVT の参照と全部一致、Video Player GPU の道 1080p30 16 s: shown 479・late 2（p004 と同じ）、compositor の誤り 0、CPU 約 22%、PNG `h264-1080p30.png` cksum 1380561032 |
| H.265 media-probe | h265-1080p30（x265、B、20 s）600/600 一致、h265-1080p60（10 s）600/600 一致、DSLICE_A の MP4 50/50（Annex B の参照と）、backend=vulkan-video、engine の誤り 0 |
| H.265 Video Player GPU の道 | 1080p30 16 s: shown 478・late 3、compositor の誤り 0、PNG `h265-1080p30.png` cksum 480165450。1080p60 10 s の clip: shown 566 / 600、late 34（始めの約 5 s、H.264 の p007 と同じ形） |
| H.265 の大きさの変化 | x265 の 3 つの大きさの流れ（上り・下り）と H.264 の同じ物: vkvideo-probe も media-probe（新しい context）も各部分の参照と 90/90 一致（流れ全体の ffmpeg の参照は上の理由で誤り） |
| 断る物 | RAP_B_Bossen_2 の MP4 は libmedia が断る（SPS の level 6.2 > device の 5.1、H.264 と同じ規則）。vkvideo-probe は level を見ないので decode し一致 |

p004・p007 の回帰（`PATH=/usr/bin:/bin:/usr/local/bin VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=… VF2_PLUG=AC276E432AE6 sh plan/ws322/tests/p007/run-vf2.sh build/tmp/p006-reg7 build/ws322-p006t build/tmp/p004-clips2 build/tmp/p007-clips build/tmp/p007-jvt build/tmp/vpu-fw`、09:21〜09:44、kernel 3355858240）: vf2-regress（vkmatrix 109/109、`-p 3` PASS、`-r 3` 327/327、vkcoherency PASS、vkcompute 4096/4096、desktop の compositor error 0・起動 1、keiland-shot の PNG cksum 518389155 は p007・p007b と同じ）。S1 1080p30 平均 13.5 ms・1080p60 13.2 ms・2160p30 42.4 ms、1080p60 120 枚・2160p30 30 枚が一致。F1 240 枚一致（flush 4）・600 枚で flush 10。F2 2 session 240 枚・誤り 0。F3 立て直しの後に次の IDR から続く。F4 ×1.0 は誤り 269、×1.05〜1.25 は誤り 0、後の 60 枚一致。F5 は 3 回で止まり、以後 session は「opened no stream (5)」、vkcompute PASS。F6 の SIGKILL の後も続く。V media-probe 1080p30 600/600 一致。VP 1080p30 16 s に 480 枚・late 1、1080p60 899 枚・late 64（p007 は 930・late 32、p007b は 1179・late 21 の 20 s の別の道具。始めの数秒の late の形は同じ）、compositor の誤り 0。J は 100/101（BA3_SVA_C だけ、Extended を理由つきで断る）。板は元に戻し cksum 一致。p004 の `run-vf2.sh`（ws083 の 7 本、--sampled）は別に流していない（H.264 の decode の道は上の S1・J・V で見た）。

### 判断（範囲の中の技術）

- H.265 の parameter set は VUI・HRD・timing を書かない（decode の値を変えない）。short-term の set は常に明示の形（h265-std が両方の形を持つ約束）。PTL の compatibility は自分の profile（Main は Main10 も）。
- Wave5 には IRAP の前に VPS・SPS・PPS を全部、それ以外は変わった set から後を送る（H.264 の規則の一般化）。
- H.265 の frame buffer の幅は 16 に丸める（vendor は 32。H.264 で 16 の倍数が通ることが分かっているので 16）。
- executor の H.265 の能力: 64x64〜4096x2176（level 5.1 の MaxLumaPs）、slot 17・参照 16、level 5.1。
- libmedia: decoder.c の表を変えず、`media_vkvideo_ops` の中で codec の印で H.265 の back end へ渡す（plan/tools の host 試験の stub を変えずに済む）。H.265 の back end は DPB の出す順で絵を出し、after_eos で DPB を空にしない。大きさ・profile・DPB の違う SPS は IRAP で新しい context。
- vkvideo-probe の H.265 は level を見ない（能力の大きさだけ）。libmedia は level で断る（H.264 と同じ）。

## 未実施

- SAODBLK_A・B の正否: ffmpeg の参照が zip の md5 と合わないので、板の出力の I420 の md5 を zip の md5 と照らす道具（vkvideo-probe の出力の md5、または dump）が要る。P1 が vkvideo-probe に copy-out を足しているので、その後に足すのが良い。
- HEVC の参照を全部 zip の md5 で照らすこと（今は ffmpeg の NV12 の SHA-256）。
- Video Player の H.265 1080p60 の始めの late（約 5 s）の改善と 4K（p012）。
- p004 の `run-vf2.sh`（ws083 の 7 本、--sampled の GPU の plane の copy）は流していない。
- Main10（driver が出さない、範囲の外）。
