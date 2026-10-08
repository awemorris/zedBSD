<!-- awesome-plan project=zedbsd record=ws177-p014 -->

# ws177-p014: Browser の sign-in code の入力（案 N4）

Parent: [WS177](../ws.md)
Status: cleared（2026-10-08 Q1 判定、T1-418。one-time-code の欄に直に入れる後半は q890）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q889 の 1（P1、2026-10-08）
Origin: [backlog-p2](../backlog-p2.md) の 85・86・87・88・115（WS169 ws169-p005）、[案](../phasing-20261008.md) の N4。89（page の上の帯）は WS074 の描画が止まっているので外した（Q1）

## 設計と変更（2026-10-08 P1）

- **欄が無い時（85・115）**: 通知か titlebar の control の click で、`browser_view_focus` の後に `browser_view_text_target` を見る。page の focus が text を取る欄に無い（欄の外を click した後、欄の無い page）時は page に key を送らない（送ると page の shortcut が動く）。code を `kl_window_copy` で clipboard に置き、offer（control と通知）を取り下げ、code を含まない通知「Sign-in code copied — No field on the page takes it: click the field and paste with Ctrl+V.」を出す。log `ZBROWSER MAIL copied length=N clipboard=1 notified=1`。password の欄は `text_target` が 0 なので clipboard になる（libbrowser の公開の API に password の欄を見分ける口が無い）。
- **別の窓（85）**: browser に tab は無く、窓ごとに process が listen する（compositor は同じ名の全ての読み手に送る）。どの窓も offer を出すので、利用者は入れたい窓の titlebar の control か通知を押して選ぶ。通知の本文に窓の page の title を入れた（「Click to fill in 482913 on Example Bank - Sign in.」、title は 120 byte まで）。1 つの窓で使っても他の窓の offer は 2 分まで残る。
- **英字の code（86 の前半）**: `mailer/code.c` が英字を含む code を取る: 「code」「passcode」「OTP」「PIN」「コード」のすぐ後（空白・`:`・`：`・`=`・`-`・is・は を飛ばす）の 4〜8 文字の大文字と数字の連なりで、数字と大文字を 1 つ以上ずつ含み小文字を含まない物。message が sign-in を語る（verification・verify・passcode・one-time・OTP・sign-in・login・認証・確認・ワンタイム・ログイン）時だけ（「Use code SAVE20」の宣伝を取らない）。数字だけの code より先に探す。browser は英字を DOM の `KeyX`（大文字は Shift）で打つ。
- **語の境（backlog 76、案 N から前倒し）**: 上の英字の code が語の位置を要るので、同じ関数で語の境を入れた。ASCII の語は語の全体だけ（前に英字が無い、後に英字が無いか複数形の s）。「shipping」の pin、「barcode」の code は数えない。N の Phase では済みとして扱う。
- **2 分の時間切れ（88）**: `shell_mail_timeout` を足し、main loop の待ちに offer の残り時間を入れた（前は loop が他の理由で起きた時に見るだけ）。
- **通知の click の経路の QEMU の確認（87）**: 通知の popup（WS156 p003）ができたので、AAT `apps.mailer.sign-in-code` の段 3 を「popup の本文を click」に変えた（helper が `KWL NOTIFY show` の後に画面の下の中央を click し、`KWL NOTIFY activate`・`fill length=4`・`code-length=4`・`TITLEBAR code=0` を見る）。titlebar の control の経路は T1-302 で確かめ済み。
- 変更: `userland/desktop/browser/shell/mail.c`・`internal.h`・`shell.c`、`userland/desktop/mailer/code.c`。試験: `plan/ws177/tests/host-mail-code.{c,sh}`（新）、`plan/ws169/tests/host-browser-mail.c`（新しい fake と check、WS169 の回帰の試験）、AAT `tests/scenarios/apps/mailer/sign-in-code.md`・`plan/tools/aat/scenarios/helpers_mailer.py`。

## 判断が要る点（ユーザー、記録して先へ）

2026-10-08 朝 ユーザー「libbrowser に口を足してよい」→ [ws177-p017](../phase017/phase.md)（口）・[ws177-p018](../phase018/phase.md)（欄に入れる）で実装した。以下は当時の記録。

- **86 の後半: `autocomplete="one-time-code"` の欄を探して入れる**は未実装。browser の shell が使える libbrowser の公開の API（`include/browser/browser.h`）には、属性で要素を探す・focus を移す口が無い。layout・DOM の text の dump を照らし合わせる方法は表示されない input があると要素を取り違えるので採らない。案: libbrowser に `int browser_view_focus_field(struct browser_view *view, const char *autocomplete)`（autocomplete の token を持つ最初の focus できる text の欄に focus を移し、見える所へ scroll、無ければ ENOENT）を足し、shell は `text_target` が 0 の時にまずそれを試し、無ければ clipboard。libbrowser（WS074）は止めているので、手を入れてよいかはユーザーの判断（[案](../phasing-20261008.md) の「O・p1 32・p2 89」と同じ問い）。それまでは clipboard に置く。

## 確認（host・build、2026-10-08）

- `sh plan/ws177/tests/host-mail-code.sh` → PASS（ASan/UBSan、15 の check）: 英字の code（is・`:`・`：`・数字より先）、宣伝の SAVE20 を取らない、小文字の語・大文字だけの語を取らない、`G-482913` は数字、shipping・barcode を数えない、複数形の codes、PIN、前からの数字の規則（年、語の中の数字）。
- `sh plan/ws169/tests/run-host-browser-mail.sh build/tmp/p1-n4/host-browser-mail` → PASS 14（前の 9 に、通知が page の title を言う、時間切れの残り（90000 ms・0・-1）、英字の code の `KeyX` と Shift、欄が無い時の clipboard と code の無い通知、log に英字の code も無い）。
- `sh plan/ws169/tests/run-host-mail-backend.sh build/tmp/p1-n4/backend` → host-mail-backend・host-mail-sync PASS（code の前の 5 つの check を含む）。
- build（warning 0）: `make -j16 BUILD=build/p1-mailer ZEDBSD_CONFIG=plan/ws169/tests/config-amd64-mailer.mk build/p1-mailer/bin/browser build/p1-mailer/bin/mailer`。style-check（mail.c・code.c・shell.c）0。`git diff --check` 0。`check-scenarios.py` PASS。

## 未実施

- QEMU（T1）: AAT `apps.mailer.sign-in-code`（段 3 の popup の click）。clipboard の経路（欄が無い時）と英字の code は QEMU で見ていない（host の試験だけ）。通知の click の後の `kl_window_copy` が compositor に clipboard として受け入れられるか（入力の serial）は未確認。
- `one-time-code` の欄を探す（上の判断）。

## Event

2026-10-08 / q889-i01（P1）: 実装と host・build の確認。

## T1-418（2026-10-08 Q1）

apps.mailer.sign-in-code の段 1〜10 が seen、新しい段 3 で `KWL NOTIFY activate`・`ZBROWSER MAIL fill length=4 error=0`・`code-length=4`、arrived-refused 無し。
