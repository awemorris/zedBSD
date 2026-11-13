<!-- awesome-plan project=zedbsd record=ws353-p007 -->

# ws353-p007: 締め切り B（UI の期限）を timer へ（main loop の 3）

Status: cleared（2026-11-14 P1、q2065-i01。build 3 機種 warning 0、host 試験 PASS、QEMU の qemu-ui-deadlines.sh・qemu-deadlines.sh・qemu-main-wake.sh PASS（下の「結果」）。lock の card と volume は QEMU 未実施。Q1 の確認待ち）
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.5、対応表の 2-04・2-18・2-22〜2-25・2-31・2-33・2-35〜2-37・2-39・2-40・2-44。

## 目的

UI の締め切りを timer にする。

## 対象（file）

- band の長押し（shell.c、10 ms ごとをやめ閾値の時刻に 1 回）、titlebar の key repeat（titlebar-shell.c）、apps bar の hover・leave（apps-bar.c）、OSK の手書きの認識（keyboard.c）、greeter の lock の card・security key（greeter.c）、home・corner（home.c・corner.c）、notify-log を閉じる（notify-log.c）、rotate の hold（output-rotate.c）、scanout の pointer の静止・lend の拒み（scanout.c）、layer cache の静けさ（layer-cache.c）、glass の blur の hold（glass.c）、volume の送りの間隔（volume.c）、system の scan の表示（system.c）、printers の 1 s（printers-shell.c、面がある間の timer。周期の読み直しが消えるのは printd の push の後＝範囲の外）。
- `kwl_titlebar_tick`・`kwl_glass_tick` など毎 pass の tick を handler に割る。

## 残す振る舞い

- 各期限の長さと結果。key repeat の間隔、hover の遅れ、layer の焼きの時期。

## 確かめ

- build warning 0。QEMU: key repeat（titlebar の field）、apps bar の hover、OSK の手書き、lock の card、notify-log、rotate、layer cache の焼き（`--log-frames` の行を file から）。idle の main の timeout の起床が、対象の分 0。

## 受け入れ

- 対象の期限が timer で動く。結果・SHA・未実施を書く。

## 依存

- p005（p006 と並べてよい。同じ file を触る所は順に）。

## 結果（2026-11-14 P1、q2065-i01、branch `agent/p1-ws353g`、基点 main 5c36b72209）

### commit

- 55bfdf3cd5: compositor（`userland/desktop/wayland/`）の p007（17 file）。
- 9385b94210: timer の名前ごとの発火数の診断（`threads.c`・`threads.h` の `kwl_timers_report`、`main.c` の KWL PERF の行）と host 試験（`plan/ws353/tests/timer/timer-test.c`）。
- この記録の commit（SHA は報告に書く）: QEMU の試験（`qemu-ui-deadlines.sh`・`config-amd64-ui.mk`・`perf-timers.sh`）、`qemu-main-wake.sh`・`qemu-deadlines.sh` の idle の閾値、design.md の 2-04、この記録と ws.md。

### 実装

対象の `kwl_due` の 17 か所を、機能ごとの timer（`kwl_timer_set`、p006 の形）にした。足した timer は 18（p006 までの 22 と合わせて 40、heap は 128）。二つの形がある:

