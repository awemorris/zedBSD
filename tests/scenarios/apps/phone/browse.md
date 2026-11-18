---
id: apps.phone.browse
title: Phone の連絡先を keyboard で選び、timeline を見る
status: active
areas: [phone]
paths: [userland/desktop/phone/]
machine: either
human: look
since: ws170
---

## 目的
Phone が `~/Documents/Phone` の連絡先と timeline を読み、keyboard で連絡先を移れることを確かめる（WS170 p002・p003）。

## 準備
user の `~/Documents/Phone` に連絡先 2 人（Ben: 1 分前の未読の SMS、Aiko: 2 時間前の RCS と 1 時間前の通話）の file を置く。App Home から Phone を開く（`PHONE READY width= height=`）。

## 操作と確認
1. 操作: 窓の中（連絡先の一覧）を click して焦点を移し、下矢印。
   確認事項: 選択。正解: `PHONE SELECT contact=1`（Aiko）、右に timeline（通話の card と message）。確認方法: log、撮影。
2. 操作: 上矢印。
   確認事項: 選択。正解: `SELECT contact=0`。確認方法: log。
3. 操作: Ctrl+Q。
   確認事項: 終わり。正解: `PHONE DONE reason=close`、窓が消える。確認方法: log。

## 合格
1〜3 の log。見え（一覧の 2 人、timeline の吹き出しと通話の card）は needs-person。

## 注記
助け: `plan/tools/aat/scenarios/helpers_phone.py`（終わりに folder を消す）。
