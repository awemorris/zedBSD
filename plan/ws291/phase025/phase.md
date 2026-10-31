<!-- awesome-plan project=zedbsd record=ws291-p025 -->

# ws291-p025: Raspberry Pi 4（V3D 4.2）で mview を動かし、qs40 の model をスペックの比で決めた copies・1920x1080 で 50 fps に

Status: in-progress（2026-10-31 20:20 新設、P4 q1784。i02 23:35 返却: mip で c21 40.5 fps、desktop は session で 51〜57 fps、どちらも目標の手前）
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

### V3D の改善と解析（2026-10-31 21:40〜22:17）

- c936bdb2d: user の pipeline の program の code を最初の draw で 1 回だけ GPU に上げ、pipeline に持たせる（pipeline の解放は、それを名指す command buffer の job が全部終わった後、
  worker の上で起きるので、そこで返す）。internal の draw（meta）は今のまま pass ごと。mview の submit あたりの `prog` 50〜180 → 9〜14 µs。desktop の `prog` 430〜545 µs は
  残る（多くは draw ごとの uniform の stream の組み立てで、code の upload ではない）。
- 同じ commit: bin の run の性能 counter（`v3d: gpu bin ...`、bin の間だけ counter の source を Mesa の v3d_performance_counters.h の番号で切り替える）。
- 確かめ: Pi 4・Pi 5 の vmunix warning 0、bin-draw-host-test PASS、shader-host-test PASS。実機: mview c21 33.9〜34.0 fps・c1 132.9 fps（kernel の fault の行は起動時の自己試験の 1 行だけ）、
  desktop の frame-ops（CONSOLE=1）は前と揺れの内で同じ（launch 57・type 57・output 29・scroll 36・drag 33・home 37・wiseview 38）。
- c21 の bin の counter（1 run あたり千）: cycles 4343（500 MHz で 8.7 ms）、QPU の命令 6236・idle 227・coordinate shading 6160、命令 cache の miss 0、uniform cache の miss 0、
  VDW・VCD の stall 0、binned 777（primitive）・reversed 434、**L2T の miss 718**。cull=all でも cycles 4164・binned 777。
  → bin は primitive あたり約 5.5 cycle で、QPU は 2 割も使っていない。L2T の miss（約 46 MB/pass と見積もると 5 GB/s 台）から、頂点と index の読みの memory の帯域で決まる見込み
  （21 copy が同じ model の頂点 0.83 MB と index を毎回読む）。render も 1 run 8749 千 cycle のうち QPU の valid は約 1 割、tmustall 6240。
- 見込み: c21 の 50 fps（20 ms/frame）は、bin（約 8.7 ms）と render（約 17 ms、うち 1920x1080 の tile の store だけで約 2.6 ms）が直列の今の形では届かない。
  残る候補（未着手）: VCM の cache を 4 batch（試しで bin −5%）、coordinate shader が使わない attribute を読まない、vertex/coordinate の program の scheduling（今は source の順で
  pairing 0）、bin と render の重ね（inflight=1 の mview では効かない）。desktop は compositor の側（全画面の再描画、pace）の話が大きい（Q1 に先に相談する約束）。

### BUG-379（2026-10-31 22:10）

- bug-analyzer の解析で原因が分かった（kernel の fork の livelock、BUG-379.md）。Q1 の決定で直しは P7（q1786）。session での desktop の計測は P7 の直しの後に Q1 と合わせて行う。

## 記録（P4 q1784-i02、2026-10-31 22:20〜）

- 22:20 main 499353231 を取り込み（merge）。22:55 main c3291b1f9（BUG-379 の直し）へ fast-forward。
- Q1 の決定（i02）: compositor の側の直し（部分の再描画・pace・CPU と GPU の重ね）はこの Queue の中で承認、変更は userland/desktop/wayland/ と V3D の driver に限る
  （libvulkan の WSI・kern/vm は触らない）。確かめは keiland-shot の画素の比べ・desktop の frame-ops（Pi 4 と QEMU Venus）・compositor の host 試験。

### desktop の測り直し（22:27〜22:45）

