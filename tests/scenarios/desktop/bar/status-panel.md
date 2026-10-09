---
id: desktop.bar.status-panel
title: 状態の島の click・tap で開く glass の操作パネル（Wi-Fi・Bluetooth・音・入力・電池）
status: active
areas: [compositor, bar, touch]
paths: [userland/desktop/wayland/status-panel.c, userland/desktop/wayland/shell.c, userland/desktop/wayland/network.c, userland/desktop/wayland/volume.c, userland/desktop/wayland/bluetooth-bar.c, userland/desktop/wayland/input-method.c]
machine: either
human: look
since: ws192-p001
---

## 目的
WS192（2026-10-09 ユーザー: タブレットでは島の個別の icon を押しにくい）: 右上の状態の島のどこを click・tap しても画面右上に glass のパネルが開き、大きい行で操作できることを確かめる。

## 準備
desktop（AAT の image）。島の位置は log の `KWL GLASS status left= width=`。

## 操作と確認
1. 操作: 島の真ん中を mouse で click、撮る。Esc。
   確認事項: 開閉。正解: `KWL STATUS panel open output= x= y= width= height= sound=` と各 `KWL STATUS item name=…`、Esc で `KWL STATUS panel close via=key`。確認方法: log、撮影 mouse。
2. 操作: 島を指で tap、撮る。パネルの外（200,600）を tap。
   確認事項: touch の開閉。正解: `panel open`、外の tap で `panel close via=outside`（下の window へは行かない）。確認方法: log、撮影 touch。
3. 操作: もう一度 tap で開き、Input の行（出ている時）を tap、Mute（音の device がある時、`sound=1`）を 2 回 tap、撮る。
   確認事項: 行の操作。正解: `KWL IME indicator next via=panel`、`KWL VOLUME set … via=panel-mute`。確認方法: log、撮影 rows。
4. 操作: Wi-Fi の行の左の方（switch でない所）を tap、撮る。Esc。
   確認事項: network の menu。正解: `KWL STATUS panel close via=item` と `KWL NETWORK open`（右上に今の menu）。確認方法: log、撮影 network-menu。

## 合格
1〜4 の正解。見え（glass、右端が時計の pill の右端、行の大きさ）は人が見る。

## 注記
Wi-Fi・Bluetooth の switch の実の切り替えは network-probe（`bugs.settings-wifi-click-tap` の準備）か実機で。音の slider の drag は `desktop.bar.volume-slider`。Alt+click の network の詳細は `desktop.bar.network-details`。helper は `plan/tools/aat/scenarios/helpers_desktop.py`。
