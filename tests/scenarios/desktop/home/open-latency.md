---
id: desktop.home.open-latency
title: App Home を開くと stage がすぐ出て、icon が後から浮かび上がる
status: active
areas: [compositor, home]
paths: [userland/desktop/wayland/home.c]
machine: either
human: look
since: ws099-p035c
---

## 目的
App Home を開く時の遅れ（BUG-225）を測る: 入力から stage の最初の frame まで、icon の最初の frame まで。

## 準備
user の desktop。

## 操作と確認
1. 操作: Windows キー。
   確認事項: 遅れ。正解: `ZWL HOME layer=cover after_ms=N`（目安 16 以下、1 frame）、`ZWL HOME layer=content after_ms=M`（目安 150 以下）。確認方法: log。QEMU の値は参考（実機で人が見る）。
2. 操作: もう一度開いて撮影。
   確認事項: 見た目。正解: icon が 1 つずつ下から浮かび上がる（30 ms ずつ、180 ms）。確認方法: 人。

## 合格
1. の 2 つの行の値（目安との比較を記録）と、人の目の感触。
