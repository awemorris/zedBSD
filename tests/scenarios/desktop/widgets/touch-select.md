---
id: desktop.widgets.touch-select
title: 文字の欄と text area の指の選択（double tap の語、端の handle の drag、編集の bar）
status: active
areas: [libkeiland, widgets, touch]
paths: [userland/desktop/libkeiland/ui/field.c, userland/desktop/libkeiland/ui/text-area.c, userland/desktop/libkeiland/ui/text-select.c, userland/desktop/libkeiland/ui/text-bar.c, userland/desktop/libkeiland/ui/ui.c, userland/tests/kuidemo/main.c]
machine: either
human: look
since: ws190-p002
---

## 目的
WS190（ユーザーの要望「テキストをダブルタップすると選択モードになり、開始端・終了端をドラッグで決め、指を離すとコピーと切り取りのボタンがポップアップ」、plan/ws190/phase001/phase.md §1）: libkeiland の 1 行の欄と text area で、指の double tap が語を選んで両端に handle と編集の bar（切り取り・コピー・貼り付け・すべて選択のうち効く物）を出し、handle の drag で端が動き、bar の button が clipboard と undo を通って効くことを確かめる。

## 準備
- 試験の image は `plan/ws190/tests/config-amd64-aat-touch.mk`（AAT の image ＋ kuidemo。aat-input に touch screen がある）。desktop は `plan/ws035/tests/zdesktop-guest.sh start IMAGE`（Venus）で起動。
- `aat start`、kei で `kuidemo --height=820` を開く（stderr の `KUIDEMO` の行は session.log）。窓の位置は `aat windows`、Controls の頁の Settings の card の Name・Password・Notes の欄の位置は撮影から読む（文字の左端は欄の左端から 12 px、1 行の欄の文字の縦の中央は欄の中央、Notes の 1 行目の中央は欄の上端から 18 px、2 行目は 38 px）。
- 指は `aat tap X Y`（`--count 2` で double tap）、`aat touch-down 0 X Y`・`aat touch-move 0 X Y`・`aat touch-up 0`。handle の knob は選択の端の文字の境の下、欄の下端の少し上（1 行の欄で上端から 30 px ほど）にある。`aat mark start` は手順 1 の前。

## 操作と確認
1. 操作: Name を tap、`aat key ctrl+a`、`aat key backspace`、`aat type 'hello world'`。`world` の `o` の上を `aat tap X Y --count 2`。
   確認事項: 語の選択と bar。正解: `KUIDEMO SELECT id=name anchor=6 caret=11`（最後の行）。撮影で `world` が選択の色、両端に丸い knob の handle、欄の上に横の bar（Cut・Copy・Select All。clipboard に文字があれば Paste も）。確認方法: `aat lines 'KUIDEMO (SELECT|FIELD)' --since start`、撮影（人が見る: bar と handle の形）。
2. 操作: 左の handle（`w` の前の knob）に `aat touch-down 0 X Y`、`aat touch-move 0` で `hello` の前（欄の文字の左端）まで 3 回に分けて動かし、`aat shot`、`aat touch-up 0`、`aat shot`。
   確認事項: 端の drag。正解: 指の間の撮影に bar が無い。up の後 `KUIDEMO SELECT id=name anchor=11 caret=0`、撮影で全文が選択の色、bar が戻る。確認方法: log、撮影。
3. 操作: bar の Copy を tap。Password を tap、`aat type x`、Password の文字の上を double tap、`aat shot`。bar の Paste を tap。
   確認事項: Copy と秘密の欄の bar と Paste。正解: Copy の後の撮影で bar が消え Name の handle は残る。Password の bar に Copy・Cut が無く Paste がある（撮影、人が見る）。Paste の後 `KUIDEMO FIELD password length=11`（`x` が全文の選択に置き換わり `hello world` の 11 字。秘密の欄の double tap は全文を選ぶ）。確認方法: log、撮影。
4. 操作: Notes を tap、`aat type 'one two'`、`aat key enter`、`aat type three`。1 行目の `two` を double tap。下の handle（`two` の後の knob）を 2 行目の `three` の後まで touch-drag（down・move 3 回・up）。bar の Select All を tap。bar の Cut を tap。`aat key ctrl+z`。
   確認事項: text area の語、行をまたぐ端、Select All・Cut・undo。正解: double tap の後 `KUIDEMO SELECT id=notes anchor=4 caret=7`、drag の後 `anchor=4 caret=13`、Select All の後 `anchor=0 caret=13`、Cut の後 `KUIDEMO FIELD notes bytes=0`、Ctrl+Z の後 `KUIDEMO FIELD notes bytes=13 text=one two|three`。確認方法: log。
5. 操作: 頁の何も無い所（card の外の右下）を tap、`aat shot`。
   確認事項: mode を出る。正解: 撮影で bar と handle が無い（Notes の選択の色は欄に残ってよい）。`KUIDEMO KEY` の行が手順 1〜5 の間に無い（bar の button の key は app に渡らない）。確認方法: 撮影、log。
6. 操作: 画面 keyboard が出る image なら、Name を tap して keyboard を出し、Name の文字の上を double tap、`aat shot`。
   確認事項: bar が keyboard に重ならない。正解: 撮影で bar が keyboard の上端より上にある。確認方法: 撮影（人が見る）。keyboard が出ない image では not-run と書く。

## 合格
1〜5 の正解（6 は image 次第）。bar と handle の見た目は人が見る。

## 注記
- 欄の double tap は 400 ms 以内（kl_ui の click の格上げ）、`aat tap --count 2` は 120 ms で叩く。handle の knob の届く範囲（半径 22 px）の中の tap は handle の物で、選択を変えない。
- mouse の double click は今のまま全文を選び、bar を出さない（手順には入れない、host 試験 `plan/ws190/tests/host-touch-select.c` が確かめる）。
- Notes の文字の box・Settings・Files の改名の欄は指の選択を使わない（WS190 §1.5、Future Work F-087・F-088）。
