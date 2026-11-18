---
id: apps.photos.open-from-home
title: App Home から Photos を開いて閉じる
status: active
areas: [photos, home, compositor]
paths: [userland/desktop/photos/, userland/desktop/wayland/home.c, userland/desktop/wayland/apps.conf]
machine: either
human: none
since: ws157
---

## 目的
Photos が App Home から起動し、窓が出て、閉じる button で終わることを確かめる（app ごとの回帰の入口）。

## 準備
desktop。Photos は動いていない。

## 操作と確認
1. 操作: Windows キーを押して離し、`photos` と打つ。
   確認事項: 検索。正解: Photos の icon（重なった 2 枚の写真）が出る。確認方法: log `KWL HOME search query="photos"` と `KWL HOME icon name="Photos" x= y=`。
2. 操作: Photos の icon を click。
   確認事項: 起動。正解: 10 秒以内に窓が出る。準備の行 `PHOTOS READY …`。`FAILED`・`ERROR` の行が無い。確認方法: log `KWL HOME launch name=Photos pid=`、撮影。
3. 操作: title bar の閉じる button を click。
   確認事項: 終わり。正解: 窓が消える。確認方法: log `KWL UNMAP client=C surface=S`。

## 合格
1〜3 の正解。
