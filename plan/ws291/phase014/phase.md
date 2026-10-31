<!-- awesome-plan project=zedbsd record=ws291-p014 -->

# ws291-p014: NVIDIA: 命令の記録の段と pass の終わりの全画面の copy を減らし、big-1000・subs-1000 を NVK に近づける

Status: in-progress（2026-10-31 07:16 P3 q1764-i01 返却: big-1000 85〜88 → 115.8〜116.6 fps（record の短い道、push buffer を短く区切る）、NVK 136.2 には届かない。subs-1000 は 10.9 のまま（pass の終わりの copy は私用の fence のため遅らせられず、run ごとの GPU の固定の約 56 µs は未解明）。cleared か uncleared かは Q1 の判定）
Parent: [WS291](../ws.md)
Queue: q1741（P6）

## 目的

2026-10-31 ユーザーから、担当の無い WS291 の RTX の続きを P6 に割り当てる、という趣旨の指示を受けた。p013（uncleared）の差の内訳: big-1000 は frame 14.4〜16.2 ms = record 6.7〜7.5 ms（48000 record の解読、NVK に無い段）+ submit 2.3〜2.7 + fence 5.1〜5.9。subs は GPU が 1 run 約 89 µs（pass の終わりの全画面の shadow→target の copy）。

## 範囲

1. record の段を無くす新しい形（Vulkan の command buffer を記録の時に直に push buffer に組む、など）の設計と実装。
2. pass の終わりの全画面の copy を減らす（要らない時は省く、または直に描く）。
3. big-100・big-1000・subs-100・subs-1000・scene-1500・scene-5000、desktop の 60 fps、画面の画素の比べ（p013 の rtx-p013.sh）。HAL は変えない、driver は hal_* を使わない。

## q1741-i01（2026-10-31 P6、worktree p6-ws291、基点 main 5f68d0fbd、途中で main eec3f56b2 を取り込み）

- 承認: Queue q1741（2026-10-31 ユーザーから、BUG-370 と WS291（RTX の続き）を P6 に割り当てる、という趣旨の指示）。範囲は上の「範囲」のとおり。
- 実機: 10.0.10.3（RTX 2070）、lock /tmp/zedbsd-rdna.lock を 01:58 から保持。

### 差の内訳の読み直し（p013 の e7 の perf の run、libvulkan の VULKAN TIMING）

- big-1000 の 1 frame（355 frame の窓）: context_hold（kernel の transaction の時間）が 1 frame に 9 回・計 約 5.9 ms。うち submit 2 回（mview と present）が
  約 2.3 ms、残り 約 7 回・約 3.6 ms が記録の batch（1 MiB ごとに app の thread が kernel の decode を待つ）と Begin・End。MVIEW STAGES の record 6.8 ms の
  残り 約 3.2 ms は app の側（libvulkan の encode と mview 自身の計算）。
- つまり record の段は「kernel の decode 約 3.6 ms」と「client 約 3.2 ms」が直列に並んでいる。host の bench（`nvrtx-draws-bench.sh`）の record 3.86 ms は
  kernel の decode だけで、1 record 約 80 ns（dispatch・wire の読み・record の memset・object の find が細かく分かれ、1 つの大きな費用は無い）。

### 直し 1: libvulkan の長い記録の batch を送る thread（b）

- `userland/desktop/libvulkan/commands.c`・`internal.h`: 記録が 1 batch（1 MiB）を超える時、完成した前の部分を、記録する thread が送って待つ代わりに、
  libvulkan の送り thread（process に 1 つ、最初の長い記録で起こす）に渡し、記録は自分の置き場で続ける。送り thread は渡された順に 1 つずつ
  transaction を送り、頭（Reset・Begin）の返事を読む。command buffer ごとに置き場 2 つ（storage は次の batch に使い回す）、送り終わっていない数と最初の失敗。
  Begin・End・Reset・Free・pool の reset と destroy・command buffer の解放・1 batch を超える 1 record の直送の前に、その command buffer の batch を待ち、
  失敗は command buffer の失敗（End が返す）にする。小さい記録（desktop の compositor など）はこの道を通らない。thread が起こせない時は従来どおり同期で送る。
- wire の bytes と kernel の側は変えていない（kernel が受ける transaction の並びも、同じ command buffer の中では同じ順）。

