<!-- awesome-plan project=zedbsd record=ws291-p014 -->

# ws291-p014: NVIDIA: 命令の記録の段と pass の終わりの全画面の copy を減らし、big-1000・subs-1000 を NVK に近づける

Status: in-progress（2026-10-31 02:55 P6 q1741-i01 返却: big-1000 67〜69 → 85〜91 fps（libvulkan の batch を送る thread）。record の段の残りと pass の終わりの copy は未着手。cleared か uncleared かは Q1 の判定）
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
