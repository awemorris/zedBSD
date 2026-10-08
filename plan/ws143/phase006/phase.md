<!-- awesome-plan project=zedbsd record=ws143-p006 -->
# ws143-p006: desktop — backend の口、zedBSD の backend、API と protocol の版、Settings の頁、system bar、pairing の確認の窓

Status: in-progress（q896、P1、2026-10-08 午後）
Disposition: normal
Parent: [WS143](../ws.md)
Queue: q896（P1、2026-10-08 午後、ユーザーの決定「Mail の添付より先に WS143 p006」、Q1 の投入）
Design: [design.md](../design.md) §1・§6.5・§8

## 範囲

- 経路（Guardrail「Bluetooth と Display も compositor 経由」）: app（Settings）→ libkeiland `kl_system_bluetooth_*` → compositor の拡張 →
  libkeiland-backend `kl_backend_bluetooth_*` → zedBSD は bluetoothd の socket。`plan/tools/keiland-os-boundary/check.sh` を PASS に保つ。
- zedBSD の backend の実体、Linux・FreeBSD は口と stub（「未対応」、`reachable=0`）。Linux の BlueZ は p007。
- bluetoothd に `POWER on|off` と `STATE off`（Q1 2026-10-08: P1 が最小の差分で。切っても保存の鍵は残す、off の間の SCAN・PAIR（と p005 の CONNECT）は
  `ERROR off`、起動の既定は on、記憶しない）、CLI `bt power on|off`。
- 変化の通知: bluetoothd に SUBSCRIBE は足さない（Q1）。backend は**見ている client が居る間だけ** 2 秒ごとと各 request の直後に読み直す。
  SUBSCRIBE は Future Work **F-086**（Q1 が台帳に載せる）。
- Settings の Bluetooth の頁（今の stub を置き換え）、system bar の icon と menu、compositor の pairing の確認の窓。

## ユーザーの決定（朝、Q1 の伝達）

- Q4: pairing の後、HID らしい device は自動で接続する（bluetoothd の p005 の仕事。desktop は PAIR の後に一覧を読み直すだけ）。
- Q5: 人が切断した device からの再接続は断る（bluetoothd の p005。desktop の「切断」は DISCONNECT を送るだけ）。
- B6: account（`_bluetooth`）の無い install では bluetoothd を起動しない。desktop は daemon が居ない時「Bluetooth は使えません」（`reachable=0`）。

## 設計

### 1. bluetoothd（済み、a532eda2c）

`POWER on|off`（D8 の許す人。off は scan・pairing の最中なら `ERROR busy`）、答え `POWER on|off` と `DONE`。SHOW は off の間 `STATE off`。
SCAN・PAIR は off の間 `ERROR off`。log `BLUETOOTHD POWER on|off uid=N`。p005 の CONNECT は main に未統合（i02 は p004 の cleared の後）なので、
CONNECT・DISCONNECT・STATUS が無い daemon（`ERROR request`）も扱う: backend はその機能を「無し」とし、Settings の「接続」「切断」を出さない。

### 2. backend の口（`libkeiland-backend/keiland-backend.h`）

```c
struct kl_backend_bluetooth;
/* 状態 */ KL_BACKEND_BT_ABSENT（daemon が居ない）・_NONE（controller が無い）・_STARTING・_OFF・_ON・_FIRMWARE（要る・失敗）・_UNSUPPORTED・_ERROR
struct kl_backend_bluetooth_state { reachable, state, scanning, pairing（PAIR の最中）, features（POWER・CONNECT の bit）, address[18], name[64] };
struct kl_backend_bluetooth_device { address[18], type（BREDR・LE_PUBLIC・LE_RANDOM）, name[64], kind（KEYBOARD・MOUSE・HEADSET・PHONE・COMPUTER・OTHER）,
                                     paired, connected, legacy, battery（-1 は不明）, rssi（0 は不明） };
open / close / update(&changed) / get_state / get_devices(list, capacity) / set_watching(on) / set_scanning(on)
request(kind: POWER_ON・POWER_OFF・PAIR・FORGET・CONNECT・DISCONNECT, address, type, &id) / take_result(&id, &error, reason, size)
take_question(&kind: CONFIRM・CONSENT・PASSKEY・END, &number, address, name) / answer(yes)
```

- 「同時に 1 request」の例外（design §8.1）: 問いへの答えは PAIR の最中に送れる（別の接続）。action の request は同時に 1 つ（EBUSY）。
- device の一覧は BONDS（paired）と DEVICES（最後の scan）と STATUS（connected・電池、無ければ略）を address で合わせる。kind は class（Major の Peripheral の
  keyboard・pointing、Audio、Phone、Computer）か appearance（0x03C1 keyboard、0x03C2 mouse、0x0940 など）から。
- 名前の `\xNN` は戻す（制御の文字は `?`）。

### 3. zedBSD の backend（`libkeiland-backend-zedbsd/bluetooth-zedbsd.c`）

- 接続は request ごとに非同期の Unix socket（`BTD_SOCKET`、`O_NONBLOCK`）: 1 行を書き、`DONE` まで読んで解く。読み直しは SHOW → BONDS → DEVICES →
  STATUS の順の 1 本の connection で（同時に 1 本）。action は別の 1 本。agent は常時の 1 本（`AGENT` を送り、`CONFIRM N`・`CONSENT`・`PASSKEY N`・
  `AGENT-END` を読む、`YES`・`NO` を書く）。bluetoothd の client は 8 まで、desktop は 3 本まで使う。
