<!-- awesome-plan project=zedbsd record=ws353-p006 -->

# ws353-p006: 締め切り A（session・system・protocol の期限）を timer へ（main loop の 2）

Status: cleared（2026-11-14 P1、q2062-i01。build 3 機種 warning 0、host 試験 PASS、QEMU の qemu-deadlines.sh PASS（下の「結果」）。sleep と handoff は毎 pass の tick が残り比べは timer と同じ pass で走る形（設計からの変更 1）。Q1 の確認待ち）
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.5、対応表の 2-05・2-06・2-15・2-16・2-19〜2-21・2-26〜2-30・2-32・2-34・2-41〜2-43。

## 目的

main が毎 pass に時刻を比べる（または `kwl_due` で出す）締め切りのうち、session・system・protocol の物を、予定がある間だけ arm する timer にする。毎 pass の tick の呼び出しを消す。

## 対象（file）

- fatal client の flush 2 s・`--timeout`（main.c）、handoff（handoff.c）、sleep・消灯・pause・rest（sleep.c）、idle の lock（shell.c）、ping（toplevel.c）、IME（input-method.c）、clipboard（clipboard.c）、drag の release（data.c）、desktop の再起動（desktop.c）、network の 30 s・切り替えと詳細の 1 s（network.c。1 s は view がある間の timer、周期の読み直しが消えるのは networkd の push の後＝範囲の外）、displays の保留（displays-shell.c、p004 で移した物）、sysmon（購読がある間）、machine の読みの打ち切り（machine-shell.c）、shot の答え（shot.c）、phone（phone-shell.c）。
- `kwl_ping_check`・`kwl_ime_tick`・`kwl_sleep_tick` など、毎 pass に呼んでいた tick を、事象の handler と timer の handler に割る。tick の中の「thread の結果の取り込み」は wake の後の take へ。

## 残す振る舞い

- 各期限の長さと、期限の後の結果（lock・消灯・sleep・ping の無応答・IME の再起動など）。時刻の基準（`kwl_milliseconds` の CLOCK_MONOTONIC）。

## 確かめ

- build warning 0。host: 変えた機能に host 試験があれば流す（sleep-rules など）。
- QEMU: idle の lock と消灯の時刻、ping の無応答の表示、IME の再起動、clipboard の打ち切り、desktop の再起動、Log Out（handoff）。WS352 の poll の診断で、idle（予定なし）の main の timeout の起床が、この Phase の対象の分 0。

## 受け入れ

- 対象の期限が timer で動き、毎 pass の比べが code に無い。結果・SHA・未実施を書く。

## 依存

- p005。

## 結果（2026-11-14 P1、q2062-i01、branch `agent/p1-ws353f`、基点 main 2c1152a565）

### commit

- bc7c2c6082: compositor（`userland/desktop/wayland/`）の p006 と `plan/ws353/tests/timer/retire-stubs.c`。
- 94ad48a837: 他の WS の host 試験の直し（`plan/ws197/tests/phone-shell-host-test.c`、`plan/ws309/tests/clip-write/host-clip.c`。Q1 の指示で範囲に入れた）。
- この記録の commit（SHA は報告に書く）: QEMU の試験（`plan/ws353/tests/qemu-deadlines.sh`・`config-amd64-deadlines.mk`・`fake-ime.sh`）、log の行の時刻（desktop の `exited ... at_ms`、`KWL TIMEOUT ... late_ms`）、この記録。

### 実装

