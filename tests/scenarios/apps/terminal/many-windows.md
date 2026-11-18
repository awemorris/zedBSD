---
id: apps.terminal.many-windows
title: Terminal を 20 個開いても全部起動する
status: active
areas: [terminal, compositor, gpu]
paths: [userland/desktop/terminal/, userland/desktop/wayland/, src/drivers/gpu/i915/, userland/desktop/libvulkan/]
machine: either
human: none
since: BUG-175
---

## 目的
BUG-175（Terminal を多数開くと errno=8 で起動しなくなる）と BUG-120（GPU の object の枠）が直ったままであることを確かめる（UAT G4）。

## 準備
desktop。Terminal は動いていない。

## 操作と確認
1. 操作: Terminal を 20 個開く（user で `terminal` を 20 回、または App Home から 20 回）。
   確認事項: 起動。正解: 20 個の `ZWL MAP` と 20 個の `ZTERM START`、`ZTERM FAILED` も `errno=` の失敗の行も無い。確認方法: log を数える、撮影。
2. 操作: 全部閉じる。
   確認事項: 終わり。正解: 20 個の window の client の `ZWL CLIENT gone`（90 秒以内、かかった秒数を記録に残す）。確認方法: log。

## 合格
20 個とも起動し、失敗の行が無い。

## 注記
T1-232 では 20 個の client を手放すのに compositor が 23 秒かかり（`ZWL PERF 22690ms ... work 94.1%`）、その間の次の scenario の Windows key が遅れた。全部 gone になってから終える。
