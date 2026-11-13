<!-- awesome-plan project=zedbsd record=ws353-p019 -->

# ws353-p019: printers の 1 s の見回りを消す（printd の push と、一覧は設定から）

Status: cleared（2026-11-14 P1、q2074-i01。build 3 機種 warning 0、host 試験 PASS、QEMU の qemu-printers.sh PASS（下の「結果」）。session の resume・実機は未実施。Q1 の確認待ち）
Parent: [WS353](../ws.md)

## 目的

2026-11-14 のユーザーの方針（ws.md「ユーザーの決定（続き）」）: printd は印刷の spool の間だけ起動する（前からの実装）。printd が起動している間は pipe で printd に繋いで push を受ける。kernel に file の変化の通知などを足さない。printer の一覧を得るのに printd と通信する必要は無く、Settings が printd と話すのは spool の中身を見る時だけ。

今は kl_system を開く app（Settings・Files・Photos・Monitor・pdfviewer、libkeiland の ui/app.c）が開いている間、compositor の printers-shell が設定の file を 1 s ごとに見る（設計の対応表 2-04、p007 の記録）。

## 範囲

1. 調べ: printers の設定の file を誰が書くか（Settings・compositor の io-file・printd）、printers の object を作る所（libkeiland）、printd の起動と終わり、今の 1 s の見回りが拾っている変化は何か。
2. 設計（この phase.md に追記）: 一覧と設定の変化を、書く側（compositor 自身か、それに要求する client）から事象で知らせる形。printd が起動している間の spool の状態は printd への pipe の push で。libkeiland は printers の object を使う時だけ作る。
3. 実装と試験（host、QEMU で Settings を開いた間に printers の timer が 0、printer の追加・削除・印刷の状態が見える）。
4. HAL・Makefile・mk/・toolchain は変えない。kernel を変えない。

## 受け入れ

- printers の 1 s の見回りが無い（Settings などを開いた間も）。
- 設計の対応表の 2-04 を「消えた」に直す。

## 調べ（2026-11-14 P1、q2074-i01、基点 main 3bc314683b）

- **設定の file を書く者**: `~/.config/keiland/printers.conf` を書くのは compositor だけ（WS145 の設計 D2）。compositor の printers-shell.c が libkeiland-backend の print（`print/print.c`）を開き、変更（add・remove・default・edit、printd の NAMED の名前）は backend が file の lock の下で読み直し・変え・rename で書く。compositor では書く・見るの両方を io-file の thread で（`kl_backend_print_host`）。Settings・pdfviewer・printtest は compositor の printers の object に要求するだけで file を触らない。printd は file を読まない（JOB の行に host・port・path が入る）。
- **今の 1 s の見回りが拾う物**（`printers_check_fired`、printers の object か printd の接続がある間）:
  1. 他の session（同じ利用者の別の compositor）が書いた file の変化（mtime が違えば読み直し、list を tell）。
  2. printd の program が今あるか（`can`、hosted の時は check だけが見直す）。
  3. 書き込みを止めた（protocol を破った）printd の 10 s の打ち切り（`shut_until`、update が時刻を比べる）。printd の接続がある間 timer を回すのはこのため。
  - 自分の変更は書いた後に lane の done で loop を起こし、次の pass の update が答えと list を tell する（1 s に頼らない）。printd の job の状態・IDLE・BYE・終わりは socket（socketpair、printd の fd 3）の行で、compositor はその fd を poll している（既に push）。
- **printers の object**: libkeiland の `kl_system_open` が manager の version 17 以上で capability があれば必ず `get_printers` する（`system.c` の system_make）。だから kl_system を開く全ての app（Settings・Files・Photos・Monitor・pdfviewer など）が object を持ち、その間 1 s が回る（p007 の記録）。
- **printd の起動と終わり**: backend が仕事（JOB・NAME）のある時に posix_spawn で起こす。printd は 60 s 仕事が無いと IDLE を言い、backend が BYE を返し、printd が終わる（socket の終わり）。printd の中の 1 s の poll（表 6-12）は WS352 の範囲で、この Phase の外。
- hosted でない形（io-file の thread が起動できなかった時）: backend の writer の thread が書き、update が毎 pass に file を stat する。writer の thread は loop を起こさないので、答えは他の事象の pass まで待つ（今は 1 s がそれを隠していた）。

