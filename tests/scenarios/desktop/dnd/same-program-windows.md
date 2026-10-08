---
id: desktop.dnd.same-program-windows
title: Terminal の 1 つ目の窓の選択を同じ process の 2 つ目の窓へ drag すると、待たずに貼られる
status: draft
areas: [compositor, dnd, terminal]
paths: [userland/desktop/wayland/data.c, userland/desktop/libkeiland/ui/clipboard.c, userland/desktop/terminal/]
machine: either
human: none
since: ws189-p002
---

## 目的
ws189 の設計 §0 の欠け 1（同じ program の 2 つ目の窓が drop を受けられない）と review B1（同じ program の別の窓の drag は 2 秒待って空になる）が直ったことを確かめる。

## 準備
desktop。Terminal を 1 つ開き、New Window（Ctrl+Shift+N）で同じ process の 2 つ目の窓を開いて重ならないように置く。1 つ目で `echo dndsame` を打って Enter。

## 操作と確認
1. 操作: 1 つ目の窓の出力の `dndsame` を double-click で選び、その上で押して 2 つ目の窓へ drag して離す。`aat mark start` は押す前。
   確認事項: enter と drop。正解: `KWL DATA drag enter ... devices=2`、`KWL DATA drag state=copy`、`KWL DATA drag drop`。確認方法: `aat lines 'KWL DATA drag' --since start`。
2. 確認事項: 貼られるまでの時間と中身。正解: drop の行から 1 秒以内に 2 つ目の窓の入力行に `dndsame`（2 秒の timeout の空ではない）。確認方法: 撮影、`ZTERM` の log。

## 合格
devices=2 の enter、drop、2 つ目の窓に `dndsame` が 1 秒以内。
