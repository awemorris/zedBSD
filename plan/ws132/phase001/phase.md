<!-- awesome-plan project=zedbsd record=ws132-p001 -->

# ws132-p001: /dev/system の事象の通知（電源・PnP）、自動 mount、Files の eject の設計

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: 後続の p002〜p005・p009 が実装・cleared）（旧: in-progress（2026-10-05 P1 generation17 / q707-i01。設計を書いた。人間の判断が要る点 D1〜D3 が残る））
Disposition: normal
Parent: [WS132](../ws.md)
Queue: q707 / q707-i01（2026-10-05 未明の実装の束、Q1 の投入）。design-reviewer は省く（2026-10-05 ユーザー）

## 前提（ユーザーの決定、ws.md）

- subscriber は open の後、read の前に受け取りたい事象の種類を登録する（登録しない種類は届かない）。
- 受け取り手は Keiland（compositor、libkeiland-backend-zedbsd）。/sbin/init は受け取らない。
- ACPI の電源ボタン・蓋・AC の事象を `/dev/system` の事象として配る。
- 範囲は全部ベータ1（電源・PnP の通知・自動 mount・eject）。締切 10/13、間に合わない所はユーザーと削る。

## 今の状態（2026-10-05 に source を読んだ）

| 所 | 今 |
| --- | --- |
| `/dev/system`（`src/drivers/generic/system-device.c`、UAPI `include/uapi/system.h`） | ioctl だけ（`.ioctl = system_ioctl`）。boot の device・mount・PCI・USB の一覧、vmstat、process、swap、halt・reboot・poweroff。open ごとの状態も read・poll も無い |
| ACPI（WS049） | 電源ボタンは固定 event の handler で log だけ（`acpi-kern.c` の `power_button`）。蓋（PNP0C0D）・AC（ACPI0003）・電池（PNP0C0A）の driver は無く、その Notify は "has no handler" の log。EC の `_Qxx` は動く |
| disk | `disk_create()`・`disk_gone()`（`src/kern/disk.c`）。USB storage の抜き差しで呼ばれる |
| input | `drv_input_device_register()`・`unregister()`。compositor は 2 秒ごとに evdev を探し直す（`ZWL_INPUT_SCAN_MS`） |
| network | `net-device.c` の登録。networkd は route socket で interface の変化を受ける |
| 電源の制御 | sessiond が greeter の descriptor からだけ `POWER poweroff/reboot` を受ける（ws131 の D12: session の中では電源の操作を出さない）。backend の power は電池の口が無い |
| mount | root の `mount(8)`。自動の mount、利用者の unmount の方針は無い。Files の eject は未着手（ws127-p002 の F-036） |
| 常駐の流儀 | networkd・audiod は root の daemon で、compositor の backend が unix socket で SUBSCRIBE して状態を受け、libkeiland の `kl_system_*` が compositor の拡張越しに app へ出す |

## 設計

### K1. 事象の核（kernel、p002）

- `/dev/system` に open・close・read・poll を足し、open ごとに subscriber の状態（購読の種類、64 件の ring、待ちの queue）を持つ。今の ioctl はそのまま。
- 購読: `KERN_SYSTEM_EVENT_SUBSCRIBE`（`_IOW`、`struct system_event_subscription { uint32_t classes; uint32_t reserved[3]; }`）。classes は種類の bit。何度呼んでもよく、最後の値が効く。購読していない open の read は EINVAL。
- 種類（class）: `POWER`（電源・sleep の button）、`LID`、`AC`、`BATTERY`、`DISK`、`INPUT`、`NETWORK`、`USB`。`OVERFLOW` は常に届く。
- 記録は固定長の binary（`struct system_event`）: size・class・action（ADD・REMOVE・CHANGE・PRESS）・value（蓋の開閉、AC の on/off など）・sequence（kernel で単調増加）・time_ns（monotonic）・subject（32 byte: `power-button`、`lid`、`da0s1`、`event5`、`ue0`、`usb1.3`）・detail（64 byte の key=value: `removable=1 size=…`、`name=…`）。read は記録の整数倍だけを返す（足りない buffer は EINVAL）。O_NONBLOCK は EAGAIN、それ以外は事象まで待つ。poll は ring が空でない時 POLLIN。
- あふれ: ring が満ちたら最も古いものを捨て、次の read の先頭に `OVERFLOW`（value = 捨てた数）を置く。読み手は今の状態を ioctl で読み直す。
- 送る口（kernel の内部）: `kern_system_event_post(class, action, value, subject, detail)`。spinlock（irqsave）で全ての subscriber の ring に入れ、待ちを起こす。割り込みの文脈・ACPI の event thread・driver の thread から呼べる（allocation をしない）。
- 権限: 事象は device の名前と電源の状態だけで秘密を含まないので、`/dev/system` を open できる者は全種類を購読できる（node の mode に従う）。制御の ioctl の権限（poweroff など）は今のまま。
- 状態の問い合わせ: `KERN_SYSTEM_GET_POWER`（蓋の開閉・AC・電池の有無と % と充電中）を足す。電池の値は [ws134-p009](../../ws134/ws.md)（電池の表示）と同じ口を使うよう Q1 と順を合わせる。

