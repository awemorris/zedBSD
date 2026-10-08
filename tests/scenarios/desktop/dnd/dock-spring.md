---
id: desktop.dnd.dock-spring
title: drag を bar の app の icon の上で止めると、その app の窓が前に出て、そこへ落とせる
status: active
areas: [compositor, dnd, bar]
paths: [userland/desktop/wayland/apps-bar.c, userland/desktop/wayland/data.c, userland/desktop/wayland/seat.c]
machine: either
human: look
since: ws189-p002
---

## 目的
dock bar の spring-loaded（ws189 の決定）: drag が bar の app の icon の上で 700 ms 止まると、その app の窓が前に出ること、bar の上では印が出ず下の窓が target にならないこと。

## 準備
desktop。Text Editor の窓を 1 つだけ開き（前の手順の Text Editor が残っていれば閉じる。窓が 2 つ以上の app は preview が出る、手順 4）、その上に Terminal を最大化しない大きさで重ねて Text Editor を隠す（bar に両方の icon）。Terminal で `echo springy` を打って Enter。

## 操作と確認
1. 操作: Terminal の出力の `springy` を選び、その上で押して bar の Text Editor の icon の上へ動かし、1 秒止める。`aat mark start` は押す前。
   確認事項: spring。正解: icon の上で `KWL DATA drag state=neutral`、bar の上で `KWL DATA drag enter` が無い、約 0.7 秒で `KWL APPS spring app=… surface=S`、Text Editor が最前面。確認方法: log、撮影（icon の光、前に出た窓）。
2. 操作: pointer を前に出た Text Editor の本文へ動かして離す。
   確認事項: drop。正解: `KWL DATA drag enter`（Text Editor の client）、`state=copy`、`KWL DATA drag drop`、本文に `springy`。確認方法: log、撮影。
3. 操作: もう一度 drag し、Text Editor の icon の上で 0.3 秒だけ止めてから外して離す。
   確認事項: 短い止まり。正解: `KWL APPS spring` の行が増えない。確認方法: log。
4. 操作: Text Editor の窓をもう 1 つ開き（Terminal で `textedit &` を打つ。T1-436 の 07 の撮影のように窓が 2 つになる）、Terminal を前に戻す。手順 1 と同じ drag で Text Editor の icon の上に 1 秒止め、出た preview の片方の上で 1 秒止めてから、前に出た窓の本文で離す。
   確認事項: 窓が複数の app の spring。正解: icon の上で約 0.7 秒で `KWL APPS spring app=… previews=2`（窓はまだ前に出ない）、preview の上で約 0.7 秒で `KWL APPS spring app=… surface=S`、その窓に drop。確認方法: log、撮影。

## 合格
1〜4 の log、前に出た窓への drop。
