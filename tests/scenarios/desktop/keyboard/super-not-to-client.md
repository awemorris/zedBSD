---
id: desktop.keyboard.super-not-to-client
title: Windows キーは app に届かない
status: active
areas: [compositor, keyboard, terminal]
paths: [userland/desktop/wayland/seat.c, userland/desktop/wayland/super-tap.c]
machine: either
human: none
since: ws142-p002
---

## 目的
Windows キーは desktop の物で、焦点の app に文字も key も届かないことを確かめる（UAT 4.5、D9）。

## 準備
App Home から Terminal を開く。

## 操作と確認
1. 操作: Terminal に `cat > /tmp/aat-work/super.txt` と打って Enter。
   確認事項: 待ち。正解: cat が入力を待つ。確認方法: 撮影。
2. 操作: Windows キーを押して離し（App Home が開く）、もう一度（閉じる）。
   確認事項: App Home。正解: 開いて閉じる。確認方法: log `ZWL HOME open`・`ZWL HOME close`。
3. 操作: `x`、Enter、Ctrl+D。
   確認事項: file。正解: `/tmp/aat-work/super.txt` が `x` の 1 行だけ。確認方法: root で file を読む。

## 合格
file が `x` だけ。
