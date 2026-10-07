<!-- awesome-plan project=zedbsd record=ws102-p025 -->
# ws102-p025: 設計 — 画面 keyboard の残り（App Home で消える・引き出しの形・full keyboard の IME）

Parent: [WS102](../ws.md)
Status: test-wait（T1 依頼中。q789-i02、P1、2026-10-06: 試験を足した。下の「q789-i02」）
Disposition: normal
Related: [BUG-229](../../bugs/BUG-229.md)・[BUG-230](../../bugs/BUG-230.md)・[BUG-231](../../bugs/BUG-231.md)

## 由来（ユーザー、2026-10-06 UAT）

BUG-229「この表示状態で、アプリ一覧を出して戻ると、オンスクリーンキーボードが消えてしまいました。」BUG-230「引き出されたエリアがスクエアで真っ白です。これを、Notesの引き出しと同じ、扇型＋テキストに変更したいです。」BUG-231「オンスクリーンフルキーボードで、IMEの有効状態を反映して、aと入力したら『あ』になって、漢字変換もできるようにしてください。」

## 設計

- **BUG-229**: OSK の表示の状態（出ている・mode）を focus の text-input に結び付けて保つ。App Home・WiseView の間は OSK を隠すだけにし（状態は保つ）、閉じて同じ text-input に focus が戻ったら出し直す。log `ZWL OSK restore`。
- **BUG-230**: 引き出しの描画を Notes の引き出し（扇形と文字）と共通の描き方に。今の白い四角の代わりに、引き出す距離に合わせて扇形の glass が開き、中央に「Keyboard」（翻訳の catalog）と keyboard の記号。
- **BUG-231**: full keyboard（英字の配列）の key を、IME が有効（ime_status が日本語の mode）の時は**文字の commit でなく key の event として IME に通す**（今は文字の commit で IME を迂回している見込み）。IME が preedit（あ）と変換（Space で候補）を行う。候補は OSK の候補の列（WS166 の予測の列と同じ場所）に出す。IME が無効の時は今どおり文字を commit。
- 共通: OSK の状態の変化は 1 つの関数に集め log。

## 試験と Phase

- AAT: `desktop.osk.restore-after-home`、`desktop.osk.full-ime`（a → あ、Space で変換、確定）。実機はユーザー。
- 実装: 1 Phase（p026、1 LW）。

## q789-i01（P1、2026-10-06）: 実装

この Phase の Queue（q789）で、設計の 3 点を実装した（設計にある p026 の実装を兼ねる）。commit 4b956b9d。

| Bug | 変更（`userland/desktop/wayland/keyboard.c`） |
| --- | --- |
| BUG-229 | App Home・Wiseview は panel を閉じずに **put away**（`keyboard_put_away`: 閉じて、panel の種類と focus の窓を覚える、log `ZWL OSK put-away kind=K reason=home|wiseview`）。両方が消え、同じ窓（まだ窓である物）が focus を持っていれば同じ panel を開き直す（`keyboard_restore`、log `ZWL OSK restore kind=K`）。別の窓が focus を持つ、または login・lock の画面なら諦める（`ZWL OSK restore-dropped reason=focus|greeter|lock`）。panel を開くと待ちは消える |
| BUG-230 | 引き出しの hint を、faint な panel（白い四角）から Notes の角（corner.c）と同じ glass の四分円に: 下の角（flick は右下、QWERTY は左下）から contact まで伸びる、影・glass・縁（離せば開く時は青）、半径 96 から対角線の上に `kl_tr("Keyboard")`（日本語の catalog に「キーボード」を追加、`userland/desktop/locale/ja/wayland.tr`） |
| BUG-231 | 画面 keyboard が送る key（`keyboard_send_key`）を `keyboard_key_event` に通す: **QWERTY の panel の key** は物理の keyboard と同じく IME の `zwl_ime_key_early`・`zwl_ime_key_grab` を先に通る（IME が field を受け持ち direct input でない時、a は preedit の「あ」、Space で変換。log `ZWL OSK send via=ime code=N`）。IME が取らない key と flick の panel の key は今どおり focus の app へ。候補は IME の popup（OSK の候補の列への表示は今回は入れていない） |

