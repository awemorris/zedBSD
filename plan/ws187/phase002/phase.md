<!-- awesome-plan project=zedbsd record=ws187-p002 -->
# ws187-p002: lock の画面の解除の操作（下部からの上へのスワイプ・wheel の上）と猶予

Status: test-wait（T1 への依頼を 2026-10-08 Q1 へ。番号は Q1 が付ける）。実装・build・host 試験まで（q864 の続き、P2、2026-10-08）。猶予は 5 分、利用者の Sleep は手動（2026-10-08 ユーザーの決定、反映済み）
Disposition: normal
Parent: [WS187](../ws.md)
Queue: q864（Q1 2026-10-08「p001（大きな時計）はそのまま続け、続けて p002・p003 も q864 の続きとして承認済み」）

## 由来（2026-10-08 ユーザー）

「画面の下部（下端でなくてよい）から上にスワイプするとロック解除でき、手動ロックされた場合を除き、ロックから一定時間ならスワイプのみで認証不要、認証する場合はスワイプのあとパスワード、PIN、ハードウェアパスキーが選択できる入力画面。タッチパッドもタッチスクリーンもない場合のために、マウスホイールを上方向に回転でロック解除も実装。」

## 設計

### 画面

- lock の画面は、最初は時計（p001）と下の中央の案内「Swipe up to unlock」（日本語「上にスワイプしてロックを解除」）だけを出す。認証の card は出さない。
- 解除の操作があると、猶予の内なら認証なしで解除する（`kwl_lock_release(server, "swipe")`）。猶予の外なら card（今の Password の入力、p003 で方式の選択）を出す。
- card は、何も打たずに 30 秒入力が無ければ隠れて、時計だけに戻る。
- 時計の位置は card が出ても動かない。card を出す前から、card の上に置く配置（p001 の `kwl_lock_clock_layout`）で描く。
- 鍵盤: card が隠れている時に key（modifier 以外）を押すと card を出し、その key は field に入る（鍵盤だけの利用者が打ち始められる。猶予の内でも key では認証なしに解除しない）。
- login の画面（greeter）は変えない（card を常に出す）。

### 解除の操作（`lock-swipe.c`、server を知らない状態機械、host で試験）

- **pointer のスワイプ**（touchscreen の指、mouse の左のドラッグ、touchpad の押し込み・タップのドラッグ。ロック中は全て pointer の press・motion・release として greeter に届く）:
  - 押した所が画面の下 1/3（`y >= height * 2/3`、下端に限らない）なら追う。
  - 離した時に、上へ `max(80, height × 12%)` 以上動いていて、上への動きが横の動きより大きければスワイプとする。
  - 追っている間は、案内が指に付いて上へ動き、薄くなる。
- **touchpad の 2 本指**: 2 本指が上へ合わせて 10 mm（scroll の notch で 4 つ、natural の設定を戻して指の向きで数える）動いたらスワイプとする。1 回の touch で 1 回だけ。下の縁からの 2 本指（BOTTOM2）・3 本指の上（UP3）の gesture は、終わりの移動が 10 mm 以上ならスワイプとする。
- **mouse の wheel**: 上へ 2 notch（間が 800 ms 以内）でスワイプとする。1 notch だけでは解除しない（mouse に触れただけで、猶予の内に開くのを防ぐ）。下へ回すと数え直す。

### 猶予

- 自動の lock（reason が `lid`・`idle`・`sleep-lid`・`sleep-idle`・`sleep-rest`）から `LOCK_GRACE_SECONDS` 秒の内は、スワイプだけで認証なしに解除する。
- 手動の lock（`key` の Super+L、`home` の Lock Screen、利用者が選んだ Sleep の `sleep-sleep-button`・`sleep-app`）と、上の一覧に無い reason は、常に認証する（知らない reason は安全側に倒す）。
- 時間は wall clock（`time()`）で測る。monotonic clock は sleep の間に進まないことがあり、長く sleep した後に猶予の内と誤ることを防ぐ。時計が戻った時（now < locked_at）は猶予の外とする。
- **既定の時間: 5 分（300 秒）**（2026-10-08 ユーザーの決定、Q1 経由。Settings で変える形は WS148）。
- **利用者が選んだ Sleep は手動の lock と同じ**（2026-10-08 ユーザーの決定「手動で Sleep を選んだ時は猶予なし」）。sleep.c の lock の reason は `sleep-<via>` にした。
  - 自動（猶予あり）: `lid`・`idle`・`sleep-lid`・`sleep-idle`・`sleep-rest`（利用者のでない起床の後の休み）。
  - 手動（常に認証）: `key`・`home`・`sleep-sleep-button`（sleep の key）・`sleep-app`（App Home や電源のメニューなど、kl_system_power の SUSPEND）、それに知らない reason。
  - commit は下の「決定の反映」。

## 実装（2026-10-08、P2）

