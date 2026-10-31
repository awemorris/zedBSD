<!-- awesome-plan project=zedbsd record=ws291-p025 -->

# ws291-p025: Raspberry Pi 4（V3D 4.2）で mview を動かし、qs40 の model をスペックの比で決めた copies・1920x1080 で 50 fps に

Status: uncleared（2026-11-01 01:52 P4 q1784-i04: ユーザーの決定で Pi 4 の作業をここで止め Pi 5（p021）へ移る。熱による throttling が疑われ、目標の desktop 60 fps と c21 inflight=1 の 50 fps は未達。再開点は下の i04。i03 までの成果は main 69109d3f2 に merge 済み）
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

### draw ごとの uniform の stream（23:35〜23:45、7974d3365）

- 7974d3365: draw の準備で各 stage の uniform の stream を upload の storage へ直に書く（bcm2711_vulkan_uniform_write、語の順と確かめは同じ）。前は stage ごとに
  heap の確保 2 回・写し・解放 2 回。kernel の `prog`（submit あたり）は desktop の drag で 232〜286 → 50〜81 µs、prep は 840〜880 → 581〜594 µs、
  home・wiseview の prog は 450〜518 → 93〜145 µs。
- 確かめ: Pi 4・Pi 5 の vmunix warning 0、bin-draw-host-test PASS、desktop の画（pi4-desktop.sh）は基準と完全一致。実機（kernel cksum 2311229471）:
  pi4-comp.sh 2 回で launch 49・52、type 23・24、output 19・21、drag 57・57、home 57・58、wiseview 59・59。mview c1 184 fps・c21 40.5 fps（GPU で決まり変わらず）。
- session（pi4-desk.sh、build/tmp/p025-g3）: launch 56・type 53・keys 45（入力）・output 22・scroll 14（入力）・hover 17（入力）・drag 55・home 56・wiseview 58、
  RENDER stalled 0。

## 記録（P4 q1784-i04、2026-11-01 01:44〜01:52）

- 01:44 main 69109d3f2（i03 の a54520ea8 を merge 済み）を取り込み。
- 2026-11-01 ユーザーから（Q1 経由）、Pi 4 は熱で throttling しているように見えるので Pi 4 の作業をここで止め、冷却が良く GPU が近い Pi 5（V3D 7.1、p021）へ
  移る、板はユーザーが取り替える、という趣旨の指示を受けた。続けて、Pi 4 の V3D の変更を main に入れることを承認する趣旨の決定を受けた（Q1 経由）。
- 計測の揃え（Q1 の指示の 1）の途中: 板の dmesg に温度の sensor が無く（hw.thermal sensors=0）、CPU の clock も読めないので、kernel に読みを足した
  （2a53a5a6d）。v3d.perf=1 の時、2 s ごとの cost report の後に `v3d: board arm=設定/実測 core= v3d= MHz temp= throttled=0x…` の 1 行
  （firmware の GET_CLOCK_RATE・GET_CLOCK_RATE_MEASURED・GET_TEMPERATURE・GET_THROTTLED。throttled の要求は sticky の bit を消さない、bit 16〜19 は
  boot からの履歴、bit 0〜3 は今: under-voltage・CPU の clock の上限・throttled・soft の温度の限界）。Pi 4（`bcm2711_firmware_get`）と Pi 5
  （`bcm2712_firmware_property`）の両方に実装、v3d.perf=1 の無い時は何も変えない。Pi 4・Pi 5 の vmunix（build/p021-k4b・k5b）は rc 0・warning 0。
  板では未実行（Pi 4 は電源を落とされた）。
- 観察（未確かめ）: 板の dmesg で core clock が display の floor により 500 MHz と 137.6 MHz の間を何度も行き来する。CPU の段の 2〜2.5 倍の揺れは
  ARM の clock の 600 MHz と 1.5 GHz の比（2.5）にも近い。どちらが原因かは上の行で測れば分かる（未測定）。

