<!-- awesome-plan project=zedbsd record=ws353-p016 -->

# ws353-p016: 試験（QEMU の回帰と 5330 の idle）

Status: cleared（2026-11-15 Q1、merge ad0ff660ae）
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §11。

## 目的

WS353 の全部の変更の後の tip で、idle の起床と電力、入力・描画・animation・hotplug の応答を確かめる。

## 確かめ

- QEMU（Venus、P1 の名前・port、1 本の script）: 起動（`plan/tools/boot-test.sh`）、WS309 の guest 試験（Q1 の割当）、WS099 の C1・C2、hotplug（venus-head.sh の抜き差しと出力の移動）、key repeat・pointer・touch・slide・fade、sleep と lock、game mode の出入り（window mode の present の完了の待ち、T17、が切り替えを止めない）、layer cache の焼きの後の描画、Log Out、device lost の後の終了（hotplug・shown の thread が join される。起こせる時だけ）。idle 60 s で compositor の全 thread の timeout の起床が 0（時計が見える時の分の 1 回を除く。WS352 の poll の診断）、`KWL PERF` の `timeouts` 0。
- 5330（Q1 の割当、`5330-idle.sh` の形、Debian から 1 回の UEFI の起動）: idle の package の電力と PC2 を WS333 p029（100 Hz 固定で 4.37 W・PC2 87 %）と比べる。入力（touch pad の tap・key repeat）と描画の目視の代わりに `--log-frames` の時刻。display の抜き差しは実機の HDMI があれば（無ければ未実施）。
- arm64・riscv64 の build（compositor・libvulkan・kernel）warning 0。実機（Pi・VF2）は Q1 の割当があれば。

## 受け入れ

- QEMU の全項目 PASS。5330 の idle が WS333 p029 と同等以上（下回る時は thread ごとの起床の数を添えて Q1 に）。実施しなかった物は未実施と書く。

## 依存

- p002〜p015（p013 は未決 3 の決定しだいで除いてよい。設計の推しは除く）。

## 結果（2026-11-15 P1、q2088-i01、branch `agent/p1-ws353n`、基点 main d83cafc5a9）

前回（q2084、branch `agent/p1-ws353m`、基点 ac5b3993f7）は host の disk の事故で中断した。基点の間で source（`src`・`userland`・`include`・`packages`）は変わっていない（差は `plan/ws353/tests` の 5330 の script だけ）ので、前回 PASS の QEMU の試験（qemu-deadlines・qemu-animation・qemu-printers・qemu-shown・ws309 scanout/grave）は流し直していない。代表として qemu-render-wait を 2 回流し直した（PASS）。

### build

- x86_64: 新しい worktree で `make CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached`・`sysroot`（rc 0）、host の Venus renderer は `plan/ws353/tests/build-venus-renderer.sh`（PASS）。各試験の image の build は warning 0（noct の外）。既定の program（firmware を除く、`aat-input`・`wlshm`・`wltest` を足す）の image と kernel 2 本（素: thermal・x86 power control、`CONFIG_INPUT_TEST_INJECT=y`）: `build/p016/build-full.sh`、rc 0・warning 0。1 回目は `-j4` の新しい BUILD で `zgears` の libGL が `libvulkan.so` より先に link されて止まった（make の依存の欠け、Makefile の側。2 回目は通った）。
- arm64・riscv64: 前回のまま（rc 0・warning 0）。source は変わっていない。

### QEMU（Venus、`build/p016/sweep.sh`・`extra.sh`、成果物は `build/p016/q/`、要約 `build/p016/q/summary.txt`）

