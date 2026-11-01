<!-- awesome-plan project=zedbsd record=ws322-p007 -->

# ws322-p007 VF2 の VPU の速さと頑丈さ: timeout の後の立て直し、RESET の flush、task buffer の大きさ、速さ、JVT の照合

- WS: [WS322](../ws.md)。状態: cleared（2026-11-02 03:45 P2 q1896-i01。Q1 の受け入れ待ち。非同期・命令の queue は測りの結果で入れなかった、下の判断）。担当: P2（worktree p2-ws322、branch agent/p2-ws322）。
- 設計: [design.md](../design.md) 第 3 版の §7.4（RESET）・§9.2（SLC）・§11（失敗と回復）・§14.3（失敗の道と p007 の実機の項目）・§15 の p007・§16 R8・R9・R12。

## 目的

p004 の H.264 decode の道を、`CONFIG_DRIVER_VF2_VPU` を既定で y にできる頑丈さにし、速さを測る。

## 範囲

1. design §11 の DEC_PIC（と命令・sequence）の timeout の後の立て直し: core を止め（software reset、reset の line の保持）、全 instance を失わせ、firmware を起こし直す。続けて 3 回なら line を保持したまま domain を off にし、engine を止める（以後の session の作成は EIO）。§14.3 の失敗の道を実機で確かめる。
2. 途中からの再生（CONTROL の RESET・seek）を FLUSH_INSTANCE で（sequence と frame buffer を保つ）。flush を断られたら今までどおり instance を閉じる。
3. task buffer の最小の大きさを測る（p004 は vendor の式の 2 倍、instance あたり約 31 MB）。
4. 速さ: 1080p60・2160p30 の decode の時間（VPU の tick と host の時間）、Video Player の GPU の道での 1080p60 の再生。非同期・SLC・命令の queue は測りの結果で要るかを決める。
5. JVT の H.264 の適合の stream（WS326 の range の in の 101 本）を VPU で照合できる所まで。
- 範囲の外: H.265（p005・p006）、encode、`CONFIG_DRIVER_VF2_VPU` の既定の変更（Q1 が決める。この Phase は材料を返す）。

## UAPI・HAL

- HAL の API は変えない。`include/uapi/` は変えない。engine の口（`powervr.h`）も変えない。

## 手順

1. wave5 の core（`wave5.h`・`wave5-core.c`・`wave5-decode.c`）に立て直し（`drv_wave5_recover`）、`drv_wave5_lost`・`drv_wave5_flush`、測りの knob。glue（`vf2-vpu.c`）に reset の line の callback。engine（`wave5-video.c`）で timeout を立て直しに、RESET を flush に。試験の build だけの `/dev/wave5-test`（`-DWAVE5_VIDEO_TEST=1`）。
2. host: `sh plan/ws322/tests/p004/run.sh OUT`（engine の試験に flush・立て直し・諦めを足した）。
3. build: VF2 の既定の config、`config-vf2-video.mk`、`config-visionfive2-vpu-probe.mk`、`config-vf2-video-test.mk` が warning 0。
4. 実機: `plan/ws322/tests/p007/make-clips.sh`・`make-jvt.sh`、`run-vf2.sh`（1 本で: vf2-regress、S1・F1〜F6・V・J、元に戻して cksum）。

## 受け入れ

| 項目 | 確かめ方 |
| --- | --- |
| host 試験 | `p004/run.sh` が全部 0 failed |
| build | 上の 4 つの config の vmunix（と試験の config の program）が warning 0 |
| 立て直し | 実機で、完了の見落とし（`lose 1`）と vendor の task buffer の本物の停止の後に、2 session とも次の I の絵から参照と一致に戻る。続けて 3 回で core が止まり、以後の session は EIO、GPU は動く |
| RESET の flush | `--reset-at-idr` で参照と全 frame 一致、flush の数が IDR の数と合う |
| task buffer | vendor の式に対する割合ごとに 1080p30 600 frame の停止の有無 |
| 速さ | 1080p60・2160p30 の 1 枚の時間、Video Player の 1080p60（FRAMES の shown と late） |
| JVT | 101 本の各 frame の NV12 の SHA-256 が参照の decoder の YUV と一致する本数 |
| 非退行 | vf2-regress、media-probe 1080p30 600 frame 一致、Video Player 1080p30 |

