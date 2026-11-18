---
id: apps.music.open-from-home
title: App Home から Music を開いて閉じる
status: active
areas: [music, home, compositor]
paths: [userland/desktop/music/, userland/desktop/wayland/home.c, userland/desktop/wayland/apps.conf]
machine: either
human: none
since: ws120
---

## 目的
Music が App Home から起動し、窓が出て、閉じる button で終わることを確かめる（app ごとの回帰の入口）。

## 準備
desktop。Music は動いていない。

## 操作と確認
1. 操作: Windows キーを押して離し、`music` と打つ。
   確認事項: 検索。正解: Music の icon（音符）が出る。確認方法: log `KWL HOME search query="music"` と `KWL HOME icon name="Music" x= y=`。
2. 操作: Music の icon を click。
   確認事項: 起動。正解: 10 秒以内に窓が出る。準備の行 `MUSIC READY …`。`FAILED`・`ERROR` の行が無い。確認方法: log `KWL HOME launch name=Music pid=`、撮影。
3. 操作: title bar の閉じる button を click。
   確認事項: 終わり。正解: 窓が消える。確認方法: log `KWL UNMAP client=C surface=S`。

## 合格
1〜3 の正解。
