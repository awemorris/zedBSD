---
id: apps.terminal.japanese-history
title: sh の履歴の日本語の長い行を呼び出して編集しても prompt が崩れない
status: active
areas: [terminal, sh, ime]
paths: [userland/desktop/terminal/, userland/base/sh/]
machine: either
human: look
since: BUG-173
---

## 目的
BUG-173（全角の長い履歴で prompt が消え、行頭から描かれる）が直ったままであることを確かめる（UAT F10）。

## 準備
入力方式 Japanese。App Home から Terminal を開く。

## 操作と確認
1. 操作: `echo ` と打ち、Alt+Space で日本語にして `nihongonobunshouwonagakuutsu` を変換・確定（Space・Enter）、Alt+Space で戻して Enter。
   確認事項: 確定と実行。正解: `ZTERM IME commit bytes=… text=…` があり、echo が日本語を出す。確認方法: log、撮影。
2. 操作: 上矢印で呼び出し、左矢印を 5 回、Backspace を 2 回、右矢印を 3 回。
   確認事項: 表示。正解: prompt（`$`）が残り、行が崩れず、改行が入らない。確認方法: 撮影（人が見る）。
3. 操作: Ctrl+C。

## 合格
1 の log。2 は needs-person。
