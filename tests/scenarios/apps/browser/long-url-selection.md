---
id: apps.browser.long-url-selection
title: Browser の URL 欄の長い URL の選択が title bar をはみ出さない
status: active
areas: [browser, compositor, titlebar]
paths: [userland/desktop/browser/, userland/desktop/wayland/titlebar-shell.c]
machine: either
human: look
since: BUG-181
---

## 目的
BUG-181（title bar の欄の長い文字の選択の描画が clip されない）が直ったままであることを確かめる。

## 準備
App Home から Browser を開く。

## 操作と確認
1. 操作: title bar の URL 欄（`ZWL TITLEBAR control client=C surface=S where=floating id=… x y width height` の欄）を click。
   確認事項: 焦点。正解: `ZWL TITLEBAR focus client=C surface=S id=… edit=1`。確認方法: log。
2. 操作: Ctrl+A の後に `https://example.com/` と、続けて 200 文字ほどの path を打ち、Ctrl+A。
   確認事項: 選択の描画。正解: 選択の色が欄の中に収まり、title bar の外（窓の題・button・隣の窓）に出ない。確認方法: 撮影（人が見る）。
3. 操作: Esc。

## 合格
needs-person（撮影）。