- handler が仕事をし、tick の時刻の比べを消した物（始めで arm、終わりは disarm か、早く・遅く発火した handler が見直す）:
  - titlebar の key repeat（2-22、`titlebar-shell.c`）: `kwl_titlebar_tick` と main.c の毎 pass の呼び出しを消した。`shell_repeat_start` が delay の後に arm、handler（`shell_repeat_fired`）が押し直して次を arm。止めた後の 1 回は何もしない。
  - band の長押し（2-18、`shell.c`）: 10 ms ごとの `kwl_due_soon` をやめ、press で閾値の時刻に 1 回 arm（`band_hold_fired`）。
  - OSK の手書きの認識 600 ms（2-24、`keyboard.c`）: stroke の終わりで arm、handler が条件（手書きの面、書いていない、未認識、panel が開いていて login・lock・Home・Wiseview でない）を見て認識。
  - Home の launch を忘れる 30 s・corner の Notes の 5 s（2-31、`home.c`・`corner.c`）: launch で arm、handler が期限の過ぎた物を忘れる。
  - notify-log を閉じる 10 s（2-33、`notify-log.c`）: 開いて arm、入力は `used_ms` を動かすだけで、早く発火した handler が新しい期限に arm し直す（p006 の idle の lock の形）。fade の `kwl_due_soon` は p008。
  - volume の送りの間隔 50 ms（2-40、`volume.c`）: 送りを待たせる時に arm、`volume_send` で disarm。
  - system の scan の表示 1 分（2-44、`system.c`）: 毎 pass の `system_scanning_expire` をやめ、asking で最も早い期限に arm、handler が期限切れを放して次を arm。
  - printers の 1 s（2-04、`printers-shell.c`）: printers の object を作る時に arm、handler が「生きた printers の object か printd の接続があるか」を見て、あれば check を io-file の thread に渡し 1 s 後に arm、無ければ止める。printd の接続が開いた時（tick）にも arm。hosted でない backend は handler が起こした pass の update で file を見る。
- handler は wake だけで、判定は今の tick（状態と絡む）に残した物（tick が `kwl_due` の代わりに timer を arm）:
  - apps bar の hover 400 ms・leave 300 ms（2-23、`apps-bar.c`）、greeter の lock の card 30 s と security key（grant の hold と KEYOWNER の 1 s の間隔、2-25、`greeter.c`。key は二つの期限の早い方に 1 つの timer）、rotate の hold（2-35、`output-rotate.c`）、scanout の pointer の静止と lend の拒み（2-36、`scanout.c`、2 つ）、layer cache の静けさ（2-37、`layer-cache.c`）、glass の blur の hold（2-39、`glass.c`）。
- 診断: `KWL PERF timers fired=N name=count ... other=M` の行を KWL PERF の窓ごとに出す（`kwl_timers_take` が名前で数える、8 種まで、残りは other）。idle の残る起床の内訳を試験で読むため。

### 設計からの変更（理由）

1. 2 つ目の形（handler は wake だけ）の 7 機能は、tick の毎 pass の判定を残した。期限の判定が状態（pointer の下の物、窓の待ち、host の image の書ける時、入力の静けさ、lock の card の入力の有無と答えの待ち）と一緒になっていて、状態の変化ごとに arm し直す形に割ると、各所に呼び出しを足すことになり p007 の範囲（期限）を越える。tick 自体を消すのは p008（毎 pass の tick を無くす）。idle では timer か事象でしか起きないので、目標（idle で眠る）には影響しない（下の QEMU の idle）。
2. `kwl_glass_tick`（shell.c の UI の tick 全体）は残る。中の期限（band・apps bar・home・corner・notify-log・volume）は上のとおり timer。animation（`kwl_due_soon`）と時計の分は p008。
3. 止める側で disarm しない timer がある（titlebar の repeat・band・手書き・home・corner・system の scan・printers）。止めた後に 1 回だけ発火して何もしない（p006 の「早く発火した handler が見直す」形の反対側）。止める所の多く（`shell_repeat_stop`・`kwl_titlebar_key_released` など）が server を持たない。
4. printers の 1 s は「printers の面がある間」を「生きた printers の object か printd の接続がある間」にした（printd の終わりの待ち 10 s の打ち切りも見るため）。

### 確かめ（実行したコマンドと結果）

