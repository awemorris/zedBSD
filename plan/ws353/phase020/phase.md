<!-- awesome-plan project=zedbsd record=ws353-p020 -->

# ws353-p020: sessiond が入力の device の node を利用者に chown するのをやめ、1 s ごとの giving を事象だけに

Status: cleared（2026-11-14 P1 q2083、Q1 の確認待ち）
Parent: [WS353](../ws.md)

## 目的

2026-11-14 ユーザーの決定（ws.md）: sessiond が入力の device を開き fd を SCM_RIGHTS で compositor に渡す（p018）ので、入力の device の node を利用者に chown する必要は無い。sessiond を通さず compositor が node を直に開くのは Linux（logind がある）と FreeBSD だけで、FreeBSD では /dev/input が chown されている前提で進める。

今の sessiond（`userland/desktop/sessiond/seat.c` の `sessiond_seat_give`）は、greeter と session の間、`/dev/input/event*` を利用者に chown・chmod し、greeter は 1 s ごと（`GREETER_SEAT_MS`）、session は loop の毎回、それを繰り返して新しい device を拾う（node ごとの stat）。

## 範囲

1. `sessiond_seat_give`・`sessiond_seat_restore` から `/dev/input/event*` の chown・chmod を外す。event の node は devfs の既定（root・wheel の mode）のまま。compositor は p018 の `OPEN eventN` で受け取る。sessiond を通さない時の compositor の hotplug の thread が node を直に開く道（FreeBSD 向け）は変えない。
2. 残る giving（GPU・backlight・hidraw（security key）・smart card・camera）を点検し、hotplug で増えうる物（hidraw・camera・smart card など）は `/dev/system` の事象（`sessiond_seat_events_collect`）が来た時だけ与える形にする。greeter の `GREETER_SEAT_MS` の 1 s の見回りと、session の loop の毎回の giving を消す。事象の届かない device の種類があれば、根拠（kernel の事象の class）を書いて Q1 へ（勝手に見回りを残さない・kernel を変えない）。
3. sessiond の loop の poll の timeout・`sleep(1)` が giving のためだけにある所は無くす。他の締め切り（greeter の起動の待ちなど）のための timeout は残してよいが、一覧を結果に書く。
4. host 試験（sessiond の既存の試験があれば追従）、build 3 機種（x86_64・arm64・riscv64）warning 0（`-Wall -Wextra -Werror` を足した `-fsyntax-only` でも 0 を確かめる。package の compile は今それを付けていないため）。QEMU: login（greeter → session）・入力（keyboard・pointer）が効く・USB の入力 device の後からの挿し込みが効く（QEMU の monitor の device_add）、ls -l /dev/input で event の node が root のままである事。

## 規則

WS353 の design.md と p018 の phase.md に従う。kernel・HAL・Makefile は変えない。

## 結果

2026-11-14 P1（q2083-i01、branch `agent/p1-ws353l`、main a5995aab0e から）。

### 変更（source dc1ad79ea6）

- `userland/desktop/sessiond/seat.c`: `sessiond_seat_give`・`sessiond_seat_restore` から `/dev/input/event*` を外した（node は devfs の既定 root・wheel 0640 のまま。compositor は p018 の `OPEN eventN` で fd を受ける）。`SEAT_INPUT_MODE` を消し、先頭の説明を「giving は greeter・session の始まりと `/dev/system` の ADD・overflow の時だけ」に直した。
- `greeter.c`: `GREETER_SEAT_MS` の 1 s の見回りを消し、事象（`sessiond_seat_events_collect` が 1）の時だけ `sessiond_seat_give(..., 0)`。poll の timeout は `greeter_timeout`: READY の前は起動の締め切り（`SESSIOND_GREETER_START_SECONDS`）まで、READY の後は無期限（-1）、認証の試み中は `SESSIOND_EXCHANGE_TICK_MS`。
- `main.c`・`sessiond.h`: SIGTERM・SIGINT の handler が 1 byte 書く pipe（`pipe2(O_CLOEXEC|O_NONBLOCK)`、`sessiond_stop_fd`）。greeter の loop が無期限に待っても止める合図を取りこぼさないため（pipe が作れない時だけ greeter は 1 s の上限）。
- `session.c`: loop の毎回の giving を消し、事象の時だけ（Log Out の後を除く）。`control` が閉じた後の `sleep(1)` を消し、同じ poll（`fd=-1` は無視される）で seat の事象と socket も聞き続ける。停止の pipe も poll する（停止を言った後は外す。読みっぱなしで空回りしないため）。

### 事象の届かない device（kernel の class が無い。見回りは残していない）

- `/dev/gpu*`: 追加の事象の class が無い（`include/uapi/system.h` の `KERN_SYSTEM_EVENT_*` に GPU は無い。p002 の HOTPLUG は display の connector の事で、node の追加ではない）。sessiond は最初の greeter の前に kernel が attach 中の GPU を待つ（`main_wait_display`・`main_gpu_attaching`）ので、起動時の GPU は与えられる。greeter・session の後から attach した GPU は次の greeter・session の始まりまで与えられない。
- `/dev/backlight/backlight*`: 同じく class が無い（`src/drivers/generic/backlight.c` は事象を出さない）。session の始まりにある物は与えられる。session の後から現れた backlight は次の始まりまで与えられない。
- hidraw（`KERN_SYSTEM_EVENT_INPUT`、`hidraw.c` は cdev の登録の後に ADD）・smart card（`KERN_SYSTEM_EVENT_USB`、登録の後に ADD）・camera（`KERN_SYSTEM_EVENT_CAMERA`、登録の後に ADD）は事象で与える。