確認: wayland（zedBSD）の build warning 0、keiland-linux.mk の host build exit 0、`plan/ws102/tests/host-keyboard.sh` PASS（layout の試験、keyboard.c 自体は host の試験が無い）、
`tools/i18n/tr.py check` で ja の wayland.tr 117 件 0 problems。QEMU は T1 に依頼（下）。実機は UAT。

残り: BUG-231 の候補を OSK の候補の列に出す（今は IME の popup）。AAT の `desktop.osk.restore-after-home`・`desktop.osk.full-ime` の追加は未実施。

## q789-i02（P1、2026-10-06）: T1-226 の所見の調べ

T1-226（証拠 `/home/awe/zedBSD-worktrees/t1/build/t1-226/`）で osk-guest は PASS、ただし BUG-229 の `put-away`・`restore-dropped` の行が
`osk-log.txt` に無く、BUG-230 の引き出しが右下の小さな白い四分円で「Keyboard」が出なかった。調べた結果、どちらも試験の取り方によるもので、source は直していない:

- **BUG-229**: `osk-log.txt` は osk-guest の**最後の step の compositor の log だけ**を grep したもの（最後の step `qwerty` が compositor を起こし直すので、
  App Home・Wiseview を扱う `close` の step の行は入らない。570 行に `close`・`home` の行が 1 つも無い）。`close` の step の
  `close kind=flick reason=home`・`reason=wiseview` は ok で、この 2 つの理由で閉じるのは `keyboard_put_away` だけ（閉じた直後に `put-away` を出す）。
  ただ、その行と restore を確かめる手順が試験に無かったので足した（下）。
- **BUG-230**: `partial-swipe2.png` の円の半径は約 65 px で、対角線に沿って約 31 px（半径 = 24 + 1.3 × 進み）。文字は半径 96（進み 55）から
  fade in し、離せば開く所（進み 108、半径 164）で濃さが満ちて縁が青になる。Notes の角（`corner.c`）とまったく同じ大きさ・色・文字の出方で、
  ユーザーの「Notesの引き出しと同じ」の通り。短い引きで文字が無いのは Notes でも同じ。

試験に足した物（`plan/ws102/tests/osk-guest.sh`）:
- `close` の step: `put-away kind=flick reason=home`・`reason=wiseview`、focus の窓が無い時の `restore-dropped reason=focus` 2 回、そして Text Editor を開いて
  focus を持たせ、flick の panel を開き、App Home を開いて Esc で閉じると `restore kind=flick`（`restored.png`）。
- 新しい step `hint`: 右下の角から押したまま、40 px（円だけ、`hint-short.png`）、80 px（文字が fade in、`hint-label.png`）、
  130 px（文字と青い縁、`hint-ready.png`）、離すと `open kind=flick`。
確認: `sh -n`。QEMU は T1 に依頼する。

## q875（P1、2026-10-08）: BUG-231 を QEMU で確かめる段

2026-10-08 P2 の確認（q868、BUG-231）で、T1-226 の image は IME が direct input のままで「a → あ」と変換を見ていないと分かった。案 (b) として osk-guest.sh に `qwerty-ime` の step を足した（source の変更は無い）。

- 前提の image: IME 入りの image（`plan/ws095/tests/build-ime-image.sh`。keiland-ime と辞書が入る）に、今の BIN を `install` の step で入れる。
- 手順: compositor を起こし（`KWL IME started pid=`）、ime-probe（`--app-id=osk-ime`）を focus にして Alt+Space で日本語（`KWL IME language=ja`）。左下の角の swipe で QWERTY の panel を開く。
  - `a` を押す → `KWL OSK send via=ime code=30`、probe の log に `preedit=あ`。
  - `space` を押す → `code=57`（変換、候補は `qwerty-ime.png`）。
  - `Enter` を押す → `code=28`、probe の `PROBE TEXT text=` が空でも `a` でもない。
- 流し方（T1）: `GUEST_RUNTIME=... BIN=<build> plan/ws102/tests/osk-guest.sh OUTDIR install qwerty-ime`。合格は最後の行 `osk-guest: PASS`。
- 確認: `sh -n`。QEMU は未実施。
- 残り（変わらず）: 候補を OSK の候補の列に出す（今は IME の popup）、AAT の `desktop.osk.full-ime`。
