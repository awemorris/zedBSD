---
id: apps.files.devices
title: Files の Devices に起動 disk が出ず、mount の前に確認が出る
status: active
areas: [files, volumed]
paths: [userland/desktop/files/, userland/base/volumed/, userland/desktop/wayland/system.c]
machine: either
human: look
since: ws132-p009
---

## 目的
Devices の一覧から起動 disk の partition が除かれ、mount の前に確認の card が出ることを確かめる（uat.md 2026-10-05 午後 #6）。

## 準備
App Home から Files を開く。root で `/` の device（`mount` か `df /`）を読む。

## 操作と確認
1. 操作: Files の Devices を見る。
   確認事項: 一覧。正解: `ZFILES DEVICES count=N`、各 `ZFILES DEVICE id=… name=… mounted=… path=…` に起動 disk（`/` の disk）の partition が無い。確認方法: log と `/` の device の比較、撮影。
2. 操作: mount していない device があれば、その行（`ZFILES DEVICE row id=… x y width height`、窓の中の座標）を double click。
   確認事項: 確認の card。正解: `ZFILES DEVICE mount confirm id=… fs=… bytes=…`、「Mount …?」と Cancel・Mount。確認方法: log、撮影。
3. 操作: Cancel（または Esc）。
   確認事項: 取りやめ。正解: `ZFILES DEVICE mount answer id=… confirmed=0`、mount されない。確認方法: log。

## 合格
1 の正解。2・3 は mount できる device がある時（無ければその旨を記録）。

## 注記
USB の記憶装置を挿して mount する所までは apps.files.mount-usb（人の手）。
