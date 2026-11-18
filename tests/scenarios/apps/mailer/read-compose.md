---
id: apps.mailer.read-compose
title: Mail に account を足し、受信箱を読み、新しい message を送る
status: active
areas: [mailer]
paths: [userland/desktop/mailer/, plan/tools/mail/fake-mail-server.py]
machine: either
human: look
since: ws169
---

## 目的
Mail の IMAP・SMTP（TLS は STARTTLS）で、account の追加・受信・本文・送信が通ることを確かめる（WS169 p003・p004）。

## 準備
host で `plan/tools/mail/fake-mail-server.py` を動かす（試験の CA と、mail.test と target から届く address（QEMU なら 10.0.2.2）の証明書、その address で）。server は address で指す（zedBSD の resolver は `/etc/hosts` を読まない）。CA を `/tmp/aat-work/mail-ca.pem` に。user の Mail の account は無い（`~/.config/keiland/mailer.conf`・`mailer-accounts` を消す）。Mail は `env SSL_CERT_FILE=/tmp/aat-work/mail-ca.pem /bin/mailer` で起動する。

## 操作と確認
1. 操作: Mail を起動する。
   確認事項: account が無い。正解: 「Add an Account」の form。`MAIL ACCOUNTS count=0`。確認方法: log、撮影。
2. 操作: Your name に `Kei Example`、Email に `kei@example.net`、Password に `secret 1`、IMAP server に `ADDRESS:IMAP の port`、SMTP server に `ADDRESS:submission の port` を入れ、Sign In。
   確認事項: login と受信。正解: `MAIL SIGNED-IN account=0`・`MAIL REFRESHED account=0`、受信箱に 3 通（`MAIL MESSAGE account=0 folder=Inbox` が 3 行以上）。`MAIL FAILED` が無い。確認方法: log、撮影。
3. 操作: 一覧の最初の行を click。
   確認事項: 本文。正解: `MAIL OPEN message=N`。確認方法: log、撮影。
4. 操作: Ctrl+N、To に `ben@example.com`、Subject に `Hello from AAT`、本文に `A test message.`、Send。
   確認事項: 送信。正解: `MAIL SENT account=0`、server が受けた message に件名と本文がある。確認方法: log、host の server の `smtp-N.eml`、撮影。

## 合格
1〜4 の正解。見えは撮影（一覧の 3 通、銀行の message の code の帯、書いた message）。

## 注記
form の場所は view.c の配置（sidebar 220、glass で 10 の間、form は幅 560 を右の領域の中央、欄は 112 から 42 ずつ、Sign In は y 347）。窓は App Home の外から開くので大きさは `MAIL READY width= height=` から。助け: `plan/tools/aat/scenarios/helpers_mailer.py`。
