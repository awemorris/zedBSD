<!-- awesome-plan project=zedbsd record=ws353-p018 -->

# ws353-p018: 入力の device の hotplug を sessiond に開かせる（100 ms の settle の scan を消す）

Status: planned
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
