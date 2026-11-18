---
id: apps.files.mount-usb
title: USB の記憶装置を挿すと Devices に出て、確認の後に mount できる
status: active
areas: [files, volumed, usb]
paths: [userland/desktop/files/, userland/base/volumed/, src/drivers/usb/]
machine: hardware
human: hands
since: ws132-p009
---

## 目的
USB の記憶装置の抜き差しと mount の確認を実物で確かめる（UAT 3.6）。

## 準備
5330 の desktop、Files を開く。FAT か exFAT の USB の記憶装置（起動の USB と別）。

## 操作と確認
1. 操作: 人が USB の記憶装置を挿す。
   確認事項: 一覧と bar。正解: Devices に出る（`ZFILES DEVICE … new=1`）、bar に媒体の icon（`ZWL MEDIA new id=… label=…`）。確認方法: log、撮影。
2. 操作: その行を double click。
   確認事項: 確認。正解: 確認の card（`ZFILES DEVICE mount confirm`）。確認方法: log、撮影。
3. 操作: Mount。
   確認事項: mount。正解: `ZFILES DEVICE result id=… mount=1 errno=0`、中身が見える。確認方法: log、撮影。
4. 操作: 取り出して、人が抜く。
   確認事項: 一覧。正解: Devices から消える。確認方法: log、撮影。

## 合格
1〜4 の正解。
