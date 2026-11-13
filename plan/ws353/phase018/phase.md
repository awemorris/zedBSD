<!-- awesome-plan project=zedbsd record=ws353-p018 -->

# ws353-p018: 入力の device の hotplug を sessiond に開かせる（100 ms の settle の scan を消す）

Status: cleared（2026-11-14 P1 q2072-i01、Q1 の確認待ち）
Parent: [WS353](../ws.md)

## 目的

2026-11-14 のユーザーの決定（ws.md「ユーザーの決定（続き）」）の形にする:

- compositor は device 全般の hotplug を見張る thread を 1 本持つ（監視の対象は入力の device だけではない）。
- sessiond 経由で compositor が動いている時: compositor の hotplug の thread が /dev/system を見張り、入力の device の追加を見たら sessiond に開く事を要求する。sessiond が開いて fd を compositor に渡す（unix socket の SCM_RIGHTS。POSIX の中核の機能だけ、kernel に機能を足さない）。
- sessiond 経由で起動していない時: hotplug の thread が入力の device を自分で open する。
- これで chown の追い越しを待つ 100 ms の settle の scan（`KWL_INPUT_SETTLE_*`、BUG-264）を消す。session の切り替え・Log Out の時に fd を取り上げる（revoke）形も設計に含める（Linux の backend の logind の TakeDevice の形が参考）。

## 範囲

1. 今の形の調べ: sessiond（`userland/desktop/sessiond/` の seat.c・session.c・greeter.c、control の protocol）、backend（`libkeiland-backend-zedbsd/` の events・session）、compositor（input・io-device・backend-host.c・main.c の settle）、p005 の申し送り（(A) の見込み）。
2. 設計（この phase.md に追記）: thread の構成（今の io-device と新しい hotplug の thread の関係）、sessiond との protocol（要求・答え・fd の受け渡し・取り上げ）、sessiond 無しの時の道、誤りの時の振る舞い。
3. 実装と試験: host の試験、QEMU（BUG-264 の hotplug-guest.sh を今の形に直すか同等の物で、login の後に挿した USB の keyboard・mouse がすぐ使える事、settle の scan が無い事、idle の起床が増えない事）。
4. HAL・Makefile・mk/・toolchain は変えない。

## 受け入れ

- `KWL_INPUT_SETTLE_*` と `input_settle_until` が無い。
- sessiond 経由で、挿した入力の device が 1 回の要求で使える（再試行無し）。sessiond 無しでも使える。
- 設計の対応表の 2-03 を「消えた」に直す。

## 設計（2026-11-14 P1、q2072-i01）

### 今の形（調べ）

- kernel: `src/drivers/generic/input.c` は cdev の登録の後に /dev/system の INPUT の ADD（subject は `eventN`）を出す。順は正しい。
- sessiond: 同じ ADD で `sessiond_seat_give` が node を seat の利用者へ chown する（greeter と session の loop。毎秒の与え直しも残る）。compositor は sessiond の fd を session の control（`--control-fd=3`、行の protocol、main が `read` で読む、要求は一度に 1 つ）と greeter の auth（`--auth-fd=3`）しか持たない。
- compositor: main が /dev/system（POWER・LID・AC・BATTERY・INPUT・USB）を poll し、INPUT で `input_changed` → 次の pass で io-device の finding（/dev/input を readdir して未知の node を open）→ main が `kwl_input_probe`。open は chown の前だと EACCES で、覚えずに 2 s の間 100 ms ごとの scan で取り直す（`KWL_INPUT_SETTLE_*`、`input_settle_until`）。
- display の hotplug は p004 の thread（output-switch.c）が Vulkan の fence を `vkWaitForFences` で待つ。標準の Vulkan では device event の fence を fd にできず、fd と同じ待ちには入れない。

### 形