## 設計

ユーザーの方針（ws.md「ユーザーの決定（続き）」: printd は spool の間だけ、起動している間は pipe で push、kernel に足さない、一覧に printd との通信は要らない）に合わせ、周期の見回りを全部やめて、それぞれを事象に付ける。

1. **一覧は設定の file から、表示する時に 1 回読む**（変化を追わない）。周期の check をやめ、file の check（io-file の thread で stat、変わっていれば読む）は printers の object が作られた時（Settings などが一覧を表示する時）に 1 回だけ渡す。
   - 2026-11-14 Q1 経由のユーザーの意見（Settings は表示した時点の printer を出せばよく、監視は要らないかもしれない）を受けて、最初の設計にあった session の resume の時の読み直しを外した（他に残す理由が無い）。
   - 自分の変更は今のまま（書いた後に lock の下で読み直した table が答えと一緒に来て、全ての object に tell。他の session の変更もそこで入る）。
   - 開いている Settings の一覧は、別の session が後で file を変えても、次に object を作る時か、この compositor の次の変更まで変わらない（表示した時点の一覧、ユーザーの意見のとおり）。
   - check を頼む印（`check_wanted`）を立て、tick が check の無い時に渡す（check が進行中なら、終わった後の tick で次を渡す）。program の有無（`can`）も同じ check で見直す。
2. **printd の状態は今の socket の push のまま**（変えない）。compositor は接続の fd を poll し、行が来た pass で update が読む。
3. **printd の 10 s の打ち切りは期限の timer**。backend に `kl_backend_print_due`（update が fd の事象が無くても呼ばれるべき monotonic の時刻 ms、無ければ UINT64_MAX）を足し、tick が update の後にその時刻で timer（`printers-shut`）を arm、無ければ disarm。handler は何もしない（起きた pass の tick の update が socket を閉じる）。printd の接続がある間の 1 s は無くなる。
4. **io-file の thread が無い時も compositor が host する**。`kl_backend_print_host` を常に呼び、lane に渡せない（ENOTSUP）書き込みと check は event loop でその場で走らせる（io.c の「thread が無い時は loop で」と同じ形）。backend の writer の thread（loop を起こさない）と update の毎 pass の stat を compositor は使わなくなる。backend の hosted でない形は API として残す（他の host のため）。
5. **libkeiland の printers の object は今のまま作る**（Q1 の範囲 2 の「使う時だけ作る」はしない）。理由: 上の 1〜3 で compositor が object ごとにする周期の仕事が無くなり、object は idle で何も起こさない（変化の時だけ event）。使う時だけ作るには、`kl_system_printers_get`・`kl_system_print_jobs_get`（const の getter）が最初の呼び出しで object を作り往復を待つ形か、新しい公開の関数（app が printers を使い始める時に呼ぶ）が要り、libkeiland の公開の約束（open の後に最初の状態がある）と Settings・pdfviewer・printtest を変える。得る物（印刷の間に printers を使わない app が起きない、object を作る時の check が減る）に対して変更が大きいので、要るなら別の Phase に。
6. 変えない物: HAL・Makefile・mk/・toolchain・kernel・printd・libkeiland・protocol。

### 対象の file

- `userland/desktop/wayland/printers-shell.c`: 周期の timer（`printers_check_timer`・`PRINTERS_CHECK_MS`・`printers_wanted`・`checked_ms`）を消し、`check_wanted` と shut の timer、常に host、lane が無い時のその場の実行。
- `userland/desktop/libkeiland-backend/keiland-backend.h`・`print/print.c`: `kl_backend_print_due`、host の check の説明（「from time to time」を「理由がある時」に）。
- （Q1 の指示で足した小さな欠陥の修正）`print/print.c`: 設定の file を書く・lock する前に、path の folder を上から順に作る（`print_make_folders`。前は `~/.config/keiland` の 1 段だけで、`~/.config` が無い home では printer の追加が保存されなかった）。

### 確かめ