| run（kernel 2338305853 = 私の branch の vmunix、libvulkan だけ入れ替え） | big-100 | big-1000（record・submit・fence ms） | subs-100 | subs-1000 | scene-1500 | scene-5000 |
| --- | --- | --- | --- | --- | --- | --- |
| a（main の libvulkan 3046047400）1 回目 | 583.9 | 69.45（6.81・2.27・5.16） | 105.1 | 10.96 | 160.2 | 50.0 |
| b 1 回目 | 589.3 | 85.34（5.04・2.63・3.89） | 102.5 | 10.86 | 154.7 | 49.5 |
| b 2 回目 | 563.3 | 91.02（4.76・2.26・3.82） | 105.0 | 10.49 | 157.6 | 49.3 |
| a 2 回目 | 551.1 | 66.88（7.10・2.49・5.18） | 101.8 | 10.85 | 158.1 | 47.7 |

- 証拠: build/ws291/measure/p2（02:06〜02:14）。100 copies の still（one・each・subs）の画素差は a・b とも 0（build/ws291/measure/p1、01:59〜02:06）。
  各 window の終わりに file と kernel を戻して cksum 一致、Debian、RTX 4 function vfio-pci。
- commit 4b7d0551c（libvulkan の commands.c・internal.h、`plan/ws291/tests/rtx-p014.sh`: 1 つの kernel で libvulkan だけを round ごとに入れ替える A/B。
  `KERNEL`・`USB_MVIEW`（bench が /bin/mview に入れる mview）・`FILES`。起動の後に `dmesg | grep i8042` も残す）。

### desktop（後退の確かめ）

- `rtx-desk-main.sh build/ws291/measure/d1 build/ws291/a/vmunix`（RUNS=1、FILES に b の libvulkan、02:49〜02:54）: launch 64・type 61・output 67・drag 60・
  home 64・wiseview 64 fps（scroll・hover・keys は入力の間隔で決まる段）。p013 の d7（63・61・68・60・64・65）と同じ。後退なし。終わりに file と kernel を戻して
  cksum 一致、Debian、RTX 4 function vfio-pci。lock は 02:54 に返した。

### 試した c（入れない）: 256 KiB で渡す

- 渡す大きさを 1 batch の 1/4 にすると、End に残る decode が短くなる見込みだったが、残りが submit と一緒に送られて submit の段が伸び、big-100（440 KiB/frame）は
  渡しが増えて遅くなった。

| run（build/ws291/measure/p3d、02:41〜02:49、b→c→c→b） | big-100 | big-1000（record・submit・fence ms） | subs-100 | subs-1000 | scene-1500 | scene-5000 |
| --- | --- | --- | --- | --- | --- | --- |
| b 1 回目 | 592.6 | 86.89（5.02・2.28・4.06） | 106.2 | 11.00 | 160.7 | 50.0 |
| c 1 回目 | 538.3 | 84.60（4.38・3.10・4.16） | 103.0 | 10.89 | 159.1 | 49.7 |
| c 2 回目 | 519.4 | 93.93（4.14・2.34・4.03） | 101.9 | 10.44 | 153.9 | 47.6 |
| b 2 回目 | 555.7 | 90.53（4.65・2.22・4.04） | 101.5 | 10.57 | 153.5 | 47.7 |

- 画素: b・c とも one と each・subs の still の差 0。big-1000 は b と c で揺れの内、big-100 は c が 6〜7 % 遅い → b を残す（worktree の libvulkan は b と byte 一致、
  cksum 811934542）。
- 機械の事象: 02:15・02:18 の run は、Debian から BootNext で起こした zedBSD（元の kernel va1 と 10-28 の userland、何も入れ替えていない）が SSH に一度答えた後
  network を失い、Debian も zedBSD も答えなくなった。Q1 が 02:3x に SwitchBot で入れ直して Debian に戻った。02:28 の run でも zedBSD が 300 s 以内に答えず
  （後で答えた）、次の run から普通に動いた。原因は調べていない（Q1 の指示で深追いしない）。

### 残りの段の見立て（未着手）

- record の段（b で 4.7〜5.0 ms）: 今は kernel の decode（約 3.6 ms）が律速で、client の encode はその陰。次は kernel の decode の短縮（1 record 約 80 ns:
  stream の dispatch、wire の 1 語ずつの読み、record の 120 B の memset、command buffer と descriptor set の hash の引き）か、記録を kernel が push に直に組む形。
  後者は executor（vk-execute.c）が record の配列を前提にしているので作り直しが大きい。
