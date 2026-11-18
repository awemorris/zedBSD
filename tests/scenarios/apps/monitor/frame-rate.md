---
id: apps.monitor.frame-rate
title: System Monitor が本物の値を出し、15 fps 以上で動く
status: active
areas: [monitor, gpu, compositor]
paths: [userland/desktop/monitor/, userland/desktop/wayland/sysmon.c]
machine: either
human: look
since: ws134
---

## 目的
System Monitor の値と描画の速さを確かめる（UAT G1・G2、ws134-p003 の判定は実機の値）。

## 準備
App Home から System Monitor を開く。

## 操作と確認
1. 操作: 開くのを待つ。
   確認事項: 準備。正解: `ZMON READY width= height= source=… cpus=N …`（N ≥ 1）。確認方法: log。
2. 操作: 10 秒待つ。
   確認事項: 速さ。正解: `ZMON FRAME fps=F` の中央値が 15 以上（5330）。確認方法: log、撮影（CPU・memory・disk・network の値は人が見る）。

## 合格
5330: 中央値 ≥ 15。QEMU は値を書いて needs-person。
