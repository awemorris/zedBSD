---
id: os.power.power-button
title: 電源 button を短く押すと電源のメニューが出て、電源は切れない
status: active
areas: [acpi, power, compositor]
paths: [src/drivers/acpi/, userland/desktop/libkeiland-backend-zedbsd/events-zedbsd.c, userland/desktop/wayland/backend-host.c, userland/desktop/wayland/power-dialog.c]
machine: hardware
human: hands
since: BUG-196
---

## 目的
BUG-196（電源 button でただちに電源が切れた）が直ったままであること、短押しで電源のメニュー（Power Off・Restart・Log Out・Cancel、WS182）が 1 回だけ出ることを確かめる。

## 準備
5330（か 5320）の desktop、wheel の利用者で login 済み。エージェントは `systemevents -c power -t 60000` を動かし、session の log に mark を付ける。

## 操作と確認
1. 操作: 人が電源 button を短く押す。
   確認事項: 電源、画面、事象。正解: 電源は切れない。desktop が暗くなり中央にメニュー（Power Off は赤、4 つとも押せる）。`systemevents` に `event N power press 1 power-button -` が 2 行（押下と解放）、session の log に `KWL EVENT power button` が 1 行、`KL EVENTS power-button release gap_ms=N`（N は数百）が 1 行、`KWL POWER dialog open source=button poweroff=1 restart=1` が 1 行。確認方法: 人が機械を見る、撮影、`aat lines 'KWL (EVENT power|POWER)|KL EVENTS power-button'`。
2. 操作: Esc。
   確認事項: メニュー。正解: 閉じる。`KWL POWER choice=cancel via=escape`。確認方法: 撮影、`aat wait-log 'KWL POWER choice=cancel'`。
3. 操作: 3 秒待ち、人が電源 button を約 1.5 秒押したままにしてから離す。
   確認事項: 事象。正解: メニューが 1 回出る。`KL EVENTS power-button release gap_ms=N` の N が約 1500（2 つ目の record が解放であることの確認）。確認方法: `aat lines`。続けて Esc。
4. 操作: Super+L で lock し、人が電源 button を短く押す。
   確認事項: 画面と log。正解: メニューは出ない（lock の画面のまま）、`KWL POWER button skip reason=locked`。確認方法: 撮影、`aat lines`。

## 合格
1〜4 の正解。

## 注記
メニューの Power Off・Restart・Log Out は押さない（押すと session か機械が終わる）。greeter では `KWL POWER button skip reason=greeter`（画面の Restart・Shut Down を使う）。設計は ws182-p001。