| 試験 | 結果 |
| --- | --- |
| qemu-main-wake | PASS（5 回目）。idle-30 passes=5 clock=1、idle-after-20 passes=1、idle-after-plug-20 passes=1、timeouts は全部 0、壁紙 265・254 ms、plug 294 ms。前の FAIL 4 回は試験の判定の誤り: (1) 20 s の窓に時計の分が入ると、timer の pass と frame の完了の起床で 4 pass になる（`perf-rate.sh` が `clock=` を出し、分ごとに 4 pass を除く形に直した）、(2) 起動の後の network の grace（開いてから 30 s）とその後の glass の map 4 枚の焼き（layer-quiet）が、guest の起動が遅いと idle-30 の窓に入る（grace の発火と log の 12 s の静けさを待ってから idle に入る形に直した）。前回の idle-30 passes=11 も (2) |
| qemu-ui-deadlines | PASS（全 log を残す `build/p016/ui-diag.sh` で）。titlebar-repeat 41 回/2 s、apps-hover・apps-leave、band 500 ms、notify 10252 ms、rotate、OSK、idle-30 passes=2 timeouts=0。前回の FAIL（Ctrl+F・hover が 10 s の内に見えない）は高い load の時機と読む |
| qemu-render-wait | PASS（2 回） |
| ws309 resize・rotate | PASS（render の image に `nohup` が無く 1 回目は compositor が起動しなかった。`config-amd64-render.mk` に `nohup` を足した） |
| ws309 task | PASS（tasks=7、窓の色 3750/3750） |
| qemu-hotplug-follow | PASS |
| boot-test | PASS（`plan/tools/boot-test.sh` を直に、render の image、`build/p016/q/boot/login.png`） |
| display-fence・hotplug-wait・present-fence | PASS |
| refresh-fence | 3 回続けて PASS（frames の max 8.0・8.2・13.9 ms、on-grid 99/99）。その前の 2 回は frames だけ FAIL（100 個の内の 1 個が登録から 24.2・25.4 ms で signal、上限は周期 13.3＋4 ms、on-grid 95・94/99）。遅れた fence の位置を出す診断（`frames: late …`）を試験に足した後は再現していない。venus の境の計算と core の watcher に 1 周期より余分に遅らせる道は見つからない（frames の間は scanning が落ちないので 10 ms の見直しも起きない）。時機（host の負荷）と推すが未確定 |
| answer-wake・seat-hotplug・seat-owner | PASS |
| WS099 C2 | PASS（既定の program の image、`VENUS_SIZE=1920x1280`） |
| WS099 C1 | 起動の部分 PASS（最初の絵の後に黒・text 0）。Shut Down の部分は判定不能（試験が古い）: 既定の image の session は Settings の Welcome と Files が開いた状態で、試験の App Home の Log Out の操作が届かず greeter に戻らない。`C1-shutdown-picture` FAIL・`/var/log/greeter.log` 無し |
| lock・session の再起動（`plan/tools/sessiond/restart-guest.sh`） | PASS（lock 中の crash で lock 画面、PIN 0000、3 回目の crash で login 画面、誤った PIN の記録と消去） |
| Log Out（`plan/ws035/tests/zdesktop-p095.sh`） | 判定不能（試験が古い）: `/bin/greeter-probe` が無い、利用者の一覧から Down で選ぶ旧い greeter を前提（今の greeter は PIN の keypad）。greeter は起動した |
| device lost の後の終了 | 未実施: QEMU の compositor で device lost を起こす手段が無い（p003 の lost の試験は試験の program で context の fd を差し替える形） |
| sleep | 未実施（QEMU で sleep の試験の道具を用意していない） |
| idle 60 s の全 thread の timeout の起床（WS352 の poll の診断） | QEMU では未実施（`KWL PERF timeouts` だけ、全部 0）。5330 で poll の診断の kernel で測った（下） |

### 5330（DMI の product name Latitude 5330、host chaos 10.0.30.3。p4・p5 は毎回戻して sha256 一致）

