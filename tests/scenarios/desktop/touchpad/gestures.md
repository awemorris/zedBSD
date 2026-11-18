---
id: desktop.touchpad.gestures
title: touchpad の指の操作（tap・drag・2 本指・3 本指・押し込み）
status: active
areas: [touchpad, compositor, input]
paths: [src/drivers/i2c/, userland/desktop/wayland/touch.c, userland/desktop/wayland/pointer-accel.c, userland/desktop/wayland/input.c]
machine: hardware
human: hands
since: ws159
---

## 目的
5330 の touchpad の規則（BUG-166・167・178・190・156、ws159-p004、ws142-p003）を人の指で確かめる（UAT 2.1〜2.10）。

## 準備
5330 の desktop、Files と Settings を開く。エージェントは session の log に mark を付けて見る。

## 操作と確認
1. 操作: 1 本指でゆっくり・速く動かす。確認事項: pointer。正解: ゆっくりは細かく、速いと大きく。確認方法: 人。
2. 操作: 1 本指で軽く tap。確認事項: click。正解: 離した時に click（BUG-190）。確認方法: 人、log。
3. 操作: title bar を tap してすぐ触れ直して動かす。確認事項: tap-drag。正解: 窓が付いて動き、離すと止まる（BUG-166・178）。確認方法: 人、log `ZWL GLASS moved`。
4. 操作: Files の folder を 2 回 tap。確認事項: double click。正解: 開く。確認方法: 人。
5. 操作: 2 本指で tap。確認事項: 右 click。正解: context menu。確認方法: 人、log `ZWL MENU context`。
6. 操作: pad を押し込む・押し込んだまま動かす。確認事項: click と drag。正解: 左 click、押した瞬間に pointer が動かない（BUG-167）、drag。確認方法: 人。
7. 操作: Settings・Files で 2 本指で上下に滑らせる。確認事項: scroll。正解: 指の向きに中身が動く（BUG-156）。確認方法: 人。
8. 操作: 3 本指で tap・押し込み。確認事項: gesture。正解: tap は switcher の gesture（ws142-p003）、押し込みは中 click。確認方法: 人、log `ZWL SWITCH`。

## 合格
人の判断（UAT）。
