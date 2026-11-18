---
id: desktop.input-method.japanese-textedit
title: Japanese の入力方式で Text Editor に日本語を打って保存
status: active
areas: [ime, compositor, textedit]
paths: [userland/desktop/ime/, userland/desktop/wayland/input-method.c, userland/desktop/textedit/]
machine: either
human: look
since: ws095
---

## 目的
romaji から仮名・漢字への変換と確定が app に届き、保存されることを確かめる（UAT F8、BUG-143・139）。

## 準備
入力方式を Japanese に（desktop.input-method.choose-method）。user で `textedit /tmp/aat-work/ja.txt` を開く（空の新しい file）。

## 操作と確認
1. 操作: Alt+Space（日本語へ。`ZWL IME language=ja` になるまで）。
   確認事項: 言語。正解: `ZWL IME language=ja`、bar の言語の印が「あ」。確認方法: log、撮影。
2. 操作: `nihongo` と打ち、Space、Enter。
   確認事項: 確定。正解: 「日本語」（またはその候補）が確定する。確認方法: 撮影（変換中の文字の大きさも見る、BUG-139）。
3. 操作: Ctrl+S。
   確認事項: 保存。正解: `TEXTEDIT SAVE path=/tmp/aat-work/ja.txt`、file に ASCII でない文字。確認方法: log、root で file を読む。
4. 操作: Alt+Space で direct に戻す。

## 合格
file に仮名か漢字がある。変換中の見えは needs-person。
