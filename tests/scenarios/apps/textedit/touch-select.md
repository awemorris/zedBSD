---
id: apps.textedit.touch-select
title: Text Editor の指の選択に編集の bar が出て、Copy・Cut・Select All が効く
status: active
areas: [textedit, touch]
paths: [userland/desktop/textedit/main.c, userland/desktop/textedit/app.c, userland/desktop/libkeiland/ui/text-bar.c, userland/desktop/libkeiland/ui/text-touch.c]
machine: either
human: look
since: ws190-p003
---

## 目的
WS190 p003（plan/ws190/phase001/phase.md §3）: Text Editor の本文で、指の double tap の語と 1 本指の drag の選択の上に編集の bar（切り取り・コピー・貼り付け・すべて選択のうち効く物）が出て、button が効き、long press の context menu と key で bar が消えることを確かめる。

## 準備
- 試験の image は `plan/ws190/tests/config-amd64-aat-touch.mk`（aat-input に touch screen）。desktop は `plan/ws035/tests/zdesktop-guest.sh start IMAGE`。
- kei で `textedit /tmp/aat-work/touch.txt`（新しい file）を開き、本文を click して `one two` `key enter` `three four` `key enter` `five` と打つ（3 行）。本文の行の位置は撮影から読む。`aat mark start`。

## 操作と確認
1. 操作: 1 行目の `two` の上を `aat tap X Y --count 2`。
   確認事項: 語と bar。正解: `TEXTEDIT TOUCH changes=… anchor=4 caret=7 handles=1`、`TEXTEDIT TOUCH bar shown buttons=…`（Cut・Copy・Select All、clipboard 次第で Paste）。撮影で `two` の上に bar（人が見る）。確認方法: `aat lines 'TEXTEDIT TOUCH' --since start`、撮影。
2. 操作: bar の Copy を tap。
   確認事項: Copy。正解: `TEXTEDIT TOUCH bar press button=copy`、`TEXTEDIT TOUCH bar hidden`、選択は残る（撮影）。確認方法: log、撮影。
3. 操作: 1 行目の始めから 2 行目の終わりまで 1 本指で `aat touch-drag X1 Y1 X2 Y2 --steps 10`。
   確認事項: drag の選択と bar。正解: drag の後に `TEXTEDIT TOUCH bar shown`（drag の間は bar が無い）、選択は 2 行（`anchor=0 caret=18` 前後、撮影で 2 行が選択の色）。確認方法: log、撮影。
4. 操作: bar の Cut を tap、`aat key ctrl+z`。
   確認事項: Cut と undo。正解: `TEXTEDIT TOUCH bar press button=cut`、撮影で 2 行が消え `five` だけ、Ctrl+Z の後の撮影で 3 行が戻る。確認方法: log、撮影。
5. 操作: 3 行目の `five` を double tap、bar の Select All を tap。
   確認事項: Select All。正解: `TEXTEDIT TOUCH bar press button=select-all`、撮影で全文が選択の色、bar が出たまま。確認方法: log、撮影。
6. 操作: 本文の文字の上を `aat touch-down 0 X Y`、`aat run sleep 1`、`aat touch-up 0`（long press）。
   確認事項: context menu と bar。正解: compositor の context menu（`TEXTEDIT MENU popup`）、`TEXTEDIT TOUCH bar hidden`。Esc で menu を閉じる。確認方法: log、撮影。

## 合格
1〜6 の正解。bar の見た目は人が見る。

## 注記
- Text Editor の double tap は gesture の 300 ms（`aat tap --count 2` は 120 ms）。
- 2 本指の scroll の間も bar は出たまま（設計 §1.1）。
