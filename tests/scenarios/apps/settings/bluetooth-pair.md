---
id: apps.settings.bluetooth-pair
title: Settings の Bluetooth の頁で電源・一覧・scan・pairing・削除ができ、pairing の問いは compositor の窓で答える
status: draft
areas: [settings, compositor, bluetooth]
paths: [userland/desktop/settings/page-bluetooth.c, userland/desktop/wayland/bluetooth-shell.c, userland/desktop/wayland/bluetooth-ask.c, userland/desktop/libkeiland/system/system.c, userland/desktop/libkeiland-backend-zedbsd/bluetooth-zedbsd.c, userland/base/bluetoothd/main.c]
machine: qemu
human: look
since: ws143-p006
---

## 目的
WS143 p006 の desktop: app（Settings）→ libkeiland `kl_system_bluetooth_*` → compositor → libkeiland-backend → bluetoothd の経路で、電源の switch、
自分の機器・周りの機器の一覧、scan、pairing（数字の確認・同意・他の人が始めた pairing の窓・lock の間の断り）、削除が動くことを確かめる。

## 準備
`plan/ws143/tests/config-amd64-bt-desktop.mk` の image（AAT の image に試験の kernel の loopback の controller・bluetoothd・bt）。desktop に kei で login。
root で `/sbin/bluetoothd >/tmp/btd.log 2>&1 &` を起動し 3 秒待つ（この image は boot で起動しない）。前の pairing の鍵が残っていれば root で
`bt forget 0A:0B:0C:0D:0E:01` と `bt forget 0A:0B:0C:0D:0E:07`、`bt power on`。loopback の機器: 01 は数字の比較（123456）、07 は Just Works。

## 操作と確認
1. 操作: bluetoothd を起動する前に Settings の Bluetooth の頁を開いて撮り、次に bluetoothd を起動して 3 秒待って撮る。`aat mark start` は開く前。
   確認事項: daemon の有無。正解: 起動の前は `ZSETTINGS BLUETOOTH state reachable=0`、頁に「Bluetooth is not available on this computer.」。起動の後は
   `KWL BT state reachable=1 state=4 power=1`、`ZSETTINGS BLUETOOTH state reachable=1 state=4 flags=…`、頁に「Bluetooth is on.」と switch、Controller の名前と address。
   `ZSETTINGS BLUETOOTH watch on=1` と `ZSETTINGS BLUETOOTH scan on=1`、`KWL BT watch on=1`・`KWL BT scan on=1`。確認方法: log、撮影。
2. 操作: 10 秒待って撮る。
   確認事項: 周りの機器。正解: `KWL BT device address=0A:0B:0C:0D:0E:01 … paired=0` を含む行、頁の Other Devices に loopback の機器が並び、各々に Pair。確認方法: log、撮影。
3. 操作: Other Devices の 0A:0B:0C:0D:0E:01 の Pair を click。compositor の窓が出たら撮り、窓の Pair を click。
   確認事項: 数字の確認。正解: `ZSETTINGS BLUETOOTH ask kind=pair error=0`、`KWL BT ask kind=confirm id=N number=123456 own=1`、`KWL BT ask card …`・`KWL BT ask button=pair …`、
   窓に機器の名前と 123456 と残り秒、Pair と Cancel。click の後 `KWL BT answer yes via=press error=0`、`KWL BT ask closed via=…`、`KWL BT result … error=0`、
   `ZSETTINGS BLUETOOTH result kind=pair errno=0`、頁の下に「The device is paired.」、01 が My Devices に移る。確認方法: log、撮影。
4. 操作: 0A:0B:0C:0D:0E:07 の Pair を click。窓が出たら撮り、Esc。
   確認事項: 同意と断り。正解: `KWL BT ask kind=consent … own=1`（daemon が同意を問わない Just Works ならこの行は無く、そのまま paired。どちらだったかを記録）、Esc で
   `KWL BT answer no via=escape`、`ZSETTINGS BLUETOOTH result kind=pair errno=<EACCES>`（EACCES の値: zedBSD は 25、Linux は 13）と「The pairing was refused.」。確認方法: log、撮影。
5. 操作: root で `echo y | bt pair 0A:0B:0C:0D:0E:07 &`。窓が出たら撮り、Cancel を click。
   確認事項: 他の人の pairing。正解: `KWL BT ask kind=… own=0`、窓に「Asked by root」、Cancel で `KWL BT answer no via=press`、bt の出力が `ERROR rejected`。確認方法: log、撮影。
6. 操作: Super+L で lock し、root で `echo y | bt pair 0A:0B:0C:0D:0E:07`、終わったら kei で解除。
   確認事項: lock の間の問い。正解: 窓は出ず `KWL BT answer no via=locked error=0`、bt の出力が `ERROR rejected`。確認方法: log、撮影（lock の画面のまま）。
7. 操作: My Devices の 01 の Remove を click。
   確認事項: 削除。正解: `ZSETTINGS BLUETOOTH ask kind=forget error=0`、`ZSETTINGS BLUETOOTH result kind=forget errno=0`、「The device is removed.」、01 が Other Devices へ戻る。確認方法: log、撮影。
8. 操作: switch を click して off、5 秒待って撮り、もう一度 click して on。
   確認事項: 電源。正解: `KWL BT request client=… action=power-off error=0`、`KWL BT state … state=3 power=0`、頁に「Bluetooth is off.」と Other Devices が無い、
   `/tmp/btd.log` に scan が止まる（`KWL BT scan on=1` のままでも bluetoothd に SCAN が来ない）。on で `state=4 power=1` に戻る。確認方法: log、撮影。
9. 操作: Settings の別の頁（Sound）へ移る。
   確認事項: 見ない間。正解: `ZSETTINGS BLUETOOTH watch on=0`・`ZSETTINGS BLUETOOTH scan on=0`、`KWL BT watch on=0`・`KWL BT scan on=0`。確認方法: log。

## 合格
1〜9 の正解（4 の consent の有無は記録）。窓・頁の見えは人が見る。

## 注記
- 接続・切断（CONNECT・DISCONNECT・STATUS）は p005 i02 の main への統合の後（この版の bluetoothd は `ERROR request`: 頁に Connect・Disconnect が出ないことだけ確かめる）。
- PASSKEY（数字を機器で打つ形）は loopback の機器に無い。窓の「やめる」は host 試験 `plan/ws143/tests/bt-desktop-host-test.sh` が確かめる。
- 窓の時間切れ（20 秒で no）は手順 3 の窓を 20 秒放って見てもよい（`KWL BT answer no via=time`）。