- 新しい道具 `plan/ws291/tests/pi4-comp.sh`: 再起動なしで tree の compositor を root で動かし frame-ops（Terminal は shell が起動、root の welcome.done=1、
  元は desktop.conf.p025old）。BUG-379 の直しの前の比べのため。
- 結果: 前の表（launch 57・output 29・scroll 36・drag 33・home 37・wiseview 38）は Terminal が起動しておらず、BUG-379 の fork の空回りが CPU を食った値だった。
  spawn を避けると同じ compositor（cksum 1440941112）で drag 55・home 57・wiseview 58、type 28・output 21（Terminal の client の速さで決まる）。

### BUG-379 の直しの確かめ（22:54〜22:56、Q1 の依頼）

- 板に kernel build/p025-4f/vmunix（config-rpi4-desk.mk、main c3291b1f9、cksum 3454287288）・/lib/libc.so（3771813226）・/lib/libvulkan.so（2543328087）
  （元は /lib/*.p025old、`pi4-desk.sh` に LIBS= を足した）。login=graphical の本当の session で `pi4-desk.sh`（CONSOLE なし、build/tmp/p025-g1）:
  frame 483、`KWL RENDER stalled` 0、App Home の検索 "terminal" results=1、Terminal と Settings の Welcome が spawn されて map、kernel の fault の行は起動時の既知の 1 行だけ。

| 段（session、板の /bin/wayland） | fps | work p50/max |
| --- | --- | --- |
| launch | 55 | 3/7 |
| type | 52 | 2/5 |
| keys | 34（入力 200 ms） | 2/6 |
| output | 19 | 10/18 |
| scroll | 14（入力で決まる） | 4/5 |
| hover | 17（入力で決まる） | 7/16 |
| drag | 52 | 8/12 |
| home | 54 | 6/11 |
| wiseview | 57 | 8/19 |

- type・output は Terminal の frame の速さで決まる（TERMINAL_PERF: 1 frame に vkQueuePresentKHR 約 6 ms・vkQueueSubmit 約 3 ms、さらに Terminal の
  2048x2048 の linear・host-mapped の glyph atlas を driver が毎 frame TFU で変換（GPU 約 2.2 ms、CPU の clean・tex 約 1.3 ms）。host-mapped の source は変化を
  知る手段が無いので driver の側では省けない。Terminal を optimal の atlas と staging の更新に変えれば消える（userland/desktop/terminal は承認の範囲の外、Q1 に提案）。

### V3D 4.2 の hardware の mip（23:00〜23:09、86ab82eec）

- 試し（採らず）: level 2 だけを sample すると c21 33.9 → 39.3 fps（render 17 → 約 11 ms）。TMU の stall が render の主因と確かめた。
- 86ab82eec: mip の image の view（RGBA8・BGRA8、2 の冪の辺、TFU あり、host-mapped でない）を hardware の配置の mip tree に変換（Mesa の v3d_setup_slices・
  v3d_get_ub_pad、level ごとに app の level の raster から TFU 1 job）。texture の record に max level・level 0 の tiling・UB pad、sampler に mip filter と max LOD。
  それ以外（1 level・host-mapped・Pi 5 の 7.1）は今の level 0 の道。
- 確かめ: Pi 4・Pi 5 の vmunix warning 0、bin-draw-host-test PASS。実機（kernel build/p025-4f/vmunix cksum 3091423017）: c21 40.5・40.2 fps、c1 125 fps、
  c21 の render 約 10 ms/frame。正面の静止の画（1280x720）は texture が正しく、level 0 だけの時より少し滑らか（窓の 921600 px のうち 9304 px が違う、
  32 を超えるのは 494）。desktop の画（pi4-desktop.sh、PROGRAMS=build/p025-4s）は前の基準（p025-d-new）と完全一致（違う pixel 0）。

### c21 の限界の証拠（23:09〜23:12）

- `--cull=all`: bin 約 8.3 ms/frame、render 約 1.5 ms、fence 10.0 ms、62.6 fps。bin は頂点の処理（21 copy × 約 26k 頂点、32-bit index）で決まる。
- `--inflight=2`（参考）: 45.95 fps、GPU busy 97〜98%（bin 約 8.3 ＋ render 約 9.9 ms が kernel の worker で直列）。
- 既定の inflight=1 の 1 frame（24.6 ms）= GPU 約 19.1 ms（bin 8.7 ＋ render 10）＋ CPU 約 5.5 ms（acquire 0.65・record 0.68・submit 1.39・present 1.73・other 1.06）。
  CPU を 0 にしても GPU だけで約 18.2 ms（55 fps）、今の CPU の段では約 41 fps が上限に近い。

### mip の後の session と試し（23:14〜23:32）

- session の frame-ops（`pi4-desk.sh`、kernel 3091423017、build/tmp/p025-g2）: launch 56・type 52・output 43・drag 51・home 57・wiseview 55（RENDER stalled 0、
  Terminal map）。g1（mip の前）と揺れの内で同じ。
- 試し（採らず）: compositor の render thread の acquire を飛ぶ frame が無い時も 1 ms で見直す（解放を寝過ごす見立て）。pi4-comp.sh で 2 回: drag 55・53、home 57・57、
  wiseview 58・58、launch 52・50、type 26・25（前の 1 回 c3: 55・57・58・51・28）。効かないので戻した（見立ては外れ）。
- 注意: graphical login の session の後に pi4-comp.sh を流すと greeter の compositor と画面を取り合う。pi4-desk.sh の後は再起動してから（script に確かめを足した）。
  23:17〜23:29 の 2 つの A/B（p025-pa）は前の run と重なって無効。

### 再開点（2026-10-31 23:35 P4、q1784-i02）

- branch agent/p4-ws291pi の先頭（この commit）。main c3291b1f9 の上に 86ab82eec（mip）・6ff4e210a ほか。Q1 の merge 待ち。
- 板（10.0.30.6）: kernel build/p025-4f/vmunix（cksum 3091423017、mip 入り、input inject）、cmdline `login=console v3d.perf=1`、/lib/libc.so・/lib/libvulkan.so は main
  c3291b1f9 の build（元は /lib/*.p025old）、/bin/aat-input、root の desktop.conf は元のまま（pi4-comp.sh を流すと welcome.done=1 を足す、元は desktop.conf.p025old）。
  元の kernel と設定は /boot/boot0/*.p025old と host の build/tmp/p025-board/*.orig。
- 達していない: desktop の 60 fps（session で drag 51〜52・home 54〜57・wiseview 55〜57・launch 55〜56、type 52・output 19〜43 は Terminal の client の速さ）、
  mview c21 の 50 fps（40.5、上の限界の証拠）。
- 次の候補: (1) Terminal の atlas を optimal ＋ staging の更新に（範囲の外、Q1 の判断）。(2) kernel の worker で次の job の bin を今の job の render と重ねる
  （inflight=2 と desktop の client と compositor の重なりに効く、inflight=1 の c21 には効かない）。(3) compositor の frame の始めから GPU の完了まで約 21 ms
  （drag）を縮める: vkQueueSubmit・vkQueuePresentKHR の kernel の prep（1 submit 約 1〜2 ms）。(4) VCM の 4 batch を shader の VPM の大きさで選ぶ（bin −5%）。

### 前の再開点（2026-10-31 22:17 P4、i01、上の i02 で置き換え）

- branch agent/p4-ws291pi の先頭（この commit）。main への merge は Q1（BUG-380 の直し・desktop の後退なしは確かめ済み）。
- 板（10.0.30.6）: 1920x1080 の config.txt、cmdline `login=console v3d.perf=1`、kernel は build/p025-4d/vmunix（input inject 入り、c936bdb2d 相当）、/bin/aat-input、
  session の file は元に戻した。元の file は `/boot/boot0/{vmunix,config.txt,cmdline.txt}.p025old` と `/etc/keiland/session.p025old`、host の build/tmp/p025-board/*.orig。
- 次: P7 の BUG-379 の直しの後に session の desktop の frame-ops（`plan/ws291/tests/pi4-desk.sh`、CONSOLE なし）。mview の c21 は上の候補を試し、届かない分は限界の記録に。
