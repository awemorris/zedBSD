---
id: apps.videoplayer.open-from-home
title: App Home から Video Player を開いて閉じる
status: active
areas: [videoplayer, home, compositor]
paths: [userland/desktop/videoplayer/, userland/desktop/mediafile/, userland/desktop/wayland/home.c]
machine: either
human: none
since: ws122
---

## 目的
Video Player が App Home から起動し、窓が出て、閉じる button で終わることを確かめる（app ごとの回帰の入口）。

## 準備
desktop。Video Player は動いていない。

## 操作と確認
1. 操作: Windows キーを押して離し、`video` と打つ。
   確認事項: 検索。正解: Video Player の icon が出る。確認方法: log `ZWL HOME search query="video"` と `ZWL HOME icon name="Video Player" x= y=`（icon の中心）。
2. 操作: Video Player の icon を click。
   確認事項: 起動。正解: 10 秒以内に窓が出る。準備の行 `VIDEOPLAYER READY …`。`FAILED`・`ERROR` の行が無い。確認方法: log `ZWL HOME launch name=Video Player pid=`・`ZWL MAP client=C surface=S`、撮影。
3. 操作: title bar の閉じる button を click。
   確認事項: 終わり。正解: 窓が消える。確認方法: log `ZWL UNMAP client=C surface=S`。

## 合格
1〜3 の正解。

## 注記
窓の位置と大きさは `ZWL GLASS launch surface=S … to=x,y size=WxH`（`aat windows`）。閉じる button は title bar の右端から 26 px。