- build: x86_64・arm64・riscv64 の wayland の package（libkeiland-backend を含む）で warning 0。
- host: `plan/ws309/tests/print-file/run.sh`（backend の hosted の書き込みと check、変えた所の回帰）と、新しい `plan/ws353/tests/printers/run.sh`（`kl_backend_print_due`: 接続が無い時と普通の printd の間は UINT64_MAX、protocol を破った printd を shut した後は約 10 s 先、その時刻の後の update で接続が閉じ UINT64_MAX に戻る）。
- QEMU: 新しい `plan/ws353/tests/qemu-printers.sh`（`config-amd64-printers.mk`＝ui の image に printtest）。Settings を printers の頁で開いた 30 s の idle で printers の timer の発火 0（`KWL PERF timers` に printers-* が無い）、printtest の add（`~/.config` の無い home で保存まで）・remove で Settings に変化が届く（`ZSETTINGS PRINTERS changed` が増える）、host の偽の LPD に print して job の状態（sending → done）が compositor と printtest に見える、printd が残っている間の idle も printers の timer 0、printd が IDLE の後に終わる、file を直に変えて（別の session の代わり）10 s 何も起きず、新しい object（printtest list）で読み直される。

## 結果（2026-11-14 P1、q2074-i01、branch `agent/p1-ws353j`、基点 main 3bc314683b）

### commit

- f160f7d253: host 試験 `plan/ws353/tests/printers/`（`host-print-due.c`・`run.sh`）。
- 4127369e4c: 実装（`printers-shell.c`・`backend-host.c`・`kwl.h`・`keiland-backend.h`・`print/print.c`）。最初の設計（resume の時も check）。
- a0d512f733: QEMU の試験（`qemu-printers.sh`・`config-amd64-printers.mk`・`fake-lpd.py`）、`qemu-ui-deadlines.sh` の古い comment（Settings の間の printers の check）の直し。
- 8e23d064d1: Q1 の指示（ユーザーの意見）で resume の時の check を外した（`backend-host.c`・`kwl.h` は基点に戻った）。`qemu-printers.sh` の不備の直し（下）。
- 77589a8bc6: Q1 の指示で `print.c` の folder の作り（`print_make_folders`、lock と保存の前に path の folder を上から作る）、host 試験 `host-print-folders.c`、QEMU の試験を `~/.config` の無い home に。
- この記録の commit（phase.md・design.md・ws.md、SHA は報告に書く）。

### 実装（最終の形）

- printers の周期の timer（`printers-check`、1 s）を消した。printers の object が作られた時に `check_wanted` を立て、次の tick が io-file の thread に設定の file の check を 1 回渡す（変わっていれば読んで全 object に tell）。session の resume では読まない。自分の変更は前のとおり（書いた後の table が答えと来る）。
- printd の状態は前のとおり socket の push（compositor はその fd を poll）。
- protocol を破った printd の 10 s の打ち切りは、backend の `kl_backend_print_due`（monotonic ms、無ければ UINT64_MAX）で tick が `printers-shut` の timer を arm・disarm（handler は wake だけ、起きた pass の update が socket を閉じる）。
- compositor は常に backend を host し、io-file の thread が無い時は書き込み・check を loop でその場で走らせる（backend の writer の thread は loop を起こさないため）。check を lane に渡せなかった時は `kl_backend_print_check_end(NULL, …)` で捨てる（前は走っていない check で `can` を 0 にしていた）。
- `print.c` の `print_make_folders`: 設定の file の lock と保存の前に path の folder を上から順に 0700 で作る（あれば EEXIST で素通り）。前は `~/.config/keiland` の 1 段だけで、`~/.config` の無い home では printer の追加が保存されなかった（QEMU の 1 回目で見つけた）。
- libkeiland の printers の object は今のまま `kl_system_open` で作る（設計の 5。object は idle で何も起こさない）。

### 確かめ（実行したコマンドと結果）