### sessiond に残る poll の timeout・sleep

| 所 | 値 | 何のため |
|---|---|---|
| greeter の loop（`greeter_timeout`） | READY 前: 起動の締め切りまで（最大 60 s） | greeter が READY を言わず固まった時に失敗として終わらせる（`SESSIOND_GREETER_START_SECONDS`） |
| greeter の loop | 認証の試み中 `SESSIOND_EXCHANGE_TICK_MS`（100 ms） | passkey の締め切り・TERM の後の KILL・失敗の返事の遅延（auth.c の時刻の判定） |
| greeter の loop | 停止の pipe が無い時だけ 1 s | 止める合図が poll の直前に来た時の取りこぼしの上限 |
| session の loop | 1 s（試み中 100 ms） | session の process の終わりを `waitpid(WNOHANG)` で見る（SIGCHLD を待つ fd が無い）と、自動 login の session が起動で固まった時の締め切り。design.md の 6-01（WS352 で SIGCHLD を self-pipe に）。giving のためではなくなった |
| `sessiond_read_line` | 呼ぶ側の上限（READY 30 s・release 5 s） | handoff の締め切り |
| `main_wait_display` | 100 ms ごとの見回り、最大 120 s | 起動時に GPU の node が現れるのを待つ（事象の class が無い） |
| `sleep(delay)`・`sleep(MAIN_RESTART_PAUSE)`・`session.c` の `sleep(2)`・`greeter.c` の `usleep(50000)` | 再試行の間・終わりの待ち | 締め切り・間隔で、giving とは無関係（変えていない） |

### 確かめた事

- build: `make -j4 BUILD_JOBS=4 CONFIGMK=plan/ws353/tests/config-amd64-seat.mk build/x86_64/packages/sessiond/.stamp` rc 0・warning 0。arm64・riscv64 は `CONFIGMK=config/config.rpi4.mk`・`config/config.vf2.mk` で `sysroot` の後に `build/rpi4/packages/sessiond/.stamp`・`build/vf2/packages/sessiond/.stamp`、rc 0・warning 0（sessiond の 13 file を compile）。
- 3 機種で main.c・greeter.c・session.c・seat.c を package と同じ flag に `-Wall -Wextra -Werror -fsyntax-only` を足して compile: rc 0。
- host 試験 `sh plan/ws353/tests/seat-host.sh build/p020-seat-host`: `seat-host: PASS 53 checks`。
- QEMU（新しい `plan/ws353/tests/qemu-seat-owner.sh`、seat の config の image、Venus）: 3 回目 `qemu-seat-owner: PASS`（16 step 全 PASS、image の build の warning（noct の外）0）。
  - session: 起動の device 3 個を sessiond の OPEN で開く。`ls -ln /dev/input` の event0〜2 は `crw-r----- 0 0`、sessiond の log に `SEAT path=/dev/input/event` は 0 行。
  - USB の mouse を QMP の device_add: event3 を OPEN 1 回で受け、node は root のまま、QMP の相対移動で pointer が x=640 y=400 → x=361 y=191（capture の POINTER）、抜くと閉じる。
  - session の 20 s の idle で compositor の timeout 0。
  - session の compositor を SIGTERM → greeter が来て event0〜2 を自分の seat の socket で開く。node は root のまま。
  - greeter に USB の keyboard を挿す: event3 を greeter の OPEN で受け、その keyboard（QMP の input-send-event）で PIN 0000 と Enter → `SESSIOND AUTH ok user=user style=pin`、新しい session（pid 308）が READY。最後の `ls -ln`: event0〜3 は root・wheel 0640、`/dev/gpu0` は 1000 の 0600。
  - 1・2 回目の FAIL は試験の誤り（login 画面は PIN を先に聞くのに password を打った）。sessiond の動作は 3 回とも同じ。
- 成果物: `build/ws353-p020-qemu/`（summary.txt・sessiond.txt・greeter.txt・session-1.txt・session-2.txt・ls-input.txt・ls-end.txt・qmp.txt）。

### 未実施

- sessiond 自身の起床の回数の計測（greeter の idle で poll が返らない事）は数えていない（sessiond に計数の log が無い。code の上では READY の後の timeout は -1）。
- sessiond への SIGTERM（停止の pipe の道）を QEMU で試していない。
- 後から attach する GPU・backlight、hidraw・smart card・camera の挿し込みの QEMU は未実施（hidraw は事象の class が INPUT で、扱いは前と同じ）。
- 実機は未実施。
- compositor 側の記述: `libkeiland-backend-zedbsd/input-zedbsd.c` の comment（「node の owner が変わる前に」「EACCES は scan ごとに試す」）と、thread が無い時の `kl_backend_input_scan`（node を直に開く）は sessiond の下では EACCES になる。範囲（compositor の直に開く道は変えない）の外なので触れていない。