- pass の終わりの全画面の copy（subs-N、1 run に GPU 約 89 µs）: 省くには「target を読む物が来るまで copy を遅らせる」形が要るが、他の session・engine・CPU の
  map が target を読む前に copy を必ず済ませる仕組み（node の submit の前の flush、map の時の flush）が要り、q1741 の残りでは安全に作れない。WSI の image を
  block-linear にする件（p013 の (B)）が本筋。subs-1000 は 10.5〜11.0 fps のまま。

### 機械の状態と再開点（2026-10-31 02:55 P6）

- 機械: Debian、RTX は vfio-pci（4 function）、NVMe の kernel は vmunix-va1（1463007375）、NVMe の userland は 10-28 の file（cksum 照合、*.p7old は残る）。lock は返した。
- branch agent/p6-ws291: 4b7d0551c（libvulkan の batch を送る thread と rtx-p014.sh）。main に入れてよい（実機で効果、画素差 0、scene と desktop の後退なし）。
  QEMU の desktop（libvulkan を使う compositor・app）は、この変更の前の BUG-370・376 の image でだけ確かめた（変更後の QEMU の回帰は未実施。小さい記録は新しい道を通らない）。
- 到達: big-1000 66.9〜69.5 → 85.3〜91.0 fps（NVK 136.2）。big-100・subs・scene は揺れの内。subs-1000 は 10.5〜11.0（NVK 75.4）。
- 次の候補: (1) kernel の record の decode の短縮（今の律速、約 3.6 ms）、(2) 記録を kernel が push に直に組む形（executor の作り直し）、(3) subs は WSI の image の
  block-linear（p013 の (B)）。

## q1764-i01（2026-10-31 P3、worktree p3-ws291r2、基点 main 901f5b5fc）

- 承認: Queue q1764-i01（2026-10-31 ユーザーは昼頃まで不在で、その間 実装と試験を進める趣旨の指示を受けている）。範囲は上の「範囲」のとおり（p014 の続き）。
- 実機: 10.0.10.3（RTX 2070）、lock /tmp/zedbsd-rdna.lock を 04:49 から保持。計測は全部 main の userland（networkd・libc・libvulkan・compositor ほか、
  `make ZEDBSD_CONFIG=plan/ws291/tests/config.mk BUILD=/tmp/p3r2/n` の個別 target）を FILES で入れ、kernel だけを入れ替える `rtx-p013.sh`（SHOT=1、ROUNDS=2）。
  各 window の終わりに file と kernel を戻して cksum 一致（vmunix-va1 1463007375）、Debian。

### 直し 1: kernel の record の短い道（commit d74ebaff5、src/drivers/gpu/nvrtx/vk/vk-record.c）

- draw ごとに記録される 5 つ（DRAW_INDEXED・DRAW・BIND_PIPELINE・set 1 つの BIND_DESCRIPTOR_SETS・PUSH_CONSTANTS）を、記録中で余裕のある command buffer
  なら stream から直に読み（固定の部分の長さを 1 回だけ確かめ、wire の語を直に組む）、続く同じ command buffer の record も stream と dispatcher を通さずに
  その場で取る。引数の確かめは一般の道と同じで、取れない物（他の opcode、短い stream、配列を伸ばす時、一般の道が断る引数）は何も読まず一般の道へ返す。
- host: `nvrtx-draws-bench.sh`（1000 copies 1 submit）の record 3.7 → 約 1.5 ms（callgrind で 1 record 約 440 → 約 160 命令）、draw ごとの state の hash が
  基点と全一致（1000×1・100×100）、nvrtx-vk 94/0・vk-d3 218/0。kernel warning 0・amd64 vmunix check PASS。

### 直し 2: push buffer を短く区切って GPU を早く動かす（vk-execute.c、commit 02c6d4a66・788885030）

- 02c6d4a66（e）: command buffer の最初の push buffer を 256 draw で GPU に渡す（frame の submit の時 GPU は空いている）。
- 788885030（r）: 2 つ目からも 2048 draw ごとに GPU に渡す（`EXECUTE_RUN_DRAWS`）。PROF（nvrtx.vk.perf=1）で、big-1000 の GPU の実行は 1 frame 約 4.3 ms（NVK の fence 4.09 ms と
  同等）なのに submit+fence が約 6.1 ms で、差の約 1.8 ms は GPU が executor の 1 MiB の push buffer の書き終わりを待つ時間と分かった（CPU は 1 draw 約 0.15 µs、
  GPU は約 0.27 µs で CPU の方が速いが、1 MiB の push buffer は約 5000 draw で GPU の手が空く）。r で 1 frame の run は約 3 → 約 10 本、GPU は 1 run 約 0.41 ms、
  submit+present+fence が約 4.6 ms（GPU の 4.3 ms にほぼ律速）。