### 再開点（2026-11-01 01:52 P4、q1784-i04）

- branch agent/p4-ws291pi の 2a53a5a6d（board の状態の行）。src の他の変更は main 69109d3f2 と同じ。
- 板（10.0.30.6）: i03 の再開点と同じ状態のまま（kernel build/p025-4z/vmunix cksum 736120021、cmdline `login=console v3d.perf=1`、/bin/wayland・terminal・
  files と /lib/libvulkan.so・libc.so は p025-4u、元は *.p025old、/root/p025・p025d・p025desk、元の kernel と設定は /boot/boot0/*.p025old と
  host の build/tmp/p025-board/*.orig）。ユーザーが電源を落とし、板を取り替える。
  2026-11-01 02:08 追記: この Pi 4 の SD カードは Q1 が Pi 5 の image（rpi5-p021.img）で上書きした（ユーザーの許可）。上の板の状態
  （p025-4z の kernel・/bin・/lib の差し替え・*.p025old）はもう無い。再開する時は Pi 4 の image を作り直す（host の build/tmp/p025-board/*.orig は残る）。
- 再開するなら: (1) 2a53a5a6d の kernel で mview と desktop を流し、`v3d: board` の行で ARM の実測の clock・温度・throttled の bit を run ごとに比べる。
  throttled の bit 1・2（今）か 17・18（履歴）が立てば熱、bit 0・16 なら電源（under-voltage）で、heatsink か電源の交換（ユーザーの判断）。
  ARM が 600 MHz のまま throttled が 0 なら、firmware に ARM の clock の最大を頼む（SET_CLOCK_RATE、Linux の cpufreq の代わり）。
  core clock の 137.6 MHz も CPU・V3D の memory の速さに効くかを同じ行で見る。
  (2) mview c21: 32-bit の index を 16-bit に写して buffer の世代で持つ（bin の memory の読みの約 2 割、inflight=1 の GPU の側を約 1 ms）。
  (3) desktop: compositor の frame の pace と部分（dirty の矩形）の再描画（承認済み）。Pi 4 と QEMU Venus で画を確かめ、compositor の host 試験を流す。
  注（i04、build/tmp/p025-g4/desk1 の frames.txt を読み直した、未確かめの見立て）: drag の 53 fps は compositor の遅れでなく入力の注入の間隔に見える。
  aat-input の drag は 15 ms の nanosleep ごとに 1 動き（1 ms の timer で約 16〜17 ms）で、表示の 60 Hz と拍を打ち、動きの来ない vsync で frame が
  出ない（work は p50 6 ms、period は 15〜20 ms と揺れ、`next` が 19〜25 ms の後の period が 29〜32 ms）。本物の mouse（125 Hz 以上）では起きない。
  測り方を直すなら drag の間隔を 8 ms にするか、表示の frame 数でなく「動きのあった vsync に frame が出た割合」で数える（frame-ops は共有の道具なので Q1 の判断）。
  home・wiseview は animation の始めの 1〜2 frame の shown が 32〜38 ms（area が 2〜3 倍の最初の frame）で、そこが 60 に届かない分。
  (4) bin と render の重ねは V3D 4.2 では帯域の取り合いで効かない（cfd09a48c に実装）。
- 消してほしい物（Q1 へ）: build/p021-k4b（Pi 4 の build の確かめだけの kernel）。

## 記録（P4 q1784-i03、2026-10-31 23:45〜）

- 23:45 main d9846754e を取り込み（fast-forward）。Q1 の決定（i03）: Terminal（userland/desktop/terminal）の glyph atlas を device-local・optimal の image と
  staging の更新（変わった矩形だけ）に変えてよい（QEMU Venus でも確かめる）。c21 は 40.5 では受け入れない: kernel の worker で次の job の bin を今の job の
  render と重ねる、CPU の段を削る、VCM の batch を shader の VPM の大きさで選ぶ。mview は既定の inflight で動かし、inflight=1 と 2 の両方を報告する（既定を変えない）。
- 注: main の取り込みで kernel と libvulkan の間の protocol が変わり、古い userland（build/p025-4s）の libvulkan は新しい kernel で動かない。以後の計測の
  userland は tree の build（`build/p025-4u`: wayland・mview・keiland-shot・terminal・files・aat-input と dynamic の lib、config-rpi4-desk.mk）。

### Terminal の glyph atlas（44cf7da27）

- atlas（2048x2048 B8G8R8A8）を OPTIMAL・device-local の image にし、font は host-visible の staging buffer（行は atlas の幅で詰める）に描く。font が描いた
  slot の箱（`dirty_*`、attach は最初の slot の行）を、frame の前に 1 本の command buffer（barrier → vkCmdCopyBufferToImage の 1 region → barrier）で
  image に写し、frame と同じ vkQueueSubmit に入れる。glyph が増えない frame は写しも TFU も無い。descriptor は SHADER_READ_ONLY_OPTIMAL。
- Pi 4（`pi4-comp.sh` に TERMINAL= を足した、同じ compositor 1440941112、kernel 2311229471）: 前の terminal（board の /bin/terminal）で type 27・output 21、
  新しい terminal で type 31・32、output 24・25、drag 55・56、home 53・58、wiseview 59（build/tmp/p025-ta・tb）。ZTERM PERF の present 3.5〜3.9 → 1.6〜2.2 ms、
  submit 約 1.1 ms（同じ）。今の Terminal は acquire（compositor が buffer を返すのを待つ、type で平均 約 12 ms）で待つ。画は前の terminal と時計の 49 px だけ違う。
- vkQueuePresentKHR と vkQueueSubmit の内訳（VULKAN_ZEDBSD_SUBMIT_TIMING、build/tmp/p025-tc）: present ＝ present_record 約 1 ms ＋ present_submit 約 2 ms
  （WSI の窓の swapchain の画を compositor の buffer へ写す copy の記録と submit）、submit ＝ kernel の execute 約 1 ms（同じ command buffer を毎 frame
  submit し直す decode）。どちらも libvulkan の WSI と kernel の decode の側で、Terminal の側では減らせない。
- QEMU Venus（`plan/tools/titlebar/build-menu-image.sh build/p025-menu` の amd64 image、runtime build/p025-run）: compositor と Terminal を起こし、文字・記号・
  `ls /` の出力が正しく描かれ、Ctrl+- の zoom（atlas の作り直し）の後も正しい（build/tmp/p025-venus-t/text16.png・text-small.png）。`menu-p003.sh` は
  compositor の起動が遅い（Venus で settings の段に約 3 s）ので Terminal が socket の前に起きて失敗した（試験の待ちの問題、今回の変更と無関係）。
  host の `plan/tools/terminal/run-host-terminal-pace.sh` PASS（29 checks）。

### kernel の bin と render の重ね（06fe78394・cfd09a48c で実装、測って効かず fac220b76 で外した）

- 実装（Q1 の指示の 1）: v3d-job（4.2）を `bcm2711_v3d_job_launch`（bin を走らせ待ち、render を launch して worker の slot を持ったまま戻る）・
  `bcm2711_v3d_job_prebin`（render の間に次の job の bin を走らせ待つ）・`bcm2711_v3d_job_land`（render を待ち clean して slot を返す）に分け、待ちは他の
  queue の完了の event を残す。vk の executor は、今の pass の bin が 2 ms 以上で session の次の render pass（同じ job の次の event か、後ろの submission の
  最初の command。間の barrier と command の無い submission は飛ばす）がある時、render の間に window を閉じてその pass を準備し、bin を render の横で
  走らせ、その pass の step は render だけを launch する（pass が今の pass の target を sample する、coordinate shader が sample する時は重ねない）。
  job の間は mapper を通さず、view の解放は遅らせ、translation の batch と新しい map は job の間も許す（新しい entry を足すだけ）。7.1 は今の動きのまま。
- 途中の失敗（記録）: (1) render の途中で window を閉じ開けした版は、待っていた mapper が job の間に map できず EIO になり、compositor の client の buffer の
  import が失敗した（build/tmp/p025-o3）。(2) translation の batch（`bcm2711_v3d_memory_begin`）は job の間 EIO を返す設計で、重ねる準備がそれを engine の
  失敗として返し、device lost になった（mview の game mode の切り替え・desktop の session の起動の glass map の pass で再現、`submit parts 0/0/0/5/...` で
  look-ahead の戻りと確かめた。build/tmp/p025-g4 の desk2・x5・y1〜y6）。(3) 重ねる相手の探し方が狭く、実は一度も重なっていなかった版がある
  （cost report に `ahead=重ねた/捨てた/長い bin/次の job 無し/pass 無し/理由` を足して確かめた）。
- 重なった時の測り（build/p025-4d3、c21 inflight=2、build/tmp/p025-y9）: 2 s に 56 pass が bin を前の render の横で走らせ（106 の長い bin のうち、50 は次の
  frame がまだ submit されていない）、submission あたりの bin 約 2.8 → 4.8〜5.2 ms・render 約 3.0 → 5.2〜5.6 ms と両方が遅くなり、51.1 fps
  （重ならない同じ kernel の run は 52.7・52.9 fps、p025-y7・y8・x3・x4）。bin（頂点と index の memory の読みで決まる）と render（TMU と tile の memory）が
  同じ memory の帯域と QPU を取り合い、V3D 4.2 のこの負荷では重ねても速くならない。安全を増やさずに複雑さだけが残るので、重ねる部分は fac220b76 で main の
  形に戻した（実装は cfd09a48c に残る。Pi 5（V3D 7.1）や別の負荷で試す時の元）。

### VCM の batch（06fe78394、残す）

- VCM Cache Size を shader ごとに: VPM の半分（IDENT1 の VPM 64 KiB → 64 sector）に入る出力の batch の数 −1、2〜4（Mesa の vcm_cache_size と同じ式、
  shared segments）。今までは bin・render とも 2。7.1 は 0x22 のまま（`bcm2711_native_shader_vcm` を 7.1 の encoder も持つ）。
- kernel の perf（c21、submission あたり）: bin 2896 → 2734〜2770 µs、render 3309 → 3016〜3097 µs。

### c21 の結果（VCM、重ねなし）

- 注: Pi 4 の CPU の速さが run ごとに大きく揺れる（同じ kernel・同じ userland で mview の acquire・record・submit・present が 2〜2.5 倍違う run がある。
  温度か clock の制御と見る、板に温度の sensor は無い）。CPU の段が速い run と遅い run の両方を示す。

| mview（1920x1080、game mode、tree の userland p025-4u） | CPU の段が速い run | CPU の段が遅い run（最後の kernel fac220b76 の build p025-4z、736120021） |
| --- | --- | --- |
| c1（inflight=1） | 184 fps（p025-w1） | 127 fps（z1） |
| c21 inflight=1（mview の既定） | 49.7 fps、20.1 ms ＝ acquire 0.29・record 0.29・submit 0.67・present 0.67・fence 17.68・other 0.49（x1） | 42.4・42.7 fps、23.4 ms ＝ acquire 0.66・record 0.73・submit 1.46・present 1.51・fence 18.09・other 1.07（z1・z3） |
| c21 inflight=2 | 52.7〜52.9 fps、18.9 ms（x3・x4・y7・y8） | 48.1 fps、20.7 ms（z2） |

- 速い run の kernel（p025-4w・4d2）は重ねる code を持つが、cost report の `ahead=0/…` の通り一度も重ならなかった run で、VCM だけの今の kernel と同じ動き。
- 前（i02、mip まで）: c21 inflight=1 40.5、inflight=2 45.95（どちらも CPU の段の速さは未記録）。
- 確かめ（fac220b76、build/p025-4z）: Pi 4・Pi 5 の vmunix warning 0、bin-draw-host-test PASS。mview の正面の静止の画は mip の kernel の画と完全一致
  （0 px、p025-z3 と p025-m2）、pi4-desktop.sh の desktop の画は main の kernel（build/p025-4m、177203853）の画と完全一致（0 px、p025-dm と dz）、compositor の
  import の失敗 0、kernel の fault の行 0（起動時の既知の 1 行の他）。

- inflight=1 の残り: 1 frame は CPU の直列の段（acquire＋record＋submit＋other）と GPU の側（present の後の fence まで、bin 約 8.2 ＋ render 約 9.1 ＋ kernel の
  prep・launch 約 1 ms）の和。mview は fence を待ってから次の frame を記録するので重ねる相手が無く、CPU が速い run で約 20 ms（50 fps 前後）、遅い run で
  21.7〜23.4 ms（42〜46 fps）。50 を安定して超えるには GPU の側を削る（候補: 32-bit の index を 16-bit に写して持つ、bin の memory の読みの約 2 割）。

### desktop（session）

- `pi4-desk.sh` に BINS= を足し（/bin の program を tree の build に、元は /bin/NAME.p025old）、LIBS で /lib/libvulkan.so・libc.so を tree の build に
  （p025-4u）。graphical login の本当の session の frame-ops（kernel は 06fe78394 の build p025-4w。desktop の pass の bin は短く、重ねは g8 の cost report で 0。
  build/tmp/p025-g4 の desk1・g5・g7・g8、main の kernel は p025-g6、どちらも /bin・/lib は p025-4u）:

| 段 | 06fe78394（run ごと） | main の kernel（3 run） |
| --- | --- | --- |
| launch | 56・55・55・56・53・56・57・56・55・56 | 55・52・57 |
| type | 61・61・60・60・60・60・59・60・60・60 | 60・60・61 |
| output | 23・23・21・23・29・57・49・53 | 51・20・21 |
| drag | 53・53・55・56・53・54・53・54・54・53 | 53・53・54 |
| home | 56・54・55・55・56・55・56・56・55・55 | 53・55・55 |
| wiseview | 56・55・57・57・58・59・59・57・57・58 | 58・58・58 |

- type は Terminal の atlas の直しで 53 → 60・61（main の kernel でも同じ、Terminal の側の効果）。drag・home・wiseview は 53〜59 で main と同じ（60 に届かない）。
  kernel の fault の行は起動時の既知の 1 行だけ（g4 の desk2 の device lost は上の (2)）。

### 再開点（2026-11-01 01:45 P4、q1784-i03）

- branch agent/p4-ws291pi の先頭（この commit）。main d9846754e の上に 44cf7da27（Terminal の atlas）・06fe78394（重ね＋VCM）・cfd09a48c（重ねの直しと
  cost report の `ahead=`）・fac220b76（重ねる部分を main の形に戻す。残るのは VCM）と記録・道具（pi4-comp.sh の TERMINAL=、pi4-desk.sh の BINS=、
  pi4-mview.sh の MVIEW_ENV=）。src の main との差は Terminal（3 file）と VCM（8 file）だけ。Q1 の merge 待ち。
- 板（10.0.30.6）: kernel build/p025-4z/vmunix（cksum 736120021、fac220b76、input inject）、cmdline `login=console v3d.perf=1`。/bin/wayland・terminal・files は
  p025-4u の build（元は /bin/*.p025old）、/lib/libvulkan.so（2248044845）・libc.so は p025-4u（元は /lib/*.p025old）、/bin/aat-input。一時の
  /root/p025・/root/p025d・/root/p025desk。元の kernel と設定は /boot/boot0/*.p025old と host の build/tmp/p025-board/*.orig。
- 消してほしい物（Q1 へ）: build/tmp/p025-junk/amd64-sysroot-link（誤って作った main の build/amd64/sysroot への symlink を退けた物）、QEMU の
  runtime build/p025-run（guest は止めた）、image build/p025-menu、使い捨ての kernel の build build/p025-4g・4h・4v・4w・4d2・4d3 と build/p025-t
  （残す: 板の kernel の build/p025-4z、userland の build/p025-4u、main の比べの build/p025-4m）。
- 達していない: desktop の 60 fps（drag 53〜56・home 53〜56・wiseview 55〜59、main の kernel と同じ。type は 60・61）、mview c21 の 50 fps は mview の既定の
  inflight=1 で CPU の段が速い run だけ（49.7、遅い run 42.4〜42.7）。
- 次の候補: (1) 32-bit の index を 16-bit に写して buffer の世代で持つ（bin の memory の読みの約 2 割、inflight=1 の GPU の側を約 1 ms）。
  (2) Pi 4 の CPU の段の速さの揺れの原因（温度・clock）を確かめ、計測の条件を揃える。(3) desktop は compositor の frame の pace と部分の再描画
  （GPU は 1 frame 約 8 ms で余る）。(4) bin と render の重ねは V3D 4.2 では帯域の取り合いで効かない（cfd09a48c に実装、Pi 5 で試す時の元）。

### 前の再開点（2026-10-31 23:50 P4、q1784-i02、下の i03 で置き換え）

- branch agent/p4-ws291pi の先頭（この commit）。main c3291b1f9 の上に 86ab82eec（mip）・7974d3365（uniform）と記録・道具。Q1 の merge 待ち。
- 板（10.0.30.6）: kernel build/p025-4f/vmunix（cksum 2311229471、mip と uniform の直し、input inject）。今は pi4-desk.sh の後で graphical の session が動いたまま、
  次の起動は `login=console v3d.perf=1`（pi4-comp.sh の前に再起動）。/lib/libc.so・/lib/libvulkan.so は main c3291b1f9 の build（元は /lib/*.p025old）、/bin/aat-input、
  root の desktop.conf は元のまま（pi4-comp.sh を流すと welcome.done=1 を足す、元は desktop.conf.p025old）。元の kernel と設定は /boot/boot0/*.p025old と
  host の build/tmp/p025-board/*.orig。
- 達していない: desktop の 60 fps（session で drag 55・home 56・wiseview 58・launch 56・type 53、output 22 は Terminal の client の速さ）、
  mview c21 の 50 fps（40.5、上の限界の証拠）。
- 次の候補: (1) Terminal の atlas を optimal ＋ staging の更新に（範囲の外、Q1 の判断）。(2) kernel の worker で次の job の bin を今の job の render と重ねる
  （inflight=2 と desktop の client と compositor の重なりに効く、inflight=1 の c21 には効かない）。(3) desktop の部分の frame の render（約 3.5 ms/pass、
  QPU の stall が多い）。(4) VCM の 4 batch を shader の VPM の大きさで選ぶ（bin −5%）。

### 前の再開点（2026-10-31 22:17 P4、i01、上の i02 で置き換え）

- branch agent/p4-ws291pi の先頭（この commit）。main への merge は Q1（BUG-380 の直し・desktop の後退なしは確かめ済み）。
- 板（10.0.30.6）: 1920x1080 の config.txt、cmdline `login=console v3d.perf=1`、kernel は build/p025-4d/vmunix（input inject 入り、c936bdb2d 相当）、/bin/aat-input、
  session の file は元に戻した。元の file は `/boot/boot0/{vmunix,config.txt,cmdline.txt}.p025old` と `/etc/keiland/session.p025old`、host の build/tmp/p025-board/*.orig。
- 次: P7 の BUG-379 の直しの後に session の desktop の frame-ops（`plan/ws291/tests/pi4-desk.sh`、CONSOLE なし）。mview の c21 は上の候補を試し、届かない分は限界の記録に。
