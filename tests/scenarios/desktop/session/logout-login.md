---
id: desktop.session.logout-login
title: Log Out で login の画面へ、password で戻る
status: active
areas: [compositor, greeter, sessiond]
paths: [userland/desktop/wayland/greeter.c, userland/desktop/wayland/handoff.c, userland/desktop/wayland/power-dialog.c, userland/desktop/sessiond/]
machine: either
human: none
since: ws131-p005
---

## 目的
log out と login の切り替え（黒い画面を挟まない）を確かめる（UAT A2）。suite の最後に流す（session が新しくなる）。

## 準備
desktop。

## 操作と確認
1. 操作: App Home で `power` と打ち、Power Off の icon を click、dialog で Up（Cancel から Log Out へ）と Enter。
   確認事項: login の画面。正解: `ZWL POWER dialog open source=home`、`ZWL POWER choice=logout via=key error=0`、`ZWL SESSION logout`、`/var/log/greeter.log` に `ZWL GREETER open users=… selected=user`。確認方法: log、撮影。
2. 操作: password `user`、Enter。
   確認事項: 新しい session。正解: greeter の log に `ZWL GREETER auth user=user`、新しい session の log に `ZWL READY … role=normal`。確認方法: log、撮影。

## 合格
1・2 の正解。

## 注記
Log Out は App Home の Power Off の dialog の中にある（ws099-p037、BUG-235。App Home の Log Out の項目は無くなった）。session の log は新しい session で作り直される（それまでの mark は使えない）。