## 結果（2026-11-02 P2、q1896-i01）

### commit（基点 main 67bd7ab45）

- c46584160（立て直し・flush・測りの knob・host 試験・試験の config）、ba458dcbd（vkvideo-probe の `--reset-at-idr`・`--no-hash`、make-jvt.sh・run-vf2.sh）、f87e6a0be（task buffer を ×1.5、run-vf2.sh の直し）、最後の commit（この phase.md・ws.md・design.md、run-vf2-play.sh）。

### 確かめた事（host・build）

| 項目 | 結果 |
| --- | --- |
| host 試験 | `sh plan/ws322/tests/p004/run.sh build/tmp/p007-host3`: ps-slots 4094・engine 56（flush、flush を断られて閉じる、立て直しの後の失った instance の close と次の IDR での再 open、8 本の上限、諦めの後の EIO と open の EIO）・wire 33・p003 state 69・executor 505・nv12 752、全部 0 failed |
| build | `config/ci/config-visionfive2.mk`（build/ws322-p007d）、`config-vf2-video.mk`（build/ws322-p007v）、`config-visionfive2-vpu-probe.mk`（build/ws322-p007p）、`config-vf2-video-test.mk`（build/ws322-p007t、vmunix と wayland・files・terminal・keiland-shot・vkmatrix・vkcoherency・vkcompute・media-probe・vkvideo-probe・videoplayer）が warning 0 |

### 確かめた事（VF2 実機 10.0.30.2）

- 4 回の通し: 1 回目（止めた）、2 回目（02:23〜02:40、`build/tmp/p007-vf2-r2`、kernel cksum 3422379471、task ×2）と 3 回目（03:14〜03:37、`build/tmp/p007-vf2-r3`、kernel 2968315243、task ×1.5、JVT つき）。`VF2_HOST=kei@10.0.30.2 VF2_PLUG=AC276E432AE6 sh plan/ws322/tests/p007/run-vf2.sh OUT build/ws322-p007t build/tmp/p004-clips2 build/tmp/p007-clips build/tmp/p007-jvt build/tmp/vpu-fw`。2 回目は vf2-regress が「同じ kernel」として boot を飛ばしたので 1 回目（止めた回）と同じ boot。3 回目は新しい boot。下の値は 3 回目（2 回目も同じ形）。
- 1 回目（02:15〜02:21）は vkvideo-probe が各 frame を uncached の memory から読むので 1080p で 1 枚約 0.5 s かかり、全部の段が時間の上限を越えるので途中で止めた（`--no-hash` と frame の数の上限を足した）。止めた時に vkvideo-probe を SIGTERM で殺し、その stream は閉じた（「a stream ends: 422 pictures」）。

