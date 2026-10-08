<!-- awesome-plan project=zedbsd record=ws143-p006 -->
# ws143-p006: desktop — backend の口、zedBSD の backend、API と protocol の版、Settings の頁、system bar、pairing の確認の窓

Status: test-wait（q896、P1、2026-10-08 夕。実装・host 試験済み、QEMU の AAT は T1 へ）
Disposition: normal
Parent: [WS143](../ws.md)
Queue: q896（P1、2026-10-08 午後、ユーザーの決定「Mail の添付より先に WS143 p006」、Q1 の投入）
Design: [design.md](../design.md) §1・§6.5・§8

## 範囲

- 経路（Guardrail「Bluetooth と Display も compositor 経由」）: app（Settings）→ libkeiland `kl_system_bluetooth_*` → compositor の拡張 →
  libkeiland-backend `kl_backend_bluetooth_*` → zedBSD は bluetoothd の socket。`plan/tools/keiland-os-boundary/check.sh` を PASS に保つ。
- zedBSD の backend の実体、Linux・FreeBSD は口と stub（「未対応」、`reachable=0`）。Linux の BlueZ は p007。
- bluetoothd に `POWER on|off` と `STATE off`（Q1 2026-10-08: P1 が最小の差分で。切っても保存の鍵は残す、off の間の SCAN・PAIR（と p005 の CONNECT）は
  `ERROR off`）。**電源は記憶する（design D11a、ユーザーの決定「前の状態を記憶（初回 on）」。Q1 2026-10-08 が「記憶しない」を取り消し）**:
  `/var/db/bluetooth/power`（鍵と同じ書き方）。SHOW の最後に `POWER on|off`。CLI `bt power on|off`。
- 変化の通知: bluetoothd に SUBSCRIBE は足さない（Q1）。backend は**見ている client が居る間だけ** 2 秒ごとと各 request の直後に読み直す。
  SUBSCRIBE は Future Work **F-086**（Q1 が台帳に載せる）。
- Settings の Bluetooth の頁（今の stub を置き換え）、system bar の icon と menu、compositor の pairing の確認の窓。

## ユーザーの決定（朝、Q1 の伝達）

- Q4: pairing の後、HID らしい device は自動で接続する（bluetoothd の p005 の仕事。desktop は PAIR の後に一覧を読み直すだけ）。
- Q5: 人が切断した device からの再接続は断る（bluetoothd の p005。desktop の「切断」は DISCONNECT を送るだけ）。
- B6: account（`_bluetooth`）の無い install では bluetoothd を起動しない。desktop は daemon が居ない時「Bluetooth は使えません」（`reachable=0`）。

## 受け入れ（Q1 2026-10-08）

1. bluetoothd: POWER（記憶・SHOW の行・off の間の断り）、問いの行の `address= type= uid=` と `ASK-END`。bt-daemon の host 試験 PASS。
2. backend: zedBSD の実体と unsupported、host 試験（偽の bluetoothd、`bt-desktop-host-test`）PASS。
3. compositor・libkeiland・Settings・bar・確認の窓: AAT（電源・一覧・scan・pairing（数字の確認・同意・他の人の pairing の窓）・削除・bar の menu）を T1 の QEMU
   （bt の desktop の image、loopback の controller）で PASS。build（amd64・Linux）warning 0、`keiland-os-boundary/check.sh` PASS。
4. 接続・切断（CONNECT・DISCONNECT・STATUS）は口と UI を作り、daemon が `ERROR request` なら出さない。確かめは p005 i02 の main への統合の後
   （p008 の UAT か p006 の追加の attempt）。p005 i02 の未統合の物の在り処: P2 の branch `agent/p2` と `plan/ws143/wip-20261008/`（tracked.patch・untracked.tar.gz）。

## design-reviewer の反映（2026-10-08、review 1）

- B1: 問いは agent の接続にも PAIR の接続にも来る（backend は両方で読み、来た接続で答える）。他の uid の pairing の問いも seat の人の agent に来る（p004 S6）→
  **窓に始めた人の名前と device を出し seat の人が判断する**（Q1 2026-10-08、推しの案）。bluetoothd の問いの行に `address= type= uid=`、終わりに `ASK-END`。
- B2: SCAN は 4 本目の接続。scan の間に来た request は scan の終わりまで backend が待たせる（daemon は scan 中の PAIR・POWER off を busy で断るため）。scan は
  6 秒、間 2 秒。scan が見た device は backend が 30 秒覚え、新しい scan が表を空にしても一覧が揺れない。scan の lease は compositor が object ごとに数え、
  頁・menu が閉じれば止まる（Wi-Fi の 1 分の満了と同じ形を compositor に）。
- B3: 電源は記憶（上）。
- I1: 問いに id。答えは id で（終わった・新しい問いへの答えは送らない）。窓は ASK-END・PAIR の結果・daemon の不在で閉じる。PASSKEY の窓は終わりまで残り、
  自分の pairing なら「やめる」で request の接続を閉じる（`kl_backend_bluetooth_cancel`）。窓の残り時間は 20 秒（daemon の 25 秒より前）。
