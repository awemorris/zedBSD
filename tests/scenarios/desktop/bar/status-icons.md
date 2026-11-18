---
id: desktop.bar.status-icons
title: system bar の右の icon（電池・network・音量・時計）
status: active
areas: [compositor, bar]
paths: [userland/desktop/wayland/shell.c, userland/desktop/wayland/network.c, userland/desktop/wayland/volume.c, userland/desktop/wayland/media.c]
machine: either
human: look
since: q722
---

## 目的
bar の右の並び（媒体・音量・network・電池・時計）が出ていて、見て分かることを確かめる（UAT 3.1、uat.md 午後 #9・#10）。

## 準備
desktop。

## 操作と確認
1. 操作: log を読む。
   確認事項: 部品の位置。正解: `ZWL NETWORK icon x= y= width= height=` と `ZWL VOLUME icon …`。確認方法: `aat lines`。
2. 操作: 画面を撮る。
   確認事項: 見え。正解: 時計、電池（電池のある機械）、network、音量の icon が重ならずに並ぶ。確認方法: 撮影（人が見る）。

## 合格
1 の行がある。2 は needs-person。
