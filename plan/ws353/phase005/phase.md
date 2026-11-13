<!-- awesome-plan project=zedbsd record=ws353-p005 -->

# ws353-p005: timer の thread と、thread の結果の flag を wake に（main loop の 1）

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.2・§5.5・§5.11、対応表の 2-02・2-03・2-07〜2-11・2-14。

## 目的

main の締め切りを持つ timer の thread（T16、未決 1 の推し C）と API を作り、main が周期で覗いている「thread の結果の flag」（名残り）を wake に変える。入力の scan を事象だけにする。

## 前提

- WS352 p002 の merge の後を推す（設計 §7）。merge しない時は、main の固定 10 ms の pass はこの Phase の後も p008 まで残る。どちらでも、main の poll の timeout を -1 にするのは p008 の終わり（全部の期限が timer に移った後）。

## 変える file

- `userland/desktop/wayland/threads.c`・`threads.h`: `struct kwl_timer`、min-heap（main だけが書く）、`kwl_timer_arm`・`kwl_timer_disarm`、timer の thread（次の期限まで `pthread_cond_timedwait`、無ければ `pthread_cond_wait`、発火で出口と wake）、main の take（期限の過ぎた物の handler）。`KWL THREADS` に `timer`。
- `main.c`: 起動と終了の順（timer は lane の後に始め、終了は lane の前に止める）、wake の後の take に timer。SIGCHLD の handler が wake pipe に 1 byte（branch で済なら確かめだけ）。去った client の buffer の解放の残りは自分の wake pipe で次の pass へ（timeout 0 にしない）。
- 名残りの 7 つ: `cursor-plane.c`（手放しで cursor の thread が wake）、`glass.c`（loader の終わりで wake）、`shot.c`（送りの終わりで wake）、`system.c`（562: lane の終わりで続ける、2613: sessiond の答えで問い直す）、`media-library.c`（pipe を poll、子の終わりは SIGCHLD の wake）、`machine-shell.c`（branch で済なら確かめだけ）。
- 入力の scan（main.c の `KWL_INPUT_SCAN_MS`、zedBSD）: /dev/system の INPUT の事象でだけ scan（Linux・FreeBSD の backend は今のまま）。`src/drivers/generic/input.c` で INPUT の事象を evdev の node が見えた後に出しているかを確かめ、前なら順を直して `KWL_INPUT_SETTLE_SCAN_MS` を消す（直せない理由があれば止まって Q1 に報告）。

## 残す振る舞い

- 各機能の結果と順（WS309 §3.3 の take の順）。Linux・FreeBSD の backend の scan。

## 確かめ

- build: compositor（x86_64・arm64・riscv64、keiland-linux は崩れてよい）warning 0。
- host: timer の heap と thread の試験（arm・disarm・同じ時刻・先頭の差し替え・停止、TSan）を `plan/ws353/tests/timer/` に（host の cc で build、Makefile を使わない）。
- QEMU: 壁紙の変更・shot・media の helper・USB の keyboard の抜き差し（BUG-264 の `hotplug-guest.sh` の形、10 tick 以内に取る）が前と同じに動く。WS352 の poll の診断で、これらの 10 ms の起床が無い。

## 受け入れ

- 上の判定が PASS。対応表の 2-02・2-03・2-07〜2-11・2-14 が消えている（2-03 は kernel の順を直せた時。直せない時は理由を書いて Q1 に）。結果・SHA・未実施を書く。

## 依存

- p004。WS352 p002 の merge（推し）。