- 共通（`main.c`・`kwl.h`）: `kwl_timer_set(server, timer, at_ms)`（arm、`UINT64_MAX` で disarm、heap が満ちたら `KWL TIMER full` を出しその pass の `kwl_due` に落とす）。timer は機能ごとに 1 つ（client ごとに作らない）で、足したのは 20（p005 の 2 と合わせて 22、heap は 128）。各機能の timer は初めて使う時に `kwl_timer_init`（server の物は起動の時）。
- fatal client の flush 2 s（2-15）: `kwl_client_fail(client)` を作り、`fatal = 1` と `fatal_time` を置いていた 23 か所をそれに替えた。失敗の時に timer を今に arm、handler は送り残しのある失敗の client のうち最も早い `fatal_time + KWL_FATAL_FLUSH_MS` に arm し直す（退かせるのは今までどおり client の loop）。poll の集合を作る所の `kwl_due(0)`・`kwl_due(fatal + 2000)` を消した。
- `--timeout`（2-16）: event loop の始めに 1 回 arm、handler が `time_up` を立て、loop の頭は `time_up` を見る（`now - started` の比べを消した）。`KWL TIMEOUT after_ms=N late_ms=M` を出す。
- ping 5 s（2-26、`toplevel.c`）: `kwl_ping_check` を消し、ping を送った時に最も早い期限へ arm、handler が無応答を決めて次へ。
- IME（2-27、`input-method.c`・`ime.h`）: 再起動の timer（`restart_ms` を置く 5 か所で arm、`ime_spawn` で disarm）と答えの timer（watching で arm、答え・lost で disarm）。`kwl_ime_tick` は waitpid と lock・App Home への追従だけ。
- clipboard 2 s（2-28）: 読みと書きの期限の最も早い物に timer、handler は最後に一度読み書きしてから打ち切る。`kwl_clipboard_offer_write` は server を取る（timer のため）。
- drag の release（2-29、`data.c`）: 待ちの始めに arm、決まった時と drag の終わりに disarm、handler は答え無しで決める。`kwl_data_tick` は答えの追従だけ。
- desktop の再起動 2 s（2-30）: 終わり・起動の失敗で `desktop_gone` が arm、`desktop_start` は timer が arm されている間は始めない（時刻の比べを消した）、handler が起動。
- handoff（2-19）: backend の Log Out の期限（`kl_backend_wait` の due）を timer に。毎 pass の `kwl_handoff_tick`（sessiond の行の読み）は残る（下の変更 1）。
- sleep の 4 つ（2-20）: `sleep_idle` の 4 つの `kwl_due` を最も早い 1 つの timer に。保留・待ちの間は disarm。handler は空で、起きた pass の `kwl_sleep_tick` が進める（下の変更 1）。保留の `kwl_due_soon`（sleep.c、対応表 2-13 の animation の 1 つ）は p008 のまま。
- idle の lock（2-21、`shell.c`）: `kwl_lock_idle_pace` を起動・unlock（greeter.c の 2 か所）・設定の変更（settings.c）で呼ぶ。入力のたびには arm し直さず、早く発火した handler が `lock_input_ms` から新しい期限に arm し直す（入力 1 回ごとの heap の操作を避ける。lock の時間に 1 回まで余分に起きる）。shell の tick の比べを消した。
- displays の保留 3 s（2-34、`displays-shell.c`）: move を保留する時に arm、move が決まると disarm、handler は applied で決める。`kwl_displays_pending_tick` を消した。
- network（2-05・2-32、`network.c`）: start の grace 30 s（watch を作る時に arm）、switch の hold（答えで arm、settle・新しい要求で disarm）、詳細の 1 s（開いて arm、handler が読んで次、閉じて disarm）。`network_switch_settle` は `expired` を取る。
- sysmon（2-06）: `kwl_sysmon_tick` を消し、object の作成・破棄・period の変更と sample の取り込み（`sysmon_done`）で pace、handler が look と sample を出す。object が無い間は timer が無い。
- machine の打ち切り 10 s（2-41）: 読みの始めに arm、終わりで disarm、handler が打ち切る。`kwl_machine_tick` は終わりの取り込みと次の始めだけ。
- shot の答え 3 s（2-42）: 要求で arm、送りの始めと終わりで disarm、handler が timeout で終える。
- phone の held 10 s・sync 150 s（2-43）: held を置く時と sync を待たせる時に pace、handler が期限の過ぎた物を捨てる・TIMEOUT を返す。tick の時刻の比べを消した。

### 設計からの変更（理由）

1. sleep（2-20）と handoff（2-19）は、毎 pass の tick を残し期限だけを timer にした（handler は空か tick を呼ぶ）。sleep の規則の入力（最後の入力、前面の fullscreen、蓋、電源、出力、Log Out、greeter の起動中）は 10 か所余りの水準で、事象で割るには各所に呼び出しを足すことになり、p006 の範囲（期限）を越える。handoff の読みは sessiond の fd の読みで、比べは backend（session-zedbsd.c）の中にある。どちらも毎 pass の tick を消すのは p008（「毎 pass に全部の tick を呼ぶ code を無くす」）の仕事として残す。idle の時の起床は timer だけなので、WS の目標（idle で眠る）には影響しない。
2. idle の lock は「入力で arm し直す」（一般の作法）でなく、早く発火したら新しい期限に arm し直す形にした（理由は上）。
3. `kwl_clipboard_offer_write` の引数に server を足した（data.c の 1 か所）。
4. IME・desktop の waitpid（子の回収）は毎 pass のまま（SIGCHLD の wake で pass は来る）。App Home の `waitpid(-1)`（home.c、p007 の file）と一緒に、SIGCHLD の事象で回収する形へ移すのは p008 に申し送る。
5. tick の中の thread の結果の取り込み（machine の done、network・phone の backend の update）は毎 pass のまま（他の take と同じ扱い。wake の後だけにするのは p008 の tick の整理と一緒に）。