| 項目 | 結果 |
| --- | --- |
| 非退行（vf2-regress） | vkmatrix 109/109、`-p 3` PASS、`-r 3` 327/327、vkcoherency PASS、vkcompute 4096/4096、desktop の compositor error 0、keiland-shot の PNG cksum 518389155（p003・p004 と同じ） |
| S1 速さ（`--no-hash`、/dev/wave5-test） | 1080p30 600 枚: VPU の命令から結果まで host で平均 7.06 ms、core の 2.15M cycle（VCE 500 MHz で 4.3 ms）、vkvideo-probe の記録→fence 平均 13.2 ms。1080p60 1200 枚: 7.03 ms、記録→fence 12.4 ms。2160p30 90 枚: 20.3 ms（最長 24 ms）、7.27M cycle、記録→fence 平均 42.3 ms（最初の 1 枚の 623 ms を除くと約 35.7 ms）。誤り 0 |
| S1 一致 | 1080p60 の最初の 120 枚、2160p30 の最初の 30 枚が host の ffmpeg と全部一致 |
| F1 RESET の flush | `--reset-at-idr` 240 枚が全部一致（flush 4）、600 枚全部で flush 10・誤り 0、instance は 1 つのまま（opened 1） |
| F2 2 session 同時 | 1080p30 と 1080p60 の 120 枚ずつ: 1080p60 は全部一致、1080p30 は 119 枚一致・1 枚（frame 119）だけ参照の frame 121 の hash（`--frames` の切れ目の表示の順の事で、復号した絵は参照どおり。単独で 120 枚に切った事は無い） |
| F3 立て直し（`lose 1`、2 session が decode 中） | 立て直し 1 回、失った instance 2。1080p30 は picture 8〜58 が誤り（51）で次の IDR（60）から一致、1080p60 は 8〜118 が誤り（111）で次の IDR（120）から一致（各 128 枚一致、ほかの 1 枚は上の切れ目）。直後の 60 枚は全部一致 |
| F4 本物の停止（vendor の task buffer） | ×1.0（1080p で 15549800 byte）: picture 69 で DEC_PIC が完了せず（p004 と同じ所）、600 枚で立て直し 5 回（instance を 6 回開いた）・誤り 269、毎回次の IDR から続いた。16 MiB 境界（2 回目）でも同じ（立て直し 5 回・誤り 269）。×1.05・1.1・1.15・1.2・1.25（3 回目）と ×1.25・1.5・1.75（2 回目）は 600 枚とも誤り 0。止まらない最小は vendor の ×1.0 より上・×1.05 以下 |
| F6 decode 中の SIGKILL | 1080p60（246 枚目）と 2160p30（63 枚目）で殺した stream はどちらも閉じ（「a stream ends」）、続く session は全部一致、fault・lost の行なし |
| V media-probe | 1080p30 600 枚が host と全部一致（MATCH 600 of 600）、backend=vulkan-video、105 s |
| VP Video Player | 1080p30: 16 s に 479 枚、late 2、CPU 約 23%。1080p60: 16 s に 918 枚（約 57 fps）、late 45（2 回目は 928 枚・late 34）、late はどちらも最初の約 12 s の間だけ（8 s に keiland-shot で画面を撮った）。compositor の誤り 0。1080p60 の画面の PNG（`build/tmp/p007-vf2-r3/` には無し、2 回目の `build/tmp/p007-vf2-r2/v-h264-1080p60.png`、cksum 930773468） |
| J JVT（101 本、`make-jvt.sh`、参照の decoder の YUV を NV12 にした SHA-256） | 75 本が全 frame 一致。engine の誤り 0・立て直し 0（11376 枚）。残り 26 本: vkvideo-probe が途中で止めた 13 本（BA3_SVA_C は Extended profile、BAMQ1・BAMQ2・NLMQ1・NLMQ2_JVC_C、LS_SVA_D、MR1_BT_A、MR2〜MR5_TANDBERG、CVWP1_TOSHIBA_E は probe の DPB の計画や解析が断った）、一致しない 13 本（重み付き予測 CAWP1・CVWP2・CVWP3_TOSHIBA_E、CACQP3_Sony_D、CABA3_TOSHIBA_E、FRExt1・FRExt3_Panasonic、Sony の 3 番の 5 本 CABA3_Sony_C・CABACI3_Sony_B・CANL3_Sony_C・CAPM3_Sony_D・CVBS3_Sony_C（各 300 枚中 5 枚）、CVSE3_Sony_H（9 枚）、MV1_BRCM_D（2 枚））。原因（probe の解析、parameter set の書き直し、firmware）は未解析 |
| F5 3 回で止まる（`lose 10`） | 2 回は firmware を起こし直し（「the core is back」）、3 回目で止めた（2 回目の log は「the core is stopped after 14 recoveries」、3 回目の /dev/wave5-test は recoveries 9・strikes 3・stopped 1）。その stream は EIO（executor が lost）。以後: vkvideo-probe --list は video の family と拡張を出したまま、session は「the engine opened no stream (5)」で作れない、vkcompute 4096/4096 PASS |
| VP 1080p60 だけ（`run-vf2-play.sh`、03:39〜03:42、`build/tmp/p007-vf2-play1`、撮影も top も無し、20 s の clip を 2 回） | 1 回目 1153 枚・late 47、2 回目 1182 枚・late 18。late は 1 回目は最初の 7.4 s、2 回目は最初の 3.6 s の間だけで、その後は 100 枚ごとに 1.667 s（60 fps）で late 0。engine は 1200 枚ずつ誤り 0（VPU 7.03 ms/枚） |
| 板の後始末（4 回とも） | `/boot/boot0/vmunix` 3758577736、`/etc/group` 4260090434、`zedbsd.cfg` 632416842、`/lib/firmware/wave511` は無い、/tmp は空 |

