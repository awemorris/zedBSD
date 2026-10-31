<!-- awesome-plan project=zedbsd record=ws291-p025 -->

# ws291-p025: Raspberry Pi 4（V3D 4.2）で mview を動かし、qs40 の model をスペックの比で決めた copies・1920x1080 で 50 fps に

Status: in-progress（2026-10-31 20:20 新設、P4 q1784）
Parent: [WS291](../ws.md)
Queue: q1784（P4）

## 目的

2026-10-31 ユーザーから、Pi 4 の GPU の driver を改善して mview を動かし、最適化する、Pi 4 は 1920x1080 で起動して進める、空いている担当に割り当てる、
この作業が終わってから Pi 5（[p021](../phase021/phase.md)）に移る、という趣旨の指示を受けた。

今（[BUG-380](../../bugs/BUG-380.md)）: main の V3D の Vulkan の executor（`src/drivers/gpu/bcm2711/vk/`、Pi 4 と Pi 5 で共有）は compositor の四角の形だけを受け、
mview の vkCreateGraphicsPipelines が FEATURE_NOT_PRESENT。足りない機能（index の draw・instance・mip・attribute の直の address）は p021 で P4 が host で
実装済み（agent/p4-ws291pi、6334ae147〜f2f87ac1b、main に未 merge）。

## 範囲

1. agent/p4-ws291pi を main と合わせ、Pi 4（V3D 4.2）で mview が動くまで driver を直す（Pi 5 の V3D 7.1 の build と host 試験を壊さない）。BUG-380 を閉じる。
2. 目標の copies を p021 と同じ規則で決める（RTX 2070 の scene-5000 の FP32 の比。Pi 4 の V3D 4.2 の QPU 数・clock は実機の dmesg で確かめる）。
3. 1920x1080・direct scanout（WS309 p015 の zero copy の game mode）で計測し、50 fps へ最適化（p021 の 4・5: CPU の詰め直し・texture の tile の写しを無くす等）。
4. desktop の後退なし（Pi 4 の desktop、compositor の四角の経路）。HAL は変えない、driver は hal_* を使わない。
5. 実機: Pi 4（10.0.30.6、SwitchBot 14C19FA80F86）。1920x1080 で起動するように起動の設定を変える。

## ユーザーの決定

- 2026-10-31 ユーザーから（Q1 経由）、Pi 4 ではまず desktop を 1920x1080 で 60 fps にし、次に mview の 3D（c21 → 50 fps）を進める、driver の改善は両方に効くことが多いので
  交互に進めてよい、順は担当が決めてよい、という趣旨の指示を受けた。検証済みの改善ごとに desktop の表と c1・c21 の fps を Q1 に報告し、Q1 が merge する。

## 記録（P4 q1784-i01）

### 準備（2026-10-31 20:20〜20:35）

- main 01b9097ea を agent/p4-ws291pi に取り込んだ（merge b4dc2cd08、親 f2f87ac1b 01b9097ea。競合は plan/ws291/ws.md の表だけ）。
- build: Pi 4（`plan/ws309/tests/config-rpi4-p015.mk`、`build/p025-4`）と Pi 5（`plan/ws291/tests/config-rpi5.mk`、`build/p025-5`）の vmunix、Pi 4 の wayland・mview・videoplayer・libvulkan など、exit 0・warning 0。
  host 試験: `plan/ws291/tests/bin-draw-host-test.sh` PASS（4.2・7.1）、qpu-format PASS、glass PASS、shader-host-test は main の時点で「browser paint」で FAIL（下の直し）。
- 実機（10.0.30.6）: 板の FAT の config.txt を 1920x1080（`hdmi_mode:0/1=82`、hdmi_cvt の行を外し、framebuffer 1920x1080）に、cmdline.txt を `login=console v3d.perf=1` に。
  元は板の `/boot/boot0/{config.txt,cmdline.txt,vmunix}.p025old` と host の `build/tmp/p025-board/*.orig`（vmunix cksum 424397698）。dmesg で `N0 fb 1920x1080`・`R0 mode 1920x1080 tmds 148500000`。
- 目標の copies: dmesg `v3d: V3 version 42 cores 1`、core IDENT1 0x81001422（slice 2 × QPU 4 = 8 QPU）、`V2 ready clock 500000000`。8 × 4 lane × 2 × 0.5 GHz = 32 GFLOPS、
  RTX 2070 の 7.46 TFLOPS との比で 5000 × 32 / 7460 = **21 copies**（Q1 承認）。

### BUG-380 の原因と直し（2026-10-31 20:35〜20:55）

- 一時の log の kernel（作業の後に元に戻した）で ENOTSUP の場所を特定した。2 つ:
  1. compiler に discard（IR の KILL）が無く、mview の cutout の fragment が拒否（`operation has no native graphics lowering`）。→ 1f3eb15b3: 4.2・7.1 の両 compiler で KILL を
     「条件の zero の flag の push ＋ 真の lane の SETMSF 0（null address へ）」に下ろす（Mesa の discard と同じ形）。scheduler は SETMSF を transaction の順（tile の書きの前）に置く。
     副産物: libGL の fixed の fragment（alpha test）も compile できるようになった（host 試験の期待を更新）。
  2. blend が premultiplied の source-over だけ。mview は SRC_ALPHA・ONE_MINUS_SRC_ALPHA。→ f6e9074a9: Blend Cfg の factor・mode を pipeline から（定数の factor 以外の Vulkan の値は V3D の code と同じ）。