- I2: lock の間に来た問いは compositor が即 NO（log）、開いている窓は lock で閉じて NO。greeter の compositor は Bluetooth の backend を開かない。
- I3: AGENT-END（同じ uid の別の program が agent になった）では取り返さない。自分の PAIR の前に AGENT を送り直す。state の `agent` が 0 の間、Settings は
  「別の program が pairing の確認に答えます」と出す。
- I4: off は今は daemon の旗（scan・pairing を断る）。p005 の自動の再接続・page scan・LE の auto-connect は off を守る必要がある → p005 への追記（Q1 経由）。
  SHOW の `POWER on|off` で backend は機能の有無を知る（POWER を送って試さない）。
- I5: 受け入れを書いた（上）。kind は BONDS に class・appearance が無いので、paired の device は scan で見えた時と STATUS（p005）の時だけ種類が分かる → Future Work。
- I6: daemon の 8 本の枠: 接続の直後の EOF（行が 1 つも来ない）は「満ち」として状態を保ち、1 秒後に読み直す。枠の予約は Future Work。
- I7: 番号は manager 23・request 15・wire 0x10000・HAS 0x20000・CHANGED 0x10000・KL_VERSION 72 を P1 が使う（Q1 2026-10-08）。
- M5: host 試験の socket の path は backend の `BT_SOCKET_PATH`（`#ifndef`）で差し替える。M8: 拡張の client は誰でも pairing・削除・電源を頼める（uid の境界と同じ）。限界として記録。

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

## Future Work（Q1 が台帳に載せる、2026-10-08）

- **F-086**: bluetoothd の SUBSCRIBE（状態の変化の通知）。今は見ている間の 2 秒ごとの読み直し。
- p004 の Q2: BR/EDR の legacy の PIN の pairing（D10「受けて警告」、PIN の入力の窓）。
- p004 の Q6: こちらが passkey を打つ形（`PASSKEY?`、KeyboardDisplay の agent）。
- bluetoothd の client の枠（8 本）の予約・uid ごとの上限・遊んでいる client の切断（他の uid が埋めると desktop が繋がらない）。
- BONDS に class・appearance（再起動の後も paired の device の種類の icon）。
- off の時の HCI（page scan・接続の切断・LE の auto-connect の停止。p005 の再接続との組）。

## 記録

- 2026-10-08 P1: bluetoothd の `POWER on|off`・`STATE off`、`bt power` を実装（a532eda2c）。build（config-amd64-bt.mk の bluetoothd・bt）warning 0、
  `bt-daemon-host-test.sh` PASS。
