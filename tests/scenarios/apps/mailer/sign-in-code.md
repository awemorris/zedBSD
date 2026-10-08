---
id: apps.mailer.sign-in-code
title: メールで届いた認証 code を Browser が受け取り、欄に入れる
status: active
areas: [mailer, browser, compositor]
paths: [userland/desktop/mailer/, userland/desktop/wayland/mail-shell.c, userland/desktop/wayland/notify-popup.c, userland/desktop/browser/shell/mail.c, plan/tools/mail/fake-mail-server.py]
machine: either
human: hands
since: ws169
---

## 目的
Mail が受けた新しいメールの認証 code が compositor（kl_system_mail_v1）を通って、許された Browser に届き、Browser の通知の popup の click から page の欄に入ることを確かめる（WS169 p002・p005、ws177-p014）。code は log に出ない。

## 準備
read-compose と同じ偽の server（`--arrivals 1`: Mail が IDLE に入ると 0.5 秒後に code 7351 のメールが 1 通届く）と target の準備。page `/tmp/aat-work/code.html`（autofocus の input、入力の長さを console に出す）。

## 操作と確認
1. 操作: Browser で `/tmp/aat-work/code.html` を開き、page の欄を click（欄の外の click は欄から keyboard を外し、code が入らない）。
   確認事項: 聞く。正解: `ZBROWSER MAIL listen error=0`。確認方法: log。
2. 操作: Mail を起動し、form に read-compose と同じく入れ、「Sign-in codes」の switch を on（`MAIL CODES allowed=1`）、Sign In。
   確認事項: 新着と code。正解: `MAIL MESSAGE account=0 folder=Inbox uid=N arrived=1 code=1`、`KWL MAIL arrived … code=4 told=1`、`ZBROWSER MAIL code length=4 titlebar=1`。どの log の行にも `7351` が無い。確認方法: log。
3. 操作: 通知の popup（画面の下の中央、`KWL NOTIFY show … title="Sign-in code from …"`）が出ている間に、その本文を click（ws177-p014）。
   確認事項: 通知の click で入る。正解: `KWL NOTIFY activate id=N`、`ZBROWSER MAIL fill length=4 error=0`、`ZBROWSER CONSOLE` の `code-length=4`、titlebar の control が消える（`ZBROWSER TITLEBAR code=0`）。確認方法: log。
4. 操作: Mail を閉じて Browser を前に。
   確認事項: 欄と titlebar。正解: 欄に 7351、titlebar に「Code」の control が無い。確認方法: 撮影（人）。

## 合格
1〜4 の正解。

## 注記
通知の popup の位置は ws156-p003 の板（画面の幅の 5 分の 1、高さ 76、下の端から 48 の中央）。titlebar の control「Code 7351」を押す経路は T1-302 で確かめた（control の位置は log に無いので撮影を見て押す）。page の欄に focus が無い時（欄の外を click した後）は、入れずに clipboard に置き、「Sign-in code copied」の通知を出す（`ZBROWSER MAIL copied length=4 clipboard=1 notified=1`、host の試験 `plan/ws169/tests/run-host-browser-mail.sh`）。助け: `plan/tools/aat/scenarios/helpers_mailer.py`。
