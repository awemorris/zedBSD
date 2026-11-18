---
id: os.power.shutdown
title: Shut Down で電源が切れる
status: active
areas: [acpi, power, sessiond]
paths: [src/drivers/acpi/, userland/desktop/sessiond/, userland/base/shutdown/]
machine: hardware
human: hands
since: BUG-197
---

## 目的
BUG-197（`\_S5` で電源が落ちない、前は BUG-119）が直ったままであることを確かめる。

## 準備
5330 の desktop。実行の最後に流す（SSH も切れる）。

## 操作と確認
1. 操作: App Home の Shut Down（または Log Out の後に login の画面の Shut Down）。
   確認事項: 画面。正解: 画面が消える。確認方法: 人が見る。
2. 操作: 20 秒待つ。
   確認事項: 電源。正解: fan が止まり電源の light が消える。電源 button を押さずに。確認方法: 人が見る。

## 合格
電源が切れる。
