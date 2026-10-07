---
id: desktop.appearance.accent
title: Settings の Accent colour で desktop と app の強調の色が変わる
status: active
areas: [settings, compositor, appearance, libkeiland]
paths: [userland/desktop/settings/page-look.c, userland/desktop/wayland/theme.c, userland/desktop/libkeiland/, userland/desktop/files/main.c]
machine: either
human: look
since: ws179-p003
---

## 目的
Appearance の頁の accent の色（8 色、ws179-p001）を選ぶと、compositor（bar・App Home の強調）と app（Settings・Files）に届くことを確かめる。

## 準備
App Home から Files を開き、Settings を Appearance の頁で開く（`settings appearance` を kei で流すと今の Settings がその頁に移る）。

## 操作と確認
1. 操作: Accent colour の 2 番目の丸（Purple、control 91）を click。
   確認事項: 外観。正解: Settings の switch・選択と Files の選択の強調が紫になる。確認方法: log `ZSETTINGS ACCENT index=1`、`KWL THEME appearance=N accent=1`、`ZFILES ACCENT index=1`（desktop の Files）、撮影（人が見る）。
2. 操作: 1 番目の丸（Blue、control 90）を click。
   確認事項: 外観。正解: 青（既定）に戻る。確認方法: log `ZSETTINGS ACCENT index=0`、`KWL THEME appearance=N accent=0`、`ZFILES ACCENT index=0`（desktop の Files）、撮影。

## 合格
2 回とも Settings・compositor・desktop の Files の log の行（BUG-262 から Files の行も必須）。見えは needs-person。

## 注記
丸は Settings の control 90〜97（`ZSETTINGS CONTROL index=90 x y width height`、窓の中の座標）。desktop の Files の `ZFILES ACCENT index=N` は必須（BUG-262: Files の窓の log の行と混ざって切れていた。fm_log を 1 回の write に直した）。最後は青に戻すので、後の scenario の撮影の色は変わらない。