- build（package の規則は -Wall -Wextra -Werror）: x86_64 は `make -j4 BUILD_JOBS=4 CONFIGMK=plan/ws353/tests/config-amd64-mainwake.mk build/x86_64/packages/wayland/.stamp` rc 0・warning 0（wayland の 134 file）、arm64・riscv64 は `make -j4 BUILD_JOBS=4 CONFIGMK=config/config.rpi4.mk sysroot` と `... build/rpi4/packages/wayland/.stamp`（vf2 も同じ）で rc 0・warning 0（134 file、診断を足した後に 98 file を compile し直して warning 0）。x86_64 は QEMU の image の build（`config-amd64-ui.mk`、userland と disk）でも warning は noct の外で 0。sysroot の vulkan の header が古く（build を前の worktree から写した。p009 の前）、x86_64 の sysroot を `make ... sysroot` で作り直した。
- 書き方: `python3 -I plan/tools/style-check.py` を変えた C の file に流し、基点の同じ file と比べて増えた指摘は 0。`git diff --check` は空。
- host: `sh plan/ws353/tests/timer/run.sh` PASS（plain・ASan/UBSan・TSan。名前ごとの数と report の後の 0 の check を足した）。p007 の file を link する host 試験は ws131 の `host-system.sh`（system.c・printers-shell.c）だけで、基点で既に link できない（p006 の記録のとおり）。流していない。
- QEMU（Venus、max_outputs=1、image は `plan/ws353/tests/config-amd64-ui.mk`＝p006 の image に kernel の input の注入（`CONFIG_INPUT_TEST_INJECT`）と aat-input、Settings、printd、`perf-timers.sh`）: `sh plan/ws353/tests/qemu-ui-deadlines.sh OUT`。判定は guest の log の file を serial で数える（console の log で判定しない）。どの step も、入力が止まった後は step の timer の他に起こす物が無いので、続く行が timer の働きを示す。
  - 1 回目（`build/ws353-p007-qemu`）: idle の窓が aat-input の device の追加（2-03 の 100 ms の settle）を含み FAIL（試験の直し: idle の前に窓を PING で閉じる）、titlebar の focus の行の `edit=1` の期待が誤り（Ctrl+F は `edit=0`）、rotate は display の key の取り出しの誤り。他は PASS。
  - 2 回目（`build/ws353-p007-qemu2`）: rotate の key に空白があり FAIL、printd の program 名の誤り（`printd`）で printers の object が出来なかった。titlebar の repeat は PASS（count=41）。
  - 3 回目（`build/ws353-p007-qemu3`）: titlebar の repeat の行を待つ 10 s が足りず FAIL（行は log にあり count=41。待ちを 30 s に）。他は全部 PASS。
  - 4 回目（`build/ws353-p007-qemu4`）: idle-30 の窓の合計が 26 s で FAIL（idle の途中の network-grace の pass が窓を出し、最後の PING が 5 s 未満で窓を出さなかった。判定を「秒数 − 5 s 以上」に）。他は全部 PASS。
  - 5 回目（`IMAGE=build/x86_64/hdd-image.img sh plan/ws353/tests/qemu-ui-deadlines.sh build/ws353-p007-qemu5`、3 回目と同じ image）: 全 step PASS（`qemu-ui-deadlines: PASS`）。idle-30 `ms=31175 passes=2 timeouts=0`（timer は network-grace 1）、idle-settings-30 `ms=30033 passes=123 timeouts=1`（timer は printers-check 30）、idle-after-settings-30 `ms=31184 passes=4 timeouts=1`（timer 0）。
  - 各 step の値（3〜5 回目で同じ）: layer-quiet は start の後の静けさで Home の絵を 1 回焼いた。notify-log は開いて見えてから 10.2〜10.4 s で `close reason=idle`。band の長押しは `hold ms=501`。titlebar の repeat は 2 s で 41 回（400 ms の delay と 25/s）。apps bar は icon の上で hover の preview、離れて `close via=leave`。rotate は 90 と 0 の 2 回が適用、old frame の hold は 2 回とも Settings の窓の commit で終わった（`held_ms=618〜629 windows=1 late=0`、timer の経路は通っていない）。OSK の手書きは 3 stroke を認識。stop で `KWL TIMER stopped` と `KWL EXIT`、`KWL FAILED` 0。
- idle の閾値（`qemu-main-wake.sh`・`qemu-deadlines.sh`・`qemu-ui-deadlines.sh`）を「5 pass/s・2 timeout/s」から「10 s に 2 pass・1 timeout」に狭め、窓を idle の前に閉じる形にした。`IMAGE=build/x86_64/hdd-image.img`（UI の image。p005・p006 の image の上位）で:
  - `qemu-deadlines.sh build/ws353-p007-deadlines` PASS: idle-30 `ms=31183 passes=5 timeouts=1`、idle-after-monitor-20 `ms=20335 passes=1 timeouts=1`、desktop の再起動 2003〜2004 ms、IME、monitor、shot、`--timeout` の late 1 ms。
  - `qemu-main-wake.sh build/ws353-p007-mainwake` PASS: idle-30 `ms=31166 passes=5 timeouts=1`、idle-after-20 `ms=21203 passes=1 timeouts=0`、idle-after-plug-20 `ms=21208 passes=1 timeouts=0`、壁紙・shot・USB の keyboard。
  - どちらも狭めた閾値で 1 回流した。窓の長さの判定を「秒数 − 5 s 以上」にした直しはその後で（窓はどちらも idle の秒数以上だった）、直しの後には流していない。
