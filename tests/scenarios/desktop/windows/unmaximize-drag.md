---
id: desktop.windows.unmaximize-drag
title: 最大化した窓を bar から下へ drag すると元の大きさで外れる
status: active
areas: [compositor, windows]
paths: [userland/desktop/wayland/shell.c]
machine: either
human: look
since: BUG-180
---

## 目的
最大化を drag で外す時に、一度最大に戻ってから小さくなる動き（BUG-180）が無いことを確かめる。

## 準備
desktop.windows.maximize-double-click の後（Files が最大化）。

## 操作と確認
1. 操作: system bar の窓の題（docked の題の所）を押して下へ 300 px drag し、途中で撮る。
   確認事項: 外れ方。正解: 窓が元の大きさで pointer に付いて動く。確認方法: log `ZWL GLASS undock surface=S`、途中と後の撮影（人が見る）。

## 合格
`GLASS undock` があり、撮影で一度最大に戻る絵が無い（needs-person）。
