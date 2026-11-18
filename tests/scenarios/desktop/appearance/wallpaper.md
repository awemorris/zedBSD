---
id: desktop.appearance.wallpaper
title: Wallpaper の頁で絵を選ぶとすぐ壁紙が変わる
status: active
areas: [settings, compositor, wallpaper]
paths: [userland/desktop/settings/page-look.c, userland/desktop/settings/look.c, userland/desktop/wayland/backdrop.c]
machine: either
human: look
since: ws135
---

## 目的
Wallpaper の頁が止まらずに開き（BUG-152）、選んだ絵が 2 秒以内に壁紙になることを確かめる（UAT E1・E2）。

## 準備
Settings を開き、Wallpaper の頁。今の壁紙を `ZSETTINGS LOOK open … wallpaper=` で控える。

## 操作と確認
1. 操作: 頁が出るのを待つ。
   確認事項: 頁。正解: 絵の tile が並ぶ（`ZSETTINGS LOOK pictures ready count=N`、N ≥ 2）。確認方法: log、撮影。
2. 操作: 今の物と違う tile を click。
   確認事項: 壁紙。正解: `ZSETTINGS LOOK set key=wallpaper value=… error=0`、2 秒以内に `ZWL PREFERENCES key=wallpaper applied`。確認方法: log の時間、撮影。
3. 操作: 元の tile を click。
   確認事項: 壁紙。正解: 元に戻る。確認方法: log。

## 合格
1・2 の正解。絵の見えは needs-person。

## 注記
tile は control 100 から（`LOOK_PICTURE_FIRST`）。
