---
id: bugs.osk-home-restore
title: 画面 keyboard を出したまま App Home を出して戻ると、画面 keyboard が戻る
status: active
areas: [osk, home]
paths: [userland/desktop/wayland/keyboard.c]
machine: either
human: none
since: BUG-229
---

## 目的
BUG-229（App Home から戻ると画面 keyboard が消える）を確かめる。

## 準備
AAT の image。Text Editor（`/tmp/aat-work/osk229.txt`）に focus。

## 操作と確認
1. 操作: 右下の角の swipe で flick の panel を開く。
   確認事項: panel。正解: `KWL OSK open kind=flick`。確認方法: log。
2. 操作: Windows key で App Home、Esc で戻る。
   確認事項: panel が戻る。正解: `KWL OSK put-away kind=flick reason=home`、`KWL OSK restore kind=flick`。確認方法: log、撮影 restored。

## 合格
2 の両方の行。restore が無ければ BUG-229 の再現（fail）。

## 注記
なし。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。
