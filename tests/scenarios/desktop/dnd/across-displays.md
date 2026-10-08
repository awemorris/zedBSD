---
id: desktop.dnd.across-displays
title: drag は画面をまたいで別の画面の窓へ落とせる
status: active
areas: [compositor, dnd, displays]
paths: [userland/desktop/wayland/data.c, userland/desktop/wayland/heads.c, userland/desktop/wayland/compose.c]
machine: either
human: look
since: ws189-p002
---

## 目的
画面をまたぐ drag（ws189 の決定）: 主の画面の窓から拡張の画面（head）の窓へ drag でき、icon と印が両方の画面に残らず描かれること。

## 準備
2 つの画面（拡張）。QEMU で 2 出力が作れなければ実機（machine: hardware として not-run）。主の画面に Terminal A（`echo across` を打って Enter）、head に Terminal B（別の process）。

## 操作と確認
1. 操作: Terminal A の出力の `across` を double-click で選び、その上で押して head の Terminal B の上へ動かして止め、両方の画面を撮る。`aat mark start` は押す前。
   確認事項: enter と描画。正解: `KWL DATA drag enter ... output=1`、`state=copy`。撮影で head に印、主の画面に印・紙の badge の残りが無い。確認方法: log、撮影（人が見る）。
2. 操作: 離す。
   確認事項: drop。正解: `KWL DATA drag drop`、Terminal B に `across`。確認方法: log、撮影。

## 合格
output=1 の enter、drop、残りの絵が無い。

## 注記
QEMU の 2 出力が runner で作れない時は p002 の clearance を塞がない（設計 §5.2）。
