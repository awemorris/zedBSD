---
id: desktop.startup.wallpaper-time
title: 起動の壁紙の段が 2 秒以内
status: active
areas: [compositor, wallpaper]
paths: [userland/desktop/wayland/backdrop.c, userland/desktop/picture/, userland/desktop/wayland/main.c]
machine: either
human: look
since: ws138
---

## 目的
起動の時の壁紙の読み込みの時間を見る（uat.md 2026-10-04: 7172 ms）。

## 準備
os.boot.session-up。

## 操作と確認
1. 操作: session の log を読む。
   確認事項: 時間。正解: `ZWL STARTUP step=wallpaper ms=N` の N ≤ 2000。確認方法: `aat lines 'ZWL STARTUP step=wallpaper'`。

## 合格
N ≤ 2000。超えたら値を書いて needs-person。