- host: bench の draw ごとの state の hash が基点と全一致（1000×1・100×100）、nvrtx-vk 94/0・vk-d3 218/0、kernel warning 0・vmunix check PASS。

### 実機（10.0.10.3 RTX 2070、main の userland、fps、括弧は big-1000 の record・submit・fence ms）

| kernel（window、round） | big-100 | big-1000 | subs-100 | subs-1000 | scene-1500 | scene-5000 | shot each・subs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| base（main 901f5b5fc、3606953930）m1 1・2 | 586.4・573.9 | 85.0（4.97・2.32・4.33）・88.2（4.92・2.16・4.11） | 105.0・106.1 | 10.91・10.91 | 159.9・160.5 | 49.83・49.84 | 0・0 |
| q（d74ebaff5、3987510353）m1 1・2、m3 1・2 | 620.4・630.3・601.4・639.9 | 92.7（4.08・2.50・4.04）・99.8（3.77・2.15・3.98）・97.6・93.2 | 105.4〜106.8 | 10.87〜10.92 | 159.6〜160.3 | 49.76〜49.84 | 0・0 |
| e（02c6d4a66、2764037720）m3 1・2、m4 1・2 | 678.0・664.3・680.0・574.7 | 98.6（3.69・2.34・3.98）・99.5・99.3・97.0 | 105.9〜107.1 | 10.87〜10.93 | 159.9〜160.4 | 49.68〜49.87 | 0・0 |
| r（e＋2048 draw、下の v 込みの 1085861505）m4 1・2 | 688.8・645.0 | 116.6（3.82・3.28・0.99）・115.8（3.77・3.45・1.01） | 106.5・106.5 | 10.99・10.98 | 159.7・160.9 | 49.79・49.73 | 0・0 |
| r（788885030、v 無し 102504526）m5 1 | 662.9 | **116.0（3.81・3.30・1.02）** | 105.9 | 10.91 | 160.4 | 49.87 | 0・0 |
| r8（r で slot を 3 → 8、3342390047）m5 1 | 672.1 | 116.3（3.84・2.35・2.23） | 107.3 | 10.90 | 160.3 | 49.79 | 0・0 |
| nc（診断: pass の終わりの copy を書かない、画素は誤り、3312580597）m5 1 | 716.8 | 116.1 | 153.6 | 16.07（submit 54.8） | 162.6 | 49.93 | — |
| NVK（p010） | — | 136.2（3.15・0.03・4.09） | — | 75.4 | 165.6 | 50.1 | — |

- 証拠: /tmp/p3r2/m1（04:49〜05:04）・m3（05:08〜05:40）・m4（05:41〜06:13）・m5（06:13〜06:37）の summary.txt（host の /tmp、git に入れない）。
  r の one・each・subs の still は base（m1）の one とも画素差 0（nc の one は base と全画素で違い、比べが効いていることも確かめた）。各 window の終わりに 13 の file と
  NVMe の kernel を戻して cksum 一致、Debian。m4 の r 2 で deploy が「neither system answers」を出したが、次の問い合わせで r の kernel（1085861505）で上がっていた
  （起動が遅かった。原因は調べていない）。
- 起動の dmesg に毎回 `vram-window bar1 before the tables: fault ...`（base でも出る）、q の 1 回目だけ `vram-window retry 1`。今回の変更の前からの行。

### desktop（後退の確かめ）

- `rtx-desk-main.sh /tmp/p3r2/desk2 r の vmunix`（RUNS=1、main の desktop の program、06:38〜06:43、kernel 102504526）: launch 64・type 62・output 70・drag 61・
  home 64・wiseview 64 fps（scroll・hover・keys は入力の間隔で決まる段）。p013 の d7（63・61・68・60・64・65）・P6 の d1（64・61・67・60・64・64）と同じ。後退なし。
  終わりに file と kernel を戻して cksum 一致、Debian、RTX 4 function vfio-pci。（06:37 の 1 回目は m5 の Debian への戻りと重なり「zedBSD did not come up」で
  何もせずに終わった。）

### 試して入れない物

- r8（executor の slot を 3 → 8）: r と揺れの内（116.3 対 116.0）→ 入れない（slot は 3 のまま）。

