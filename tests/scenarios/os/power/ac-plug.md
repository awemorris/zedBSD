---
id: os.power.ac-plug
title: AC の抜き差しが分かる
status: active
areas: [acpi, power, compositor]
paths: [src/drivers/acpi/, userland/tests/systemevents/, userland/desktop/wayland/system.c]
machine: hardware
human: hands
since: ws132-p002
---

## 目的
AC の抜き差しの事象が届き、bar の電池の + が追うことを確かめる。

## 準備
5330、AC を挿したまま。エージェントは `systemevents -c ac,battery -t 120000` を動かす。

## 操作と確認
1. 操作: 人が AC を抜く。
   確認事項: 事象と bar。正解: `event N ac change 0 ac charging=0`、bar の電池の + が消える。確認方法: 出力、撮影。
2. 操作: 人が AC を挿す。
   確認事項: 事象と bar。正解: `event N ac change 1 …`、+ が戻る。確認方法: 出力、撮影。

## 合格
2 つの事象と bar の変化。
