---
id: bugs.docked-f11
title: 最大化（dock）した Terminal で F11 が全画面になり、もう一度で dock に戻る
status: active
areas: [compositor, terminal]
paths: [userland/desktop/wayland/protocol.c, userland/desktop/wayland/shell.c]
machine: either
human: none
since: BUG-208
---

## 目的
BUG-208（dock 中の Terminal で F11 を押すと bar が消えるだけで content の領域が変わらない）を確かめる。

## 準備
AAT の image。

## 操作と確認
1. 操作: App Home から Terminal を開き、title bar の double click で dock、F11。
   確認事項: 全画面の configure。正解: `KWL CONFIGURE client=C surface=S … width=画面の幅 height=画面の高さ fullscreen=1`。確認方法: log、撮影 docked-full。
2. 操作: もう一度 F11。
   確認事項: dock に戻る。正解: `KWL WINDOW unfullscreen surface=S … docked=1` と `KWL CONFIGURE … fullscreen=0`。確認方法: log、撮影 docked-back。

## 合格
1・2 の行が出る。1 が無ければ BUG-208 の再現（fail）。

## 注記
なし。 helper は `plan/tools/aat/scenarios/helpers_bugs.py`、一覧と読み方は `plan/agents/bug-ui-sweep-20261008.md`（q911）。