1. **sessiond の seat の socket**（seat.c）: greeter と session を起こす時に、それぞれ AF_UNIX の SOCK_STREAM の socketpair を 1 つ作り、子の端を fd 4（`SESSIOND_SEAT_FD`）、引数 `--seat-fd=4` で渡す（session.sh は `"$@"` で compositor に渡すので script は変えない。子の中では先に fd を 5 以上へ逃がしてから 3・4 に dup2 する）。sessiond の端は seat.c の表（最大 4）に入れ、greeter・session の loop の poll に足す。
   - 要求（compositor → sessiond、1 行）: `OPEN eventN`。名前は `event` と数字だけ（path は sessiond が `/dev/input/` を前に付ける）。
   - 答え（1 行、1 回の `sendmsg`）: `OK` と SCM_RIGHTS の fd 1 つ（`open(O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW)`、fstat で文字 device を確かめる、root として開くので chown を待たない）、または `FAIL errno`（数字）。
   - holder: seat を今持つ compositor の socket だけが fd を得る（`sessiond_seat_hold`）。greeter の起動で greeter の socket、session の起動で session の socket、Log Out の handoff の greeter の起動でその greeter の socket に移る。seat を root に戻す時（`sessiond_seat_restore`）は誰も持たない。holder でない socket の OPEN は `FAIL EPERM`。
   - 相手の端が閉じた（compositor の終わり）socket は閉じて表から外す。
   - 取り上げ: kernel に revoke が無く足さない（ユーザーの決定）ので、開いた fd は取り上げられない。holder の移り（login で session へ、Log Out で新しい greeter へ）で以後の OPEN を断り、開いた fd は compositor の終わり（と session の sweep の kill）で閉じる。強制の取り上げ（Linux の logind の PauseDevice と EVIOCREVOKE にあたる物）は kernel の機能が要るので範囲の外（記録だけ）。今の chown（`sessiond_seat_give`）はそのまま残す（他の利用者の都合と security の判断は別）。
2. **compositor の devices の thread**（io.c、1 本）: device 全般の hotplug（/dev/system の INPUT と USB）を見張る。display の hotplug は p004 の thread のまま（Q1 確認済みの読み）。
   - 起こす時（main、`kwl_io_start` の中、最初の scan の前）: backend が /dev/system を別に開き INPUT|USB を購読し、main の購読から INPUT と USB を外す（POWER・LID・AC・BATTERY は main のまま。設計の決定 2 の A の残り）。
   - 待ち: `kl_backend_devices_wait` で {その /dev/system、backend の取り消しの pipe} を timeout 無しで poll。記録を読み、INPUT の ADD の `eventN` は backend の `kl_backend_input_open`（sessiond の seat の socket があれば OPEN の 1 回の要求、無ければ自分で open）で開いて capability を読み、batch に入れる。INPUT の記録（ADD・REMOVE）は「入力の device が来た・去った」、FIDO の node（detail の usage=f1d0:0001）と USB の smartcard は「鍵が変わった」、OVERFLOW は「全部を見直す」。
   - 知らせ: batch を malloc して列に繋ぎ、main を wake（mutex の下）。main の `kwl_io_take` が列を取り、各 node を `kwl_input_probe`（既に読んでいる path なら io-device で閉じる、io_find_done と同じ）、backend の `kl_backend_devices_take` で refusal の世代を進め、鍵は `keys_changed`、OVERFLOW は `input_changed`（finding を 1 回）。
   - 止め（`kwl_io_stop`）: `kl_backend_input_cancel`（取り消しの pipe に書く。devices の待ちと、sessiond の答えの待ちが ECANCELED で返る）→ join → 取られていない batch の fd を閉じる → `kl_backend_devices_stop`（main の購読を全部の class に戻す）→ io-device の lane を止める。
   - thread を起こせない（backend が対応しない Linux・FreeBSD、/dev/system が無い、失敗）時は今の main の購読のまま（下の 3）。thread が誤りで終わった時も main の購読を戻す。