- 2026-10-08 P1: desktop の側を実装（129a1c659 ほか、本節）。
  - compositor: `wayland/bluetooth-shell.c`（`kl_system_bluetooth_v1` の object、backend を desktop の最初の tick で 1 度開く・greeter は開かない、watch は object ごと・
    scan は object ごとに 60 秒の lease（client が 30 秒ごとに頼み直す）、bar の menu の watch、backend の id と client の request の表、errno → `KL_SYSTEM_RESULT_*`、
    問いを窓へ）、`wayland/bluetooth-ask.c`（pairing の窓: power-dialog の形の card。CONFIRM・CONSENT は Pair と Cancel、PASSKEY は Cancel だけで自分の pairing を
    `kl_backend_bluetooth_cancel`。他の人の pairing は「Asked by NAME」、自分の pairing は残り秒。20 秒で no、PASSKEY は END まで（安全のため 60 秒で閉じる）。
    lock・greeter の間の問いは即 no、lock で開いている窓は no。daemon の不在・ASK-END で閉じる。log `KWL BT ask …`・`KWL BT answer yes|no via=…`）、
    `wayland/bluetooth-bar.c`（bar の rune: daemon と controller がある時だけ slot を取る、on は濃い・それ以外は淡い・接続ありは両脇に点。menu: switch（CAN_POWER の時）、
    on でない時の状態の文、paired の機器（CAN_CONNECT の時 click で接続・切断）、「Bluetooth Settings...」で `settings bluetooth` を起動、失敗の行。network の menu の形）。
    hook: system.c・protocol.c・objects.c・kwl.h・glass.h・shell.c（bar の配置・描画・button・key・motion・tick・home）・seat.c・keyboard.c・corner.c・scanout.c。
    icon `GLASS_ICON_BLUETOOTH`（icons.c）。3 つの Makefile。
  - libkeiland: `kl_system_bluetooth_state`・`_devices`・`_watch`・`_scan`・`_power`・`_device`（KL_VERSION 72、`KL_SYSTEM_HAS_BLUETOOTH 0x20000`、
    `KL_SYSTEM_CHANGED_BLUETOOTH 0x10000`、結果は既存の `kl_system_take_result`）。**設計 §6 からの変更**: pair・forget・connect・disconnect は 1 つの
    `kl_system_bluetooth_device(action, address, type)`（wire の device request と同じ形）、結果は専用の take を作らず共通の結果の ring。state の flags に
    `KL_BLUETOOTH_POWERED`（利用者の switch）と `KL_BLUETOOTH_ANSWERS`（この desktop が問いに答える、I3）を足した（wire `KL_SYSTEM_BT_POWERED 0x4`・`_ANSWERS 0x8`）。
  - Settings: `settings/page-bluetooth.c`（stub を置き換え、pages.c の ready=1）。Bluetooth の card（状態の文・switch・controller の名前、I3 の注）、My Devices
    （種類・接続・電池・legacy の注、Connect/Disconnect は CAN_CONNECT の時、Remove）、Other Devices（on の時、Pair）、結果の行。頁が出ている間 watch と scan
    （30 秒ごとに頼み直し、main loop の待ちも `se_bluetooth_wait` で短くする）、頁を離れると止める。文は他の頁と同じく英語のまま。
  - backend の直し: (1) 自分の PAIR の前に agent の役が無ければ AGENT の接続を PAIR より先に作る（AGENT-END の後の pairing で問いが他の program へ行かない）。
    (2) 取られていない結果・問いは update ごとに CHANGED を立て直す（`kl_backend_bluetooth_cancel` が update の外で結果を積むと、次の結果まで compositor が拾わなかった）。
    (3) gcc の -Wrestrict を避けて名前の初期値を memcpy に。
  - 翻訳: `locale/ja/wayland.tr` に 18 件（`tr.py update`・`check` 167/167）、glossary に「ペアリング」「機器」「アダプタ」「ファームウェア」。
  - 試験: host `plan/ws143/tests/bt-desktop-host-test.sh`（偽の bluetoothd の thread: 不在→到達、読み取り（名前の \xNN・種類・paired が先・接続と電池）、agent、
    POWER、off の間の PAIR は ENETDOWN、数字の比較（id で yes、古い id は ENOENT、END）、root の pairing の同意（own=0・user=root、no → ECONNREFUSED）、
    PASSKEY の取り消し（ECANCELED と END）、FORGET、scan の間の request は scan の後に送る、同時に 1 つ（EBUSY）、AGENT-END の後は取り返さず PAIR の前に取り直す、
    満ちた daemon で状態が保たれる、daemon が去ると unreachable、古い daemon（POWER・STATUS・CONNECT が無い））PASS（3 回）。WS131 の host 試験
    `plan/ws131/tests/host-system.sh` に bluetooth-shell.c と unsupported の backend を足し（system.c が呼ぶため）、client の経路（HAS、unreachable、power・pair の
    ENOTSUP、形の悪い address・action の EINVAL）を足した: PASS。AAT（draft）: `apps.settings.bluetooth-pair`、`desktop.bar.bluetooth-menu`。image:
    `plan/ws143/tests/config-amd64-bt-desktop.mk`（AAT の image ＋ loopback の controller ＋ bluetoothd・bt）。
  - 確認: build（amd64 wayland・libkeiland・settings、Linux all）warning 0、`keiland-os-boundary/check.sh` PASS、style の新しい指摘 0、`exports.py`、`check-scenarios.py` PASS。
  - 未実施: QEMU の AAT（T1）、FreeBSD の build（Makefile.freebsd に足しただけ）、実機。接続・切断の確かめは p005 i02 の統合の後。loopback に PASSKEY の機器が無いので
    窓の PASSKEY は host 試験だけ。

### T1-438 の差の直し（2026-10-08、P1、Q1 の依頼）

- (1a) daemon が無い時に `ZSETTINGS BLUETOOTH state reachable=0` が出なかった: Settings は `KL_SYSTEM_CHANGED_BLUETOOTH` の時だけ state を log していて、service が動いていなければ変化が来ない。`se_bluetooth` に `state_logged` を足し、最初の poll で一度は state を読んで log するようにした（`settings/page-bluetooth.c`・`settings.h`）。
- (4) loopback の scan に 07 が出ず、Settings から consent→Esc を試せなかった: 試験の kernel の loopback（`src/drivers/generic/bt-hci-loopback.c`）の inquiry に、Just Works の機器 0A:0B:0C:0D:0E:07（class 0x240404、RSSI -50）の Inquiry Result with RSSI を足した（pairing の振る舞いは前から 07 を Just Works として演じている）。`plan/ws143/tests/bt-daemon-p003.sh` の scan の期待を 5 台（07 の行を足す）に直した。シナリオ `apps.settings.bluetooth-pair` は変えない（手順 4 がそのまま流せる）。
- 確認: `CONFIG_BT_TEST_LOOPBACK` の config（plan/ws143/tests/config-amd64-bt-desktop.mk、BUILD=build/ws143-bt）の vmunix と settings、Linux all は warning 0、style の新しい指摘 0、`bt-desktop-host-test.sh` PASS、ws089 host-build ok。QEMU は T1 に（T1-438 の 1a と 4 の再試験、bt-daemon-p003 の scan）。
