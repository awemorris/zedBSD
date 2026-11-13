<!-- awesome-plan project=zedbsd record=ws353-p008 -->

# ws353-p008: animation・frame の pacing・時計、main の poll を -1 に（main loop の 4）

Status: cleared（2026-11-14 P1、q2068-i01。build 3 機種 warning 0、host 試験 PASS、QEMU の qemu-animation.sh・qemu-main-wake.sh・qemu-ui-deadlines.sh PASS、qemu-deadlines.sh は 2 回目で PASS（下の「結果」）。poll は -1、client の無い idle の timeout 0。状態に追従する tick は毎 pass に残る（設計からの変更 3、Q1 の判断待ち）。Q1 の確認待ち）
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.2・§5.9、対応表の 1-01・2-13・2-17・2-38。

## 目的

animation を「動いている間だけ frame の完了で一歩」にし、frame の pacing と時計を timer にする。残りの期限を timer に移し終えたら、main の poll の timeout を常に -1 にし、`kwl_due`（branch を merge した時）と毎 pass の tick を消す。

## 対象（file）

- animation の 16 か所（apps-bar.c・greeter.c・keyboard.c・arrange-shell.c・corner.c・home.c・notify-log.c・power-dialog.c・notify-popup.c・touch.c・shell.c・sleep.c）: 描く物は「描いた frame の完了で一歩進め、まだ動いていれば次を描く」。完了は render の COMPLETED（p011 の後は REAPED。どちらも GPU の完了の直後）で、p010 の後は T17（shown）の output の SHOWN。描かずに時刻だけで進む物（sleep の保留、spring の判断）は timer。
- frame の pacing（display.c:255 `frame_done + frame_wait`）: timer。
- 時計（main.c の次の分、shell.c・greeter.c）: 時計が見える時だけ arm、消灯・時計の面が無い間は disarm。実時刻の次の分までの長さを monotonic に直して arm し、handler で実時刻を読み直して次を arm する（時刻の設定の後のずれを次の発火で直す。設計 §5.5、レビュー M7）。
- main.c: poll の timeout を -1 に固定。`kwl_due`・`kwl_due_soon` と `due_ms` を消す（branch を merge した時）。`kwl_backend_wakes()` が 0 の backend（Linux・FreeBSD）の 10 ms は、その backend の事象の口が出来るまで残してよい（keiland-linux・freebsd は崩れてよい platform）。

## 残す振る舞い

- animation の速さ（時刻で位置を決める物は時刻で、frame ごとの物は frame で）、pacing の待ちの長さ、時計の表示。

## 確かめ

- build warning 0。QEMU: slide・fade・OSK・touch の追従・notify の popup が前と同じ時間で終わる（`--log-frames` の時刻を file から）。guest の時刻を進めた・戻した後に時計の分が次の発火で合う。idle 60 s の main の `KWL PERF` の `timeouts` が 0、WS352 の poll の診断で main の timeout の起床が 0（時計が見える時は分に 1 回）。

## 受け入れ

- main の poll が -1 で、毎 pass に全部の tick を呼ぶ code が無い。結果・SHA・未実施を書く。

## 依存

- p006・p007。

## 結果（2026-11-14 P1、q2068-i01、branch `agent/p1-ws353h`、基点 main 9fb6ff0010）

### commit

- 2bb0d4a44d: compositor（`userland/desktop/wayland/` の 25 file）と `plan/ws353/tests/timer/`（retire の host 試験を `kwl_timer_set` に）。
- aa872a02a3: 他の WS の host 試験の直し（`plan/ws197/tests/phone-shell-host-test.c`: phone の取り込みを wake の後だけにしたので試験の server を woken に。`plan/ws191/tests/host-audio-server.c`: 基点で既に link できなかった。`kwl_watch`・`kwl_client_fail`・`kl_backend_audio_stream_descriptor` の偽物と、この Phase の `kwl_soon` の偽物を足した）。
- 1eba71dfba: `notify-popup.c` の KWL NOTIFY show・hide・gone の行に `at_ms`（試験が時間を読む）、`shell.c` の comment、QEMU の試験 `plan/ws353/tests/qemu-animation.sh`・`config-amd64-anim.mk`。
- この記録の commit（SHA は報告に書く）: `qemu-main-wake.sh`・`qemu-deadlines.sh`・`qemu-ui-deadlines.sh` の idle の閾値（timeout 0）、design.md §5.9 の注、この記録と ws.md。

