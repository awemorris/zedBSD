---
id: apps.phone.message
title: Phone で message を送って返事を受け、電話を掛ける（loopback の backend）
status: active
areas: [phone, compositor]
paths: [userland/desktop/phone/, userland/desktop/wayland/phone-shell.c]
machine: either
human: look
since: ws170
---

## 目的
Phone の送信・受信・通話が compositor の phone（kl_system_phone_v1）を通り、file に残ることを確かめる（WS170 p003・p004）。番号と本文は log に出ない。

## 準備
browse と同じ連絡先。`/bin/keiland-settings set phone.backend 1`（loopback: 送ると届いた事になり、同じ番号から「Echo: …」が返る。通話は応答なし）。App Home から Phone を開く。

## 操作と確認
1. 操作: Ben の行を click、下の欄を click して `Hello from AAT`、Enter。
   確認事項: 送信と返事。正解: `PHONE SEND contact=0 channel=0 length=14 … error=0`（間の `paired=` などの欄は問わない）、`PHONE STATUS … state=2 failed=0`（delivered）、`PHONE RECEIVED contact=0 channel=0 length=20 error=0`、`~/Documents/Phone/messages/aat-ben/` に「Echo: Hello from AAT」の file。確認方法: log、target の file、撮影。
2. 操作: header の右の電話の button。
   確認事項: 通話。正解: `PHONE CALL contact=0 error=0`、`PHONE STATUS … state=5`（応答なし）、timeline に「Call not answered」。確認方法: log、撮影。
3. 操作: `keiland-settings set phone.backend 0`、欄に `Again`、Enter。
   確認事項: backend 無し（BUG-287 の後は Phone が compositor へ送る前に断る、BUG-297）。正解: `PHONE REFUSED action=send contact=0 length=5 why=no-backend`、`PHONE SEND`・`KWL PHONE send` の行が無い、`~/Documents/Phone/messages/aat-ben/` に「Again」の file が無い、欄に「Again」が残り下に notice「No phone is set up: …」（4 秒）。log の行に番号と本文が無い。確認方法: log、target の file、撮影。

## 合格
1〜3 の正解。

## 注記
助け: `plan/tools/aat/scenarios/helpers_phone.py`（終わりに folder を消し phone.backend を 0 に戻す）。
