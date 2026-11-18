---
id: bugs.title-tap-drag
title: 浮いた title bar の tap and drag は窓を動かし、最大化しない（mouse と touch screen）
status: active
areas: [compositor, windows, touch]
paths: [userland/desktop/wayland/shell.c, userland/desktop/wayland/title-tap.c]
machine: either
human: none
since: BUG-265
---

## 目的
BUG-265（double tap からの drag が double click として最大化される）を mouse と touch screen で確かめる。

## 準備
AAT の image。App Home から Files を開く（浮いた窓）。

## 操作と確認
1. 操作: title bar を click し、120 ms 後に 2 回目を押したまま 80,80 動かして離す（mouse）。
   確認事項: 窓が動き、dock しない。正解: `KWL GLASS double-click wait surface=S`、`KWL GLASS double-click moved|moved surface=S`、`KWL GLASS dock surface=S` が無い、窓が約 80 px 動く。確認方法: log、撮影 mouse。
2. 操作: 同じ操作を touch screen で（tap、touch-down、touch-move、touch-up）。
   確認事項: 同じ。正解: 同じ。確認方法: log、撮影 touch。
3. 操作: title bar を double click。
   確認事項: 最大化は今どおり。正解: `KWL GLASS dock surface=S via=double-click`。確認方法: log。

## 合格
1〜3 の全部。1・2 で dock したら BUG-265 の再現（fail）。

## 注記
5330 の touchpad の tap and drag は UAT。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。
