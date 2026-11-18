---
id: desktop.input-method.choose-method
title: Languages の頁で入力方式（Japanese・SKK・None）を選ぶと desktop が従う
status: active
areas: [settings, ime, compositor]
paths: [userland/desktop/settings/page-languages.c, userland/desktop/wayland/input-method.c, userland/desktop/ime/]
machine: either
human: none
since: ws154
---

## 目的
入力方式の選択が保存され、compositor がすぐ切り替えることを確かめる。

## 準備
Settings の Languages の頁。今の方式を `ZWL IME method=` か `ZSETTINGS LANGUAGES` で控える。

## 操作と確認
1. 操作: 「SKK」の switch を click。
   確認事項: 方式。正解: `ZSETTINGS LANGUAGES ime method=2` と `ZWL IME method=2`。確認方法: log。
2. 操作: 「Japanese」の switch。
   確認事項: 方式。正解: `method=1` の 2 つの行。確認方法: log。
3. 操作: 「None (English)」の switch。
   確認事項: 方式。正解: `method=0` の 2 つの行。確認方法: log、撮影。
4. 操作: 元の方式に戻す。

## 合格
1〜3 の正解。

## 注記
switch は control 2（Japanese）・3（SKK）・1（None）。
