<!-- awesome-plan project=zedbsd record=ws177-p005 -->

# ws177-p005: 通知とメールの通知の口の堅さ（案 A）

Parent: [WS177](../ws.md)
Status: cleared（2026-10-08 Q1 判定、T1-408 PASS）（旧: test-wait（T1-408）（2026-10-08 P1 q884 の 2: 実装・host PASS・build warning 0、main に統合））
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q884 の 2（P1、2026-10-08）
Origin: [backlog-p2](../backlog-p2.md) の 32・34・35・36（WS156 通知の口）、65・66・68（WS169 compositor のメールの口）、[案](../phasing-20261008.md) の A

## 設計と変更（2026-10-08 P1）

| 行 | 形 |
| --- | --- |
| 32 client・notify の object が去る | model に `kwl_notify_orphan(model, client, object)`（object 0 は client の全部）: その通知は表示・log に残るが `KWL_NOTIFY_ACTION` を外し object を 0 に（click も closed も誰にも言わない）。notify の destroy の要求と `kwl_client_destroy`（新しい `kwl_notify_client_gone`）から呼ぶ。log `KWL NOTIFY left client= object= count=` |
| 34 不正な UTF-8・制御文字 | `kwl_notify_clean`: 正しい UTF-8 の列だけを写し、不正な byte（はぐれた続き・overlong・surrogate・U+10FFFF 越え・途中で切れた列）は U+FFFD、C0・DEL・C1 の制御文字は空白、本文だけ改行を残す。入らない文字は丸ごと落とす。`notify_words` で使う（app・題・本文） |
| 35 速さの制限 | `kwl_notify_rate_take`: client ごとに 1000 ms の窓で 10 個まで（新しい通知と置き換えの両方）、越えたら BUSY の result（log `rate=1`）。compositor 自身は無制限。最後に post した 16 の client を覚え、空きが無ければ窓の始まりが最も古い行を使う |
| 36 libkeiland の事象の ring の溢れ | 溢れて古い事象を捨てた数を数え、次の `kl_system_take_notify_event` が先に `KL_NOTIFY_LOST`（id = 捨てた数）を返す（**KL_VERSION 65**） |
| 65 17 個目の listen | 行が埋まっている時、client か object が去った行を先に掃除してから空きを探す（`mail_sweep`）。全員が居れば従来どおり BUSY |
| 66 読み手の許可の変化 | protocol の kl_system_mail_v1 に event 2 `allowed(uint on)`（**manager version 20**、`KL_SYSTEM_SINCE_MAIL_ALLOWED`）: listen が受け入れられた後に 1 回、`mail.codes.<app>` の設定が変わった時（`settings_apply` から `kwl_mail_settings_changed`）に、前に伝えた値と違えば送る。version 20 未満の object には送らない。libkeiland は `kl_system_mail_allowed()`（1・0・-1 は未通知）と `KL_SYSTEM_CHANGED_MAIL` |
| 68 arrived を送れる client | arrived は mail program の窓（surface の app_id `mailer`）を持つ client からだけ受け取り、他は INVALID（log `KWL MAIL arrived-refused`）。限界: app_id は client が自分で言う語なので、同じ user の program が意図して装うことは防げない（防ぐには peer の program の同一性が要る: backend の OS ごとの口、範囲の外） |

- 変更: `wayland/notify.c`・`notify.h`・`notify-shell.c`・`objects.c`・`kwl.h`・`mail-shell.c`・`settings.c`、`libkeiland/system/kl-system-protocol.h`・`system-protocol.c`・`system.c`・`system-view.c`・`system-private.h`、`include/keiland/keiland.h`（KL_VERSION 65、`KL_NOTIFY_LOST`、`kl_system_mail_allowed`）、`libkeiland/exports.map`。
- 試験（どちらも未完了の WS の試験なので直して足した）: `plan/ws156/tests/host-notify-model.c`（orphan・rate・clean・post の語の修理）、`plan/ws169/tests/host-mail-shell.c`（mailer の窓の無い arrived の拒否、version 20 の allowed とその変化、掃除で空く listen、view の allowed、notify の LOST）。

## 確認（host・build、2026-10-08）

- `sh plan/ws156/tests/run-host-notify-model.sh build/p1-a/notify-model` → 34 passed（plain・sanitized）。`run-host-notify-flow.sh` → 32 checks 0 failures。`sh plan/ws169/tests/run-host-mail-shell.sh build/p1-a/host-mail-shell` → PASS（ASan/UBSan）。
- build（warning 0）: `make BUILD=build/p1-d ZEDBSD_CONFIG=plan/ws169/tests/config-amd64-mailer.mk build/p1-d/dynamic/libkeiland.so build/p1-d/bin/wayland build/p1-d/bin/mailer build/p1-d/bin/settings`、`make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/p1-lx all`（warning 0）。exports.py --check OK。style-check: 変えた file は増えない。

## 未実施

- QEMU（T1）: AAT `apps.mailer.sign-in-code`（実の Mail が窓を持って arrived を送れること、Browser に届くこと = 68 の門が正常系を壊さないこと）。allowed の event の実機の道（Browser は今は使わない、libkeiland の口だけ）。browser の build は未実施（keiland.h の追加だけ）。

## Event

2026-10-08 / q884-i02（P1）: 実装と host・build。


## Q1 の判定（2026-10-08）

T1-408: apps.mailer.sign-in-code の step 1〜8 が seen、`KWL MAIL arrived ... told=1`、`ZBROWSER MAIL code length=4 ... notified=1`、arrived-refused 無し。