### K2. 事象の送り手（kernel、p002）

| 種類 | どこで | subject・value |
| --- | --- | --- |
| POWER | ACPI の固定 event の電源・sleep の button（`power_button` の handler） | `power-button`・`sleep-button`、PRESS |
| LID | 新しい ACPI の device の口（PNP0C0D）: attach で `_LID` を読み、Notify 0x80 で読み直す | `lid`、CHANGE、value 1 = 開 |
| AC | ACPI0003: `_PSR`、Notify 0x80 | `ac`、CHANGE、value 1 = 接続 |
| BATTERY | PNP0C0A: Notify 0x80（状態）・0x81（情報）、値は `_BST`・`_BIX`（`_BIF`） | `battery0`、CHANGE、value = % |
| DISK | `disk_create()`・`disk_gone()` | disk の名前（`da0`、`da0s1`）、ADD・REMOVE、detail `removable=…` |
| INPUT | `drv_input_device_register/unregister()` | `eventN`、ADD・REMOVE、detail `name=…` |
| USB | USB の device の attach・detach | `usbB.A`、ADD・REMOVE、detail `vendor=… product=… class=…` |
| NETWORK | net device の登録・削除 | interface 名、ADD・REMOVE（link の上下は networkd の route socket のまま） |

ACPI の蓋・AC・電池は `src/drivers/acpi/` の新しい file（`acpi-power.c`）で、`drv_acpi_notify_install`・`drv_acpi_evaluate_integer` を使う（WS049 の口、HAL は変えない）。

### U1. Keiland（p003）

- `libkeiland-backend-zedbsd/events-zedbsd.c`（新）: `/dev/system` を open し、POWER・LID・AC・BATTERY・INPUT・DISK を購読、descriptor を compositor の poll に入れる。backend の host の callback（`power_button`・`lid_changed`・`power_source_changed`・`input_changed`・`volumes_changed`）で compositor に渡す。Linux・FreeBSD は事象の無い stub（2026-10-05 ユーザー「libkeiland-backendのLinux、FreeBSDの実装は、ベータ1までにはやらなくていいです」）。
- compositor: INPUT の事象で evdev を探し直す（2 秒の polling は保険として残す）。AC・電池で system bar の電池の表示を更新（今は見本の絵）。電源ボタン・蓋は D1・D2 の決定に従う。

### U2. 自動 mount の daemon（p004）

- `userland/base/volumed/`（新、root、networkd と同じ流儀）: `/dev/system` の DISK を購読し、removable な disk の partition（または partition の無い disk）を見つけたら、filesystem を調べ（FAT・UFS、zedBSD の対応する物）、`/media/<label または名前>` に mount する。REMOVE（抜かれた）で強制の unmount と directory の片付け。
- compositor の backend（`volume-zedbsd.c`）が unix socket（`/var/run/volumed.sock`）で SUBSCRIBE し、volume の一覧と変化を受け、eject（unmount、使用中なら使っている process を `KERN_SYSTEM_GET_FILE_USAGE` で調べて理由を返す）を頼む。
- mount の持ち主・権限は D3。

### U3. libkeiland と Files（p005）

- `kl_system_manager_v1` に volume（一覧・mount 先・label・removable・eject の可否）と eject の要求を足す（compositor の拡張と libkeiland の `kl_system_*`、WS131 の配置の規則どおり）。
- Files: Locations の volume の一覧を事象で更新、volume の eject の button と context menu（ws127-p002 で止めた F-036）。使用中で eject できない時は理由を出す。

### 試験（各 Phase）

