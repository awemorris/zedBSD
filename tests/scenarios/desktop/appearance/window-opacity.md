---
id: desktop.appearance.window-opacity
title: 窓の opacity を Opaque にすると窓が不透明になる
status: active
areas: [settings, compositor, appearance]
paths: [userland/desktop/settings/page-look.c, userland/desktop/wayland/glass.c, userland/desktop/wayland/settings.c]
machine: either
human: look
since: BUG-171
---

## 目的
BUG-171（opaque にしても窓が不透明にならない）が直ったままであることを確かめる（UAT E2）。

## 準備
Settings の Appearance の頁。今の値を `ZSETTINGS LOOK open opacity=` で控える。

## 操作と確認
1. 操作: 「Window opacity」の slider を右端まで drag。
   確認事項: 値。正解: `ZSETTINGS LOOK set key=window.opacity value=100 error=0` と `ZWL PREFERENCES key=window.opacity applied value=100`。確認方法: log。
2. 操作: 画面を撮る。
   確認事項: 窓。正解: 窓の後ろの壁紙が透けない。確認方法: 撮影（人が見る）。
3. 操作: slider を元の値の所へ drag。
   確認事項: 値。正解: 元の値。確認方法: log。

## 合格
1 の log。2 は needs-person。

## 注記
slider は control 1。値は 85〜100（`page-look.c`）。
