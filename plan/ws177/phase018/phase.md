<!-- awesome-plan project=zedbsd record=ws177-p018 -->

# ws177-p018: sign-in code を one-time-code の欄に直に入れる（ws177-p014 の後半）

Parent: [WS177](../ws.md)
Status: test-wait（2026-10-08 P1 q890 の 2: 実装・host PASS・zedBSD の build warning 0。QEMU は T1 の AAT `apps.mailer.sign-in-code`）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q890 の 2（P1、2026-10-08）
Origin: [backlog-p2](../backlog-p2.md) の 86 の後半（[ws177-p014](../phase014/phase.md) の判断）。依存: [ws177-p017](../phase017/phase.md)（`browser_view_focus_field`）

## 設計と変更（2026-10-08 P1）

- `browser/shell/mail.c` の `shell_mail_fill`: page の focus が text を取る欄にある時は今まで通りそこへ打つ（利用者が選んだ欄）。無い時は `browser_view_focus_field(view, "one-time-code")` で `autocomplete="one-time-code"` の欄に focus を移して打つ（log `ZBROWSER MAIL one-time-code-field error=0`）。それも無い時（ENOENT）だけ、p014 の通り clipboard と通知。password の型の one-time-code の欄も、この経路で key を打てる（`text_target` は password に 0 を返すが、focus_field は password も探す）。
- 試験: `plan/ws169/tests/host-browser-mail.c` に fake の `browser_view_focus_field` と check `one-time-code-field`。AAT `apps.mailer.sign-in-code`: page の input に `autocomplete=one-time-code`、段 1 で欄の上の文を click して focus を外し、段 3 で `one-time-code-field error=0` を見る（`plan/tools/aat/scenarios/helpers_mailer.py`、`tests/scenarios/apps/mailer/sign-in-code.md`）。

## 確認（host・build、2026-10-08）

- `sh plan/ws169/tests/run-host-browser-mail.sh build/tmp/p1-o/host-browser-mail` → PASS 15（新しい `one-time-code-field`: focus の無い page で one-time-code の欄に 135790 が打たれ、clipboard に置かない。log に code が無い）。
- build（warning 0）: `make ... build/p1-mailer/bin/browser`（config `plan/ws169/tests/config-amd64-mailer.mk`）。style-check（mail.c）0。`check-scenarios.py` PASS。

## 未実施

- QEMU（T1）: AAT `apps.mailer.sign-in-code`（libbrowser の新しい口と一緒に）。

## Event

2026-10-08 / q890-i02（P1）: 実装と host・build の確認。
