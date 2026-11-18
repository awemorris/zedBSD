---
id: desktop.home.switch-running
title: App Home で起動中の app を選ぶと、新しく起動せずにその窓へ切り替わる
status: active
areas: [compositor, home]
paths: [userland/desktop/wayland/home.c]
machine: either
human: none
since: ws099-p035c
---

## 目的
App Home で既に起動している app を選んだ時、2 つ目を起動せず、その app の最後の窓に切り替えることを確かめる（BUG-232）。

## 準備
App Home から Files を開き、その上に Terminal を開く（Files は後ろ）。

## 操作と確認
1. 操作: App Home で `files` と打ち、Files の icon を click。
   確認事項: 切り替え。正解: `ZWL HOME switch name=Files surface=S client=C`（Files の窓）、`ZWL HOME launch name=Files` が増えない、新しい `ZWL MAP` が無い、Files が前（`ZWL APPS raise … via=home`）。確認方法: log。
2. 操作: App Home を開く。
   確認事項: 起動中の印。正解: 起動中の app（Files・Terminal）の名前の下に短い線。確認方法: 撮影。

## 合格
1. の log の行。2. は撮影を人が見る。