### 実装

- main loop（`main.c`・`kwl.h`）: `kwl_due`・`kwl_due_soon`・`kwl_due_next_minute`・`server->due_ms` を消した。poll の timeout は `loop_timeout` で常に -1。例外は 2 つだけ: backend が wake しない（`kl_backend_wakes` が 0、Linux・FreeBSD）時の 10 ms（`KWL_SOON_MS`）と、timer の thread が起動できなかった時の最も早い timer。scheduler の後に dirty が立った時の `kwl_due(0)` は自分の wake pipe への 1 byte にした（timeout 0 を使わない）。
- 共通の道具: `kwl_timer_wake_at(server, timer, name, at_ms)`（handler が何もしない timer。起きた pass で tick が判定する、p007 の 2 つ目の形を 1 行に）、`kwl_soon(server)`（事象の無い物の 10 ms の歩み、毎 pass 動かす timer `soon`。使うのは `kwl_watch` の溢れと、descriptor の無い audio の pump＝Linux・FreeBSD の backend だけ）。`kwl_timer_set` は同じ時刻の arm し直しを省き、heap が満ちた時は log だけ（`kwl_due` への落としを消した）。
- animation（対応表 2-13）: 16 か所の `kwl_due_soon` を消した。描く物は tick が dirty を立て、描いた frame の完了（render の COMPLETED・READY の wake）の pass で tick が時刻から一歩進めて次の dirty を立てる（home・shell の desktop の slide・anim・Wiseview・keyboard の slide と窓の移動・arrange の glide・corner の hint・notify-log の fade・notify の board・power dialog・greeter の spinner・sleep の保留の frame・apps bar の spring の光）。描かずに時刻だけで決まる物は wake だけの timer: OSK と corner の contact の arm の期限（`osk-arm`・`corner-arm`）、apps bar の spring の判断（`spring`）、touch の title bar の待ち（`touch-title`、一本指の待ちと二本指の flick の期限）、greeter の power の最長の待ち（`greeter-power`）、sleep の保留の期限（frame が来ない `KWL_SLEEP_LOCK_MS`、sessiond が busy の `KWL_SLEEP_SEND_MS`、既存の `sleep_timer`）。
- frame の pacing（2-38、`display.c`）: `frame_done + frame_wait` を `pace_timer`（`frame-pace`）に。
- 時計（2-17）: `kwl_clock_follow(server, shown)`（`main.c`）と `kwl_clock_shown`（`shell.c`: 消灯・lid の黒・sleep の要求の間は見えない）。見える間だけ、実時刻の次の分＋20 ms までを monotonic に直して arm し、timer の pass（glass と greeter の tick）が実時刻を読み直して次を arm する。見えなければ disarm。`--log-frames` の時 `KWL CLOCK arm into_ms= left_ms=` を出す。
- 入力の scan（2-03 の残り、`input_scan_pace`）: 事象の後 2 s の 100 ms と、事象を知らせない backend の 2 s を `input_scan_timer`（`input-scan`）に。
- 子の回収（p006・p007 の申し送り）: SIGCHLD の handler が flag を立てて wake、pass は wake の後に flag を下ろして `children_reap` が `waitpid(-1, WNOHANG)` で全部を回収し、IME（`kwl_ime_child_ended`）・desktop（`kwl_desktop_child_ended`）・media の helper（`kwl_media_library_child`）・その他（`KWL HOME ended`）に渡す。IME の tick・desktop の `desktop_watch`・home の tick の `waitpid` を消した（一つの回収者なので互いの子を取り合わない）。
- thread の結果の取り込み（p006 の申し送り (3)）: `server->woken`（wake pipe を空にした pass、backend が wake しない時は毎 pass）の間だけ network（`kl_backend_network_update`）・phone（`kl_backend_phone_update`）・machine の読みの終わりを見る。flag は poll の直前に下ろす。

### 設計からの変更（理由）

