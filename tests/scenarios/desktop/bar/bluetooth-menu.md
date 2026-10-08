---
id: desktop.bar.bluetooth-menu
title: system bar の Bluetooth の icon と menu（電源・自分の機器・設定）
status: draft
areas: [compositor, bar, bluetooth]
paths: [userland/desktop/wayland/bluetooth-bar.c, userland/desktop/wayland/bluetooth-shell.c, userland/desktop/wayland/shell.c, userland/desktop/wayland/icons.c]
machine: qemu
human: look
since: ws143-p006
---

## 目的
controller のある機械で bar に Bluetooth の rune が出て、menu から電源を切り替え、pairing した機器を見て、Settings の Bluetooth の頁を開けることを確かめる。

## 準備
`plan/ws143/tests/config-amd64-bt-desktop.mk` の image、kei で login、root で `/sbin/bluetoothd >/tmp/btd.log 2>&1 &`、3 秒待つ。root で
`echo y | bt pair 0A:0B:0C:0D:0E:07` を 1 回（自分の機器を 1 つ。窓が出たら Pair を click）。

## 操作と確認
1. 操作: bluetoothd の起動の前と後で bar を撮る。`aat mark start` は起動の前。
   確認事項: icon の有無。正解: 前は rune が無い（`KWL BT icon` の行が無い）、後は `KWL BT icon x= y= width= height=` と network の fan の左に rune（on なので濃い）。確認方法: log、撮影。
2. 操作: rune を click して撮る。
   確認事項: menu。正解: `KWL BT bar open`、`KWL BT watch on=1`、`KWL BT menu x= y= width= height= rows=N` と各 `KWL BT row index= kind= …`（kind 0 の switch、
   kind 3 の 07 の機器、kind 4 の「Bluetooth Settings...」）、switch は on。確認方法: log、撮影。
3. 操作: switch の行（`KWL BT row … kind=0`）を click、3 秒待って撮る。
   確認事項: off。正解: `KWL BT bar act row=0 kind=0`、`KWL BT request client=0 action=2 error=0`、`KWL BT bar answer error=0`、`KWL BT state … state=3 power=0`、
   menu に「Bluetooth is off」、bar の rune が淡い。確認方法: log、撮影。
4. 操作: もう一度 switch を click、3 秒待つ。
   確認事項: on。正解: `action=1 error=0`、`state=4 power=1`、rune が濃い。確認方法: log、撮影。
5. 操作: 「Bluetooth Settings...」の行を click。
   確認事項: 設定。正解: `KWL BT bar settings pid=…`、`KWL BT bar close via=settings`、Settings が Bluetooth の頁で開く（`ZSETTINGS BLUETOOTH watch on=1`）。確認方法: log、撮影。
6. 操作: rune を click して menu を開き、menu の外を click。次に開いて Esc。
   確認事項: 閉じ方。正解: `KWL BT bar close via=outside`、`KWL BT bar close via=key`、閉じた後 `KWL BT watch on=0`（Settings を閉じた後）。確認方法: log。

## 合格
1〜6 の正解。見えは人が見る。

## 注記
接続・切断の行の動作（機器の行の click）は p005 i02 の統合の後。それまでは機器の行は click しても何もしない（`KWL BT bar act` の行が出ない）。