## 判断（範囲の中の技術、design との差）

- 立て直しの止め方: design §11 は reset の line の assert だけだが、先に software reset（bus を止めてから全 block の reset、vendor の手順）をし、その後 line を保持する。line だけでは AXI の途中で止めると bus が残る恐れがあるため。line を持たない glue でも software reset が通れば止まったと見る。止められなければ EIO（stream を broken に、pool を残す）。
- 「続けて 3 回」は、絵が 1 枚完了するたびに数を 0 に戻す数え方（`strikes`）。F4 の vendor の大きさでは停止の間に絵が完了するので止まらず、続けて 5 回でも諦めない。3 回目の timeout で line を保持し domain を off にする（3 回目は boot し直さない）。
- 失った instance は firmware が忘れたので DESTROY を出さずに memory を返す。立て直しの時に orphan（DESTROY を断られた instance）の memory も返す。
- RESET の flush の後は、firmware の read の位置（flush の後の問い）から ring に続けて書く（vendor の driver と同じ）。parameter set は送り直す。flush の後の最初の絵は I の絵でなければ ERROR。
- task buffer は vendor の式の ×1.5（`TASK_PERMILLE`）。×1.05 で止まらなかったが、測った流れは 1080p30 8 Mbit/s だけなので余裕をとった。1080p で 31.1 MB → 23.3 MB、2160p で 105.6 MB → 79.2 MB。
- 非同期の decode・命令の queue 2 は入れていない。VPU の 1 枚（1080p 7 ms、2160p 20 ms、core の tick では 4.3 ms・14.5 ms）は design の p007 の受け入れ（16.6 ms・33 ms）の内で、Video Player の 1080p60 は始めの数秒の後は late 0 の 60 fps だったため。始めの late と 2160p の記録→fence（約 36 ms）を縮めるなら非同期が要る（未実施）。SLC は p004 の pool が SLC を迂回して map している（`powervr_resource_make` の bypass）ので、decode の側の SLC の flush は要らず費用は 0。
- 測りの knob と見落としの注入は試験の build（`-DWAVE5_VIDEO_TEST=1`）の `/dev/wave5-test` だけ。既定の build には入らない（`drv_wave5_set_task_share`・`drv_wave5_lose_completions` の関数は core にあるが呼ばれない）。

## 未実施

- 非同期の decode（submit が VPU を待たない、video の点と settle）と命令の queue 2（上の判断。Video Player の 1080p60 の始めの数秒の late 18〜47 と、2160p の記録→fence 約 36 ms が残る）。
- JVT の一致しない 13 本と probe が止めた 13 本の原因の解析（重み付き予測の 4 本、chroma の QP の offset、Sony の 3 番の 5 枚の差など）。
- 4K の Video Player の再生（4K の MP4 を作っていない）。2160p の 1 枚は VPU で 20 ms、記録→fence 約 36 ms で 33 ms を越える。
- DST を device local（SLC を通る）memory にした decode（vkvideo-probe と libmedia は host visible の memory を使う）。
- `CONFIG_DRIVER_VF2_VPU` の既定は n のまま（Q1 が決める）。
- firmware の無い板での起動（log 1 行で video 無し）は今回は試していない（p004 の形のまま）。

## 問い（Q1 へ）

- `CONFIG_DRIVER_VF2_VPU` を VF2 の既定の config で y にするか。材料: 立て直しが本物の停止（5 回ずつ 2 回）と見落とし（2 session）で効き、3 回で止まった後も GPU と desktop は動き、video の session だけが EIO になる。RESET の flush、decode 中の SIGKILL、2 session 同時、JVT 75/101 の bit 一致、Video Player の 1080p30（late 2）と 1080p60（始めの数秒の後は late 0）、vf2-regress の非退行。firmware の無い板は log 1 行で video 無し（p004 のまま、今回は試していない）。残る危険は JVT の不一致の 13 本（その絵が誤るだけで、engine の誤り・停止は無かった）。