3. **finding（io-device、起動の時・OVERFLOW・thread の無い時）**: node の open は `kl_backend_input_open` と同じ道（sessiond があれば OPEN）。io-device の無い時の main の scan は sessiond に聞かない（main を sessiond の答えで止めない。sessiond は handoff の READY の待ちの間は答えないため）で自分で open する。
4. **seat の socket の並び**: devices の thread と io-device の finding が同じ socket を使うので、backend の mutex で「要求を書いて答えを読む」を 1 組ずつにする。答えの待ちは {socket, 取り消しの pipe} を timeout 無しで poll（sessiond の答えか socket の終わりで返る。sessiond が handoff・sweep で止まっている間は待つ）。socket の終わり・読めない答え・取り消しの後は以後この socket を使わない（`FAIL` の答えの後は使い続ける）。
5. **消す物**: `KWL_INPUT_SETTLE_MS`・`KWL_INPUT_SETTLE_SCAN_MS`・`input_settle_until`。`kwl_backend_input_changed` は「次の pass で scan」だけ。`input_scan_pace` の settle の枝。
6. **sessiond 無し**（`--testing`・root、text console から起こした compositor）: `--seat-fd` が無いので devices の thread が自分で open する（node は devfs の root:wheel 0640 で、root・wheel は開ける。chown の追い越しは無い）。
7. **誤り**: sessiond の `FAIL`・socket の終わりの node は開かない（再試行しない。次の事象か finding で）。log は `KL SEAT open node=eventN error=E`。devices の thread の /dev/system の read の失敗は thread を終え（`KWL DEVICES unavailable`）、main の購読を戻す。batch の malloc の失敗は開いた fd を閉じ、OVERFLOW と同じに finding を頼む。
8. **API**（userland、HAL ではない）: `keiland-backend.h` の `kl_backend_options` に `seat_descriptor`、`keiland-backend-evdev.h` に `struct kl_backend_device_batch` と `kl_backend_devices_start`・`_wait`・`_take`・`_stop`・`kl_backend_input_cancel`。Linux・FreeBSD（`evdev/input-evdev.c`）は ENOTSUP と何もしない stub。Makefile・`sources.mk`・mk/ は変えない（既存の file に置く）。kernel・HAL・toolchain は変えない。
9. **試験**: build（x86_64・arm64・riscv64、warning 0）、style-check。QEMU（Venus、新しい `plan/ws353/tests/qemu-seat-hotplug.sh`、graphical login の自動 login の session）: login の後に挿した USB の keyboard・mouse が 1 回の OPEN で取られる（sessiond の log の OPEN が 1 回、settle の scan が無い = `KWL EVENT input changed` が出ない）、抜くと閉じる、idle の起床が増えない（devices の thread は事象の時だけ）、`--testing`（sessiond 無し）でも挿した keyboard が使える。

## 結果（2026-11-14 P1、q2072-i01、branch `agent/p1-ws353i`、基点 268de0e7cc）

### 変えた物

- sessiond: `seat.c`（seat の socket の表・holder・`OPEN eventN` の答え・SCM_RIGHTS）、`sessiond.h`（`SESSIOND_SEAT_FD` 4・`SESSIOND_SEAT_SOCKETS` 4・API）、`greeter.c`・`session.c`（socket を作り子に fd 4 と `--seat-fd=4`、holder を移す、loop の poll に seat の socket を足す）、`session.sh`（comment だけ）。
- backend: `keiland-backend.h`（`kl_backend_options.seat_descriptor`。-1 と 0 は「無し」）、`keiland-backend-evdev.h`（`struct kl_backend_device_batch`、`kl_backend_devices_start`・`_wait`・`_take`・`_stop`、`kl_backend_input_cancel`）、`backend-private.h`（devices の fd・取り消しの pipe・seat の lock・`seat_gone`、`kl_backend_input_open`）、`events-zedbsd.c`（devices の購読と待ち、main の購読から INPUT・USB を外し止めで戻す）、`input-zedbsd.c`（sessiond への OPEN と答えの受け取り、finding も sessiond 経由）、`evdev/input-evdev.c`（Linux・FreeBSD の stub）。
- compositor: `io.c`（devices の thread、batch の列、take・止め）、`main.c`（`--seat-fd`、`input_scan_pace` の settle の枝を消した）、`kwl.h`（`KWL_INPUT_SETTLE_MS`・`KWL_INPUT_SETTLE_SCAN_MS`・`input_settle_until` を消した、devices の field）、`backend-host.c`（`kwl_backend_input_changed` は次の pass の scan だけ）。
- 試験: `plan/ws353/tests/seat-host.sh`・`seat-host-test.c`（host）、`qemu-seat-hotplug.sh`・`config-amd64-seat.mk`（QEMU）。`plan/tools/system-events/run-host-events.sh` の cc の行に `input-zedbsd.c`（Q1 の許可）。`design.md` の 2-03 と §4 の入力の scan・§9 の残りの記述。
- HAL・Makefile・mk/・`sources.mk`・kernel・toolchain は変えていない。

### commit

048974e412（設計）、bc24f7d0b8（実装）、9b5a290b8e（規約の直し・QEMU の試験）、f0a8576f6f・41ab86a4af（QEMU の試験の判定の直し）、c0c7f44be5（design.md）、と記録の commit。

### 確かめ

