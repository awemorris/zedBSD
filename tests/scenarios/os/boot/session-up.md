---
id: os.boot.session-up
title: 起動の後に user の desktop が出る
status: active
areas: [boot, sessiond, compositor]
paths: [src/, userland/desktop/sessiond/, userland/desktop/wayland/, userland/desktop/include/]
machine: either
human: none
since: ws159-p005
---

## 目的
image が起動し、自動の login（`/etc/keiland/autologin`）で user の desktop が使える状態になることを確かめる。ほかの全部のシナリオの前提で、画面の大きさもここで知る。

## 準備
AAT の image（`plan/tools/aat/config-amd64-aat.mk`）で起動した。エージェントは SSH で入っている。

## 操作と確認
1. 操作: desktop が出るのを待つ（最大 120 秒）。
   確認事項: compositor の準備。正解: session の log に `ZWL READY socket=… width=W height=H … role=normal`。確認方法: `aat wait-log 'ZWL READY .* role=normal'`（log は `/run/user/1000/session.log`）。
2. 操作: 画面を撮る。
   確認事項: desktop の見え。正解: 壁紙、上の system bar、下の apps bar。error の dialog が無い。確認方法: 撮影。

## 合格
`ZWL READY … role=normal` があり（W×H は 5330 で 1920x1200、QEMU で 1280x800）、撮影に desktop が写る。

## 注記
W×H はほかのシナリオが使う画面の大きさ。
