---
id: desktop.input-method.skk-textedit
title: SKK の入力方式で Text Editor に日本語を打って保存
status: active
areas: [ime, compositor, textedit]
paths: [userland/desktop/ime/, userland/desktop/wayland/input-method.c, userland/desktop/textedit/]
machine: either
human: look
since: ws154
---

## 目的
SKK（大文字で語を始め、Space で変換）が desktop の入力方式として使えることを確かめる。

## 準備
入力方式を SKK に。user で `textedit /tmp/aat-work/skk.txt` を開く。

## 操作と確認
1. 操作: Alt+Space（`ZWL IME language=ja` まで）。
   確認事項: 言語。正解: `ZWL IME language=ja`。確認方法: log。
2. 操作: `Nihongo`（N は大文字）、Space、Enter。
   確認事項: 確定。正解: 「日本語」が確定する。確認方法: 撮影。
3. 操作: Ctrl+S。
   確認事項: 保存。正解: `TEXTEDIT SAVE path=/tmp/aat-work/skk.txt`、file に ASCII でない文字。確認方法: log、file を読む。
4. 操作: Alt+Space で direct に戻し、入力方式を元に戻す。

## 合格
file に仮名か漢字がある。