- host: 事象の ring（購読・filter・あふれ・OVERFLOW・記録の整数倍の read）を kernel の file を host で compile して確かめる。ACPI の蓋・AC・電池は WS049 の aml-host の harness に ASL の試験（Notify と `_LID`・`_PSR`）を足す。5330 の DSDT で LID0・ADP1・BAT0 の attach。
- QEMU（T1）: USB storage の hotplug（QMP の `device_add usb-storage` / `device_del`）で DISK の ADD・REMOVE が `/dev/system` に届き、volumed が mount・unmount すること。`system_powerdown` で POWER の PRESS。小さな読み手の道具（`systemevents -d`）を足す。
- 実機（UAT）: 蓋・AC・電源ボタン・USB メモリの抜き差し・Files の eject。

## Phase の分け方（案）

| Phase | 内容 | 依存 |
| --- | --- | --- |
| p002 | K1・K2: 事象の核、UAPI、送り手（ACPI の電源ボタン・蓋・AC・電池、disk、input、USB、network）、`KERN_SYSTEM_GET_POWER`、host の試験、`systemevents` の道具 | p001 |
| p003 | U1: Keiland の backend の事象、compositor の input の探し直し・電池の表示・電源ボタンと蓋（D1・D2） | p002、D1・D2 |
| p004 | U2: volumed と自動 mount・抜去の片付け・eject の口 | p002、D3 |
| p005 | U3: libkeiland の volume の口と Files の eject・Locations | p004 |
| p006 | QEMU の試験と全文の規約 | p002〜p005 |
| p007 | 実機の UAT | p006 |

## 人間の判断が要る点

- **D1 電源ボタンの動作**: 案は、session の中で押すと電源の dialog（Lock・Log Out・Restart・Shut Down・Cancel）を出し、greeter では Shut Down の確認を出す。session の中の Restart・Shut Down は sessiond が session の descriptor からの POWER を受ける形への変更（ws131 の D12 の改訂）が要る。代わりの案は、session では Log Out の後に greeter の Shut Down（D12 のまま）。長押し（firmware の強制断）は触らない。
- **D2 蓋を閉じた時**: WS052 のユーザーの決定（蓋を閉じた時・電源ボタンの短押し・一定時間の無操作で S0i3 に入る、[WS052](../../ws052/ws.md)）に従う。S0i3 ができるまでの間は session を lock する（案、2026-10-05 Q1 の補足）。電源ボタンの短押し（D1）も、S0i3 ができた後は WS052 の決定（S0i3）と合わせて見直す。
- **D3 自動 mount の持ち主と権限**: 案は、console の session の利用者を持ち主として `/media/<label>` に mount（FAT は uid・gid の option、UFS は mount 先の directory の持ち主）し、その利用者が eject（unmount）してよい。nosuid・noexec を付ける。session が無い時（greeter）は mount しない。

D1〜D3 が決まるまでも p002（kernel の核と送り手）は進められる。

## ユーザーの決定（2026-10-05 未明）

- D1: 電源ボタンの短押しは **dialog を出さず、モダンスリープ（S0i3、WS052）に入る**。ユーザー「電源ボタンでダイアログ、と回答してしまいましたが、ダイアログは不要です。モダンスリープに入ることにします。」（一度「dialog を出す」と答えた後の訂正。ws131 D12 の改訂は不要）。S0i3 ができるまでの間の扱いは未定（p008 は WS052 に依る）。
- D2（蓋、S0i3 ができるまでの間）: **画面を消して lock する。蓋を閉じてから 15 分以内に開けた時は password 無しで自動で unlock する**。ユーザー「画面を消しロックしますが、一定時間以内のスリープ解除のとき、アンロックはパスワードなしで自動にします。」・「15 分」。画面を消すのは内蔵の LCD の backlight（WS113 p013 の backlight の口の後。それまでは黒の画面で代える）。
- D3（自動 mount）: **自動 mount はしない。通知を出す**。ユーザー「自動mountはせず、通知を出します。通知をクリックするとFilesの左ペインのDevicesグループにアイコンが表示されるほか、Todayにもアイコンが表示され、何回か点滅します。このアイコンをダブルクリックすると/media/以下にマウントできます。」→ volumed は自動 mount せず、媒体の追加を通知（WS156 の通知）。Files の左の pane の Devices の group と Today に icon（数回点滅）、double click で /media/ の下に mount。p004・p005 の設計を直す。
