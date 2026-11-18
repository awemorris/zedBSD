---
id: os.power.lid
title: 蓋を閉じると画面が消え、開けると戻る
status: active
areas: [acpi, power, compositor, lock]
paths: [src/drivers/acpi/, userland/desktop/wayland/lid.c, userland/desktop/wayland/system.c]
machine: hardware
human: hands
since: ws132-p008
---

## 目的
蓋の事象で画面が消え、短い間なら password 無しで同じ desktop に戻ること、lock の画面は lock のまま戻ることを確かめる。

## 準備
5330 の desktop、login 済み。session の log に mark。

## 操作と確認
1. 操作: 人が蓋を閉じ、5 秒後に開ける。
   確認事項: 画面と log。正解: 閉じて画面が消え、開けて同じ desktop（password を聞かない）。`ZWL EVENT lid closed`・`ZWL LID screen off`・`ZWL EVENT lid open`・`ZWL LID screen on`。確認方法: 人が見る、`aat lines 'ZWL (EVENT lid|LID screen)'`。
2. 操作: Super+L の後に蓋を閉じて開ける。
   確認事項: 開けた後の画面。正解: lock の画面のまま。確認方法: 撮影。

## 合格
1・2 の正解。

## 注記
15 分を超えて閉じると lock の画面（ws132-p008）。普段の実行には入れない。
