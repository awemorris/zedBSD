---
id: os.power.battery-state
title: 電池と AC の状態が分かる
status: active
areas: [acpi, power, systemevents, compositor]
paths: [src/drivers/acpi/, userland/tests/systemevents/, userland/desktop/wayland/system.c, userland/desktop/wayland/shell.c]
machine: hardware
human: look
since: BUG-195
---

## 目的
電池と AC（ACPI の `_BST`・`_BIF`・EC）が読め、system bar に電池が出ることを確かめる（BUG-195、uat.md 2026-10-05 午後 #9）。

## 準備
素の 5330、AC を挿したまま。

## 操作と確認
1. 操作: `systemevents -p` を実行。
   確認事項: 電源の状態。正解: `power lid=1 ac=1 battery=NN charging=…`（NN は 0〜100）。確認方法: 出力。
2. 操作: session の log を読む。
   確認事項: compositor の知る電池。正解: `ZWL POWER source=… percent=NN charging=…`（NN ≥ 0）。確認方法: `aat lines 'ZWL POWER source='`。
3. 操作: system bar を撮る。
   確認事項: 電池の icon。正解: 時計の左に残量の長さの電池、充電中は右に +。確認方法: 撮影（人が見る）。

## 合格
1・2 の正解。3 は人の確認（needs-person）。

## 注記
電池の無い機械では bar に電池の場所を空けない（2026-10-05 ユーザーの決定）ので QEMU は対象外。AC の抜き差しは os.power.ac-plug。