- host 試験の直し（plan/tools/v3d-qpu）: lane の模型に SETMSF、source の解釈に KILL、discard の一致を比べる（mview cutout は 96 試行のうち 56 が discard）。
  共有の optimizer は KILL のある program を書き換えないので、その program に限り `optimized == 1` を求めない。
- 9434d0ef0: main の「browser paint」の FAIL は比べ方の問題だった。source の解釈と lane の模型が C の fminf・fmaxf を使い、(-0, +0) の答えが引数の順で変わる。compiler は min・max の
  引数を符号化の規則で入れ替えてよく、SPIR-V の FMin・FMax も 0 同士ではどちらの 0 でもよい。両方の模型を順に依らない 1 つの規則（min は片方が -0 なら -0、max は片方が +0 なら +0）にした。
  shader-host-test 全体 PASS（tree の全 shader、discard を含む）。

### 実機の確かめ（2026-10-31 20:47〜21:17、kernel build/p025-4/vmunix cksum 268643565）

- mview（`plan/ws291/tests/pi4-mview.sh`）: 窓 1280x720 の spin 59.1 fps（FIFO）。正面の静止の画（STILL=1）は texture・目の blend・髪の cutout・depth が正しく見える（目視。同じ view の
  他の GPU の参照の画は無く、pixel の比べは未実施）。game mode: chain 2 が `lent=1 direct=1`、`copies=0`、compositor の direct=1・window へ戻る・refused 0。
  c1 86.3 fps、c21 28.9 fps（windowed と c1 の後の run）・33.65 fps（再起動の直後に c21 だけ）。
- desktop の後退なし（`plan/ws291/tests/pi4-desktop.sh`、compositor ＋ Files の desktop ＋ Terminal、keiland-shot）: 板の元の kernel（424397698、main eff578c）と新しい kernel で、
  同じ programs の 1920x1080 の PNG が完全一致（違う pixel 0）、compositor の error 0、kernel の fault の行 0。
- c21 の内訳（v3d perf、1 frame に約 3 submit）: bin 約 8.7 ms、render 約 17 ms、CPU の段（acquire・record・submit・present）約 1〜6 ms。inflight=1 なので直列。
  `--cull=all` の c21 は bin 8.2 ms・render 1.4 ms（bin は頂点と binning で決まる）。render は c5 7.6・c10 10.9・c21 17 ms（copy あたり約 0.62 ms ＋ 固定 約 4.5 ms）。
- 試し（採らず、patch は build/tmp/exp-vcm4.patch）: VCM の cache を 2 → 4 batch: c21 33.65 → 34.7 fps（bin 2.9 → 2.75 ms/submit）、出力の segment を足しても同じ。

### desktop の計測（2026-10-31 21:20〜21:35）

- 道具: `plan/ws291/tests/config-rpi4-desk.mk`（shot の build ＋ CONFIG_INPUT_TEST_INJECT と aat-input）、`plan/ws291/tests/pi4-desk.sh`（板に kernel・/bin/aat-input・
  `--log-frames` の session を置き、再起動して frame-ops、表は frame-times.py）。arm64 の vmunix.mk に input-inject の規則が無かったので足した（527ff7e18、Q1 OK）。
- graphical login の session（autologin user）は 1920x1080 で 1 frame も描かない（`KWL RENDER stalled` が続く、output の acquire が TIMEOUT のまま）。main の kernel（eff578c）でも同じ。
  console の boot で root の compositor は描くが、`/dev/gpu*` を user の物にして user で動かすと再現 → BUG-379（Q1 が ticket を更新、P4 が直す）。
- console の boot の root の compositor（`--glass --log-frames --wallpaper`、既定の desktop の client、CONSOLE=1）での frame-ops（kernel build/p025-4d/vmunix cksum 2532187830）:

| 段 | frames | work p50/max | period p50/max | fps |
| --- | --- | --- | --- | --- |
| launch | 24 | 5/19 | 16/47 | 58 |
| type | 55 | 5/11 | 16/21 | 61 |
| keys | 21 | 16/23 | — | — |
| output | 5 | 13/17 | 34/37 | 31 |
| scroll | 184 | 12/23 | 25/46 | 38 |
| hover | 0 | — | — | — |
| drag | 122 | 13/19 | 29/39 | 35 |
| home | 44 | 14/25 | 26/43 | 37 |
| wiseview | 29 | 12/31 | 24/43 | 38 |

- 注: App Home の検索が 0 件（`KWL HOME search query="terminal" results=0`、`KWL HOME read` の行が出ない）で Terminal が起動せず、launch 以降の段は RTX の表と同じ操作になっていない。
- 1 frame の内訳（drag）: 毎 frame 全画面（dirty 1、2073600 px、1 pass、約 30 draw）。pace 7〜16、cpu 4〜7、submit 2〜3、present 4〜7（work 12〜16）、period 25〜36、shown 7〜13 ms。
  kernel の v3d perf: submit あたり prep 1.3〜2.1 ms、render 4.0〜4.4 ms（frame に約 1.8 submit で GPU 約 8 ms）、bin 0.1〜0.2 ms。flip perf の wait 平均 7.5〜10 ms。