1. animation の一歩は render の COMPLETED（と READY）の pass から（設計 §5.9 の p010 より前の段階のとおり）。p010 で T17 の SHOWN に替える。
2. touch の指の追従（`follow_step`）は frame でなく、指が追われている間 10 ms の timer（`touch-follow`）。動くのは cursor の plane だけで frame の完了が来ず、motion の点は report の間も動くため。指が無い間は無い。
3. 毎 pass の tick の呼び出しは、時刻の比べと自分を起こす code を消した形で残した（`kwl_settings_tick`・`kwl_system_tick`・`kwl_ime_tick`・`kwl_window_bounds_refresh`・`input_scan_pace`、schedule の中の sleep・displays・glass・desktop の tick）。どれも事象か timer で来た pass の中で状態に追従するだけで、待ち・起床を作らない。tick を状態の変化の所から呼ぶ形に割るのは、sleep の規則の入力（10 か所余り）、p007 の 2 つ目の形の 7 機能（pointer の移動・窓の commit・host の image・入力・lock の card の答え）などに呼び出しを足す作業で、この Phase の範囲（期限と animation）を越える。受け入れの「毎 pass に全部の tick を呼ぶ code が無い」は、この意味（時刻で見回る tick が無い）でだけ満たした。字義どおりに満たすかは Q1 の判断を仰ぐ（下の「Q1 へ」）。
4. sleep の保留中に sessiond が EBUSY の時の再送は、前の 10 ms ごとから frame ごと（保留の間は毎 pass dirty）と sessiond の答えの pass に。打ち切りは今までどおり `KWL_SLEEP_SEND_MS`（timer）。
5. 通知の board は画面にある間（stay の 3 s も）毎 frame 描く（前と同じ。stay の間を timer にするのは hover の数え方を変える事になり範囲の外）。

### 確かめ（実行したコマンドと結果）

- build（package の規則は -Wall -Wextra -Werror）: x86_64 は `make -j4 BUILD_JOBS=4 CONFIGMK=plan/ws353/tests/config-amd64-mainwake.mk sysroot` の後に `... build/x86_64/packages/wayland/.stamp` rc 0・warning 0。arm64・riscv64 は `make -j4 BUILD_JOBS=4 CONFIGMK=config/config.rpi4.mk sysroot` と `... build/rpi4/packages/wayland/.stamp`（vf2 も同じ）で rc 0・warning 0。x86_64 は QEMU の image の build（`config-amd64-anim.mk`、userland と disk）でも warning は noct の外で 0。toolchain と Venus の renderer は p1-ws353g の build から写した（symlink ではない）。
- 書き方: `python3 -I plan/tools/style-check.py` を変えた C の file に流し、基点の同じ file と比べて増えた指摘は 0。`git diff --check` は空。
- host:
  - `sh plan/ws353/tests/timer/run.sh`: PASS（plain・ASan/UBSan・TSan）。
  - `OUT=build/tmp/p008-phone3 sh plan/ws197/tests/phone-shell-host-test.sh`: 直す前は woken の門で FAIL、直して PASS。
  - `sh plan/ws191/tests/host-audio-stream.sh`: 基点で link できなかった物を直して PASS（host-audio-stream・host-audiod・host-pump）。
- QEMU（Venus、max_outputs=1）: `sh plan/ws353/tests/qemu-animation.sh build/ws353-p008-qemu`（image を作る、1 回目）で全 step PASS。判定は guest の log の file を serial で読む（console の log で判定しない）。
  - idle-60: `ms=61127 passes=5 timeouts=0`、timer は network-grace 1・clock 1（p007 の後に残った時計の分の timeout 1 が timer に移った）。
  - home-open: `KWL HOME open via=super` から `opened` まで 302 ms（設計 280 ms、9 frame）。home-close: 261 ms（240 ms）。
  - notify-board: show から gone まで 3607 ms（280＋3000＋280＝3560 ms）。
  - clock-forward・clock-back: `date` で壁の時計を 10 分進め・戻し、どちらも次の発火で arm し直し、その次の arm は分の 21 ms 後（`KWL CLOCK arm into_ms=21`）。
  - osk-slide: `KWL OSK slide end leaving=0 frames=6 max_gap_ms=47`（200 ms の slide を 6 frame、最長の間 47 ms）。
  - idle-after-20: `passes=4 timeouts=0`、timer は clock 2。stop: `KWL TIMER stopped` と `KWL EXIT`。KWL FAILED 0。