- 残る起床の内訳（client の無い idle、`KWL PERF timers` の行と passes・timeouts から）: 30〜60 s で passes 1〜5・timeouts 0〜1。timer の発火は network の start の 30 s の grace（`network-grace`、起動の後に 1 回）だけ。timeout の 1 は時計の分（`kwl_due_next_minute`、p008 で timer へ）。残りの pass は試験の PING の client（窓を閉じる pass）。p006 の後の 2 pass/s・1 timeout/s（printers の 1 s）は無くなった。
- printers の 1 s（Q1 の問い）: printd のある image で、client の無い idle は printers の timer 0。Settings を開くと（libkeiland の `kl_system_open` が printd のある時 printers の object を必ず作る）30 s に `printers-check=30`、閉じると 0（idle-after-settings-30 `passes=1〜4 timeouts=0〜1、timers 0`）。Settings が開いている間は他に 2〜3 pass/s あり、timer でも timeout でもない（Settings の client の要求。この Phase の外）。

### 未実施

- QEMU で確かめていない期限: greeter の lock の card と security key（lock は sessiond の managed な session が要る）、volume の送り（audiod が要る）、scanout の pointer の静止と lend の拒み（game mode の client が要る）、glass の blur の hold（壁紙を変える間に images が書けない時だけ）、rotate の hold の timer の経路（QEMU では窓の commit が先に来た）、home の launch の 30 s と corner の Notes の 5 s、system の scan の 1 分。どれも code の読みと build。
- Linux・FreeBSD の backend の build。arm64・riscv64 は build だけ。実機は p016。
- ws131 の `host-system.sh` は基点で link できないまま（p006 の記録）。

### p008 への申し送り

- 2 つ目の形の 7 機能（apps bar・greeter の card と key・rotate・scanout 2 つ・layer cache・glass の blur の hold）は tick が毎 pass に判定して timer を arm する。tick を消すには、状態の変化（pointer の移動、窓の commit、host の image が書けるようになる時、入力、lock の card の答え）から判定を呼ぶ形が要る。
- `kwl_glass_tick`（shell.c）と `kwl_keyboard_tick`・`kwl_corner_tick`・`kwl_home_tick` などの UI の tick は、animation（`kwl_due_soon`）と状態の追従のために毎 pass に残る。touch.c の title bar の指の待ち（`kwl_due_soon` で見ている期限）、keyboard.c・corner.c の contact の arm の期限（`KEYBOARD_ARM_MS`・`CORNER_ARM_MS`、contact の間の `kwl_due_soon`）も期限なので、animation の整理の時に timer に。
- 時計の分（`kwl_due_next_minute`）が client の無い idle に残る唯一の timeout。
- 試験: idle の内訳は `KWL PERF timers` の行と `/root/perf-timers.sh`（`config-amd64-ui.mk` の image）で読める。`qemu-ui-deadlines.sh` の idle の閾値は 10 s に 2 pass・1 timeout。p008 の後は timeout 0 にできる。

### 範囲の外（Q1 へ）

- printers の 1 s（2-04）は「道が無い」まま残る: printers.conf の変化を知らせる物が無く、printers の object がある間 1 s ごとに file を見る。libkeiland は printd がある時 kl_system を開く全ての app に printers の object を作るので、Settings・Files・Photos・Monitor・pdfviewer などが開いている間 1 s の起床が続く。本来の形: printd が設定を持ち変化を push する、か kernel の file の変化の通知。小さく狭めるなら libkeiland が printers の object を使う時（`kl_system_printers_*` の最初の呼び出し）だけ作る形。design.md の 2-04 に書いた。
