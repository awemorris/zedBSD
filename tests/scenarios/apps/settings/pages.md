---
id: apps.settings.pages
title: Settings の実装済みの頁が全部開く
status: active
areas: [settings]
paths: [userland/desktop/settings/]
machine: either
human: look
since: ws089
---

## 目的
Settings の各頁が失敗なく描かれることを確かめる（頁ごとの回帰の入口）。

## 準備
App Home から Settings を開く。

## 操作と確認
1. 操作: 頁を順に開く: wifi・ethernet・network・appearance・wallpaper・sound・display・languages・storage・keyboard・mouse・touchpad・sharing・users・about（kei で `settings 頁の名前` を流すと、今の Settings がその頁に移る。左の一覧の click でもよい）。
   確認事項: 各頁。正解: `ZSETTINGS PAGE 頁` と `ZSETTINGS LAYOUT page=頁 controls=N`、`ZSETTINGS FAILED` が無い。storage は `ZSETTINGS MACHINE result request=N errno=0`（file system の読み、ws188-p002）の後に「Reading the disks...」でなく disk の card。確認方法: log、頁ごとの撮影（人が見る、about・users・storage は上の答えの行の後に撮る）。

## 合格
15 頁とも log の正解。見えは needs-person。

## 注記
Bluetooth・VPN・Notifications・Battery・Printers・Privacy・Security・Accessibility・Updates は「準備中」の頁で対象外。