- 証拠: `build/ws353-p008-qemu`（summary.txt、kwl.log は serial で読んだ写し）。
- 回帰（同じ image、`IMAGE=build/x86_64/hdd-image.img`、idle の閾値を「10 s に 2 pass・timeout 0」に狭めて）:
  - `qemu-main-wake.sh build/ws353-p008-main-wake`: PASS（idle-30 `passes=2 timeouts=0`、idle-after-20 `passes=2 timeouts=0`、idle-after-plug-20 `passes=6 timeouts=0`、壁紙 2 回・shot・USB の keyboard の抜き差し）。
  - `qemu-deadlines.sh build/ws353-p008-deadlines`: 1 回目は timeout の step だけ FAIL（「bounded run が 60 s で終わらない」）。guest の log には `KWL TIMEOUT after_ms=10000 late_ms=1` と `KWL EXIT` があり、timer は働いた。前の compositor の EXIT から bounded の EXIT まで 55 s（2 回目は 25 s）で、起動（READY まで、timer は event loop の始めから数える）が遅れたと見るが原因は確かめていない。同じ条件で流し直した `build/ws353-p008-deadlines2` は全 step PASS（desktop の再起動 2004〜2085 ms・IME の再起動と give-up＝SIGCHLD の後の回収、idle-30 `passes=5 timeouts=0`、idle-after-monitor-20 `passes=1 timeouts=0`、monitor、shot の打ち切り、`--timeout` の late 1 ms）。
  - `qemu-ui-deadlines.sh build/ws353-p008-ui-deadlines`: PASS（idle-30・idle-after-settings-30 は timeout 0、titlebar の repeat 41、apps bar、rotate、OSK の手書き、notify-log、band）。Settings を開いた 30 s は `passes=123 timeouts=0`、timer は printers-check 30・clock 1（printers の 1 s は p007 のとおり道が無い）。
- 証拠: `build/ws353-p008-main-wake`・`build/ws353-p008-deadlines`・`build/ws353-p008-deadlines2`・`build/ws353-p008-ui-deadlines`。

### 未実施

- QEMU で確かめていない物（code の読みと build）: touch の指の追従と title bar の二本指（touch screen の窓の試験が無い）、corner の hint と contact の期限、apps bar の spring（drag and drop）、sleep の保留（sessiond の managed な session が要る）、greeter の power の spinner、arrange の glide、power dialog の fade、Wiseview・desktop の slide、frame の pacing の timer の経路（client の FIFO の窓が要る）、lock・login の画面の時計、消灯の間の時計の disarm、network・phone の取り込みの woken の門（networkd・bluetoothd が要る）、machine の読み。
- WS352 の poll の診断（kernel 側）での確かめ。KWL PERF の timeouts で代えた。
- Linux・FreeBSD の backend の build（keiland-linux は崩れてよい）。arm64・riscv64 は build だけ。実機は p016。

### Q1 へ

- 受け入れの「毎 pass に全部の tick を呼ぶ code が無い」: 時刻で見回る tick と自分を起こす code は無くした（poll は -1、idle の timeout 0）が、状態に追従する tick（settings・system・IME・sleep・displays・glass・desktop など）は事象と timer の pass ごとに呼ぶ形のまま（上の変更 3）。字義どおり（tick を消し、状態の変化の所から呼ぶ）にするなら別の Phase が要る。どちらで受けるかの判断を仰ぐ。

### p010・p016 への申し送り

- p010: animation の一歩は今 render の COMPLETED・READY の pass の tick（`kwl_glass_tick` などが時刻から位置を決め dirty）。T17 の SHOWN に替える時は、SHOWN の pass で同じ tick が走れば足りる（tick は時刻で位置を決めるので、どの完了で来ても速さは変わらない）。frame の callback を SHOWN で送る時、`display.c` の `pace_timer`（`frame_done_ms + frame_wait_ms`）の `frame_done_ms` を何で立てるかを合わせる。`kwl_timer_wake_at` は「起こすだけ」の timer の道具として使える。
- p010: `KWL NOTIFY show・hide・gone`・`KWL HOME open・opened`・`KWL OSK slide end` の `at_ms`・frames・max_gap_ms と `qemu-animation.sh` で、SHOWN に替えた後も同じ時間で終わる事を確かめられる。
- p016: idle の判定は `timeouts=0`（KWL PERF）で、client の無い idle の timer は時計の分（`clock`、1 分に 1 回）と起動の後の `network-grace` 1 回だけ。5330 では消灯の間は時計の timer も無い（`kwl_clock_shown`）。touch の指がある間の 10 ms（`touch-follow`）、Linux・FreeBSD の audio の pump と `kwl_watch` の溢れの `soon` は idle には無い。