- build（package の規則は -Wall -Wextra -Werror）: x86_64 は `make -j4 BUILD_JOBS=4 CONFIGMK=plan/ws353/tests/config-amd64-ui.mk sysroot` の後 `... build/x86_64/packages/wayland/.stamp` rc 0・warning 0、最終の code は QEMU の image の build（`config-amd64-printers.mk` の userland と disk、warning は noct の外で 0）。arm64・riscv64 は `make -j4 BUILD_JOBS=4 CONFIGMK=config/config.rpi4.mk sysroot`（vf2 も）と `... build/rpi4/packages/wayland/.stamp`（vf2 も）を最初の code で rc 0・warning 0（425 file）、最終の code で rc 0・warning 0（125 file を compile し直し、print.c・printers-shell.c を含む）。build は前の worktree p1-ws353i の build から toolchain 等を copy した（symlink でない）。
- 書き方: `python3 -I plan/tools/style-check.py` を変えた C の file と新しい試験に流し、指摘 0（printers-shell.c は基点の 1 件も消えた）。`git diff --check` 空。
- host: `sh plan/ws353/tests/printers/run.sh`（ASan+UBSan と TSan）を最終の code で PASS: host-print-due（接続が無い時と protocol を守る printd の間は UINT64_MAX、line が長過ぎる printd を shut した後は約 9.0〜9.9 s 先＝秒の粒度、その後の update で閉じ、printd が去れば UINT64_MAX）、host-print-folders（`~/.config` の無い home で最初の変更が保存され、`~/.config` と `~/.config/keiland` が 0700、lock の file、別の backend が読む）、ws309 の host-print-file。
- QEMU（Venus、max_outputs=1、`IMAGE=build/x86_64/hdd-image.img sh plan/ws353/tests/qemu-printers.sh OUT`。判定は guest の file を serial で数える）:
  - 1 回目（`build/p019/qemu1`、最初の code）: 試験の不備で FAIL。guest の runtime dir が 0755 で printd が spool を作らず（EPERM、job は failed daemon）、`~/.config` の無い home で設定の file が書けず add の答えが not saved（error=5、上の print.c の欠陥）、`wait_in` の失敗が `set -e` で script を止めた。printers の timer 0（Settings の 30 s・printd の 20 s）は PASS。
  - 2 回目（`build/p019/qemu2`、resume を外した code、試験は runtime を 0700・`~/.config/keiland` を作って）: 全 step PASS。
  - 3 回目（`build/p019/qemu3`、最終の code、`~/.config` の無い home）: 全 step PASS（`qemu-printers: PASS`）:
    - idle-30 `ms=33235 passes=5 timeouts=0`（timer は network-grace 1）。
    - Settings を Printers の頁で開いた idle-settings-30 `ms=31254 passes=6 timeouts=0`、timer は clock 1、printers-* は 0（p007 では 30 s に printers-check 30）。2 回目は `ms=147336 passes=12`（窓が Settings の起動の前から続いていた）で printers-* 0。
    - add: printtest の答え error=0、`~/.config/keiland/printers.conf` に行が保存、Settings の `ZSETTINGS PRINTERS changed` が 1 増えた。
    - print: host の偽の LPD に PDF を印刷、printtest は done で終わり（status 0）、compositor の `KWL PRINTERS job=1 printer=1 state=4` 1 行、偽の LPD が job を 1 つ受けた、Settings の changed が 1 → 4（queued・sending・done）。
    - printd が残っている間の idle-printd-20 `passes=1 timeouts=0`、timer 0。printd は idle の step の後 31 s で自分で終わった（IDLE → BYE）。
    - 別の session の代わりに file を直に変えて 10 s、Settings の changed は 4 → 4（見回らない）。printtest list（新しい object）で 5 になり、次の list に新しい printer が出た。
    - remove: error=0、Settings が聞いた。Settings を閉じた idle-after-settings-30 `passes=4 timeouts=0`（timer は clock 4）。stop と KWL FAILED 0 も PASS。

### 未実施

- io-file の thread が無い時の loop でのその場の実行（thread の起動の失敗は作れない。code の読みと build）。
- `printers-shut` の timer の QEMU での発火（protocol を破る printd が要る。host 試験で `kl_backend_print_due` と打ち切りを確かめた）。
- session の resume（外したので無し）、Linux・FreeBSD の backend の build、実機。

### Q1 へ

- 設計の 5（libkeiland の「使う時だけ作る」）はしていない（理由は設計の 5）。要るなら別の Phase。
- printd の中の 1 s の poll（表 6-12）は WS352 の範囲のまま（この Phase の外）。