### 確かめ（実行したコマンドと結果）

- build（package の規則は -Wall -Wextra -Werror）: `make -j4 BUILD_JOBS=4 CONFIGMK=plan/ws353/tests/config-amd64-mainwake.mk build/x86_64/packages/wayland/.stamp` rc 0・warning 0（全 file を compile）。arm64・riscv64 は `make -j4 BUILD_JOBS=4 CONFIGMK=config/config.rpi4.mk sysroot` と `... build/rpi4/packages/wayland/.stamp`（vf2 も同じ）で rc 0・warning 0（wayland の 134 の compile）。x86_64 は QEMU の image の build（userland と disk、warning は noct の外で 0）でも最後の source を compile した。arm64・riscv64 は最後の log の行の直しの後に、変えた 4 file（main.c・desktop.c・clipboard.c・input-method.c）を compile し直して rc 0・warning 0。
- 書き方: `python3 -I plan/tools/style-check.py` を変えた C の file に流し、基点（2c1152a565 の同じ file）と比べてこの Phase で増えた指摘は 0。`git diff --check` は空。
- host:
  - `sh plan/ws353/tests/timer/run.sh`: PASS（timer と retire の歩調、plain・ASan/UBSan・TSan）。objects.c が `kwl_client_fail` を呼ぶので stub を足した。
  - `OUT=build/tmp/p006-phone sh plan/ws197/tests/phone-shell-host-test.sh`: PASS（89 の check、150 s の sync の TIMEOUT と held の 10 s を含む）。基点で link できなかった（WS352 の `kwl_due` の偽物が無い）。timer の偽物（`kwl_timer_init`・`kwl_timer_set` と、期限の過ぎた timer を発火してから tick する pass）を足して直した。
  - `sh plan/ws309/tests/clip-write/run.sh build/tmp/p006-clip`: PASS（読まない reader の打ち切りを含む）。基点で link できなかった（`kwl_watch` の偽物が無い）。同じく timer の偽物と pass を足し、新しい引数に合わせた。
- QEMU（Venus、max_outputs=1、runtime `build/ws353-p006-run`）: `sh plan/ws353/tests/qemu-deadlines.sh OUT`（image は `config-amd64-deadlines.mk`＝main-wake の image に keiland-system と、すぐ終わる偽の input method `fake-ime.sh` を `/usr/libexec/keiland-ime` として足した物）。判定は guest の log の file を serial の grep・awk で数える（console の log で判定しない）。
  - 1 回目（`build/ws353-p006-qemu`）: desktop の間隔の判定を start の `at_ms` どうしで取ったが、最初の start の行は spawn の報告が遅れる分だけ後ろにずれ（2102・3016・3008 ms）FAIL。stop は 20 s の待ちが serial の乱れで足りず FAIL（log には `KWL TIMER stopped` と `KWL EXIT` があった）。他は PASS。desktop の終わりの行に `at_ms` を、`KWL TIMEOUT` に `late_ms` を足し、終わりから次の start までを測る形と、待ちの最後にもう一度見る形に直した。
  - 2 回目（`build/ws353-p006-qemu2`、image を作り直し）: `--timeout` の `late_ms` を guest の sed で取れず FAIL（写しの log には `late_ms=1`）。他は全 PASS。awk に替えた。
  - 3 回目（`IMAGE=build/x86_64/hdd-image.img sh plan/ws353/tests/qemu-deadlines.sh build/ws353-p006-qemu3`、2 回目と同じ image）: 全 step PASS。
    - start: window mode、`KWL THREADS` に timer、capture が listening。
    - desktop-restart: `starts=4 restarts=3 low=2003 high=2086`（終わりから次の start まで。2 s の timer と start の報告の遅れ）、その後 `start-limit`。
    - ime-restart: started 3・lost 3 の後に given-up（1 s の再起動の timer、3 回で諦める）。
    - idle-30: `ms=35048 passes=75 timeouts=36`（約 2 pass/s・1 timeout/s、p005 と同じ。残りは printers の 1 s）。
    - monitor: 5 s で sample 19（250 ms の period）、client の後に `subscribers=0`。idle-after-monitor-20: `ms=25036 passes=51 timeouts=25`（250 ms の look は止まった）。
    - shot-timeout: frame の来ない partial の要求が 3652 ms で `KWL SHOT error=timeout`（serial の確かめの粒度を含む）。
    - stop: SIGTERM で `KWL TIMER stopped` と `KWL EXIT`。
    - timeout: `--timeout=10` の compositor が `KWL TIMEOUT after_ms=10000 late_ms=2` と `KWL EXIT`。
    - failed: 2 つの compositor とも `KWL FAILED` 0。
