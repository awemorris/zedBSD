---
id: apps.settings.open-from-home
title: App Home から Settings を開いて閉じる
status: active
areas: [settings, home, compositor]
paths: [userland/desktop/settings/, userland/desktop/wayland/home.c]
machine: either
human: none
since: ws089
---

## 目的
Settings が App Home から起動し、窓が出て、閉じる button で終わることを確かめる（app ごとの回帰の入口）。

## 準備
desktop。Settings は動いていない。

## 操作と確認
1. 操作: Windows キーを押して離し、`settings` と打つ。
   確認事項: 検索。正解: Settings の icon が出る。確認方法: log `ZWL HOME search query="settings"` と `ZWL HOME icon name="Settings" x= y=`（icon の中心）。
2. 操作: Settings の icon を click。
   確認事項: 起動。正解: 10 秒以内に窓が出る。準備の行 `ZSETTINGS READY …`。`FAILED`・`ERROR` の行が無い。確認方法: log `ZWL HOME launch name=Settings pid=`・`ZWL MAP client=C surface=S`、撮影。
3. 操作: title bar の閉じる button を click。
   確認事項: 終わり。正解: 窓が消える。確認方法: log `ZWL UNMAP client=C surface=S`。

## 合格
1〜3 の正解。

## 注記
窓の位置と大きさは `ZWL GLASS launch surface=S … to=x,y size=WxH`（`aat windows`）。閉じる button は title bar の右端から 26 px。