- agent は compositor が watching の有無によらず daemon が居る間ずっと持つ（相手から始まる pairing は daemon が断るので、問いは自分の PAIR の時だけ）。
  `ERROR busy`（別の uid の agent）なら 30 秒ごとに試し直す。
- 読み直し: watching の間 2 秒ごと、各 action の結果の直後、PAIR・SCAN の終わりの直後。watching でない間は 30 秒ごと（bar の icon の状態のため）。
- scan: set_scanning(1) の間、`SCAN 8` を繰り返す（終われば次。PAIR の最中と off の間はしない）。
- daemon が居ない・切れた: `reachable=0`、1 秒ごとに試し直す（network-zedbsd.c と同じ）。
- 生の fd は compositor が poll する（既存の backend の fd の集め方に従う、§6 の実装で合わせる）。

### 4. Linux・FreeBSD

既存の慣習（`libkeiland-backend/unsupported/*-unsupported.c`）に従い `libkeiland-backend/unsupported/bluetooth-unsupported.c` を Linux・FreeBSD の両方が使う:
open は成功し state は `reachable=0`・`KL_BACKEND_BT_ABSENT`、request は ENOTSUP。
Linux の BlueZ の実体は p007（ws.md のまま）。

### 5. compositor と protocol

- `kl_system_manager_v1` の版を 22 → 23（`KL_SYSTEM_SINCE_BLUETOOTH 23`、manager の request `GET_BLUETOOTH 15`、wire の capability 0x10000、公開の
  `KL_SYSTEM_HAS_BLUETOOTH 0x20000`・`KL_SYSTEM_CHANGED_BLUETOOTH 0x10000`。machine の追加（534a8c6b4）と同じ file の組）、`kl_system_bluetooth_v1`:
  event: state（reachable・state・scanning・pairing・features・name）、device（一覧の 1 台ずつ、`done` で区切る）、result（request の id・errno・理由）。
  request: watch（0/1）、scan（0/1）、power（0/1）、pair・forget・connect・disconnect（address・type、id）。
- compositor（`wayland/bluetooth-shell.c`、新、printers-shell.c の形）: backend を開き、tick（`kwl_system_tick`）で update、watch・scan は object ごとに数えて
  backend に伝え（bar の menu が開いている間も watch）、変化を `system_tell` の形で各 object へ。
- pairing の確認の窓（compositor の窓、power-dialog の形）: CONFIRM「<名前> の数字が 123456 と同じなら Pair」（Pair・Cancel）、CONSENT「<名前> と pairing しますか」、
  PASSKEY「<名前> で 123456 を打って Enter」（Cancel だけ）。25 秒で消える（daemon の NO に合わせる）。log `KWL BT ask kind=… number=…`・`KWL BT answer yes|no`。
- system bar: Bluetooth の icon（off・on・接続あり）と menu（電源の switch、paired の一覧と接続・切断、「Bluetooth の設定…」で Settings を開く）。Wi-Fi の menu の形。

### 6. libkeiland（KL_VERSION は main の今の値 71 の次 72 を仮に。P2 の WS191 と merge で Q1 が揃える）

`kl_system_bluetooth_state`・`kl_system_bluetooth_devices`・`kl_system_bluetooth_watch`・`_scan`・`_power`・`_pair`・`_forget`・`_connect`・`_disconnect`・
`kl_system_take_bluetooth_result`、`KL_SYSTEM_CHANGED_BLUETOOTH`、`KL_SYSTEM_HAS_BLUETOOTH`。network・displays の形に合わせる（§7 の実装で確定）。

### 7. Settings の頁（`settings/` の Bluetooth、今の `se_soon_draw` を置き換え）

電源の switch、「自分の機器」（paired: 名前・種類の icon・接続中・電池、「接続」「切断」「削除」）、「周りの機器」（頁を開いている間 scan、「Pair」）。
daemon が居ない・controller が無い・firmware が要る時の説明。legacy の bond に「古い方式」の注（D10）。

### 8. 試験

- host: `plan/ws143/tests/bt-desktop-host-test.c`（bluetooth-zedbsd.c を socket の path を `#ifndef` で差し替えて compile、偽の bluetoothd の listener を
  同じ process の thread で: 行の解き方・合わせ方・名前の戻し・状態・request と結果・問いと答え・daemon の不在、volume の host 試験 run-host-volumes.sh の形）。
- AAT（T1、draft）: `tests/scenarios/apps/settings/bluetooth-pair.md`（bt の desktop の image: AAT の image + 試験の kernel の loopback の controller + bluetoothd・bt・
  試験の account。Settings の Bluetooth の頁 → 周りに 0A:0B:0C:0D:0E:01 → Pair → compositor の窓に 123456 → Pair → 自分の機器に出る → 削除。電源 off で
  `STATE off`・scan が止まる）、`desktop.bar.bluetooth-menu`。
- 境界: `plan/tools/keiland-os-boundary/check.sh` PASS。

## Future Work

- **F-086**: bluetoothd の SUBSCRIBE（状態の変化の通知）。今は見ている間の 2 秒ごとの読み直し（Q1 2026-10-08）。

## 記録

- 2026-10-08 P1: bluetoothd の `POWER on|off`・`STATE off`、`bt power` を実装（a532eda2c）。build（config-amd64-bt.mk の bluetoothd・bt）warning 0、
  `bt-daemon-host-test.sh` PASS。
