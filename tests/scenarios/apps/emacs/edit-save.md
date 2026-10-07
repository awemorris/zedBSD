---
id: apps.emacs.edit-save
title: Terminal の Emacs で書いて保存する
status: active
areas: [emacs, terminal]
paths: [userland/base/emacs/, userland/desktop/terminal/]
machine: either
human: none
since: ws129-p012
---

## 目的
Emacs（release の image に入る）が Terminal で起動し、書いて保存できることを確かめる（UAT F7）。

## 準備
App Home から Terminal を開く。

## 操作と確認
1. 操作: `emacs -nw /tmp/aat-work/emacs.txt` と打って Enter（GNU Emacs の利用者の打ち方。REmacs は terminal の editor で、`-nw` を受けて何も変えない、BUG-238）。
   確認事項: 起動。正解: Emacs の画面。確認方法: 撮影。
2. 操作: `hello from emacs` と打ち、Ctrl+X Ctrl+S、Ctrl+X Ctrl+C。
   確認事項: 保存と終わり。正解: shell に戻り、file が `hello from emacs`。確認方法: file を読む、撮影。

## 合格
file の中身。