- 証拠: `build/ws353-p006-qemu`・`build/ws353-p006-qemu2`・`build/ws353-p006-qemu3`（summary.txt、kwl.log・bounded.log は serial で読んだ写しで記録用）、host は `build/tmp/ws353-timer`・`build/tmp/p006-phone`・`build/tmp/p006-clip`。

### 未実施

- QEMU で確かめていない期限（code の読みと build、host 試験のある物はそれだけ）: fatal client の flush（protocol の error を起こす試験の client が無い）、ping の無応答（ping は toplevel への押しで出る。応えない client が無い）、clipboard の 2 s（QEMU は未。host 試験で読まない reader の打ち切りを確かめた）、drag の release、idle の lock・消灯・sleep・Log Out の handoff（sessiond の managed な session が要る。BUG-264 の AAT の image の形の試験は流していない）、displays の保留、network の 30 s・switch・詳細（networkd が要る）、machine の打ち切り、phone（host 試験で held と sync の期限を確かめた）。
- WS352 の poll の診断の kernel での確かめ（p005 と同じく KWL PERF の窓で代えた: idle の 30 s で約 2 pass/s・1 timeout/s、残りは printers の 1 s（2-04、p007））。
- Linux・FreeBSD の backend の build（keiland-linux は崩れてよい）。arm64・riscv64 は build だけ。実機は p016。
- ws131 の `plan/ws131/tests/host-system.sh`（system.c・sysmon.c・machine-shell.c・phone-shell.c などを host で link する試験）は基点で既に link できない（kl_backend_phone_*・kl_backend_notify_*・kl_backend_session_key_*、kwl_due・kwl_watch・kwl_io_*_submit・kwl_thread_* など 40 余りの偽物が無い）。直していない。判断: system の拡張の protocol（不正な要求で client を終える、範囲の外を invalid で答える）と libkeiland の view を両端で確かめる本当の回帰試験で、WS113・WS132・WS142・WS172・WS191・WS314 の記録と BUG-222・BUG-413 が回帰に使っている。直す価値はある。ただし system.c が依存を増やすたびに偽物が足りなくなる作りで、直すなら偽物を 1 つの file に分け、system.c の依存を link の境で切る別の Phase が要る。推し: 消さずに別の Phase で直す。

### p007・p008 への申し送り

- 使い方: 機能の static な `struct kwl_timer` を初めて使う時に `kwl_timer_init`、`kwl_timer_set(server, &timer, at_ms)` で arm（`UINT64_MAX` で disarm）、handler `(void *context, struct kwl_timer *timer)` は context が server。client ごと・物ごとの timer は作らず、機能ごとに 1 つで「最も早い期限」に arm し、handler が期限の過ぎた物を片付けて次に arm する（ping・clipboard・phone・fatal の形）。heap は 128、今 22。
- 期限が入力などで後ろにずれるだけの物は、ずれるたびに arm せず、早く発火した handler が新しい期限に arm し直せばよい（idle の lock の形）。前にずれる事象（設定の変更など）では arm し直す。
- p008: (1) sleep の tick（規則の評価）と handoff の tick（sessiond の読み）は毎 pass のまま。tick を消すには sleep の水準（入力、前面の fullscreen、蓋、電源、出力、Log Out、greeter）が変わる所から `kwl_sleep_tick` を呼ぶ形と、backend の session の fd の readiness で読む形が要る。sleep の保留の `kwl_due_soon` も残っている。(2) 子の回収（IME・desktop の waitpid、home.c の `waitpid(-1)`）は毎 pass。SIGCHLD の handler で flag と wake を立て、wake の後に回収する形へ。(3) machine の done、network・phone の backend の update の取り込みは毎 pass。
- QEMU の idle の閾値（5 pass/s・2 timeout/s）は p007 の後（printers の 1 s が timer になれば 0 に近い）に狭められる。`qemu-deadlines.sh` の idle も同じ。
