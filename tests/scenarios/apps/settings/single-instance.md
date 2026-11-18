---
id: apps.settings.single-instance
title: Settings は 1 つだけ動き、2 回目の起動は頁を渡して終わる
status: active
areas: [settings]
paths: [userland/desktop/settings/main.c, userland/desktop/settings/menu.c]
machine: either
human: none
since: ws089-p016
---

## 目的
2 回目の起動が新しい窓を作らず、今の Settings に頁を渡すことを確かめる。

## 準備
App Home から Settings を開く（窓 1 つ）。

## 操作と確認
1. 操作: user で `settings about` を流す（または App Home からもう一度 Settings）。
   確認事項: 窓と頁。正解: 2 つ目の process は `ZSETTINGS DONE reason=handed-over page=about` で終わり、新しい `ZWL MAP` が無い。今の窓が About に移る（`ZSETTINGS PAGE about`）。確認方法: log、撮影。

## 合格
新しい窓が無く、頁が移る。
