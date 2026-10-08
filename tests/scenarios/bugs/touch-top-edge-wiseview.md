---
id: bugs.touch-top-edge-wiseview
title: touch screen の上端から下への swipe で Wiseview が開き、bar の tap は今どおり
status: active
areas: [compositor, touch, wiseview]
paths: [userland/desktop/wayland/edge.c, userland/desktop/wayland/touch.c, userland/desktop/wayland/shell.c]
machine: either
human: none
since: BUG-270
---

## 目的
BUG-270（touchscreen の上端からの swipe down で Wiseview が出ない）を確かめる。

## 準備
AAT の image。Files を開いておく。

## 操作と確認
1. 操作: touch screen で画面の上端（y=3）から下へ 360 px drag。
   確認事項: Wiseview が開く。正解: `KWL EDGE band press …`、`KWL WISEVIEW gesture via=top-edge` か `KWL WISEVIEW opening`。確認方法: log、撮影 wiseview。
2. 操作: Esc。bar の音量の icon を tap。
   確認事項: bar の tap が効く。正解: `KWL WISEVIEW close`、`KWL VOLUME popup open`。確認方法: log。

## 合格
1・2 の行。1 が無ければ BUG-270 の再現（fail）。

## 注記
1 本指だけ。複数の指の上端は実機の touch screen。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。