- build（package の規則は -Wall -Wextra -Werror）: x86_64 `make -j4 BUILD_JOBS=4 CONFIGMK=plan/ws353/tests/config-amd64-seat.mk build/x86_64/packages/wayland/.stamp build/x86_64/packages/sessiond/.stamp` rc 0・warning 0。arm64・riscv64 は `CONFIGMK=config/config.rpi4.mk`・`config/config.vf2.mk` で `sysroot` の後に同じ 2 つの stamp、rc 0・warning 0。QEMU の image の build（seat・mainwake の config の userland と disk）も noct の外の warning 0。toolchain と Venus の renderer は p1-ws353h の build から写した（symlink ではない）。
- host: `sh plan/ws353/tests/seat-host.sh` → `seat-host: PASS 53 checks`（ASan・UBSan。sessiond の答え: 名前の検査・無い node の FAIL 2・holder でない FAIL 1・2 回に分かれた要求・終わった socket の片付け。backend: OPEN の行、FAIL の errno と socket を保つ、OK の fd を受ける、socket の終わりで EPIPE、取り消しで ECANCELED。devices の待ち: ADD・FIDO・smart card・OVERFLOW・取り消し、take の callback）。`sh plan/tools/system-events/run-host-events.sh` → `host-events: 54 checks passed`。`plan/tools/style-check.py` で変えた行の指摘 0（seat.c の 192〜238 行の既存の 3 件は触っていない）。
- QEMU（Venus、`sh plan/ws353/tests/qemu-seat-hotplug.sh`、3 回目 build/p018/qemu-3 で全 step PASS）: 自動 login の session の compositor が `--seat-fd` で devices の thread を起こし、boot の 3 device を sessiond の OPEN で開いた。login の後に挿した usb-kbd と usb-mouse は、sessiond の OPEN 1 回（要求 1・許可 1、sessiond の log で open が chown より先）で devices の thread が開き、compositor が取った（ssh の確かめの分解能で約 2.35 s）。挿した後の scan（`KWL EVENT input changed`）は 0、抜くと閉じた。idle の 2 つの窓（20 s）は timeout 0（前 1 pass/25.5 s、後 4 pass/26.7 s、timer の発火 0。後の 3 pass の出所は未調査。devices の thread の起床は全体で 4 回 = 挿し 2・抜き 2 で、idle の間は 0）。session の compositor を SIGTERM で終えると `KWL DEVICES stopped wakes=4`、続く greeter は自分の seat の socket で event0〜2 を sessiond に開かせた（holder の移り）。sessiond の refused OPEN・lost answer は 0。1・2 回目の FAIL は試験の script の判定（node 名の取り出し、idle の窓の区切り、厳しすぎた pass の比べ）で、直して 3 回目。
- QEMU（sessiond 無し、`--testing`、`sh plan/ws353/tests/qemu-main-wake.sh`）を 3 回: 挿した usb-kbd は 3 回とも取られ（184〜200 ms）、抜くと閉じ、挿した後の idle は 1 pass/21 s・timeout 0。全 step の PASS は 1 度も揃わなかった: 1 回目 idle-30（起動の pass が窓に入った。KWL MODE window が遅い）、2 回目 start（window mode まで 120 s を越えた、at_ms=70734）、3 回目 stop（SIGTERM から 20 s の内に KWL EXIT を見なかったが、記録の log には `KWL TIMER stopped`・`KWL EXIT` が出ている）。どれも host の負荷（load 4〜7）の時間の境で、p008 の記録でも KWL MODE window の at_ms は 62〜148 s とばらつく。この Phase の変えた所（挿した device、idle の起床）の step は 3 回とも PASS。

### 設計からの変更

- devices の thread の起床の数え（`KWL DEVICES stopped wakes=N`）を足した（試験の判定に使う）。
- `kl_backend_input_events` は devices の thread が動く時も 1（devices が事象で開くので、周期の scan は要らない）。
- devices の thread が誤りで終わった時は、取り消し（for good）をせずに join だけして main の購読を戻す（finding の sessiond への要求は使い続ける）。

### 未実施・残り

- Log Out の handoff（session → 新しい greeter の holder の移り）は QEMU で流していない（SIGTERM で終えた後の greeter は確かめた）。login 画面から password での login（greeter → session）も未実施。
- 実機（5330 など）は未実施。
- idle の後の窓の 3 pass（timer 0・timeout 0）の出所は未調査（devices の thread の起床ではない）。
- 開いた fd の強制の取り上げは kernel の機能（revoke）が要り範囲の外（Q1 確認済み）。chown（`sessiond_seat_give`）はそのまま。sessiond 経由の compositor は chown 無しでも開けるので、event の node を利用者に chown しない形（他の利用者が鍵入力を読めなくなる）は別の判断として残る。
- `qemu-main-wake.sh` の start・stop の時間の境（120 s・20 s）は負荷の高い host で足りない事がある（試験の道具の問題、直していない）。