- 試験の道具: `plan/ws352/tests/5330-idle.sh`（変えずに）、`plan/ws353/tests/5330-p016.sh`。tree は `build/p016/full/rootfs`（既定の program から firmware を除き aat-input・wlshm・wltest を足した物）。static の道具 `build/p016/static/`（前回の物の写し）。成果物は `build/p016/hw5330/<NAME>/summary.txt`。
- 起動の停止（1 回、間欠）: idle-a で tree を敷いた後の 2 回目の起動が 5 分答えず、BootNext の約 10 分後に Debian が起きた（watchdog=600 の TCO の reset と読む。reload は各 CPU の kernel の thread なので、ある CPU が 600 s 走れなかった）。その後の tree を敷いた起動 5 回（同じ kernel の diag-b、injector 付きの kernel の input-e・f・h、poll の診断の kernel の diag-poll。input-c も）は全部 21〜23 s で答えた。原因は未確定（停止の時の記録が無い）。
- idle の電力（diag-b、kernel 6286bbd6f2f436c9、60 s ずつ）: held 4.541 W・PC2 73.8 %、held2 4.537 W・PC2 66.3 %、quiet（Wi-Fi off・audiod 停止）4.456 W・PC2 74.1 %。core C6 は CPU0・1 が約 90 %、他は 98 %。GT の RC6 99.9 %。動いていた desktop: wayland（session）・keiland-ime・files・settings。WS352 fix4（4.52 W・PC2 73 %）と同等、WS333 p029（100 Hz 固定、4.37 W・PC2 87 %）より下。
- thread ごとの起床（poll の診断の kernel、`plan/ws352/tests/poll-diag.patch` を一時に当てて build、commit していない）: diag-poll（kernel 7d47295e9e973f7a、同じ tree）。電力は held 4.545 W・PC2 70.9 %、held2 4.522 W・65.6 %、quiet 4.473 W・64.3 %（diag-b と同じ程度）。各窓の最後の 10 s の poll の timeout（pid/tid t=timeout の数）: compositor（pid 7）は held・held2 で 0。起動の直後の窓（diag-idle）は 2 thread が 30 s の timeout を 1 回ずつ、quiet（Wi-Fi を落とし audiod を止めた後）では 1 thread（tid 81）が 1 s の timeout を 10 回（daemon の thread の再接続と推す、未確認）。compositor の外: settings（pid 18）1 s ごとに 10 回、files（pid 17）2 s ごとに 5 回、system の daemon（pid 5・6・20・22・23）が 1〜10 s ごと。kernel の近い眠り（neardiag）は xhci_port_worker の 2 thread が 10 s に約 900〜1000 回ずつ（tick 1〜100）が最多で、ほかに flusher・thermal guard・i915 resident（各 1 s 前後）。timer の割り込みは CPU 0 か 1 の 1 つが 700〜870/s、他は 100〜200/s。WS333 p029 より PC2 が低い分は、compositor の外（xhci の port の見回り・client・daemon）の起床と読む（未確定、xhci の見回りは WS353 の範囲の外）。成果物 `build/p016/hw5330/diag-poll/diag-*-poll.txt`・`diag-*.txt`
- 入力・描画（input-h、kernel bf61a89098fdef8c（injector 付き）、`--log-frames` の compositor を 1920x1080 の panel に）: 注入の device 4 本、pointer の移動 400 回、key（textedit が文字を受けた）、drag、wlshm 600 frame を 10 s・wltest FIFO 300 frame を 8 s、present の受け付け（submit → shown）平均 9.5 ms・最大 16.6 ms（903 frame）、game mode（mview の direct scanout に入り、窓が消えて合成に戻る、park の失敗 0）、SHOWN stopped shown=2487 failed=0、KWL FAILED・VULKAN_ERROR 0。idle の窓は timeouts=0（19.6 s の窓は移動の pass を含む 459、次の 31.7 s は network の grace と時計で 12）。
- cursor plane: i915 は cursor plane を libvulkan に出さない（`cursor_set` を持つ driver は amdrdna・ghci・nvrtx・videocore）ので、5330 では `KWL CURSOR_PLANE open` も `unavailable` も出ず、cursor は frame で描かれた。p012 の plane の経路の実機の確かめは 5330 ではできない（Pi・RDNA 機が要る）。
- display の抜き差し: 未実施（5330 に試験の HDMI の機器が無い）。sleep: 未実施（遠隔の電源が無い）。
- 試験の script の直し: 5330-p016.sh は guest の script を SSH から切り離して走らせ（greeter を止めると利用者の session の process がこの login ごと終わる）、PID で終わりを待つ。5330-p016-guest.sh は `ps` の path の最後の名前で process を選び、test image 専用の `--rotate-policy` を外した。途中の失敗の run（input-c・d・e・f・g）は script の不具合か、Debian の起動の直後（約 3 分）に ssh の中の処理が `Killed` で切れた物（p5 の写しの途中。理由は未確認、5 分待ってから流すと起きない）。

### 申し送り

- 古い試験の config（WS353 の範囲の外なので直していない）: `plan/ws099/tests/config-amd64-criteria.mk`・`plan/ws035/tests/config-amd64-graphical.mk`・`plan/ws035/tests/config-amd64-login.mk` が相対の include（入れ子の make の directory で `No such file`）と今の build system に無い変数（`ZEDBSD_USER_PROGRAMS`・`ZEDBSD_GRAPHICAL_BOOT`・`ZEDBSD_KEILAND_WALLPAPERS`）のままで、image が作れない。`plan/ws309/tests/config-amd64-p1.mk` も `ZEDBSD_USER_PROGRAMS` のまま。C1 の Shut Down の部分と zdesktop-p095 は今の session・greeter に合っていない。
- 5330 の起動の停止（1 回）の原因は未確定。