- `userland/desktop/wayland/lock-swipe.c`・`.h`（新規、server を知らない状態機械）: `kwl_lock_swipe_press`・`_motion`・`_release`・`_distance`・`_wheel`・`_pad`・`_pad_gesture`・`_pad_end`・`kwl_lock_reason_manual`・`kwl_lock_grace`。
- `userland/desktop/wayland/greeter.c`:
  - `kwl_lock` は reason から手動かを覚え、`time()` で lock の時刻を覚え、card を隠し、追っている操作を消す。
  - `kwl_lock_release` は card と操作を戻す。
  - `kwl_greeter_draw` は、ロック中で card が無い時は案内（`greeter_draw_hint`、押して上へ動かすと付いて上がり薄くなる）だけを描く。
  - `kwl_greeter_button` は card の前なら左の press・release をスワイプに渡す。
  - 新しい関数: `kwl_greeter_motion`・`kwl_greeter_wheel`・`kwl_greeter_pad_scroll`・`kwl_greeter_pad_end`、それに `greeter_lock_swiped`（猶予の内なら `kwl_lock_release(server, "swipe")`、外なら card）。
  - `kwl_greeter_key` は card の前に modifier 以外の key が来たら card を出し、続けてその key を処理する。
  - `kwl_greeter_tick` は、card が 30 秒入力なし・何も打っていない・応答待ちでない時に隠す。
  - log: `KWL LOCK locked reason=… manual=…`、`KWL LOCK swipe via=pointer|wheel|pad grace=… manual=…`、`KWL LOCK card via=key`、`KWL LOCK card hidden`、`KWL LOCK unlocked reason=swipe`。
- `userland/desktop/wayland/seat.c`: ロック中の pointer の motion で `kwl_greeter_motion`、wheel（AXIS_SOURCE_WHEEL）で `kwl_greeter_wheel`。
- `userland/desktop/wayland/input.c`: ロック中の touchpad の SCROLL は、指の向きの移動（natural を戻す）を `kwl_greeter_pad_scroll` に渡す。GESTURE の終わりは `lock_pad_gesture` → `kwl_greeter_pad_end`（BOTTOM2・UP3 の END はその移動）に渡す。
- `userland/desktop/wayland/kwl.h`: 4 つの宣言。`Makefile`・`Makefile.linux`・`Makefile.freebsd`: `lock-swipe.c`。
- `userland/desktop/locale/ja/wayland.tr`: 「Swipe up to unlock」→「上にスワイプしてロックを解除」。
- 試験: `plan/ws187/tests/host-lock-swipe.c`・`run-host-lock-swipe.sh`。

## 確認の結果（2026-10-08）

- `sh plan/ws187/tests/run-host-lock-swipe.sh <dir>`: ok（40 checks）。ASan・UBSan でも ok。
- zedBSD の compositor（`make -j16 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/ws181 build/ws181/bin/wayland`）と keiland-linux（`make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws181-linux all`）: どちらも rc 0、warning 0。
- `python3 plan/tools/style-check.py`（変えた C の file と試験）: 0。`git diff --check`: 0。`python3 tools/i18n/tr.py check userland/desktop/locale/ja/wayland.tr userland/desktop/wayland userland/desktop/locale/wayland.keys`: 148 entries、0 problems。
- 未実施: FreeBSD の build、QEMU、実機。

## 確認（計画）

- host: `plan/ws187/tests/run-host-lock-swipe.sh`（下 1/3 の判定、距離、斜め、上 1/3 からの押しは無視、wheel の 2 notch と間・下への回転、2 本指の 10 mm、natural の向き、猶予の手動・自動・時計の戻り・期限）。
- build: zedBSD の compositor・keiland-linux、warning 0。
- QEMU（T1、p003 の後にまとめて）: 縦長・横長の lock の PNG（時計と案内だけ → swipe の後の card）、mouse の wheel での解除、touchscreen の注入のスワイプ（`plan/tools/` の注入の道具がある時）。
- 実機（ユーザー）: touchpad の 2 本指・押し込みのドラッグ、蓋を閉じて開けた後の猶予。

## 決定の反映（2026-10-08、P2）

- `userland/desktop/wayland/sleep.c`: sleep の前の lock の reason を `sleep-<via>`（`kwl_sleep_via_name`）にした。
- `userland/desktop/wayland/lock-swipe.c`: 自動の reason を `lid`・`idle`・`sleep-lid`・`sleep-idle`・`sleep-rest` にした（`sleep` を外した）。
- `plan/ws187/tests/host-lock-swipe.c`: sleep の key・app の sleep は手動、lid・idle・rest の sleep は自動、の検査を足した。ok（45 checks）。
- `KWL LOCK locked reason=sleep` を待つ試験は無い（`grep -rn` で確かめた。`reason=idle`・`reason=home` だけ）。