- v（pipeline 間で同じ compiled program を共有し、vertex program の付け替えを無くす。mview の 2 系統の pipeline は vertex shader が同じ）: m4 で e と揺れの内
  （big-1000 98.3・98.1 対 99.3・97.0）→ 入れない。差分は `plan/ws291/tests/nvrtx-program-share.patch`。bench は program を address でなく中身で hash する形にした
  （program の置き場を変える executor の比べに使う）。
- d（pass の終わりの shadow→target の copy を、signal する submit まで遅らせる）: subs-1000 は 10.9 のまま。libvulkan は fence の無い vkQueueSubmit にも私用の
  fence（private_fence）を付けて送るので、kernel は「誰かが image を見るか」を submit から区別できず、毎 submit で copy した。d は入れない
  （`plan/ws291/tests/nvrtx-shadow-defer.patch`）。copy を省くには、私用の fence を wire で示す（libvulkan と各 driver の protocol の変更）か、WSI の image を
  block-linear にする（p013 の (B)）のどちらかが要る。

### subs-N の内訳（m5 の nc と PROF）

- PROF（subs-100）: GPU の 1 run は約 89 µs（r・r8 とも）。nc（診断、copy を書かない）で 1 run 約 60 µs（1 run は 1 copy の 16 draw、描画そのものは
  scene-5000 から見積もって約 4 µs）。つまり pass の終わりの copy は約 29 µs で、残りの約 56 µs は run ごとの固定の費用。nc の subs-1000 は 16.07 fps
  （submit 54.8 ms ＝ 1 submit 約 55 µs）。NVK は subs-1000 で submit 4.63 ms・fence 2.45 ms（1 submit 約 5〜7 µs）。
- copy を省けても subs-1000 は約 16 fps までで、NVK の 75 fps に近づくには run ごとの固定の約 56 µs（GPU）の内訳（run の始めの texture pool と shader cache の
  invalidate、最初の draw の全 state、pass の始めと終わりの barrier の WFI、run の終わりの WFI の fence、channel の切り替えの有無）を 1 つずつ外して測る段が要る
  （今回は未着手）。

### 機械の状態と再開点（2026-10-31 P3）

- 機械: Debian、RTX は vfio-pci（4 function）、NVMe の kernel は vmunix-va1（1463007375）、NVMe の userland は 10-28 の file（各 window の終わりに cksum 照合、
  *.p7old は残る）。
- branch agent/p3-ws291r2: d74ebaff5（record の短い道）、02c6d4a66（最初の push buffer を 256 draw で）、788885030（以後 2048 draw ごと）、a940ef0e1（bench の
  program の中身の hash、rtx-runcost.sh、試した差分の patch）と記録の commit。
  main に入れてよい（実機で効果、画素差 0、scene の後退なし。desktop は下）。
- 到達: big-1000 85.0〜88.2（base）→ 115.8〜116.6 fps（NVK 136.2）。frame 8.6 ms = record 3.8（NVK 3.15）+ submit・present・fence 4.6（GPU 約 4.3、NVK の fence 4.09）。
  big-100 574〜586 → 645〜689。subs-1000 は 10.9 のまま（NVK 75.4）。scene-1500・scene-5000 は揺れの内。
- 診断の run（rtx-runcost.sh、診断 kernel 4002989971、06:44〜）は計測に至らなかった: script が main の userland を入れた後、kernel を入れ替える前に
  `timing on` で再起動したため、NVMe の古い kernel va1 と main の networkd の組で zedBSD の network が上がらず（BUG-371/373 の ABI 違い）、06:56〜07:12 は
  Debian も zedBSD も答えなかった。Q1 が SwitchBot（AC276E42B252）で入れ直し、Debian から p4 の vmunix を診断 kernel に書き（`rdna-nvme.sh kernel`）、
  zedBSD を起こして 13 の file と kernel を元に戻し cksum 一致（07:13〜07:15）、nvrtx.vk.perf の行 0、Debian、RTX 4 function vfio-pci。lock は 07:15 に返した。
  script は kernel を先に入れてから timing on する順に直した（a940ef0e1）。run ごとの固定の費用の内訳（変種 0〜5）は未計測。
- 次の候補: (1) subs: 私用の fence を wire で示して pass の終わりの copy を遅らせる（libvulkan と各 driver の protocol の変更、計画の判断が要る）か、WSI の
  block-linear（p013 の (B)）。(2) subs: run ごとの GPU の固定の約 56 µs の内訳。(3) big-1000: record の段の残り（client の encode と最後の batch の decode、
  約 0.7 ms）と GPU の約 0.2 ms。
