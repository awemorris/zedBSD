---
id: apps.textedit.type-save
title: Text Editor で打って保存する
status: active
areas: [textedit, keyboard]
paths: [userland/desktop/textedit/]
machine: either
human: none
since: ws128
---

## 目的
keyboard の入力が Text Editor に入り、Ctrl+S で file に保存されることを確かめる（UAT F8・F9）。

## 準備
user で `textedit /tmp/aat-work/textedit.txt`（新しい file）を開く。

## 操作と確認
1. 操作: 本文を click し、`hello from aat` と打つ。
   確認事項: 入力。正解: 窓に文字。確認方法: 撮影。
2. 操作: Ctrl+S。
   確認事項: 保存。正解: `TEXTEDIT SAVE path=/tmp/aat-work/textedit.txt bytes=…`、file が `hello from aat`。確認方法: log、file を読む。

## 合格
file の中身。
