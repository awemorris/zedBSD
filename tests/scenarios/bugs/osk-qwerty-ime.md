---
id: bugs.osk-qwerty-ime
title: 画面の full keyboard（QWERTY）が IME を通る: a で「あ」、Space で変換、Enter で確定
status: active
areas: [osk, ime]
paths: [userland/desktop/wayland/keyboard.c]
machine: either
human: none
since: BUG-231
---

## 目的
BUG-231（full keyboard で IME の状態を反映し、a で「あ」、漢字の変換も）を Text Editor で確かめる。

## 準備
AAT の image。入力方式を日本語（method 1）にし（helper、終わりに戻す）、Text Editor（`/tmp/aat-work/osk231.txt`）に focus、Alt+Space で日本語。

## 操作と確認
1. 操作: 左下の角の swipe で QWERTY の panel を開き、a・space・Enter を tap。
   確認事項: key が IME へ。正解: `KWL OSK send via=ime code=30`・`code=57`・`code=28`。確認方法: log、撮影 converting。
2. 操作: panel を閉じ、Alt+Space、Ctrl+S。
   確認事項: 日本語が入った。正解: file に非 ASCII の文字。確認方法: file の中身。

## 合格
1・2 の全部。send via=ime が無ければ BUG-231 の再現（fail）。

## 注記
なし。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。
