---
id: apps.terminal.type-command
title: Terminal に命令を打って実行する
status: active
areas: [terminal, keyboard]
paths: [userland/desktop/terminal/, userland/desktop/wayland/seat.c]
machine: either
human: none
since: ws128
---

## 目的
keyboard の入力が Terminal の shell に届き、命令が動くことを確かめる。

## 準備
App Home から Terminal を開く。

## 操作と確認
1. 操作: `echo aat-terminal > /tmp/aat-work/terminal.txt` と打って Enter。
   確認事項: file。正解: `aat-terminal` の 1 行。確認方法: root で file を読む、撮影。

## 合格
file の中身が正しい。
