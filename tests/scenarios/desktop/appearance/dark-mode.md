---
id: desktop.appearance.dark-mode
title: Settings の Dark appearance で desktop と app が dark になる
status: active
areas: [settings, compositor, appearance, libkeiland]
paths: [userland/desktop/settings/page-look.c, userland/desktop/settings/palette.c, userland/desktop/wayland/theme.c, userland/desktop/libkeiland/]
machine: either
human: look
since: ws089-p017
---

## 目的
dark mode の switch が desktop（bar・App Home）と app（Settings・Files）に届くことを確かめる。

## 準備
App Home から Settings を開き、Appearance の頁（`settings appearance` を user で流すと今の Settings がその頁に移る）。App Home から Files も開いておく。

## 操作と確認
1. 操作: 「Dark appearance」の switch を click。
   確認事項: 外観。正解: desktop と Settings と Files が dark の色に変わる。確認方法: log `ZWL THEME appearance=1`、`ZSETTINGS APPEARANCE appearance=1`、撮影（人が見る）。
2. 操作: もう一度 switch を click。
   確認事項: 外観。正解: light に戻る。確認方法: log `ZWL THEME appearance=0`、撮影。

## 合格
2 つの log の行。見えは needs-person。

## 注記
switch は Settings の control 2（`ZSETTINGS CONTROL index=2 x y width height`、窓の中の座標）。
