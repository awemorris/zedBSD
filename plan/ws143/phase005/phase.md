<!-- awesome-plan project=zedbsd record=ws143p005 -->

# ws143-p005: HID host（`/dev/input/bridge`・共有の HID glue・SDP・HIDP・GATT/HOGP・再接続）

Phase ID: `ws143-p005`
Parent: [WS143](../ws.md)
Status: in-progress / test-wait（i02、T1 の依頼は Q1 経由: bt-hid-p005.sh と p002〜p004 の回帰）（2026-10-08 夜 q904 P1: i02a〜i02d を実装、host 試験 PASS。下の「i02c の記録」「i02d の記録」。i03（LE の HOGP）は未着手で Q1 の指示待ち、i04 は 5330。i01a〜c の判定は Q1）（旧: in-progress（2026-10-08 夜 q904 P1: i02a・i02b 済み（host 試験 PASS、QEMU は i02d の T1 で）、i02c は hid.c と main の口まで（build のみ、host 試験は未、pair の handoff は未接続）、i02d は未。下の「i02c の途中の記録」が再開点。i01a〜c の判定は Q1）（旧: in-progress（2026-10-08 q904 P1: i02a（純粋な部品と host 試験）まで済み、i02b〜i02d は未。下の「i02a の記録」。i01a〜c の判定は Q1）（旧: in-progress（2026-10-08 q902 P1 の照合: i01a T1-419 PASS、i01b T1-421 FAIL の後 T1-426 input-bridge-p005 PASS・T1-432 PASS、i01c T1-423 boot-test PASS（i2c-hid の touchpad の実機の回帰は 5330）→ i01a〜c は Q1 の判定待ち。i02・i03 は未着手、i04 は 5330）（旧: in-progress / test-wait（i01a、q888 P2 2026-10-08: glue の refactor と USB の回帰を実装し host 試験 PASS。QEMU は T1 待ち。以前: 詳細設計の第 1 版と改訂 2、design-reviewer の review-1（[review-1.md](review-1.md)）を反映済み）））
Phase disposition: normal
Queue: 設計（P2、2026-10-08）→ q888（P2、2026-10-08、承認済み）: i01a → i01b。実装の attempt の区切りは §「attempt の区切り」。

## 範囲

[design.md](../design.md) §5.2（`/dev/input/bridge`、D3「形を承認し struct は p005 で review」）、§6.1 の SDP・GATT・HID host、§6.3 の「忘れる」の Virtual Cable Unplug、§6.4（再接続と切断）、§10.1 の p005 の行（usb-hid の glue の共有の module への refactor と USB の回帰、`/dev/input/bridge`、hid-report.c の fuzz、SDP・GATT client、HID host（BR/EDR と HOGP）、再接続、切断で key を離す）。p004 の bluetoothd（session・pair・l2cap・smp・keys・privsep・口）の上に足す。

Q1 の条件（2026-10-08）: 「HID は既存の usb-hid・hidraw・evdev の経路（WS161 の hidraw）と矛盾しない形で、入力は compositor の evdev に届くこと」。この設計は、Bluetooth の HID の入力を **既存の kernel の HID の層（hid-report.c の parser、hid-touch.c・hid-digitizer.c の状態機械、input.c の evdev）に流し、`/dev/input/eventN` として compositor が USB の device と同じ道で見つける**形にする（§「事実」の 7〜9 行）。**p005 で確かめられるのは kernel の evdev の node まで**（root の `evdev-probe`）: login の後に現れた node を seat の人（compositor）に渡す sessiond の仕事は未実装で、USB の hotplug も同じ（事実 33、review S13、§Q23）。

## 範囲の外（理由と行き先）

- **出力の report（キーボードの LED、HOGP の Output Report・Boot Output）**: kernel に出力の経路が無い（`include/kern/input-device.h` 64〜84 行の `input_device_info` に出力の callback は無く、hid-report.h に encoder も無い。design §5.2 [F6, N16]）。`/dev/input/bridge` の read は EAGAIN、poll は POLLIN を言わない。evdev の EV_LED から作る経路は後の Phase（Future Work に登録を Q1 に依頼）。
- **i2c-hid の共有 glue への乗せ替え**: §「判断の記録」Q1。i2c-hid は touch の device しか出さず（`src/drivers/i2c/i2c-hid.c` 1019〜1030 行: touch が無ければ ENODEV、1330〜1370 行: touch の report だけ translate）、key の集約や pen を使わない。乗せ替えは 5330 の touchpad（実機だけで確かめられる）の回帰の危険があり、Bluetooth には益が無いので p005 ではしない。**design.md §5.2 の「USB・I2C・input bridge が共有する module」の文言と違う**ので、ユーザーの確認を求める（Q1）。
- **hidraw への Bluetooth の HID の公開**: §Q3。`/dev/input/hidrawN` は input でない raw の interface（FIDO）のための口で（`include/uapi/hidraw.h` 92〜95 行、`usb-hid.c` 689〜703 行は top の usage が FIDO の時だけ raw）、Bluetooth のキーボード・マウスは evdev へ流す。top の usage が FIDO の descriptor を `/dev/input/bridge` に書いたら ENXIO で断る（review M2 で EOPNOTSUPP から変えた。Bluetooth の FIDO は WS161 の範囲でも無い）。
- **A2DP・PAN・SCO、LE Audio**: design §7（別の WS、D1）。
- **desktop（Settings・system bar・pairing の窓・backend の口の `SUBSCRIBE` の event）**: p006。この Phase は socket の request（CONNECT・DISCONNECT・STATUS）と CLI `bt` まで。p006 が要る状態の変化の通知（接続・切断・電池）は、口の行の形をここで決めておき（§5）、SUBSCRIBE の配りは p006 で足す。
- **PIN の legacy pairing、`PASSKEY?`（こちらが打つ）、KeyboardDisplay の agent**: p004 の Q2・Q6 のまま（p006 の窓の後）。
- **LE の private address（D11c、自分の IRK と RPA）**: p004 は「p005 の再接続と一緒に」とした。この Phase は **相手の RPA の解決（controller の resolving list、相手の IRK は p004 が保存済み）** を扱い、**自分の RPA（LE Set Random Address・own address type 0x02/0x03）は i03 の後の判断**にする（§Q11）。理由: 自分の RPA を使うと bond 済みの相手が自分を見つけられない組（相手が自分の IRK を配られていない）が起き、鍵の配りの要求（p004 の smp.c は initiator の鍵を配らない: 「鍵の配り: initiator 0」）の変更が要る。
- **5330 の実機**: QEMU で確かめられる物までをこの Phase の attempt に置き、実機の相手（本物のキーボード・マウス）は p008 の UAT。i03 の後に 5330 で短い接続の確かめをする余地は残す（§attempt の i04）。
- **HID の boot protocol（SET_PROTOCOL boot、Boot Keyboard/Mouse Input Report の characteristic）**: report protocol だけを使う。boot だけの device は無い前提（HID 1.1.1 は report protocol を必須にしている。**未確認**: 版と節）。
- **resume の時の Read Version のやり直しと firmware の load し直し**（design §5.3、p003 が「p004 以降」とした物。事実 16）: **Q1 の決定（2026-10-08）: p003 の残件に分ける（Sleep はベータ3）。** この Phase は `/dev/system` の購読を足し、`sleep.end` を hid.c に渡す口だけを作る（§Q19）。
- **login の後に現れた `/dev/input/eventN` を seat の人に渡すこと**: 第 1 版の「渡されない」は誤りだった（BUG-264 の読み、2026-10-08 q888）。sessiond は greeter と session の loop で毎秒与え直しており、実際の不具合は compositor が使うまでの約 2 秒の遅れ。BUG-264 で sessiond が `/dev/system` の INPUT・USB を購読して直ちに与え、compositor が event の後 2 秒は 100 ms ごとに走査する形に直した（T1 の試験待ち）。この Phase の QEMU の確かめは root の `evdev-probe` で kernel の node までを示す（変わらず）。
- **HID_CONTROL の SUSPEND・EXIT_SUSPEND を sleep の時に送る**（review M12、任意）: Sleep はベータ3。記録だけ。

## 事実（調べた既存の経路。行番号は 2026-10-08 の worktree p2）

| # | 事実 | 出典 |
| --- | --- | --- |
| 1 | usb-hid.c は「XXX: Need coding style fitting.」で始まる。report から input device を作る glue は、descriptor の取得と parse・capability・axis・pen・touch の記述（`usb_hid_fetch_layout`）、名前の決め（`usb_hid_identity`: product 名が無い時は capability から「USB HID mouse」など）、report の公開（`usb_hid_publish_report`: decode → touch か pen は状態機械へ → report ID ごとの held の bit map と全 report の集約 → 変わった key だけ EV_KEY → EV_REL の 0 は出さない → 出した時だけ SYN_REPORT）、pen・touch の公開、`drv_input_device_register` の呼び（BUS_USB、touch は別の device、capability が SYN だけなら device を作らない）、取り消し（`usb_hid_unpublish`） | `src/drivers/usb/usb-hid.c` 8 行、653〜797、852〜915、1012〜1167、1187〜1221、1427〜1559、1588〜1653 行（design §5.2 の「128〜139・970〜1117・1424〜1435・1556〜1588」は古い行。今の行はこれ） |
| 2 | usb-hid の transport の部分: attach（endpoint、descriptor、SET_PROTOCOL report、URB、`usb_hid_input_is_ready` の pending の列）、detach（URB の cancel と drain、worker の join、unpublish）、worker（URB の完了 → `usb_hid_publish_report` → 出し直し）。FIDO の raw の経路は `drv_hidraw_describe` の後に descriptor を保って parser に通さない | 同 322〜436、438〜483、1266〜1361、689〜703 行 |
| 3 | 名前・path・unique_id の上限は 63 byte + NUL（`USB_HID_TEXT_MAX 64`、`drv_input_device_register` は `INPUT_TEXT_MAX 64` 以上を ENAMETOOLONG） | `usb-hid.c` 46〜47 行、`src/drivers/generic/input.c` 41、2459〜2474 行 |
| 4 | input.c の register は node `eventN`（rdev `0x00030000 + N`）を作り、`/dev/system` の INPUT の class に ADD（subject `eventN`、detail `bus=%u props=%x name=%s`）を出す。**unregister は、押されている key を全部 EV_KEY 0 で離し（queue と subscriber の両方）、SYN_REPORT を出し、DETACH を publish し、REMOVE の event を出す** | `input.c` 252〜376（341〜343・370 行）、654〜869（750〜830・843 行）、2131〜2147 行 |
| 5 | input-inject（試験の口）は root だけ（`cred_is_superuser`）、open は最大 4（`INPUT_INJECT_OPENS_MAX`）、最初の write が `struct input_inject_setup`（純粋な `inject_setup_valid` で検査）、close で unregister（「the input layer releases its held buttons」） | `src/drivers/generic/input-inject.c` 190〜196、356、99、237 行、`include/uapi/input-inject.h` 9〜30・94 行 |
| 6 | hidraw: class の register・input・unregister、node `hidrawN`（`0x000f0000 + N`、最大 16）、open ごとの ring 64、devfs は `/dev/input` に置き 0600、sessiond が seat の人に渡す。read・write・poll の意味は Linux の hidraw と同じ。`HIDRAW_GRAB` | `include/drivers/generic/hidraw.h` 8〜78、`src/drivers/generic/hidraw.c` 9〜19・130〜326 行、`include/uapi/hidraw.h` 88〜109・174〜181 行、`src/kern/devfs.c` 471〜485・509〜529 行、[ws161-p002](../../ws161/phase002/phase.md) 25〜26 行 |
| 7 | compositor は `/dev/input` を opendir して `eventN` を全部 probe し、capability で pointer・keyboard・pen・touch を分ける（bus は見ない） | `userland/desktop/libkeiland-backend-zedbsd/input-zedbsd.c` 23・35〜77 行、`userland/desktop/wayland/input.c` 8〜24 行 |
| 8 | compositor は `/dev/system` の INPUT の event で device の一覧を見直す（OVERFLOW でも） | `userland/desktop/libkeiland-backend-zedbsd/events-zedbsd.c` 40〜44・216〜221 行、`include/uapi/system.h` 305〜308 行 |
| 9 | `BUS_BLUETOOTH` は 0x05 | `include/uapi/input.h` 255 行 |
| 10 | hid-report.h の上限: descriptor 4096（`HID_REPORT_DESCRIPTOR_SIZE_MAX`）、collection の深さ 16、global の深さ 16、report ID 32、field 256、bits 8192。decode は layout が report ID を使う時は先頭 1 byte を ID と読み、使わなければ ID 0 の report として読む | `include/drivers/generic/hid-report.h` 10〜16 行、`src/drivers/generic/hid-report.c` 2400〜2416 行 |
| 11 | `sleep.end` の event は `KERN_SYSTEM_EVENT_POWER` の class に `sleep.end`（detail は理由）で出る | `src/kern/sleep.c` 171・181・200 行 |
| 12 | kernel の build: amd64 は `CONFIG_DRIVER_USB_HID` で usb-hid と hidraw、HID の parser・状態機械は USB HID・LPSS I2C・INPUT_TEST_INJECT のどれかで入る。usb-bt は `CONFIG_DRIVER_USB_BT`（既定 y）。loopback の controller は `CONFIG_BT_TEST_LOOPBACK`、`vfs.c` が `#ifdef BT_TEST_LOOPBACK` で登録。arm64 は USB の source に usb-hid と hid-report を持つ | `platform/amd64/vmunix.mk` 264〜271、277〜278、339〜348 行、`platform/arm64/vmunix.mk` 95 行、`Makefile` 231〜237・582・596〜604 行、`src/kern/vfs.c` 378〜410 行 |
| 13 | bluetoothd の session: handler は `btd_session_input` からだけ呼ばれ（command の中では呼ばれない）、ACL と接続の event（Connection Complete・Disconnection Complete・LE Connection Complete は数えてから）を handler に渡す。`btd_session_send(handle, cid, payload ≤ 1024)` は link が無いと ENOTCONN、frame の queue は 16。数える link は 8 | `userland/base/bluetoothd/session.h` 82〜88・45〜53・247 行、`session.c` 431〜475・632〜726・1647〜1657 行 |
| 14 | pair.c は **相手からの Connection Request を全部 Reject**（0x040A、理由 0x0F）、ACL は自分の handle 以外を `ignored`、Link Key Request は自分の address の時だけ答える。接続は `btd_pair_start`（LE Create Connection の parameter 254〜272 行、Create Connection 274〜286 行）。PAIR は接続・pairing・切断で終わる（Q5） | `userland/base/bluetoothd/pair.c` 538〜545、627〜676、191〜298、1487〜1519 行、[phase004](../phase004/phase.md) 判断 Q5 |
| 15 | l2cap.c は **相手からの Connection Request を PSM not supported で断り**、Configure Request の **hint でない知らない option（Flush Timeout 0x02・QoS 0x03・RFC 0x04 を含む）を Unknown option で断る**。channel の表は 16（`BTD_CHANNELS_MAX`）、MTU は 672、最小 48。表 `struct btd_l2cap` は持ち主ごとで、channel ごとに handle を持ち、`btd_l2cap_drop(handle)` で link の channel を捨てる。**pair は自分の `btd_l2cap` と `btd_reassembly` を持ち、pairing ごとに init し直す**。`struct btd_signal_effect` は今 `update`（LE の parameter）と `information` だけ | `userland/base/bluetoothd/l2cap.c` 400〜411、569〜583 行、`l2cap.h` 32〜36、51〜74、82〜91 行、`pair.h` 113〜114 行、`pair.c` 181・230 行 |
| 16 | 口: client は 8（`BTD_CLIENTS_MAX`）、request は 1 行（`BTD_LINE_MAX` 512）、`YES`/`NO`・SHOW・DEVICES・BONDS・AGENT・SCAN・PAIR・FORGET。D8 の判定は `btd_permitted`。PAIR の終わりは `btd_paired`（pair.c の callback）が client に `PAIRED …` と `DONE` を書く。**bluetoothd は今 `/dev/system` を開いていない**（`sleep.end` の購読は p003 が「p004 以降」とし、p004 もしていない） | `userland/base/bluetoothd/main.c` 71、694〜773、1214〜1232、1241〜1294 行、`protocol.h` 35〜36 行、`userland/base/bt/main.c` 8〜30 行、[phase003](../phase003/phase.md) 21 行 |
| 17 | privsep: 親（root）は datagram の `OPEN`／`OPEN /dev/bluetoothN` に fd（SCM_RIGHTS）で答え、`OPEN` 以外は知らない。子は `_bluetooth`（80）。bond の file は `/var/db/bluetooth/<controller>/<address>-<型>` の `key=value`（type・name・link_key・link_key_type・ltk・ediv・rand・key_size・authenticated・secure・legacy・irk・identity・identity_type。知らない key は読み飛ばす）。`struct btd_bond` は `name[BTD_NAME_MAX]`（249）・`key_size`・`legacy`・`irk` を持つ | `privsep.c` 374〜399 行、`privsep.h` 23 行、`keys.c` 11〜13、318〜364、383、590〜591 行、`keys.h` 37〜65 行、`hci.h` 29 行、[phase004](../phase004/phase.md) §3・§4 |
| 18 | loopback の controller: BR/EDR の相手 0A:0B:0C:0D:0E:01（DisplayYesNo、Inquiry の「Loopback Keyboard」class 0x002540）・05・06・07（Just Works）は handle 0x0040、LE の 03 は 0x0041（pairing を断る）。Inquiry Result with RSSI の 02 は class 0x002580（マウス）。ACL は handle ごとに Number Of Completed Packets、L2CAP の Information Request と SMP の Pairing Request にだけ答える。**`loopback_frame` の payload は 16 byte まで**（`body[4+4+16]`、超えたら EINVAL）。LE Create Connection（0x200D）は address が 03（`LOOPBACK_DEVICE_MOUSE`、LE の広告の名前は「Loopback Mouse」appearance 0x03C2）の public の時だけ LE Connection Complete（19 byte、interval 0x0018、timeout 0x01F4）を返し、他の address は繋がない。LE Create Connection Cancel は status 0x02 の LE Connection Complete。HCI Disconnect は理由 0x16 の Disconnection Complete | `src/drivers/generic/bt-hci-loopback.c` 8〜60、120〜160、889、964〜972、1120〜1404（1340〜1389）、1480〜1566（1535〜1546）行 |
| 19 | 試験の道具: `bt-probe`（node は daemon と同時に開けない: EBUSY）、`hidraw-probe`、`peninject -d`（inject の evdev の node を待って event を出す）、`systemevents`（`/dev/system` の event を行で出す）。guest の QEMU は `-device usb-kbd,bus=xhci.0,port=3` を既に持ち、`--qemu-extra` で足せる。QMP は `-qmp unix:<runtime>/qmp.sock,server,nowait` で開いている（`boot-test.sh` が screendump に使う同じ socket）が、guest.py に汎用の QMP の command を送る関数は無い。`aat-input`・`touchinject`・`peninject` は inject の口の書き手で、任意の `/dev/input/eventN` を bus・名前で選んで読む道具は無い（`evdev-probe` を新しく作る理由） | `userland/tests/bt-probe/main.c` 8〜40、`hidraw-probe/main.c`、`peninject/main.c` 8〜33、`systemevents/main.c` 8〜33、`aat-input/main.c` 8〜20 行、`plan/tools/guest/guest.py` 159・190・209・211・222・368 行、`plan/tools/boot-test.sh` 15 行 |
| 20 | host 試験の形: bluetoothd の部品は host の cc で ASan・UBSan（`plan/ws143/tests/bt-daemon-host-test.sh`）、kernel の HID の file は freestanding（`-ffreestanding -nostdlibinc -fno-builtin -D__ZEDBSD__ -DKERN_USER_ABI_LP64`）で compile して host の試験と link（`plan/ws079/tests/run-hid-pen.sh`、`plan/ws159/tests/run-host-i2c-hid.sh`）。5330 の touchpad の descriptor は `plan/ws159/tests/latitude5330-linux/synaptics-06cb-ce65-rdesc.bin` | 各 file |
| 21 | kernel に AES-128 の block の暗号化がある（`wlan_aes128_encrypt_block`）。試験の kernel の config で link されるかは**未確認** | `src/kern/net/wifi/wlan-crypto.h` 73〜75 行、`Makefile` 697 行 |
| 22 | cdev の rdev の割り当て（既存）: console `0x00010000`・`0x00010010+n`・tty `0x00010020`、ptmx `0x00010001`、system `0x00010002`、pty `0x00020000+n`、input `0x00030000+N`、gpu `0x00090000`、audio `0x000A0000`、acpi `0x000b0000`、backlight `0x000d0000`、input-inject `0x000e0000`、hidraw `0x000f0000+N`、**smartcard `0x00100000`**、typec `0x00110000`、bt-hci `0x00120000`。`0x00130000` は空いている | `src/drivers/generic/console.c` 1024・1034・1042、`src/kern/tty.c` 1262、`src/drivers/generic/system-device.c` 158、`src/kern/devfs.c` 862・935、`src/drivers/gpu/gpu.c` 45、`src/drivers/audio/audio.c` 31、`src/drivers/acpi/acpi-dev.c` 34、`src/drivers/generic/backlight.c` 33、`input-inject.c` 44、`hidraw.c` 39、`smartcard.c` 42、`src/drivers/typec/typec-kern.c` 41、`src/drivers/generic/bt-hci.c` 49 行 |
| 23 | ioctl の group の文字（既存）: 'A' audio、'S' ccid、't' tty、's' system、'B' blkid、'c' console、'b' bluetooth、'E' evdev、'g' graphics、'G' gpu（header に直書き）、'L' backlight、'H' hidraw、'f' FIONBIO（`fcntl.h:43`・`ioctl.h:43`）。'h' は未使用（review M1 で補った） | `include/uapi/*.h` の `*_IOC_GROUP` と `_IO*(` |
| 24 | `struct input_device` は header では opaque（`struct input_device;`）で、`number`（eventN の N）は input.c の中の field。外から番号を知る関数は無い | `include/kern/input-device.h` 12 行、`src/drivers/generic/input.c` 58・68 行 |
| 25 | SMP の Pairing Request の鍵の配り: initiator（自分）0、responder は EncKey と IdKey（p004 の記述どおり） | `userland/base/bluetoothd/smp.c` 155〜161 行 |
| 26 | **input device は system 全体で 8 つまで**（`INPUT_DEVICE_MAX 8U`、`input_device_reserved[]`）。満ちると register は ENOSPC。番号の slot は cdev の最後の参照が消えた時（`input_device_release`）に空く。PS/2 が 2 つ（keyboard・mouse）使う | `src/drivers/generic/input.c` 40・106・325〜335・2648 行、`src/drivers/platform/pcat/ps2-8042.c` 1211・1236 行。compositor の source の表は `KWL_INPUT_MAX 16`（`userland/desktop/wayland/kwl.h` 73 行、`input.c` 253・317 行） |
| 27 | bond の file の読みは **2048 byte 未満**（`KEYS_TEXT_MAX`、以上は EBADMSG）。`btd_keys_list` は読めない file を飛ばし、名前は「17 文字の address + `-` + 型」で、型が parse できない名前（例 `…-bredr.hid`）は飛ばす。`btd_keys_write` は `struct btd_bond` の field だけを書く | `userland/base/bluetoothd/keys.c` 31・177〜180・265〜288・318〜364 行 |
| 28 | controller が消えた・reset された時: `btd_close` は pair（`btd_pair_lost`）と scan の client だけを片付ける。session は reset の notice と Hardware Error で `BTD_STATE_ERROR` になり、main の loop が `btd_close` を呼ぶ。Disconnection Complete は来ない | `main.c` 310〜311・477〜492 行、`session.c` 642〜646・676〜680 行 |
| 29 | loopback は BR/EDR の相手を 1 つしか演じない: `loopback.device[6]`（最後に page した address）と handle 0x0040 が 1 組。Create Connection は address を書き換える | `bt-hci-loopback.c` 189〜204・1184〜1203 行 |
| 30 | session の LE event mask は `{0x87, 0x01}`（subevent 0x01〜0x03・0x08・0x09。**0x0A LE Enhanced Connection Complete は無い**）。`session_counted_event` は subevent 0x01 だけを数える | `session.c` 1288・1686〜1696 行 |
| 31 | syscall の write の分割: 512 byte ずつ `file_io_transfer` を呼び、**error か短い書きで止まる**（最初の chunk が EINVAL なら残りは書かれない）。hid-report の decode は payload が最小の長さ**以上**なら受ける（長い report の余りは読まない） | `src/kern/syscall.c` 85・2995〜3040 行、`hid-report.c` 2424〜2427 行 |
| 32 | parser は EOPNOTSUPP も返す（対応しない item）。usb-hid の touch の名前は `"%s Touchscreen"` を 64 byte の buffer に snprintf（切れる） | `hid-report.c` 383・1109・1165 行、`usb-hid.c` 85・891〜897 行 |
| 33 | sessiond は seat を与える時に `/dev/input` の今ある `event*` を seat の人の 0600 に chown し、戻す時に root:wheel の 0640 にする。hotplug（`/dev/system` の INPUT）は聞いていなかったが、greeter と session の loop で**毎秒**与え直す（第 1 版はこれを見落とした。BUG-264 で event の購読を足した）。devfs は chown・chmod を rdev ごとに記憶し、同じ番号の node が作り直されても保つ。compositor は user で動き、2 秒ごとと INPUT の event で走査する | `userland/desktop/sessiond/seat.c`、`session.c` の待ちの loop、`greeter.c` の `GREETER_SEAT_MS`、`src/kern/devfs.c` 560〜590 行、`userland/desktop/wayland/main.c` の `KWL_INPUT_SCAN_MS`、[BUG-264](../../bugs/BUG-264.md) |
| 34 | p003・p004 の QEMU の試験の期待値: `BT SCAN devices=4`（p003 47 行、p004 82 行）、01 の pairing の直後に 01 を再 pairing（`stored=1`、CONFIRM 無し）、PAIRED の行に `l2cap=1`、その後 07・05・06 の pairing と FORGET、`bt show` が `ready`。p003 の withdraw の試験は `bt-probe -W` を daemon の前に打つ | `plan/ws143/tests/bt-daemon-p003.sh` 45〜48 行、`bt-pair-p004.sh` 44〜82 行、[phase003](../phase003/phase.md) 174 行 |
| 35 | pcat も `CONFIG_DRIVER_USB_HID` で usb-hid.o・hidraw・hid-report などを link し、`CONFIG_DRIVER_USB_BT`（Makefile の既定 y）で usb-bt.o を link する。`hidraw-describe.c` は amd64 では USB_HID の時だけ | `platform/pcat/vmunix.mk` 113〜120 行、`platform/amd64/vmunix.mk` 264 行、`Makefile` 228〜231 行 |
| 36 | amd64 の syscall の stack は 16 KiB。`struct hid_report_input` は `values[256]` × 8 byte ≈ 2 KiB。input-inject は状態機械と report を open の状態に置き stack に置かない | `src/hal/amd64/task.h` 20 行、`include/drivers/generic/hid-report.h` 59〜64 行、`input-inject.c` 59〜79 行 |
| 37 | host（この開発機）に `tshark` は無い（`which tshark` 空）。python3 はある | shell |
| 38 | pair の口: `btd_pair_init(pair, session, folder, ask, done, context, random, …)`、`pair_finish` が HCI Disconnect（理由 0x13）を送って PAIR を終える。`btd_pair_lost`・`btd_pair_stop`・`btd_pair_active` がある | `pair.h` 54〜130 行、`pair.c` 1487〜1519 行 |

## 詳細設計

### 1. 構成（file の表）

| file | 新規／変更 | 中身 | host の試験 |
| --- | --- | --- | --- |
| `include/drivers/generic/hid-input.h`、`src/drivers/generic/hid-input.c` | 新規 | **共有の HID glue**（§2）: report descriptor から layout・capability・axis・pen・touch の記述を作り、input device を登録し、report を decode して evdev に出す。transport（USB・input bridge）を知らない | する（freestanding、input の層の stand-in） |
| `src/drivers/usb/usb-hid.c` | 変更 | glue を呼ぶ。transport（descriptor の取得、SET_PROTOCOL、URB、worker、raw の hidraw）と名前の決めは残す。動かす code は規約の全文に合わせる（design §5.2 [N16]） | USB の回帰は build と QEMU（§7） |
| `include/uapi/input-bridge.h` | 新規 | `/dev/input/bridge` の UAPI（§3。D3 の形、struct はこの Phase の review） | — |
| `include/drivers/generic/input-bridge.h`、`src/drivers/generic/input-bridge.c` | 新規 | `/dev/input/bridge` の cdev（§3） | する（setup の検査の純粋な部分だけ） |
| `src/kern/devfs.c` | 変更 | `bridge` を 0600 に（`input-inject` と同じ行に足す） | — |
| `src/drivers/generic/input.c`、`include/kern/input-device.h` | 変更 | `INPUT_DEVICE_MAX` 8 → 32（§9.1、B1）、`drv_input_device_number()`（事実 24） | 既存の host 試験が無い: review と QEMU |
| `src/kern/vfs.c`、`Makefile`、`platform/amd64/vmunix.mk`、`platform/pcat/vmunix.mk` | 変更 | `CONFIG_INPUT_BRIDGE`（Makefile の既定 y、amd64 だけが見る。§9.10 S10(e)）で input bridge の登録と `hidraw-describe.c`、hid-input.c を amd64 の HID の source と pcat の USB の object に足す | build（amd64・pcat・arm64・試験の config） |
| `userland/base/bluetoothd/session.[ch]` | 変更 | LE event mask に subevent 0x0A、`session_counted_event` が LE Enhanced Connection Complete も数える（§9.4 S4） | する（既存の試験の更新） |
| `userland/base/bluetoothd/hidcache.[ch]` | 新規 | HID の device の記録（descriptor の cache・flags・PnP・名前・`candidate`/`confirmed`）を bond と**別の file** `<address>-<型>.hid` に読み書き（§9.3 B4） | する |
| `userland/base/bluetoothd/snoop.[ch]` | 新規 | btsnoop の記録（純粋: header と record の形。`-s PATH` で全 HCI の packet を書く。§9.11 S11） | する（file の形） |
| `plan/ws143/tests/hid-input-host-test.{sh,c}` | 新規 | glue の host 試験（§7） | — |
| `plan/ws143/tests/hid-report-fuzz.{sh,c}` | 新規 | hid-report.c・hid-touch.c・hid-digitizer.c・hid-input.c の fuzz（§7） | — |
| `userland/tests/evdev-probe/` | 新規 | guest で `/dev/input/eventN` を bus・名前で待ち、event を行で出す（§7） | — |
| `userland/tests/input-bridge-probe/` | 新規 | root で `/dev/input/bridge` にキーボードを作り report を書く（kernel だけの QEMU の確かめと、口の誤用の確かめ） | — |
| `userland/base/bluetoothd/router.[ch]` | 新規 | session の handler。接続の event と ACL を pair か HID の link に配る（§4.1） | する |
| `userland/base/bluetoothd/l2cap.[ch]` | 変更 | 相手からの Connection Request を policy で受ける、inbound の channel の表、Flush Timeout・QoS・RFC の option（§4.2） | する |
| `userland/base/bluetoothd/sdp.[ch]` | 新規 | 純粋: SDP client の PDU（ServiceSearchAttributeRequest の組み立て、Response の continuation と data element の解析、HID の record の属性）（§4.3） | する（台本と fuzz） |
| `userland/base/bluetoothd/hidp.[ch]` | 新規 | 純粋: HIDP の header（§4.4） | する |
| `userland/base/bluetoothd/att.[ch]` | 新規 | 純粋: ATT の PDU の組み立てと解析（§4.5） | する（台本と fuzz） |
| `userland/base/bluetoothd/hog.[ch]` | 新規 | HOGP の発見の状態機械（ATT の上。system call 無し、呼び手が送る）（§4.6） | する（台本の ATT server） |
| `userland/base/bluetoothd/hid.[ch]` | 新規 | HID host の核: device の表、BR/EDR と LE の接続の流れ、security の検査、`/dev/input/bridge` への橋、再接続の policy と timer（§4.7〜4.9） | する（偽の controller、偽の input bridge の fd） |
| `userland/base/bluetoothd/privsep.[ch]` | 変更 | 子の `OPEN-HID` に親が `/dev/input/bridge` を開けて fd で答える（§4.10） | しない（QEMU） |
| `userland/base/bluetoothd/keys.[ch]` | 変更**しない**（改めた） | B4: bond の file は 2048 byte 未満、`btd_keys_write` は struct の field だけ。HID の記録は `hidcache.[ch]` の別 file。FORGET が `.hid` も消す呼びは main.c | — |
| `userland/base/bluetoothd/pair.[ch]` | 変更 | Connection Request の Reject と他 address の Negative Reply を router に移す。**PAIRED の時に link を hid に引き継ぐ callback `handoff`**（切らずに同じ link で HID を始める。§9.2 B2） | する（既存の試験の更新） |
| `userland/base/bluetoothd/main.c`、`protocol.h` | 変更 | CONNECT・DISCONNECT・STATUS、SHOW の `hid=`、`/dev/system` の POWER の購読、timer（§5） | しない（QEMU） |
| `userland/base/bt/main.c` | 変更 | `bt connect`・`bt disconnect`・`bt status` | しない（QEMU） |
| `src/drivers/generic/bt-hci-loopback.c` | 変更（**大きい**: link ごとの状態への作り直し、§9.7 B7・§6） | HID の相手: 01 の SDP・HIDP のキーボード、02 の HIDP のマウス（相手から再接続、page scan の bit を見る、host の page は断る）、04 の LE の HOGP のマウス（ATT server、Exchange MTU Request を自分から、LTK の照合）。link ごとの `struct loopback_link`（address・handle・暗号化・L2CAP の channel・timer）。frame の payload の上限を広げる。時刻で動く送りと command の経路の排他（§6） | — |
| `plan/ws143/tests/bt-input bridge-test.{sh,c}`、`bt-hid-p005.sh`、`hid-usb-p005.sh`、`input-bridge-p005.sh`、`config-amd64-bt.mk`・`build-bt-image.sh` | 新規／変更 | §7 | — |

### 2. kernel: 共有の HID glue `hid-input.c`（refactor の範囲と USB の回帰の危険）

**何を動かすか（usb-hid.c から）**: 事実 1 の glue、すなわち layout の parse から記述（capability・axis・pen・touch）を作る部分（653〜797 行の parse 以降）、report の公開（1012〜1167 行）、pen・touch の公開（1588〜1653 行）、register と unregister（1427〜1559 行の `info` の組み立てと「capability が SYN だけなら作らない・touch は別 device」の規則、1187〜1221 行）。**usb-hid.c に残す物**: USB の transport の全て（事実 2）、FIDO の raw の判定と hidraw（事実 2・6）、名前・path・unique_id の決め（852〜915 行。capability からの名前の fallback は glue の `drv_hid_input_kind()` を使って usb-hid が付ける）、`usb_hid_input_is_ready` の pending の列。

**glue の口**（`include/drivers/generic/hid-input.h`。struct の中身は header に置かず `hid-input.c` の中で持つ）:

```c
struct hid_input;   /* opaque: layout、capability、axis、report ごとの held、pen・touch の状態、input device の参照 */

/* 1 つの device の識別。文字列は 63 byte + NUL まで（input.c の INPUT_TEXT_MAX）。touch_* は touch の device の名前と place（NULL なら "<name> Touchscreen"・"<path>/touch"）。 */
struct hid_input_identity {
	const char *name;
	const char *physical_path;
	const char *unique_id;
	const char *touch_name;
	const char *touch_physical_path;
	struct input_id id;
};

/* 何の device か（名前の fallback のため）: HID_INPUT_KIND_KEYBOARD・MOUSE・TABLET・PEN・TOUCH_ONLY。 */
int drv_hid_input_prepare(const void *descriptor, size_t size, struct hid_input **result);   /* parse と記述。失敗は parser の errno（EINVAL・E2BIG・ENOMEM）か EINVAL（report が 0・上限超え）。register はしない */
size_t drv_hid_input_report_max(const struct hid_input *);   /* 最も長い input report（byte。ID の byte を含む）。usb-hid の buffer の大きさ、input bridge の write の検査 */
unsigned drv_hid_input_kind(const struct hid_input *);
int drv_hid_input_publish(struct hid_input *, const struct hid_input_identity *);   /* input device（と touch の device）を登録。何も作れなければ ENODEV */
void drv_hid_input_report(struct hid_input *, const uint8_t *report, size_t length, uint64_t milliseconds);   /* decode して evdev へ。壊れた report は数えて捨てる（kern_logf は最初の 16 回） */
unsigned drv_hid_input_malformed(const struct hid_input *);
int drv_hid_input_numbers(const struct hid_input *, int *event, int *touch_event);   /* 登録した eventN の番号（-1: 無い）。struct input_device は opaque（事実 24）なので input.c に unsigned drv_input_device_number(const struct input_device *) を足す */
void drv_hid_input_unpublish(struct hid_input *);   /* unregister（input.c が押されている key を離す、事実 4） */
void drv_hid_input_destroy(struct hid_input *);     /* layout と記憶を解放（unpublish の後） */
```

- `prepare` と `publish` を分けるのは、usb-hid が attach で parse し（buffer の大きさが要る）、input の層の準備の後（`drv_usb_hid_input_ready`）に登録するから。input bridge は setup の write で両方を続けて呼ぶ。
- **振る舞いは変えない**（規約「Style-only changes must preserve evaluation order, ownership, lifetime, error reporting, and observable behavior」）: report ID ごとの held と全 report の集約、`keyboard_error` の時は前の key の状態を保つ、EV_REL の 0 を出さない、EV_KEY 以外の値はそのまま、出した時だけ SYN_REPORT、touch と pen の振り分けの順（touch → pen → 普通）、「capability が SYN だけなら device を作らない」「何も作れなければ ENODEV」。これらを host の試験（§7.1）で、新旧の出力の一致として確かめる。
- 時刻: `drv_hid_input_report` は呼び手の時刻（usb-hid は URB の完了の時刻 `completed_milliseconds`、input bridge は write の時刻 `clock_milliseconds`）を取る（`drv_input_device_emit_at`、`input-device.h` 101〜107 行の約束）。
- 作業領域（review S8）: decode の出力 `struct hid_report_input`（約 2 KiB）、touch の状態機械の出力、report ID ごとの held と集約の bit map、identity の文字列の写しは **`struct hid_input` の中に持ち、stack に置かない**（syscall の stack 16 KiB、事実 36。input-inject と同じ形）。usb-hid の worker の stack に今ある `decoded` も glue の中へ移る。
- 新旧の比較（review S10(d)）: §7.1 の host 試験は emit の列だけでなく、**register に渡す `input_device_info`（capability の列、axis、properties、名前・path・unique_id、flags）**も新旧で byte 単位に比べる。
- lock: glue は自分の lock を持たない。呼び手が report と unpublish を直列にする（usb-hid は worker の thread 1 本、detach は worker の join の後に unpublish（438〜483 行）。input bridge は open ごとの mutex の下で write、close は最後の write の後（§3））。
- 配置（review S10(e) で改めた）: `src/drivers/generic/hid-input.c` を amd64 の `AMD64_HID_SOURCES`（`vmunix.mk` 268〜271 行）、pcat の `PCAT_USB_CLASS_OBJS`（`platform/pcat/vmunix.mk` 114 行、usb-hid.o と同じ条件）、arm64 の `ARM64_USB_SOURCES`（95 行）に足す（usb-hid が呼ぶ所は全部）。input bridge は **新しい `CONFIG_INPUT_BRIDGE`**（Makefile の既定 y、`-DCONFIG_INPUT_BRIDGE`）で、`platform/amd64/vmunix.mk` だけが見る: `CONFIG_INPUT_BRIDGE=y` なら `input-bridge.c` と `hidraw-describe.c`（USB_HID が n の時も）と HID の source を入れ、`vfs.c` の `#ifdef CONFIG_INPUT_BRIDGE` で登録。pcat・arm64 は参照しないので link は崩れない（`CONFIG_DRIVER_USB_BT` を条件にしない: pcat でも既定 y で、pcat には input bridge を入れない。§Q24）。rpi4 の Bluetooth（UART）は範囲の外。
- FIDO の raw の分岐（review S10(a)）: usb-hid.c の raw の経路（`fetch_layout` 689〜703、`activate` 1476〜1480、`unpublish` 1203〜1205、`publish_report` 1053〜1057 行）は**書き換えない**（規約への合わせもしない。diff でその行が変わっていないことを review で確かめる）。確かめは §9.10。
- **USB の回帰の危険と確かめ方**: 危険は (a) key の集約の意味が変わる、(b) touch・pen の経路が変わる、(c) attach・detach の順（register が input ready の前に起きる、unpublish が 2 回呼ばれる）、(d) 名前の fallback が変わる。確かめ: (a)(b)(d) は §7.1 の host 試験で新旧の出力の byte 単位の一致（旧の usb_hid_publish_report を試験に写して並べて走らせる。写しは試験の中だけ）、(c) は code の review と QEMU（`hid-usb-p005.sh`: guest の usb-kbd に QMP の `send-key`、usb-tablet か usb-mouse を `--qemu-extra` で足して `input-send-event`。`evdev-probe -b usb` で event を読む。bus=3 の node が USB の数だけある）。実機（5330 に USB のキーボードを挿す）は p008 か T1 の空き（任意）。QEMU の規約「回帰試験では GPU を使わず framebuffer で login prompt だけ」は boot test の話で、この試験は guest の中の evdev の読みで判定する（console log は読まない）。

### 3. kernel: `/dev/input/bridge`（D3 の形、struct の配置）

**UAPI `include/uapi/input-bridge.h`**（D3 で承認した形: 最初の write が作成、続く write が input report、read が output report、close で消す。**形への追加は `INPUT_BRIDGE_GET_DEVICE` の ioctl 1 つ**（§Q2、ユーザーの判断））:

```c
#define KERN_INPUT_BRIDGE_IOC_GROUP	'h'          /* 未使用（事実 23: 'H' hidraw、'b' bluetooth、'E' evdev、's' system、'S' ccid …） */
#define INPUT_BRIDGE_MAGIC		0x74686968U  /* "hiht" */
#define INPUT_BRIDGE_VERSION	1U
#define INPUT_BRIDGE_DESCRIPTOR_MAX	4096U        /* hid-report.h の HID_REPORT_DESCRIPTOR_SIZE_MAX */
#define INPUT_BRIDGE_REPORT_MAX	512U         /* 1 回の write の上限 = syscall の bounce の chunk（SYSCALL_IO_CHUNK）。review S9: 分割が起きない長さに（§9.9、Q22）。ID の byte を含む */
#define INPUT_BRIDGE_TEXT_MAX	64U          /* input.c の INPUT_TEXT_MAX */
#define INPUT_BRIDGE_OPENS_MAX	6U           /* 同時の device の数 = bluetoothd の BTD_HID_MAX。touch を持つ device は input の slot を 2 つ使うので最大 12 slot（INPUT_DEVICE_MAX 32 の中。review B1、§9.1） */

/* 最初の write: device の宣言。write の長さは sizeof ちょうど（4324 byte: 36 + 3×64 + 4096。field は自然に並び padding は無い。input-bridge.c と host 試験に _Static_assert(sizeof(struct input_bridge_setup) == 4324U) を置く）。 */
struct input_bridge_setup {
	uint32_t magic;
	uint32_t version;		/* INPUT_BRIDGE_VERSION */
	uint16_t bus;			/* BUS_BLUETOOTH か BUS_VIRTUAL だけ（他は EINVAL。USB を名乗れない） */
	uint16_t vendor;
	uint16_t product;
	uint16_t release;		/* input_id.version */
	uint32_t descriptor_size;	/* 1..INPUT_BRIDGE_DESCRIPTOR_MAX */
	uint32_t reserved[4];		/* 0 */
	char name[INPUT_BRIDGE_TEXT_MAX];		/* NUL で終わる。空なら kernel が種類から付ける（"Bluetooth HID keyboard" など） */
	char physical_path[INPUT_BRIDGE_TEXT_MAX];	/* 例 "bluetooth/00:11:22:33:44:55/0A:0B:0C:0D:0E:01" */
	char unique_id[INPUT_BRIDGE_TEXT_MAX];	/* 相手の address（再接続でも同じ、design §5.2） */
	uint8_t descriptor[INPUT_BRIDGE_DESCRIPTOR_MAX];
};

/* INPUT_BRIDGE_GET_DEVICE: 作った device の番号（/dev/input/eventN の N、-1 は無い）、layout が断った report の数、layout の性質（review M8: daemon が Q12 の推定を kernel と照合する）。open の mutex の下で読む。 */
struct input_bridge_device {
	int32_t event;
	int32_t touch_event;
	uint32_t malformed;
	uint32_t flags;			/* INPUT_BRIDGE_FLAG_REPORT_IDS: layout が report ID を使う */
	uint32_t report_max;		/* 最も長い input report（ID の byte を含む） */
	uint32_t reserved[3];
};
#define INPUT_BRIDGE_FLAG_REPORT_IDS	0x1U
#define INPUT_BRIDGE_GET_DEVICE	_IOR(KERN_INPUT_BRIDGE_IOC_GROUP, 0, struct input_bridge_device)
```

**file の操作**（`src/drivers/generic/input-bridge.c`、cdev 名 `bridge`、rdev は **`0x00130000`**（事実 22 の表の次の空き。第 1 版の `0x00100000` は smartcard と衝突していた）、`cdev_register("input bridge", …)` は `input-inject` と同じ形）:

- open: **root だけ**（`cred_is_superuser`。devfs の 0600 とは別に、chmod で広げられないため。input-inject と同じ）。同時に `INPUT_BRIDGE_OPENS_MAX` まで、1 open = 1 device。open の状態は `struct input_bridge_open { struct mutex lock; struct hid_input *input; unsigned declared; uint64_t reports; }`。超えたら EBUSY。
- write（最初）: 長さが `sizeof(struct input_bridge_setup)` でなければ EINVAL。magic・version・bus・descriptor_size・reserved・文字列の NUL（`strnlen < 64`）を検査（純粋な `input_bridge_setup_valid()`、host で試験）。top の usage が FIDO（`drv_hidraw_describe` で見る）なら **ENXIO**（§Q3。review M2: parser の EOPNOTSUPP と分ける）。`drv_hid_input_prepare` → `drv_hid_input_publish`（identity: setup の文字列、`id.bustype = bus`）。失敗はその errno（parser の EINVAL・E2BIG・ENOMEM・**EOPNOTSUPP**（対応しない item、事実 32）、何も作れない ENODEV、**input の slot が無い ENOSPC**（B1、§9.1））。publish が失敗したら prepare した layout を destroy し、open は宣言の前の状態に戻る（やり直せる。review M9）。成功は size。**2 度目の setup は無い**（宣言の後の write は全部 report）。touch の device の名前は glue が `"<name> Touchscreen"` を 63 byte に切る（usb-hid と同じ snprintf）。切れ目で UTF-8 が割れないよう **daemon は名前を 51 byte（63 − 12）の UTF-8 の境で切る**（review M3、§Q16）。
- write（続き）: 1 回の write は 1 つの input report（layout が report ID を使うなら先頭の byte が ID。hidraw の read と同じ約束）。長さ 0 か `INPUT_BRIDGE_REPORT_MAX`（512）超は EINVAL。**その report ID の宣言の長さより短い物は EINVAL**（分割の断片を decode しない、review S9）、**長い物は余りを読まずに受ける**（padding を付ける device、review M7。decode は最小の長さ以上を受ける、事実 31）。decode に失敗した report（知らない ID、壊れた値）は捨てて数える（`malformed`。write は size を返す。壊れた report で daemon を止めない）。宣言の前の report は EINVAL。
- read: 出力の report の経路が無いので EAGAIN（O_NONBLOCK の有無に関わらず。寝ない。範囲の外）。将来 LED を足す時は「read は 1 つの output report（ID の byte が先頭）」の形で、poll の POLLIN を足す。
- poll: POLLOUT 常に（宣言の後も前も write できる）、POLLIN は言わない、POLLHUP は無い（device を消すのは自分の close だけ）。
- ioctl: `INPUT_BRIDGE_GET_DEVICE`（宣言の前は event = -1）。他は ENOTTY。
- close: `drv_hid_input_unpublish`（input.c が押されている key を全部離す、事実 4）→ `destroy` → open の枚数を返す。**切断で key が残らない**（design の受け入れ 3）は、daemon が切断で fd を close することで満たす（§4.7）。
- 境界と寿命: setup は open の中で 1 回、device は open と同じ寿命（daemon が落ちれば kernel が fd を close して device が消え、key が離れる）。write は open の mutex で直列、close は file の最後の参照で呼ばれるので write と重ならない。report の decode は write の process の文脈で走る（sleep しない、spin lock は input.c の中だけ）。
- 権限と privsep（D16 (a)）: node は root の 0600（devfs の規則に `bridge` を足す）。bluetoothd の **特権の親が開けて SCM_RIGHTS で子に渡す**（§4.10）。子（`_bluetooth`）は open しない。乗っ取られた子が任意のキーボードを作れる危険は design §5.2 [N9] のまま（防ぐのは電波の相手の解析の上限と、bond 済み・暗号化・鍵の長さ 16 の link だけを受けること。§4.8）。
- 名前: Bluetooth の名前（248 byte まで、`BTD_NAME_MAX 249`）は **daemon が UTF-8 の境で 51 byte に切る**（touch の device の `"<name> Touchscreen"` が 63 に収まる長さ。review M3。kernel は 64 以上を EINVAL で断るだけ。切る規則を kernel に持たせない）。
- syscall の bounce（p002 §2 の注）: 512 byte を超える write は heap の buffer を使い、取れなければ 512 byte の stack に落ちて write が分割される（`src/kern/syscall.c` 85 行 `SYSCALL_IO_CHUNK 512`。chunk の write が error なら残りは書かれない、事実 31）。**report の write は 512 以下なので分割されない**（`INPUT_BRIDGE_REPORT_MAX`、Q22）。setup（4324 byte）は分割され得る: 先頭の 512 byte は `sizeof` でないので EINVAL、残りは書かれない（黙って 2 つに decode される経路は無い、review S9）。daemon は setup の EINVAL を timer で 100 ms おきに 3 回やり直す（§4.7、review M10）。
- 試験の口 `/dev/input-inject` との違い: inject は固定の形の device だけ、input bridge は descriptor から作る。inject は試験の kernel だけ、input bridge は製品。

### 4. bluetoothd

#### 4.1 router（session の handler の分配）

今は `btd_session.handler = btd_pair_handle` で、pair が自分の物でない packet を捨てるか断る（事実 14）。HID の link が増えるので、handler を `btd_router_handle` にし、router が配る:

- 表: `handle → owner`（owner は pair か `hid[i]`）。Connection Complete・LE Connection Complete で、address が pair の相手なら pair、bond 済みの HID の address か HID が始めた接続なら hid、どれでもなければ **その接続を切る**（pair.c の `pair_disconnect_other` の役を router に移す）。Disconnection Complete で表から消す（session の `session_link_remove` と同じ順: router は session の後に呼ばれるので、同じ handle の次の Connection Complete と取り違えない）。
- address で届く event（**review B6 で改めた**。§9.6）: **pairing の event（IO Capability Request・Response、User Confirmation Request、User Passkey Request・Notification、PIN Code Request、Simple Pairing Complete、Link Key Notification）は pair がその address の pairing を進めている時だけ pair へ。それ以外は address が bond 済みの HID でも router が断る**: IO Capability Request → Negative Reply（0x18 pairing not allowed）、User Confirmation Request → Negative Reply、User Passkey Request → Negative Reply、PIN Code Request → Negative Reply、Link Key Notification → **保存しない**（記録して数える）。hid は pairing の event を受けない（pairing の mode の外の再 pairing と鍵の上書きを防ぐ、design §6.5）。**Connection Request と Link Key Request だけ**は、bond 済みの HID（`.hid` の記録がある、§9.3）の address なら hid へ（Accept と保存した鍵の Reply。相手からの再接続に要る）、pair の相手なら pair、他は Reject 0x0F・Negative Reply。pair.c の 538〜545 行の Reject と「他 address の Negative Reply」はここへ移す。
- ACL: handle の owner へ。owner が無ければ `ignored`。
- Encryption Change・Encryption Key Refresh Complete・Authentication Complete（handle 付き）: handle の owner へ。
- LE Meta: LE Connection Complete は上の規則、LE Read Local P-256 Public Key Complete・LE Generate DHKey Complete は pair、Advertising Report は session（scan）。LE Connection Update Complete は owner。
- pair.c の変更は「Connection Request を見ない」「他 address の Negative Reply をしない」の 2 点と、Connection Complete で自分の相手でない時に切らない（router がする）。host 試験の bt-link-host-test の「相手から始まる pairing の拒否」は router の試験に移す。

#### 4.2 L2CAP の拡張（inbound の channel と option）

- 相手からの Connection Request: `btd_l2cap` に policy の callback `int (*accept)(void *context, uint16_t handle, uint16_t psm, uint16_t *result, uint16_t *status)` を持たせる。無ければ今の PSM not supported。HID の link は hid.c が渡す: PSM 0x0011・0x0013 で、link が暗号化済み・鍵 16・bond 済み・HID の状態が「channel を待つ」なら 0（成功）、**暗号化の前なら Pending（result 0x0001、status 0x0000）を返し、hid.c が自分から Authentication Requested → Set Connection Encryption を始め、Encryption Change（on）と Read Encryption Key Size（16）の後に最終の Connection Response（成功）を送る。暗号化が失敗・10 秒で終わらなければ Security Block（0x0003）**（review S1 で改めた。§9.8）、他は PSM not supported（0x0002）。受けた channel は表に入れ（remote CID、local CID を割り当て、Pending の間は `PENDING` の状態）、成功の Response に続けて自分の Configure Request（MTU 672）を送る。相手の Configure Request は今の `signal_configure_request` で受け、両方の done で OPEN。表が満ちれば **No resources（0x0004）**。
- option: Flush Timeout（0x02、2 byte）は値を記録して受ける（controller の flush の設定は変えない。HID 1.1.1 は interrupt channel に flush timeout を推奨するが、受けるだけで動く。**未確認**: 相手が自分の側の flush の実施を期待するか）。QoS（0x03、22 byte）は **service type を問わず記録して受ける**（Guaranteed も。こちらは QoS を実施せず、Unacceptable で相手が接続をあきらめる恐れの方が大きい。review S7、§Q10）。Retransmission and Flow Control（0x04、9 byte）は mode が Basic（0x00）なら受け、他は Unacceptable で Basic を返す。FCS（0x05）は受ける（Basic mode では意味が無い）。知らない hint でない option は今のまま Unknown。`signal_configure_request` の `response[16]` は Unacceptable の答え（RFC 6 + 11 byte）が入るよう **48** に広げる（review M11）。p004 の S10 の残り。
- channel の open・close の通知: `btd_signal_effect` に `opened`（local CID）と `closed`（local CID、理由）を足し、hid.c が channel の状態の変化を知る（今は表を見るだけで通知が無い）。
- channel の表は持ち主ごと（`struct btd_l2cap` と `struct btd_reassembly` を HID の device ごとに 1 つ。pair は今も自分の物を持ち pairing ごとに init し直す、事実 15）。1 link の channel は SDP 1 + control 1 + interrupt 1 の 3 つ（SDP は HID の channel の前に閉じるので同時は 2〜3）。選ばなかった案: daemon で 1 つの表を pair と hid が共有する → 6 device × 3 = 18 が `BTD_CHANNELS_MAX` 16 を超えて上限を上げる変更が要り、`next_identifier` と Information Request の状態も link をまたいで混ざる。持ち主ごとなら既存の API（handle 付きの `btd_l2cap_signal`・`btd_l2cap_connect`・`btd_l2cap_drop`）をそのまま使える（§Q18）。

#### 4.3 SDP client（`sdp.[ch]`、純粋）

- 役: HID の service record から、HID の report descriptor、HIDReconnectInitiate、HIDNormallyConnectable、HIDVirtualCable、HIDBootDevice、HIDCountryCode、PnP（Device ID の record: VendorID・ProductID・Version）を読む。PSM は HID の規約の固定値（control 0x0011、interrupt 0x0013。ProtocolDescriptorList も読むが固定値と違えば `no-hid`）。
- PDU: ServiceSearchAttributeRequest（PDU ID 0x06）、Response（0x07）、ErrorResponse（0x01）。header は PDU ID 1 byte・TransactionID 2 byte・ParameterLength 2 byte（big-endian。SDP は big-endian、HCI・L2CAP は little-endian）。Request の parameter: ServiceSearchPattern（data element sequence の UUID 0x1124（HID）か 0x1200（PnP Information））、MaximumAttributeByteCount（送る値は **0x0280 = 640**: header 5 + count 2 + 640 + continuation ≤ 17 = 664 が自分の MTU 672 の 1 frame に収まる。review M4 で 1008 から直した。相手は小さく返してよい）、AttributeIDList（data element sequence の uint32 の range 0x0000FFFF = 全属性。全部読んで要る物を選ぶ。record は小さい）、ContinuationState（1 byte の長さ + 0〜16 byte）。Response: AttributeListsByteCount 2 byte、AttributeLists の断片、ContinuationState。断片を繋いで（上限 `BTD_SDP_MAX` 8 KiB）、ContinuationState の長さが 0 になるまで Request を繰り返す（同じ continuation を返し続ける相手は 8 回で `protocol`）。**未確認**: PDU ID・parameter の並び・MaximumAttributeByteCount の下限（Core 5.4 Vol 3 Part B §4.7.1・§4.7.2 と照合）。
- data element: 先頭 1 byte の type（上位 5 bit）と size index（下位 3 bit）、size index 5・6・7 は続く 1・2・4 byte が長さ。型: nil 0、uint 1、sint 2、UUID 3、text 4、bool 5、sequence 6、alternative 7、URL 8。解析は長さの検査、入れ子の深さ 8 まで、1 要素の長さが残りを超えたら `malformed`。属性は「uint16 の ID、値」の組の並び（1 record = 1 sequence、AttributeLists は record の sequence の sequence）。
- HID の属性の ID（**未確認**: HID 1.1.1 §5.3.4 と照合。Bluetooth SIG の Assigned Numbers の HID の service の attribute）: ServiceClassIDList 0x0001、ProtocolDescriptorList 0x0004、AdditionalProtocolDescriptorLists 0x000D（interrupt の PSM）、HIDDeviceSubclass 0x0202、HIDCountryCode 0x0203、HIDVirtualCable 0x0204、HIDReconnectInitiate 0x0205、HIDDescriptorList 0x0206（sequence の sequence: {uint8 の class descriptor type 0x22、text の descriptor の byte 列}）、HIDBatteryPower 0x0209、HIDRemoteWake 0x020A、HIDNormallyConnectable 0x020D、HIDBootDevice 0x020E。PnP Information（0x1200）の VendorID 0x0201、ProductID 0x0202、Version 0x0203、VendorIDSource 0x0205（**未確認**: Device ID Profile 1.3 と照合）。
- 口: `btd_sdp_request(struct btd_sdp *, uint16_t uuid, uint8_t *out, size_t size, size_t *length)`、`btd_sdp_input(struct btd_sdp *, const uint8_t *pdu, size_t length)` → `BTD_SDP_MORE`（次の request を送る）・`BTD_SDP_DONE`・`BTD_SDP_FAILED`、`btd_sdp_hid(const struct btd_sdp *, struct btd_hid_record *)`（descriptor は `btd_sdp` の buffer を指す。4096 超は `descriptor`）。

#### 4.4 HIDP（`hidp.[ch]`、純粋）

- header 1 byte: 上位 4 bit が message type、下位 4 bit が parameter。type: HANDSHAKE 0x0、HID_CONTROL 0x1、GET_REPORT 0x4、SET_REPORT 0x5、GET_PROTOCOL 0x6、SET_PROTOCOL 0x7、DATA 0xA。HID_CONTROL の parameter: SUSPEND 0x3、EXIT_SUSPEND 0x4、VIRTUAL_CABLE_UNPLUG 0x5。DATA の parameter（report type）: OTHER 0、INPUT 1、OUTPUT 2、FEATURE 3。SET_PROTOCOL の parameter: Boot 0、Report 1。HANDSHAKE の result: SUCCESSFUL 0、NOT_READY 1、ERR_INVALID_REPORT_ID 2、ERR_UNSUPPORTED_REQUEST 3、ERR_INVALID_PARAMETER 4、ERR_UNKNOWN 0xE、ERR_FATAL 0xF。**未確認**: 値（HID 1.1.1 §7.3・§7.4 と照合）。
- control channel: SET_PROTOCOL（Report）は **HIDBootDevice が true の device にだけ**送り（HID 1.1.1 は boot protocol を持つ device だけに SET_PROTOCOL を許す。**未確認**）、HANDSHAKE を 2 秒待つ。ERR_UNSUPPORTED_REQUEST は無視して進む。相手からの HID_CONTROL VIRTUAL_CABLE_UNPLUG は「相手が unpair を求めた」: link を切り、bond を消す（design §6.3 の逆向き。§Q9）。SUSPEND・EXIT_SUSPEND は記録だけ。相手からの GET_REPORT・SET_REPORT・GET_PROTOCOL（host への要求）は HANDSHAKE ERR_UNSUPPORTED_REQUEST。
- interrupt channel: 相手からの DATA（INPUT）の header を外して `/dev/input/bridge` へ write（layout が report ID を使うなら相手の report の先頭に ID があるまま。HIDP は ID を report の中に持つ）。DATA（OTHER・OUTPUT・FEATURE）は無視して数える。host から interrupt channel に送る物は無い（出力は範囲の外）。
- 長さ: 1 frame は HIDP の header 1 byte + report ≤ 512（`INPUT_BRIDGE_REPORT_MAX`、ID の byte を含む）。自分の MTU 672 と `BTD_L2CAP_MAX` 1024 の中に収まる（review M4: 1 + 1024 は組み直しで捨てられる長さだった）。超えた report は捨てて数える（`oversize`）。

#### 4.5 ATT client（`att.[ch]`、純粋）

- PDU の opcode（**未確認**: Core 5.4 Vol 3 Part F §3.4 と照合）: Error Response 0x01（request opcode、handle、error code）、Exchange MTU Request 0x02／Response 0x03、Find Information Request 0x04／Response 0x05（format 1 = 16 bit UUID、2 = 128 bit）、Read By Type Request 0x08／Response 0x09（length、{handle、value} の並び）、Read Request 0x0A／Response 0x0B、Read Blob Request 0x0C／Response 0x0D、Read By Group Type Request 0x10／Response 0x11（length、{handle、end group handle、value} の並び）、Write Request 0x12／Response 0x13、Write Command 0x52、Handle Value Notification 0x1B、Handle Value Indication 0x1D／Confirmation 0x1E。error code: Invalid Handle 0x01、Read Not Permitted 0x02、Insufficient Authentication 0x05、Request Not Supported 0x06、Insufficient Authorization 0x08、Attribute Not Found 0x0A、Insufficient Encryption Key Size 0x0C、Insufficient Encryption 0x0F。
- MTU: 既定 23。Exchange MTU で `BTD_ATT_MTU` 185 を求める（LE の ACL の data 27 byte で 7 packet。session の分割と組み直し（1024）の中）。相手の値と小さい方。
- 1 つの request が出ている間は次を出さない（ATT の規則）。request の timeout 30 秒（ATT の transaction timeout。過ぎたら link を切る）。
- 口: `btd_att_build_*`（request の組み立て）、`btd_att_parse(const uint8_t *pdu, size_t length, struct btd_att_pdu *)`（opcode ごとに長さを検査して field を指す。Response の list は要素の数と長さの整合を検査）。Notification・Indication は呼び手に渡し、Indication には Confirmation を返す。
- **最小の ATT server（review B3、§9.14）**: 相手からの request には必ず答える（答えないと相手の 30 秒の transaction timeout で切られる）。`btd_att_answer(const uint8_t *request, size_t length, uint8_t *out, size_t size, size_t *out_length)`（純粋）: Exchange MTU Request（0x02）→ Exchange MTU Response（server Rx MTU 185）、発見と読みの request（Find Information 0x04、Find By Type Value 0x06、Read By Type 0x08、Read 0x0A、Read Blob 0x0C、Read Multiple 0x0E、Read By Group Type 0x10）→ Error Response **Attribute Not Found（0x0A）**（この daemon は GATT の属性を持たない。handle は request の開始の handle）、Write Request（0x12）・Prepare Write（0x16）・Execute Write（0x18）→ Error Response **Request Not Supported（0x06）**、command（Write Command 0x52、Signed Write 0xD2）→ 捨てる、知らない opcode の request（bit 6 が 0 の奇数でない物）→ Request Not Supported。client の request の transaction とは独立（server の答えは待ちに影響しない）。

#### 4.6 HOGP の発見（`hog.[ch]`、ATT の上の状態機械。system call 無し）

状態の順（HOGP 1.0 §4・GATT の手順。**未確認**: 版と節、UUID の値は Bluetooth SIG Assigned Numbers と照合）:

1. Exchange MTU。
2. primary service の発見: Read By Group Type（UUID 0x2800）を 0x0001 から末尾の handle まで繰り返す（Attribute Not Found で終わり）。要る service: HID 0x1812（**複数あり得る: 最大 2 つを使い、service ごとに `/dev/input/bridge` の device を 1 つ作る**（unique_id は 2 つ目から `<address>#1`、名前に ` (2)`。consumer control の key などを別の service に置くキーボードのため。review S3、§9.12）。3 つ目以降は記録して無視）、Battery 0x180F、Device Information 0x180A。HID が無ければ `no-hid`。
3. HID service の characteristic: Read By Type（UUID 0x2803）を service の範囲で繰り返す。要る物: Report Map 0x2A4B、Report 0x2A4D（複数）、Protocol Mode 0x2A4E、HID Information 0x2A4A、HID Control Point 0x2A4C。Boot Keyboard Input Report 0x2A22・Boot Mouse Input Report 0x2A33 は無視（report protocol だけ）。characteristic は service ごとに **64** まで、Report は 32 まで（超えた分は記録して無視し、切らない。review S3）。
4. 各 Report の descriptor: Find Information を value handle + 1 から次の characteristic の宣言 − 1 まで。Report Reference 0x2908（value 2 byte: report ID、report type 1 = Input・2 = Output・3 = Feature）、Client Characteristic Configuration 0x2902。Report Reference を Read。
5. Report Map を Read、応答が MTU − 1 ちょうどなら Read Blob を offset で繰り返す（4096 まで。超えたら `descriptor`）。**終わりの印は 3 つのどれでも**: 応答が MTU − 1 より短い、空の応答、Error Response の Invalid Offset（0x07）か Attribute Not Long（0x0B）（長さが MTU − 1 の倍数の Map のため。review S3）。External Report Reference 0x2907（他の service の characteristic を report として使う形）は非対応（記録して無視）。
6. Device Information の PnP ID 0x2A50（Vendor ID Source 1 byte、Vendor ID 2、Product ID 2、Product Version 2）を Read（無ければ vendor・product は 0）。
7. Protocol Mode があれば Write Command で 0x01（Report Protocol Mode）。
8. `/dev/input/bridge` に setup を書く（§4.7。descriptor は Report Map）。
9. 各 Input Report の CCC に Write Request で 0x0001（notification）。Battery Level 0x2A19 を Read し CCC があれば notification。
10. OPEN。Handle Value Notification（handle → Report Reference の ID と type）: **Report Map が report ID を使うか**は、Report Reference の ID が 1 つでも 0 でなければ「使う」と見て、notification の値の先頭に ID を付けて write する（HOGP では notification に ID が無い。design §5.2 [F7]、§Q12）。全部 0 なら付けない。setup の後に `INPUT_BRIDGE_GET_DEVICE` の `flags` と照合し、違えば log して kernel の判定に合わせる（review M8）。知らない handle の notification は数える。Battery Level の notification は STATUS の `battery=` に出す。
11. **発見の前・途中に来た notification**（bond 済みの client の CCC は接続をまたいで保たれ、再接続の直後の key や click が発見の前に来る。review S2、§9.12）: (handle, 値) を **32 まで待ち行列**に入れ、setup の後に Report Reference の表で ID を付けて順に write する。溢れた分は捨てて数える。
- security: 暗号化（LE Enable Encryption、§4.7）の **後**に 1 から始める。それでも Insufficient Authentication（0x05）・Insufficient Encryption（0x0F）・Insufficient Encryption Key Size（0x0C）が返れば `security` で切る（bond が相手の要求（authenticated）に足りない: Settings が「もう一度 pairing」と出す材料。p006）。
- Service Changed（GATT 0x2A05 の indication）: Confirmation を返し、link を切って再接続（発見をやり直す）。
- 口: `btd_hog_start(struct btd_hog *)` → `BTD_HOG_SEND`（`out` を ATT で送る）／`BTD_HOG_SETUP`（descriptor と PnP が揃った）／`BTD_HOG_OPEN`／`BTD_HOG_FAILED`（why）。`btd_hog_input(struct btd_hog *, const uint8_t *pdu, size_t length)` → 同じ action の bit と、notification の時は `BTD_HOG_REPORT`（`report`・`report_length`、ID を付けた後）。`btd_hog_tick(now)` で transaction の timeout。

#### 4.7 HID host の核（`hid.[ch]`）: device の表と接続の流れ

- 表: `struct btd_hid_device hid[BTD_HID_MAX]`（6。session の link の数 8 のうち pairing 1 と余り 1 を残す）。field: address・type、transport（`BTD_HID_BREDR`／`BTD_HID_LE`）、state（IDLE・CONNECTING・AUTHENTICATING・ENCRYPTING・SDP・CHANNELS・SETUP・OPEN・CLOSING）、handle、`connected`、`encrypted`、`key_size`、`btd_reassembly`・`btd_l2cap`（BR/EDR）、channel の local CID（sdp・control・interrupt）、`btd_sdp`・`btd_hog`・`btd_att` の状態、`/dev/input/bridge` の fd（-1）、`struct btd_hid_record`（descriptor 4096・flags・vid・pid・version・名前）、report の待ち行列（setup の前に来た DATA を 32 まで、§4.8）、`wanted`（再接続する）、`reconnect_due`・`retry_ms`、counters（reports・malformed・dropped）、`battery`（-1）、`since_ms`、`last_error`。
- **PAIRED からの引き継ぎ（BR/EDR・LE。review B2、§9.2）**: pair が鍵を保存した後、相手が HID らしければ（§9.2 の判定）pair は切らずに link を hid に渡す（`handoff`）。hid は `AUTHENTICATED`（pair が暗号化と鍵 16 を済ませている）から始め、BR/EDR は下の CONNECT の「L2CAP connect PSM 0x0001」から、LE は「ATT/HOGP」から同じ流れを進む。`.hid` の記録を `candidate` で書く（page scan と相手からの接続の受けがこの時点から効く）。
- **CONNECT（host から）BR/EDR**: bond を読む（無ければ `not-bonded`。`.hid` の記録は無くてもよい: 最初の CONNECT が SDP で HID を確かめ、記録を作る）→ Create Connection（pair.c と同じ parameter。packet type 0xCC18、R1、clock offset 0、role switch 可）→ Connection Complete → Authentication Requested → Link Key Request に保存した鍵で Reply → Authentication Complete（0x06 なら `key-missing`、他の失敗は `security`）→ Set Connection Encryption（on）→ Encryption Change（0 でない値を on）→ Read Encryption Key Size（16 でなければ `key-size` で切る、KNOB）→ L2CAP connect PSM 0x0001 → SDP（§4.3。HID の record が無ければ `no-hid`）→ L2CAP disconnect（SDP）→ L2CAP connect PSM 0x0011 → PSM 0x0013（両方 OPEN）→ SET_PROTOCOL（HIDBootDevice の時）→ **setup を `/dev/input/bridge` へ**（fd は親から §4.10、名前は bond の `name`、無ければ SDP の ServiceName（**未確認**: 属性 0x0100 + language base）、無ければ空で kernel の種類の名前。UTF-8 の境で 51 byte に切る（M3）。`physical_path` は `bluetooth/<controller>/<address>`、`unique_id` は address の文字列、bus `BUS_BLUETOOTH`）→ `INPUT_BRIDGE_GET_DEVICE` で eventN（Q2 の ioctl は daemon では `btd_bridge_numbers()` の 1 か所に閉じ、断られた時は `/dev/system` の INPUT の event の照合に差し替える）→ `.hid` の記録を `confirmed` で書く（§9.3）→ OPEN、`CONNECTED` の答え。
- **CONNECT LE**: bond（LTK。無ければ `not-bonded`）。auto-connect（§4.9）が出ていれば LE Create Connection Cancel で止めてから、LE Create Connection（pair.c の parameter と同じ、相手の address・型は bond の identity）→ LE Connection Complete → LE Enable Encryption（0x2019: handle、rand 8、ediv 2、LTK 16。SC の bond は rand・ediv 0）→ Encryption Change（status 0x06 PIN or Key Missing は `key-missing`、他の失敗は `security`。bond の key_size が 16 でなければ始めから `key-size`）→ ATT/HOGP（§4.6）→ setup → OPEN。接続の parameter: 相手の Connection Parameter Update Request は l2cap.c の Accept（p004）に LE Connection Update を送る。
- **相手から（BR/EDR、HIDReconnectInitiate な device）**: Connection Request（address、class、link type ACL）→ router が `.hid` の記録（`candidate` か `confirmed`）があり `wanted` の device だけ受ける（Accept Connection Request 0x0409、**role 0x00 = master になる**（review S6、§Q17。2 台以上が繋ぐ時の scatternet を避ける。**未確認**: HID 1.1.1 の推奨。Role Change が失敗しても続ける））→ Connection Complete → **相手が認証を始めれば** Link Key Request → Reply（鍵が無ければ Negative、相手が切る）→ Encryption Change（on）→ Read Encryption Key Size（16 でなければ切る）。**相手が認証せずに L2CAP の Connection Request を出せば** §4.2 の Pending の流れで自分から Authentication Requested・Set Connection Encryption（§9.8）→ 両方 OPEN → setup（`.hid` の cache の descriptor。`candidate` で descriptor が無ければ SDP を先にし、その間の DATA は 32 まで待ち行列、超えたら捨てて数える）→ OPEN。`CONNECTED` の行は STATUS（と p006 の event）で見える。
- **LE の相手から**: LE の peripheral の役（相手が central になる）は範囲の外（HOGP の device は peripheral）。再接続は §4.9 の auto-connect。HID の LE の link に来る **SMP の PDU**（review B6）: Pairing Request → Pairing Failed（0x05 Pairing Not Supported）、Security Request（0x0B）→ まだ暗号化していなければ bond の LTK で LE Enable Encryption（既に暗号化済みなら無視）、他は無視して数える。
- **controller が消えた・reset された時（review B5、§9.5）**: `btd_hid_lost()`（`btd_close` から。session の ERROR も `btd_close` に来る、事実 28）が全ての device の `/dev/input/bridge` の fd を close（kernel が key を離す）、l2cap・reassembly・sdp・att・hog の状態と待ち行列を捨て、state を IDLE に、router の表を空にする。`wanted` は保つ。新しい controller が READY になれば `btd_hid_refresh()`（§4.9）が再接続を始める。
- **切断**: Disconnection Complete（理由を記録: 0x08 supervision timeout、0x13 remote user、0x16 local、0x05 authentication failure …）→ `/dev/input/bridge` の fd を close（kernel が key を離す）→ channel の表・ATT の状態を捨てる → `wanted` なら再接続の予定（§4.9）。**DISCONNECT（人から）**: BR/EDR は HID_CONTROL は送らず（VCU は unpair の意味）L2CAP の Disconnect（interrupt → control）→ HCI Disconnect（0x13）。LE は HCI Disconnect。`wanted = 0`（§Q5）。
- **FORGET の時に接続中**: BR/EDR は control channel に HID_CONTROL VIRTUAL_CABLE_UNPLUG を送ってから切る（design §6.3）。LE は切るだけ。filter accept list・resolving list から消す。
- setup の write が EINVAL（bounce の落ち、§3）なら **main の loop の timer**（`btd_hid_tick`）で 100 ms おきに 3 回（sleep で loop を止めない、review M10）。ENOSPC は `input-full`（§9.1）、ENXIO・E2BIG・ENODEV・EOPNOTSUPP は `descriptor` で切る。kernel が断る descriptor（電波から来る）で daemon は止まらない。report の write の EINVAL（宣言より短い、知らない長さ）は数えて捨てる（`malformed`、link は保つ。review M7）。512 byte を超える report は write せず捨てて数える（`oversize`、Q22）。
- timeout: 接続 10 秒（Create Connection Cancel／LE Create Connection Cancel）、認証・暗号化 10 秒、SDP 10 秒、channel 10 秒、HOGP の発見 30 秒（ATT の transaction 30 秒とは別）、全体 60 秒。過ぎたら `timeout` で切る。
- 1 つの device に同時に 1 つの流れ（CONNECT 中の CONNECT は `busy`）。別の device の CONNECT は並ぶ（BR/EDR の Create Connection は controller が 1 つずつ: 2 つ目は Command Disallowed になり得るので、daemon は **BR/EDR の page を 1 つずつ**（待ち行列）、LE の接続も 1 つずつ）。**PAIR・SCAN との排他（review B7、§9.7）**: SCAN と PAIR の間は新しい CONNECT と自動の page・auto-connect を始めない（`btd_hid_hold()`。人の CONNECT は `busy`。既に OPEN の device はそのまま）。**CONNECT の流れが進行中の device がある間の PAIR は `busy`**。**OPEN・waiting の device と同じ address への PAIR**（再 pairing）は、hid がその device を切って（`wanted=0`、`.hid` を消す）から pair を始める（PAIRED で引き継ぎ、`wanted=1`）。

#### 4.8 security の検査（design §6.4、[F3, N4, N5]）

HID の channel（BR/EDR）と HOGP の発見（LE）を始める前に、全て満たす: (1) bond がある（鍵の file）、(2) link が暗号化されている（Encryption Change on）、(3) 鍵の長さ 16（BR/EDR は Read Encryption Key Size、LE は bond の `key_size`）。満たさない link で来た L2CAP の Connection Request は Security Block、ATT は送らない。legacy の bond（`legacy=1`）は D10 で受ける（STATUS に `legacy=1`）。相手の descriptor・report は全部 kernel の parser に渡す前に長さだけ daemon が検査し（4096・512）、中身は kernel の parser（fuzz 済み、§7.2）が見る。待ち行列（setup 前の DATA・notification）は 32 report で捨てる。SDP の record は 8 KiB、ATT の MTU は 185、HOGP の characteristic は service ごとに 64、Report は 32、HID service は 2、notification の handle の表は 32。pairing の event は pair が進めている address 以外は全部 Negative（§4.1、B6）。

#### 4.9 再接続（design §6.4、受け入れ 3）

- 起動（controller が READY）と bond・`.hid` の変化で `btd_hid_refresh()`: `.hid` の記録（§9.3）ごとに bond があれば表に入れ `wanted = 1`（bond の無い `.hid` は消す）。**表を全部作ってから** Write Scan Enable と filter accept list を出す（R18）。
  - BR/EDR: `.hid` のある device が 1 つでもあれば **Write Scan Enable に page scan（bit 1）を立てる**（inquiry scan は pairing の mode の間だけ、D11b。pair.c／main.c の pairing の mode と or で合成する関数 `btd_scan_enable_update()` を session に置く）。`reconnect_initiate=0`（host から）か `normally_connectable=1` の device は daemon が page する: 起動時すぐ、失敗したら 5 秒 → 10 → 20 → 40 → 60 秒（上限）で繰り返し、**10 回で止める**（約 5 分。review S5: 永久に page し続けない）。止まった後は **きっかけ**（resume、人の CONNECT、bond・`.hid` の変化、controller の READY、その device からの Connection Request）で数え直して再開する。`reconnect_initiate=1` の device は待つだけ（page scan で受ける）。
  - LE: bond 済みの HOGP の device を **filter accept list**（LE Add Device To Filter Accept List 0x2011。identity address）に入れ、IRK のある bond は **resolving list**（LE Add Device To Resolving List 0x2027: peer identity address・peer IRK・local IRK（0 で可）、LE Set Address Resolution Enable 0x202D = 1、LE Set Resolvable Private Address Timeout は既定）に入れ、**LE Create Connection を Initiator_Filter_Policy = 1（filter accept list）で出しておく**（auto-connect。相手が directed/undirected advertising をすれば controller が繋ぐ）。LE Connection Complete の address は resolving list で identity に戻る（**未確認**: 戻るのは LE Enhanced Connection Complete（subevent 0x0A）の時で、普通の LE Connection Complete は RPA のまま返す controller があるか。実装では両方の subevent を受け、RPA なら p004 の `pair_resolved_bond` と同じ ah で bond を探す）。resolving list が無い controller（Read Local Supported Commands の bit、**未確認**: AX211 の値。loopback は持つ）では、相手が RPA を使う bond の再接続は **背景の passive scan**（Advertising Report の address を ah で解決 → LE Create Connection 直接）に落とす。
  - auto-connect の LE Create Connection が出ている間は LE scan・LE の PAIR・LE の CONNECT（直接）・**filter accept list と resolving list の変更**ができない（Command Disallowed になり得る）ので、SCAN・PAIR・CONNECT・**PAIRED・FORGET・refresh** の前に LE Create Connection Cancel（→ LE Connection Complete status 0x02 を待つ）を送り、終わった後に出し直す（`btd_hid_le_hold()`／`_release()`。review S4）。resolving list を変える時は LE Set Address Resolution Enable を 0 にしてから変え、1 に戻す。**未確認**: scan と initiating の同時を許す controller の範囲（Core 5.4 は許すが、controller の実装次第）。
  - auto-connect の LE Create Connection の scan の parameter は pair の 60 ms/30 ms（50% の duty）を使わず、**interval 0x0800（1.28 秒）・window 0x0012（11.25 ms）**（約 0.9% の duty。bond 済みの device の再接続の広告は密なので間に合う見込み。電力と Wi-Fi の共存のため。review S4。**未確認**: 実機での繋がるまでの時間。i03 の後に 5330 で測って決め直す）。
  - LE Enhanced Connection Complete（subevent 0x0A、31 byte）を受けるため、session の LE event mask を `{0x87, 0x03}` にし、`session_counted_event` が 0x0A も数える（事実 30、review S4。p004 の host 試験の mask の期待値を更新する）。
- supervision timeout・相手の切断（0x08・0x13・0x16 以外）→ `wanted` のまま → BR/EDR は上の page の規則、LE は auto-connect を出し直す。
- resume: main.c が `/dev/system` を開いて `KERN_SYSTEM_EVENT_POWER` を購読し（bluetoothd は今 `/dev/system` を開いていない、事実 16。design §5.3 が `sleep.end` に求める **Read Version のやり直しと firmware の load し直し（S0ix で bootloader に戻る時）と、受けて渡していない report の捨て**のうち、この Phase は HID の分（report の捨て = 切断で fd を close、再接続）だけを作り、Read Version のやり直しは p003 の transport の残件として Q1 に行き先を聞く（§Q19）。購読の口は 1 つにし、`btd_hid_resume()` と後の `btd_session_resume()` を同じ event から呼ぶ形にする）、`sleep.end` で `btd_hid_resume()`: OPEN の BR/EDR の link に **L2CAP の Echo Request（signalling CID 1、code 0x08）** を送り、3 秒で Echo Response が無ければ HCI Disconnect して page に戻す（review S5 で改めた: 「5 秒 report が無ければ切る」は打鍵の無いキーボードを resume のたびに切る。S0ix で controller の link の状態だけ残り相手が居ない時のため。**未確認**: AX211 の S0ix の後の link の状態）。Echo Response が来れば何もしない。LE は auto-connect を出し直す（waiting の device）。page の回数の上限（S5）を数え直す。re-enumerate（新しい `/dev/bluetoothN`）は p003 の「新しい controller」の経路（`btd_close` → `btd_hid_lost` → READY で `btd_hid_refresh()`）。
- DISCONNECT の後（`wanted = 0`）: 相手からの Connection Request は Reject（§Q5）。CONNECT で `wanted = 1` に戻る。daemon の再起動で `wanted` は 1 に戻る（記憶しない）。
- 鍵の離し: 切断の経路で fd を close するだけ（kernel が離す、事実 4）。daemon が落ちた時も kernel が fd を閉じるので残らない。

#### 4.10 privsep の拡張

- 子 → 親の datagram に `OPEN-HID` を足す。親は `/dev/input/bridge` を `O_RDWR|O_CLOEXEC` で開けて `OK /dev/input/bridge` と SCM_RIGHTS で答え、自分の写しを close する。失敗は `ERR <errno>`（EBUSY: 6 枚、EPERM）。親は枚数を数えない（上限は kernel の `INPUT_BRIDGE_OPENS_MAX`。子が閉じたかを親は知れないので、親の側の数えは誤る）。送られる途中の fd（子が受け取る前に死んだ時）は、子の死で親も終わり socketpair が閉じられて kernel が捨てる（open の枠を占め続けない。review M9）。
- 親の変更はこの 1 verb だけ。親の `OPEN` の path の検査（`/dev/bluetooth` と 1〜2 桁）は変えない。

#### 4.11 HID の device の記録（`hidcache.[ch]`。第 1 版の「bond の file に HID の field」は review B4 で改めた → §9.3）

bond の file は変えない（2048 byte の上限と `btd_keys_write` の形、事実 27）。HID の記録は別の file `<address>-<型>.hid` に置く。形・寿命・読み書きは §9.3。

#### 4.12 PAIRED の後（第 1 版の「切って自動で CONNECT」は review B2 で改めた → §9.2）

PAIRED の時に相手が HID らしければ pair は link を切らずに hid に引き継ぎ、同じ link で SDP・channel（LE は HOGP）を進める。HID らしくなければ pair は今までどおり切る。判定と切り替えの形は §9.2、問い直した Q4 は §「判断の記録」。

### 5. 口の request と CLI

| request | 誰 | 答え |
| --- | --- | --- |
| `CONNECT <address> <bredr|le-public|le-random>` | D8 の許す人（`btd_permitted`） | 繋いで setup まで。`CONNECTED address=… type=… transport=hid|hog input=/dev/input/eventN [touch=/dev/input/eventM] name="…" legacy=0|1 vendor=XXXX product=XXXX` と `DONE`。失敗は `ERROR <理由>` と `DONE`: `not-bonded`・`busy`（この device の流れが進行中、または SCAN・PAIR の最中）・`limit`（表が満ちた、6）・`input-full`（kernel の input の slot が無い、ENOSPC。B1）・`unreachable`・`timeout`・`key-missing`・`key-size`・`security`（認証・暗号化の失敗、ATT の 0x05/0x0F/0x0C）・`no-hid`（SDP に HID の record が無い、GATT に HID service が無い）・`descriptor`（kernel が断った、4096 超）・`protocol`・`lost`・`not-ready`・`permission`。既に OPEN なら `CONNECTED` をそのまま返す |
| `DISCONNECT <address> <型>` | 同 | 切って `DONE`（Disconnection Complete の後、3 秒）。`ERROR not-connected` |
| `STATUS` | 誰でも | `.hid` の記録のある device ごとに `HID address=… type=… transport=hid|hog state=idle|connecting|open|waiting input=/dev/input/eventN|- name="…" reconnect=auto|off|paused battery=NN|- since=<秒> last=<理由>|- reports=N malformed=N oversize=N` と `DONE`。`waiting` は切れていて再接続を待つ、`paused` は page の回数の上限で止まっている（S5） |
| `SHOW` | p003 のまま | CONTROLLER の行の末尾に `hid=<open の数> page_scan=0|1 le_auto=0|1`（p003 の試験は部分の一致なので通る、事実 34） |
| `PAIR` | p004 のまま | **CONNECT の流れが進行中なら `ERROR busy`**。OPEN・waiting の同じ address なら hid が切ってから（§4.7） |
| `FORGET` | p004 のまま | 接続中なら VCU（BR/EDR）と切断の後に消す。`.hid` も消す |

- 1 client に 1 request の規則のまま（CONNECT の client は `waits_connect`）。CONNECT の client が切れても流れは続ける（結果は STATUS）。
- CLI: `bt connect ADDRESS [TYPE]`（最後の行 `BT CONNECT result=connected|error input=/dev/input/eventN`）、`bt disconnect ADDRESS [TYPE]`（`BT DISCONNECT result=ok|error`）、`bt status`（daemon の行と `BT STATUS devices=N open=M`）。終了の値は p003 と同じ。
- p006 のための備え: 状態の変化の行（`CONNECTED`・`DISCONNECTED address=… reason=…`・`BATTERY address=… level=NN`）の形をここで固定し、`SUBSCRIBE` の配りは p006。

### 6. 試験の kernel の loopback の controller の相手

`bt-hci-loopback.c` に足す（QEMU で確かめるため。`loopback_frame` の payload の上限 16 byte を 256 byte に広げ、SDP・ATT の応答を出せるようにする）。**作り直し（review B7・S11、§9.7）**: 今は BR/EDR の相手が `device[6]` と handle 0x0040 の 1 組だけ（事実 29）なので、**link ごとの状態 `struct loopback_link`**（address の末尾の byte、handle、繋がっているか、暗号化済みか、L2CAP の channel の表（SDP・control・interrupt の remote/local CID と状態）、HID の timer の次の時刻と段）を 4 つ持つ: 01 → 0x0040、02 → 0x0043、03 → 0x0041、04 → 0x0044、05・06・07（pairing だけ、同時に 1 つ）→ 0x0045。command の経路（`loopback_pairing`・`loopback_peer_acl`）と時刻で動く送り（worker の tick）は同じ spinlock の下で link の状態を触り、送る frame は queue に入れるだけ（排他の注記を struct に書く）。05・06・07 の pairing の振る舞いは変えない。

- **0A:0B:0C:0D:0E:01（Loopback Keyboard、既存の DisplayYesNo の相手）に HID のキーボードを足す**: 接続は p004 のまま（handle 0x0040）。**pairing の後 host が切らずに L2CAP を続ける**（B2 の引き継ぎ）ので、同じ link で SDP・HID の channel に答える。暗号化の後の L2CAP: Connection Request PSM 0x0001 → Connection Response（remote CID 0x0040、成功）→ 自分の Configure Request（MTU 672、Flush Timeout 0xFFFF を付けて option の受けを通す）と相手の Configure Request への Response。SDP の ServiceSearchAttributeRequest に **固定の byte 列の record**（ServiceClassIDList {0x1124}、ProtocolDescriptorList {{L2CAP, PSM 0x0011}, {HIDP}}、AdditionalProtocolDescriptorLists {{{L2CAP, 0x0013}, {HIDP}}}、HIDDescriptorList {{0x22, boot keyboard の descriptor 63 byte（HID 1.11 Appendix E.6 の例の形を自分で書く（review M6。B.1 は boot の report の形の節）。report ID 無し、8 byte の report）}}、HIDReconnectInitiate false、HIDNormallyConnectable true、HIDVirtualCable true、HIDBootDevice true）を、MaximumAttributeByteCount が record より小さければ **continuation で 2 回に分けて**返す（continuation の経路を通す）。PnP の record（0x1200: vendor 0x1209、product 0x4B42、version 0x0100）。PSM 0x0011・0x0013 の Connection Request を受ける（CID 0x0041・0x0042）。control の SET_PROTOCOL に HANDSHAKE SUCCESSFUL。**interrupt が開いた直後（0 ms）に** DATA INPUT `00 00 04 00 00 00 00 00`（'a' 押す。channel の直後の DATA の経路を通す、review S11）、200 ms 後に全部 0（離す）、300 ms 後に `00 00 05 00 …`（'b' を押したまま）。以後 3 秒ごとに 'a' の押し・離しを繰り返す（'b' は押したまま。切断で kernel が離すのを見る）。host からの L2CAP Disconnect・HCI Disconnect に答える。host からの再 pairing（p004 の試験の 2 回目の `bt pair 01`）の前に host が切るので、切断の後は link の状態を戻す。
- **0A:0B:0C:0D:0E:02（class 0x002580 のマウス、Inquiry Result with RSSI の既存の address）を BR/EDR の HID のマウスにする**: pairing は NoInputNoOutput（Just Works、07 と同じ流れ、鍵 type 7）、handle 0x0043。SDP の record は HIDReconnectInitiate **true**、HIDNormallyConnectable **false**、descriptor は boot mouse（3 byte: button・X・Y、report ID 無し）。channel が両方開いた直後から 1 秒ごとに DATA INPUT `00 05 00`（X +5）。**接続から 4 秒後に自分で切る**（Disconnection Complete、理由 0x08 supervision timeout）、その 1 秒後に **Write Scan Enable の page scan の bit が立っている時だけ**（review S11）**Connection Request（address 02、class 0x002580、link type ACL）** を出し、host の Accept Connection Request（0x0409）に Command Status と Connection Complete（handle 0x0043）。**認証はせず、直ちに自分から L2CAP Connection Request PSM 0x0011 を出す**（暗号化の前の request: host の Pending → Authentication Requested → Link Key Request（host が Reply）→ Authentication Complete → Set Connection Encryption → Encryption Change（on）→ host の最終の Response、を通す。review S1・S11）→ PSM 0x0013 → DATA を再開（切断は 1 回だけ。2 度目の接続は切らない）。Reject（0x040A）が来たら何もしない（`wanted = 0` の試験）。**host からの page（Create Connection の address 02）は NormallyConnectable=false どおり Page Timeout で断る**（review S11。pairing の時の最初の page だけ受ける: pairing は host から始まるので、「bond の無い間は受け、bond の後は断る」とする）。
- **0A:0B:0C:0D:0E:04（Loopback HOG Mouse、新規。LE の public）**: **pre-bonded**（§Q7: 試験が bond の file を root で書く。固定の LTK `00 11 22 … FF`、ediv 0、rand 0、key_size 16、legacy 0）。**広告の報告には出さない**（p003・p004 の `BT SCAN devices=4` を保つ、review B7）。LE Create Connection（直接の address 04、または filter policy 1 で filter accept list に 04 がある時）に LE Connection Complete（handle 0x0044）。**接続の直後に自分から Exchange MTU Request（client Rx MTU 23）を送る**（host の ATT server の答えを通す、review B3）。LE Enable Encryption（0x2019）は **LTK・EDIV・Rand を pre-bonded の値と照合し**、合えば Encryption Change（on、0x01）、違えば status 0x06（review S11）。LE Add Device To Filter Accept List・Resolving List・Set Address Resolution Enable・LE Create Connection Cancel は Command Complete（Cancel は status 0x02 の LE Connection Complete も）。ATT server（固定の表）: 0x0001〜0x0005 GAP（Device Name）、0x0010 primary HID 0x1812（終わり 0x0020）: 0x0011/0x0012 HID Information、0x0013/0x0014 Report Map（マウス、report ID 1、約 50 byte → MTU 23 なら Read Blob が要る）、0x0015/0x0016 Report（Input）+ 0x0017 CCC + 0x0018 Report Reference {1, 1}、0x0019/0x001A Protocol Mode、0x001B/0x001C HID Control Point、0x0030 primary Battery 0x180F: 0x0031/0x0032 Battery Level 80 + 0x0033 CCC、0x0040 primary Device Information 0x180A: 0x0041/0x0042 PnP ID {1, 0x1209, 0x4842, 0x0100}。Exchange MTU は 23 のまま（Read Blob を通す）。CCC 0x0017 に 0x0001 が書かれたら 1 秒ごとに Handle Value Notification（0x0016、`00 05 00`: button 0、X +5、Y 0。ID 無し）。Write Request に Write Response、Read By Group Type・Read By Type・Find Information・Read・Read Blob を表どおり、無い物は Error Response Attribute Not Found。暗号化の前の HID の Read には Error Response Insufficient Encryption（0x0F）（security の順の試験）。
- **03（既存の LE、pairing を断る）はそのまま**（p004 の試験）。
- 変えない物: 05・06・07 の pairing、Information Request の答え、SMP の Pairing Request への断り（03）、Inquiry と広告の報告の既存の内容（01 の class 0x002540、02 の class 0x002580 は HID に合う。04 は出さない）。
- 0xFC03（withdraw）の後の再登録で link の状態は全部消える（B5 の試験）。

### 7. 試験

#### 7.1 host: kernel の glue（`plan/ws143/tests/hid-input-host-test.sh`、新しい `.c`）

freestanding で `hid-report.c`・`hid-digitizer.c`・`hid-touch.c`・`hid-input.c` を compile（`run-hid-pen.sh` の flag）、試験は `drv_input_device_register`・`_unregister`・`_emit_at`・`drv_input_device_number`・`kern_*`・`kern_logf` の stand-in（emit を記録）を持つ。試験の中に **旧の `usb_hid_publish_report`（2026-10-08 の usb-hid.c 1031〜1167 行）と `usb_hid_fetch_layout` の記述の部分の写し**を置き（試験だけ。製品の code は glue）、同じ descriptor と report の列で新旧の emit の列が byte 単位で一致することを確かめる: boot keyboard（複数 key の押し・離し・ErrorRollOver の keyboard_error）、report ID 2 つのキーボード（集約: 片方の report の key を他方の report で離さない）、マウス（EV_REL 0 の抑制、button）、pen（digitizer の経路）、touch（`plan/ws159/tests/latitude5330-linux/synaptics-06cb-ce65-rdesc.bin` の touchpad の report、touch の device が別に登録される）、capability が SYN だけ（device が作られない、ENODEV）、prepare → publish → unpublish → destroy の寿命、`report_max`、`numbers`。**register に渡す `input_device_info` の新旧の一致**（capability の列・axis・properties・名前の fallback、S10(d)）。stand-in の register が ENOSPC を返す時の publish の失敗と destroy のやり直し（M9）。

#### 7.2 host: hid-report.c の fuzz（`plan/ws143/tests/hid-report-fuzz.sh`、`.c`、design §5.2 [F6]）

ASan・UBSan（freestanding の file を `-fsanitize=address,undefined` で compile できるかは**未確認**: `-ffreestanding` と sanitizer の組み合わせは clang で通る見込み。通らなければ試験の側の stand-in の `kern_malloc` に赤帯を付ける）。固定の seed、1 回 60 秒まで。(a) ランダムな descriptor（item の形を守るランダム: short item の tag・type・size、long item、collection の深さと ID の数を上限の周りで振る）→ `drv_hid_input_prepare` は 0・EINVAL・E2BIG・ENOMEM・**EOPNOTSUPP**（review M2）だけを返し、成功なら `report_max ≤ 1025`（parser の上限。input bridge の write の上限 512 とは別）、capability の数と axis の数は上限の中。(b) 本物の descriptor（boot keyboard・boot mouse・5330 touchpad・pen・multitouch）の変異（byte の置換・挿入・削除・切り詰め）。(c) parse の成功した layout にランダムと変異の report（長さ 0〜1025）を `drv_hid_input_report` → 落ちない、emit の type・code が capability の中、EV_KEY の code ≤ KEY_MAX。回数と見つけた物を出力。回帰に使うので Q1 に master の Tools への登録を依頼する。

#### 7.3 host: `/dev/input/bridge` の純粋な検査

`input_bridge_setup_valid()`（magic・version・bus・descriptor_size・reserved・NUL）と report の長さの検査を `input-bridge.c` から切り出した純粋な関数として、hid-input-host-test に足す（cdev・file・cred の stand-in は作らない。cdev の経路は QEMU）。

#### 7.4 host: bluetoothd（`bt-daemon-host-test.sh` に足す、新しい `bt-input bridge-test.c`）

- sdp: request の組み立ての byte、応答の解析（§6 の固定の record、continuation 2 回、同じ continuation を返し続ける相手、入れ子 9 段、長さが残りを超える要素、uint の size の違い、HIDDescriptorList が無い、4097 byte の descriptor）、fuzz。
- hidp: header の組み立てと解析、HANDSHAKE の値、DATA の type、1025 byte の DATA。
- att: 各 PDU の組み立てと解析（list の要素の数の整合、format 1・2、Error Response）、fuzz。
- hog: 台本の ATT server（§6 の表と、変種: Report Map 3 回の Blob、Protocol Mode 無し、Report Reference が全部 0（ID を付けない）、HID service 無し（`no-hid`）、Insufficient Encryption（`security`）、Service Changed、characteristic 17 個、transaction の timeout）→ action と setup の内容、notification に ID が付く・付かない。
- l2cap: inbound の Connection Request（policy の accept・**Pending → 暗号化 → 成功、Pending → 失敗 → Security Block**・PSM not supported・No resources）、Configure Request の Flush Timeout・QoS（best effort・guaranteed を共に受ける）・RFC（basic・ERTM）・FCS、`opened`・`closed` の effect、`response[48]` に収まること、Echo Request/Response。
- att の server: Exchange MTU Request への Response、各 request への Error Response の opcode・handle・code、command が捨てられること。
- router: handle・address の配り、知らない相手の Connection Request の Reject、`.hid` のある（wanted）相手の Accept（role 0x00）、wanted=0 の Reject、Disconnection Complete と同じ handle の再利用、**bond 済みの HID の address から来る IO Capability Request・User Confirmation Request・PIN Code Request・Link Key Notification が Negative・無視になり鍵が変わらないこと（B6）**、HID の LE の link の SMP Pairing Request に Pairing Failed、Security Request で LE Enable Encryption。pair.c の既存の試験（bt-link-host-test）の「相手から始まる pairing の拒否」を router に移す。
- hid（偽の controller の台本 + 偽の `/dev/input/bridge` = socketpair の片方を読む）: **PAIRED からの引き継ぎ（同じ handle で SDP → channel → setup、pair は切らない。HID らしくない class では pair が切る。B2）**、BR/EDR の CONNECT の全段（setup の byte、report の write、close）、key-missing、key-size 7、no-hid、descriptor 4097、ENOSPC → `input-full`、setup の EINVAL 3 回のやり直し（tick）、DATA の待ち行列 32 と溢れ、513 byte の report の捨て（`oversize`）、相手からの接続（Connection Request → 認証 → channel → setup、と、認証の前の L2CAP request → Pending の流れ）、VCU（bond と `.hid` が消える）、DISCONNECT（wanted=0 → 次の Connection Request は Reject）、再接続の backoff の時刻と 10 回の上限と再開のきっかけ（tick）、**`btd_hid_lost()`（fd が全部 close され wanted が残る。B5）**、**PAIR と CONNECT の排他（CONNECT 中の PAIR は busy、OPEN の address への PAIR は先に切る。B7）**、LE の CONNECT（LE Enable Encryption の parameter、Encryption Change 0x06 → key-missing、HOGP → setup → notification → write、**発見の前の notification の待ち行列と流し直し（S2）**、HID service 2 つ → device 2 つ、characteristic 65 個）、auto-connect の hold/release（SCAN・PAIRED・FORGET の前の Cancel と後の出し直し、resolving list の変更の前の Address Resolution Enable 0）、LE Enhanced Connection Complete の受け、resume（Echo Request → Response 無しで切る、Response ありで保つ）。
- hidcache: `.hid` の file の書き読み（descriptor 4096 の 32 行、欠けた行・重複した行・size と合わない時の捨て方）、`candidate` → `confirmed`、bond の無い `.hid` の削除、`btd_keys_list` が `.hid` を飛ばすこと（p004 の試験の形で確かめる）。
- snoop: btsnoop の header と record の byte（固定の packet の列 → 期待の file と一致）。

#### 7.5 QEMU（T1。image は `build-bt-image.sh`。`config-amd64-bt.mk` に `evdev-probe input-bridge-probe` を足す）

- `input-bridge-p005.sh`（kernel だけ、i01b）: `input-bridge-probe` が root で `/dev/input/bridge` にキーボード（boot の descriptor、bus 5、名前「Probe Keyboard」）を作り、`evdev-probe -b bluetooth -t 5000` が `/dev/input/eventN` を見つけ（EVIOCGID の bustype 5、EVIOCGNAME）、probe が 'a' の press・release、'b' の press を書き、evdev が KEY_A 1/0・KEY_B 1 を読む。probe が close → evdev は KEY_B 0・SYN と、その後の read の ENODEV（node が消えた）。誤用: setup の前の report（EINVAL）、壊れた magic（EINVAL）、descriptor_size 4097（EINVAL）、FIDO の descriptor（ENXIO）、宣言より短い report（EINVAL）と長い report（受ける）、513 byte の report（EINVAL）、**7 枚目の open（EBUSY）**、6 枚の input bridge と `peninject`・`touchinject` の inject の device が PS/2・usb-kbd と同時に全部 register できる（B1: INPUT_DEVICE_MAX 32）、`runas btuser` の open（EACCES か EPERM）、read（EAGAIN）、`INPUT_BRIDGE_GET_DEVICE` の番号・`flags`・`report_max` と `evdev-probe` の node が同じ。`systemevents -c input -n 2` が ADD と REMOVE（subject eventN、detail `bus=5`）を出す。**node の持ち主を `ls -ln` で記録する**（root の 0640 のまま、事実 33。S13 の証拠として「kernel の node まで」を明示する）。
- `hid-usb-p005.sh`（USB の回帰、i01a）: guest の `-device usb-kbd`（既存、id 無し）に加えて `--qemu-extra "-device usb-kbd,bus=xhci.0,port=5,id=hidkbd -device usb-tablet,bus=xhci.0,port=6,id=hidtab -device usb-mouse,bus=xhci.0,port=7,id=hidmouse"`。`evdev-probe -b usb` が 4 つの node（bus 3）を数え、T1 の QMP で **`input-send-event` に `device` を付けて**（`send-key` は送り先を選べず PS/2 に行き得る、review S10(b)）hidkbd に KEY_A の押し・離し、hidtab に abs の x・y、hidmouse に rel の x +5 を送り、evdev が各 node で KEY_A 1/0、ABS_X/ABS_Y、REL_X 5 を読む。**抜き差し（S10(c)）**: QMP の `device_add usb-kbd,bus=xhci.0,port=8,id=hotkbd` → node が増える（ADD の event）、hotkbd に KEY_B の押しだけを送る → `device_del hotkbd` → evdev が **KEY_B 0 と SYN を読んでから ENODEV**（detach → unpublish → input.c が離す、§2 の危険 (c)）、REMOVE の event。QMP の socket は guest.py が `-qmp unix:<runtime>/qmp.sock,server,nowait` で開いている（事実 19。`boot-test.sh` の screendump と同じ socket）が、guest.py に汎用の command の口は無いので、T1 の script が `qmp_capabilities` → command の 2 行を socket に書く小さな手順（python の `socket` と `json`）を持つ。guest の usb-kbd の node を `evdev-probe` が開いている間、compositor の読みは要らない（この image は Files の image で desktop は無い）。
- `bt-hid-p005.sh`（i02 以降。bt-pair-p004.sh の形。daemon は `-s /tmp/btd.snoop` で起こす）: (1) `bt pair 01`（CONFIRM に y、p004 のまま。PAIRED の行は `l2cap=1` のまま）→ **切らずに引き継ぎ**（§9.2）→ `bt status` が `state=open input=/dev/input/eventN transport=hid`、`evdev-probe -b bluetooth` が KEY_A 1/0、KEY_B 1 を読む、`bt disconnect 01` → evdev が KEY_B 0 と ENODEV、`bt status` が `reconnect=off`。`bt connect 01` → open。(2) `bt pair 02`（CONSENT に y）→ 引き継ぎ → open、evdev が REL_X 5 → 4 秒後に loopback が切る → `state=waiting` → 1 秒後に相手から再接続（認証の前の L2CAP request → Pending の流れ）→ open、REL_X が続く（`bt status` の `since` が新しい）。`bt connect 02` は `ERROR unreachable`（NormallyConnectable=false の相手は host の page に答えない）。`bt disconnect 02` の後に loopback の Connection Request は Reject（loopback は 1 回しか再接続しないので、この確かめは host 試験）。(3) 試験が root で `/var/db/bluetooth/00:11:22:33:44:55/0A:0B:0C:0D:0E:04-le-public` を書く（`type=le-public ltk=… ediv=0 rand=0000000000000000 key_size=16 authenticated=0 secure=1 legacy=0`、0600、owner 80）→ `bt connect 0A:0B:0C:0D:0E:04 le-public` → `transport=hog input=…`、evdev が REL_X 5 を 1 秒ごと、`bt status` に `battery=80`。`bt disconnect 04`。(4) 権限: `runas btuser bt connect …` は `ERROR permission`、`runas btuser bt status` は通る。(5) daemon の再起動（kill → `/sbin/bluetoothd &`）: 01（host から）が自動で open、04（auto-connect の filter accept list、loopback が LE Connection Complete）が open、02 は `waiting`（相手から来るまで）。(6) `bt forget 01`（接続中: VCU → 切断 → bond と `.hid` の file が消える）。(7) 回帰: `bt-pair-p004.sh`・`bt-daemon-p003.sh`・`bt-loopback-p002.sh` が **変更なしで PASS**（01・02 の pairing の振る舞いは変えない。p003 の Inquiry の名前・class はそのまま。04 は広告に出さない。p004 の「01 の再 pairing」は OPEN の address への PAIR として hid が先に切る（§9.7）。事実 34）。(8) `hid-usb-p005.sh` と同じ guest で USB の node が残っていること（bus 3 の node の数）。(9) **controller が消えた時（B5）**: 試験が daemon を止め、`bt-probe -W 15000` を打ってから daemon を起こす（p003 の形、事実 34）→ 01 が open（KEY_B 押したまま）→ 15 秒で loopback が withdraw → evdev が KEY_B 0 と ENODEV、`bt show` が `closed (lost` → 再登録の後 ready → 01 が自動で open に戻る。(10) `/tmp/btd.snoop` が btsnoop の header で始まり、host に写して（guest.sh の複写）記録を残す（tshark が host に無いので照合は手で、または入れてから。§9.11）。(11) PAIR と CONNECT の排他: `bt connect 01` の直後（connecting の間）の `bt pair 07` が `ERROR busy`。
- 合否: 各 `ok` と最後の `bt-hid-p005: PASS`。1 回の試験は 3 分以内を目標（loopback の待ち時間は秒の単位）。

#### 7.6 実機

- p008 の UAT（本物の BR/EDR と LE のキーボード・マウス、再接続、suspend）。**i04 は実機の gate（推奨: 必須、§Q20）**: 5330 で BR/EDR 1 台と LE 1 台（ユーザーの device。design §9 の「情報のお願い」）に `bt pair` → 引き継ぎ → `evdev-probe`、`bt disconnect`・`bt connect`、電源の切り入れでの再接続、を T1 の lock の下で試す。review S12: B2・B3・S1・S2 は実機でしか見えないので、p006 の desktop の前に見る。
- 共通の誤りの限界（p004 の review-2 S-h と同じ）: loopback・偽の controller・daemon は同じ理解で作るので、SDP の byte の順、HIDP の header、ATT の PDU、Report Reference の順、LE Enable Encryption の parameter の順の取り違えは QEMU では見つからない。**外部の判定（review S11、§9.11）**: daemon の btsnoop の記録を host の `tshark`（無ければ入れる、事実 37）の dissector で読み、SDP・HIDP・ATT・L2CAP の byte の解釈を照合する。実機（i04・p008）でも同じ記録を取る。

### 8. ライセンスと参照

- 新しい code は全て Zlib（`plan/coding-style.md` §13 の header）。
- 参照は Bluetooth Core 5.4（Vol 3 Part A L2CAP、Part B SDP、Part F ATT、Part G GATT、Vol 4 Part E HCI）、HID Profile 1.1.1、HOGP 1.0、HID 1.11（descriptor の形）、Bluetooth SIG Assigned Numbers、Device ID Profile 1.3。**値の多くに「未確認」を付けた**（§4.3〜4.6）。実装の attempt の最初に仕様の PDF で照合して表にする（照合の結果はこの phase.md に書く）。
- Linux の GPL の code（BlueZ の kernel 部、hidp、uhid、hid-core）は読まない。BlueZ の userland（GPL/LGPL）も写さない。FreeBSD の `bthidd`・`sdpd`（BSD-2-Clause）は手順の確認の参照に使ってよいが code は写さない（design の冒頭の iwmbtfw と同じ扱い。使った時は commit・path・sha256 を記録する）。
- 既存の zedBSD の code の移動（usb-hid.c → hid-input.c）は同じ著作者の Zlib の中。
- btsnoop の file の形（header `btsnoop\0`、version 1、datalink 1002 = HCI UART (H4) の形で packet type の byte を先頭に付ける）は公開の形式の事実として使う。tshark（GPL）は host の道具として読むだけ（code を写さない）。

### 9. review-1 の反映（改訂 2、2026-10-08）

review-1（[review-1.md](review-1.md)）の Blocking・Should への設計の答え。§1〜§8 の該当の行は上で直し、ここに根拠と形をまとめる。表は §「design-reviewer の結果（review-1）と扱い」。

#### 9.1 B1: input device の slot（`INPUT_DEVICE_MAX`）

- 事実 26: system 全体で 8。5330 は PS/2 2 + i2c-hid 1 + USB の receiver（interface ごと）+ Bluetooth で超え、乗っ取られた子が全部取れる。
- 形: `INPUT_DEVICE_MAX` を **32** に（`input_device_reserved[32]`、rdev `0x00030000+N` と `eventN` の名前は 32 でも足る。`input_device_release` の `number < INPUT_DEVICE_MAX` の検査はそのまま）。input bridge は **`INPUT_BRIDGE_OPENS_MAX` 6**（1 open = 1 device + touch なら 2 slot → 最大 12）で全部は取れない。ENOSPC は daemon が `ERROR input-full`（§5）。
- compositor の `KWL_INPUT_MAX 16`（事実 26）: kernel の node が 16 を超えると compositor が 17 個目以降を開かない。この Phase の QEMU（PS/2 2 + usb 4 + input bridge 6 + inject 2 = 14）は超えない。**16 → 32 への追従は p006（desktop）の仕事として記録**（§Q23 と一緒に Q1 へ）。
- 試験: §7.5 の `input-bridge-p005.sh` で 6 枚の input bridge と inject の device と USB・PS/2 が全部 register できること、7 枚目は EBUSY。ENOSPC の経路は QEMU で 32 個を作る道具が無いので host 試験（glue の stand-in が ENOSPC を返す、§7.1）と code の review。

#### 9.2 B2: PAIRED の後は切らずに同じ link で HID を始める（引き継ぎ）

- 問題: 第 1 版は PAIR が切ってから CONNECT し直す形で、HIDNormallyConnectable=false・HIDReconnectInitiate=true の device（§6 の 02 そのもの）は host の page に答えず、相手からの再接続は `hid=1` が無いので Reject され、二度と繋がらない。pairing の link の上で自分から HID の PSM を開く device も断られる。
- 形: `pair.h` に **`typedef int (*btd_pair_handoff_fn)(void *context, const uint8_t *address, unsigned type, uint16_t handle, const struct btd_bond *bond)`** を足し、`btd_pair_init` で渡す。`pair_finish`（事実 38）は鍵を保存して `PAIRED` を報告した後、**切る前に** handoff を呼ぶ: 1 が返れば HCI Disconnect を送らず、handle を忘れて IDLE に戻る（router が owner を pair → hid に変える）。0 なら今までどおり切る。LE も同じ（SMP の鍵の配りが終わった後。link は既に暗号化済み）。
- hid の側（`btd_hid_handoff`）: 表に device を作り（満ちていれば 0 を返して pair が切る）、`.hid` を `candidate`（transport・名前・class/appearance）で書き、BR/EDR は `AUTHENTICATED`（pair が暗号化と鍵 16 を済ませた）から L2CAP の SDP へ、LE は HOGP の発見へ。SDP で HID の record が無ければ（`no-hid`）切って `.hid` を消す。
- **HID らしさの判定**（review B7: 「試す」は外す）: BR/EDR は scan の表（p003 の DEVICES）か Connection Request の class の **major device class（bit 12〜8）が Peripheral（0x05）**。LE は scan の表の **appearance の category（上位 10 bit、`appearance >> 6`）が HID（0x00F、値 0x03C0〜0x03FF。review M5）**。表に無い・class が無い device は HID と見ず、pair が切る（人が `bt connect` で繋げば SDP が確かめる）。
- 切り替え（Q4 の判断待ちのため）: `btd_hid_policy_after_pair(class, appearance)` の 1 関数が `HANDOFF`／`DISCONNECT` を返す。既定は HID らしければ HANDOFF。ユーザーが「pairing の後は自動で繋がない」を選べば常に DISCONNECT（`.hid` は `candidate` で書き、page scan と相手からの接続は受ける）。
- page scan: `.hid`（`candidate` を含む）が 1 つでもあれば立てる（§4.9）。これで B2 の「pairing の後に相手から繋ぎ直す device」は受けられる。
- p004 の Q5（「接続を保つのは HID の仕事」）と整合: pair は接続を保たず、保つのは hid。

#### 9.3 B4: HID の記録は bond と別の file（`hidcache.[ch]`）

- 事実 27: bond の file は 2048 byte 未満、`btd_keys_write` は struct の field だけを書く → bond に descriptor を足すと bond が読めなくなり、再 pairing で消える。
- 形: `/var/db/bluetooth/<controller>/<address>-<型>.hid`（`_bluetooth` の 0600。`btd_keys_list` は型が parse できない名前を飛ばすので bond の一覧に混ざらない、事実 27）。text の `key=value`: `state=candidate|confirmed`、`transport=bredr|le`、`reconnect_initiate=`、`normally_connectable=`、`virtual_cable=`、`boot_device=`、`vendor=`・`product=`・`version=`（hex 4 桁）、`country=`、`name=`（escape 済み）、`class=`／`appearance=`、`descriptor_size=`、`descriptor_<n>=`（n は 0 から、1 行 128 byte = hex 256 文字、32 行まで。欠け・重複・size の不一致は記録全体を捨てる）。読みの buffer は 12 KiB（keys.c の 600 byte の行の読みは使わず、hidcache.c が自分で読む）。
- 寿命: PAIRED の引き継ぎで `candidate`、SDP の成功で `confirmed`（descriptor を含む）。LE は descriptor を書かない（毎回発見。handle の表も書かない、S2 は待ち行列で解く）。**再 pairing（PAIR）で `.hid` を消す**（相手が変わった・firmware が変わったと見る。引き継ぎが新しく作る）。FORGET と VCU で消す。bond の無い `.hid` は refresh で消す。
- 書き: 一時 file → fsync → rename（keys.c と同じ形。file 全体を毎回書く、read-modify-write の口は `btd_hidcache_read` → 変更 → `btd_hidcache_write`）。

#### 9.4 S4: LE の list と session

- 内容は §4.9 に反映: hold/release を PAIRED・FORGET・refresh にも、resolving list の変更の前後で Address Resolution Enable 0/1、LE event mask `{0x87, 0x03}` と `session_counted_event` の subevent 0x0A（31 byte）、auto-connect の scan の duty を下げる（1.28 秒/11.25 ms、**未確認**: 繋がるまでの時間、i04 で測る）。

#### 9.5 B5: controller が消えた・reset された時の片付け（`btd_hid_lost`）

- 事実 28: `btd_close` は pair と scan だけ。session の ERROR も `btd_close` に来る。
- 形: `btd_close` から `btd_hid_lost()`（§4.7）。fd の close で kernel が key を離す（受け入れ 3 [N14]）。`wanted` は保ち、READY で `btd_hid_refresh()`。
- 試験: host（偽の controller の台本の途中で lost）と QEMU（§7.5 (9)、loopback の 0xFC03 の withdraw）。

#### 9.6 B6: bond 済みの HID の address から来る pairing の event

- 形は §4.1 に反映: pairing の event は pair がその address を進めている時だけ pair へ、他は router が Negative（IO Capability 0x18、Confirmation、Passkey、PIN）・Link Key Notification は保存しない。hid が受けるのは Connection Request と Link Key Request（と handle 付きの event）だけ。LE の HID の link の SMP は Pairing Failed（0x05）、Security Request は暗号化で答える（§4.7）。
- 試験: §7.4 の router の台本に入れる。

#### 9.7 B7: PAIR と CONNECT の排他、loopback の link ごとの状態、p003・p004 の期待値

- 排他は §4.7・§5 に反映: SCAN・PAIR の間は自動の page・auto-connect・人の CONNECT を止める（hold）。CONNECT の流れの進行中の PAIR は `busy`。OPEN・waiting の address への PAIR は hid が切って（`.hid` を消して）から。PAIR は自動の page より優先。
- 自動の接続は class・appearance が HID の device だけ（§9.2。「試す」は外した）。
- loopback: link ごとの状態に作り直す（§6 の冒頭。handle 01 → 0x0040、02 → 0x0043、03 → 0x0041、04 → 0x0044、05/06/07 → 0x0045）。p004 の試験が 01 の HID の link を開いたまま 07・05・06 を pairing するので、BR/EDR の link が 2 つ要る。**大きさ**: loopback の変更は「変更」より「作り直し」（見積もり: i02 の半分）。§Q25。
- p003・p004 の試験の期待値（事実 34）は**変えない**: 04 を広告に出さない（`devices=4`）、PAIRED の行の `l2cap=1` はそのまま（pair の Information Request は引き継ぎの前）、01 の再 pairing は「OPEN の address への PAIR」で hid が先に切る、`bt show` の行は末尾に足すだけ。i02 の受け入れに「p002〜p004 の 3 つの script が変更なしで PASS」を置く。

#### 9.8 S1: 暗号化の前に来た L2CAP の Connection Request は Pending で受け、自分から認証する

- 形は §4.2・§4.7 に反映: Pending（0x0001、status 0x0000）→ Authentication Requested → Link Key Request に Reply → Authentication Complete → Set Connection Encryption → Encryption Change（on）→ Read Encryption Key Size（16）→ 成功の Connection Response → Configure。失敗・10 秒で Security Block（0x0003）。相手が自分から認証した時は今までの流れ。
- loopback の 02 の再接続がこの経路を通る（§6）。

#### 9.9 S9・M7・Q22: report の write の長さ

- 事実 31: 分割は chunk の error で止まる。`INPUT_BRIDGE_REPORT_MAX` を **512**（= `SYSCALL_IO_CHUNK`、ID の byte を含む）にして report の write が分割されない長さにし、kernel は「宣言より短い → EINVAL、長い → 余りを読まずに受ける」。setup の分割は先頭の chunk の EINVAL で止まり、黙って 2 つの report に decode される経路は無い。daemon は 512 を超える report を捨てて数える（`oversize`。BR/EDR の HID の report は MTU 672 − 1 まで来得るが、キーボード・マウスの report は十数 byte）。

#### 9.10 S10: USB の回帰の確かめ方

- (a) raw（FIDO）の分岐は書き換えない（§2）。確かめ: diff で該当の行が変わっていないこと、ws161 の QEMU の試験（`plan/ws161/tests/fidoctl-p004.sh`・`fidoctl-p005.sh`・hidraw の loopback の試験）を i01a の T1 の依頼に入れる（loopback の鍵は usb-hid を通らないが、hidraw の class が壊れていないことは見える）、本物の鍵（YubiKey）は i04 で 5330 の USB に挿して `hidraw-probe`（任意）。QEMU の `u2f-emulated` は**未確認**（T1 が試せれば足す）。
- (b)(c) は §7.5 の `hid-usb-p005.sh` に反映（`input-send-event` の `device`、`device_add`/`device_del` で抜き差しと key の離し）。
- (d) は §7.1 に反映（register の情報の新旧一致）。
- (e) は §2 の配置に反映（pcat に hid-input.o、`CONFIG_INPUT_BRIDGE`、`hidraw-describe.c`）。build の受け入れに pcat（`config/ci/config-pcat.mk`）と arm64 を含める。

#### 9.11 S11: 外部の判定と loopback の追加

- btsnoop: bluetoothd に `-s PATH` を足し、子が全ての HCI の packet（送受、packet type と時刻）を `snoop.[ch]` で書く（親は関わらない。path は子が開ける所: 試験は `/tmp`）。host 試験は偽の controller の packet も同じ口で file にし、`tshark -r` が host にあれば `-V` の解釈（SDP の属性、HIDP の header、ATT の opcode）を期待の文字列と照合、無ければ skip して「未実施」と出す（事実 37: 今は無い。入れるかは Q1・T1）。
- loopback の追加は §6 に反映: 04 の LTK の照合と Exchange MTU Request、02 の page scan の bit の確認・host の page の拒否・認証の前の L2CAP request、01 の 0 ms の DATA。
- CSR の dongle と host の BlueZ（design §10.2）: 手元にあるかは**未確認**（design §9 の「情報のお願い」のまま）。

#### 9.12 S2・S3: HOGP の再接続の直後の notification と上限

- S2: bond 済みの client の CCC は接続をまたいで保たれる（GATT の規定、節は**未確認**）ので、再接続の直後の notification が発見の前に来る。待ち行列（32）に (handle, 値) を入れ、setup の後に流し直す（§4.6 の 11）。Q6 の「LE は CCC を書くまで来ない」は誤りだったので改めた。handle と Report Map の cache は作らない（firmware の更新で狂う。待ち行列で足りる: 遅れは発見の時間（1 秒前後）だけ）。
- S3: characteristic 64・Report 32、HID service 2 つまで（device を 2 つ作る）、Read Blob の終わりの 3 条件（§4.6）。`INPUT_BRIDGE_OPENS_MAX` 6 は device の数なので、HID service が 2 つの device は 2 枚使う（`limit` の計算に入れる）。

#### 9.13 S5・S6: 再接続の policy と role

- S5: page は 10 回で止めてきっかけで再開（§4.9）。resume は Echo Request で生存を確かめる（§4.9）。
- S6: Accept の role は 0x00（master）に改めた（Q17）。

#### 9.14 B3: 最小の ATT server

- §4.5 に反映。loopback の 04 が Exchange MTU Request を送る（§6）。host 試験は §7.4。

#### 9.15 Minor の反映

M1 → 事実 23。M2 → §3（FIDO は ENXIO、parser の EOPNOTSUPP を errno の一覧と fuzz の判定に）。M3 → §3（daemon が 51 byte）。M4 → §4.4（1 + 512）と §4.3 の MaximumAttributeByteCount を **0x0280（640）** に（header 5 + count 2 + 640 + continuation ≤ 17 = 664 ≤ 672 で 1 frame に収まる。1008 の理由は誤りだった）。M5 → §9.2（category は上位 10 bit）。M6 → §6（Appendix E.6）。M7 → §3・§4.7。M8 → §3（`flags`・`report_max`）。M9 → §3・§4.10。M10 → §4.7（timer）。M11 → §4.2（`response[48]`）。M12 → 範囲の外（記録）。

#### 9.16 S12・S13: 依存と attempt、「compositor に届く」の証拠

- S12: i02 の依存は「**p004 の cleared（最後の attempt が main に統合されている）**」に直した（p004 は test-wait で i03 を再依頼中、同じ file を触る）。i04 の「p004 の i03」は 5330 の実機の attempt を指す（ws.md の「i03 は 5330」）。Q2 の ioctl は i01b の前にユーザーの決定が来なければ推しで作り、daemon・試験・kernel の 3 か所の 1 関数ずつに閉じる（Q1 の指示）。実機の gate を i04 として置く（§Q20、推奨: p006 の前に必須）。i01 は **i01a（glue の refactor と USB の回帰）と i01b（`/dev/input/bridge` と UAPI）に分ける**（§Q21: 危険の大きい refactor の T1 の証拠を UAPI の議論と分け、i01a の diff を小さく保つ）。
- S13: 事実 33 のとおり、p005 の証拠は root の `evdev-probe` で読む kernel の node まで。login の後に現れた node を seat の人に渡すのは sessiond の仕事で未実装（USB の hotplug も同じ）。Q1 の条件「compositor の evdev に届く」は p005 では「kernel の evdev の node まで」と明示し、残りは §Q23。

| ID | 判断 | 理由 |
| --- | --- | --- |
| Q1 | **判断待ち（ユーザー）**: i2c-hid は共有 glue に乗せない（USB と input bridge だけ）。design.md §5.2 の「USB・I2C・input bridge が共有する module」の文言からの縮小 | i2c-hid は touch の device だけを出し、key の集約・pen・名前の規則を使わない（事実の表の i2c-hid の行）。乗せ替えは 5330 の touchpad（実機だけ）の回帰の危険に見合う益が無い。選ばなかった案: (b) 乗せる（touch だけの経路を glue に通す。回帰は p008 の実機で）→ Bluetooth の Phase に無関係の危険を足す。Future Work に「i2c-hid を hid-input に乗せる」を登録（Q1 に依頼） |
| Q2 | **判断待ち（ユーザー）**: `INPUT_BRIDGE_GET_DEVICE`（eventN の番号と malformed の数）の ioctl を D3 の形に足す | 無いと daemon が自分の作った node を知れず、`CONNECTED input=…`・STATUS・試験が `/dev/system` の INPUT の event と `bus=5 name=` の照合に頼る（競合し、同じ名前の device が 2 つあると見分けられない）。選ばなかった案: (b) ioctl 無し、daemon が `/dev/system` の event を見る → 上の弱さ。(c) write の返り値に番号を乗せる → write の約束（size を返す）を壊す |
| Q3 | Bluetooth の HID は hidraw に出さない。FIDO の descriptor は input bridge が ENXIO（review M2: parser の EOPNOTSUPP と分ける） | hidraw は「input でない raw の interface」の口（ws161 U1）。キーボードを hidraw にも出すと、grab していない読み手に打鍵が見える口が増える。Bluetooth の FIDO は WS161 の範囲でも無い。将来要れば `input_bridge_setup` に flag を足して hidraw に出す（reserved を使う） |
| Q4 | **判断待ち（ユーザー、製品の振る舞い。review B2 で問いを直した）**: 「pairing の後、HID らしい device（class が Peripheral／appearance が HID）は**切らずに同じ link で接続を続け**、すぐ使える状態にする」（推し）／「pairing の後は切り、人が `bt connect`（Settings の「接続」）で繋ぐ」。推しで作り、`btd_hid_policy_after_pair()` の 1 関数で切り替える（§9.2） | 利用者は pairing の後にすぐ使えることを期待する。第 1 版の「切ってから CONNECT し直す」は HIDNormallyConnectable=false の device が二度と繋がらない（review B2）ので案から外した。「切る」を選んでも `.hid` の `candidate` で page scan と相手からの接続は受ける（device が自分で繋ぎ直す物は使える）。p004 の Q5「接続を保つのは HID の仕事」と整合 |
| Q5 | **判断待ち（ユーザー、製品の振る舞い）**: 人が DISCONNECT した device は、CONNECT・再 pairing・daemon の再起動まで、相手からの再接続を Reject する（`wanted=0`）。推しで作り、`btd_hid_policy_after_disconnect()` の 1 関数で「常に受ける」に切り替えられる形 | 「切断」の直後に device が自分で繋ぎ直すのは人の意図に反する。選ばなかった案: 相手からの接続は常に受ける（bond 済みなので）→ 切断がすぐ戻る。記憶しない（再起動で戻る）のは、電源の入れ直しで使えなくなる事故を避けるため |
| Q6 | **改めた（review B4・S2）**: BR/EDR は SDP の結果（descriptor・flags・PnP）を **別の file `.hid`** に cache し、相手からの再接続で使う。LE は毎回 GATT の発見で、発見の前に来た notification は待ち行列（32）に入れて流し直す | 相手からの BR/EDR の再接続は channel が開いた直後に DATA が来る。bond の file は 2048 byte の上限（事実 27）。LE は bond 済みの client の CCC が保たれ再接続の直後に notification が来るので待ち行列が要る（第 1 版の「CCC を書くまで来ない」は誤り）。handle の cache は firmware の更新で狂うので作らない |
| Q7 | QEMU の LE の HOGP の相手は pre-bonded（試験が bond の file を書く）。loopback に SMP の相手は作らない | kernel に AES-128 はある（事実 21）が legacy の c1・s1 を試験の kernel に書くのは大きく、SC は P-256 が要る。LE の pairing の全体は host 試験（p004 の bt-link-host-test）で確かめ済み。p004 の Q3 と同じ結論 |
| Q8 | 出力の report（LED）は作らない。read は EAGAIN、poll は POLLIN を言わない | kernel に出力の経路が無い（事実の表）。形は「read は 1 つの output report」と決めておく |
| Q9 | 相手からの VIRTUAL_CABLE_UNPLUG で bond を消す | HID 1.1.1 の virtual cable の意味（device が unpair を求める。**未確認**: §7.4.x）。消さないと、相手が鍵を捨てた後に host が繋ぎに行き続ける |
| Q10 | **改めた（review S7）**: L2CAP の Flush Timeout と QoS（Guaranteed を含む）は受けて記録だけ、RFC は basic だけ | HID の device は interrupt channel に Flush Timeout を求める物があり、今の「Unknown option」の断りでは繋がらない（事実 15）。controller の flush timeout を書く（Write Automatic Flush Timeout）のは相手の送りの話ではなく、自分の送りは無いので意味が無い。QoS を Unacceptable で返すと接続をあきらめる device の恐れ（推測）があり、こちらは QoS を実施しないので受ける方が安全 |
| Q11 | LE の再接続は controller の filter accept list + resolving list の auto-connect。無い controller では背景の passive scan。自分の RPA（D11c）は i03 の後に判断 | 電波と電力の面で auto-connect が普通の形。自分の RPA は相手に自分の IRK を配る鍵の配りの変更が要り、p004 の smp.c の「initiator は鍵を配らない」を変える。D11c の「使う」は保つが、この Phase では相手の RPA の解決まで |
| Q12 | HOGP の notification に付ける report ID は Report Reference から。1 つでも ID が 0 でなければ付ける、全部 0 なら付けない | Report Map が ID を使うか daemon は parse せずに知れない（kernel の parser に任せる）。HOGP の Report Reference は Map の ID をそのまま持つ（ID を使わない Map では 0）。**未確認**: HOGP 1.0 §4.x の文言 |
| Q13 | kernel の `/dev/input/bridge` の open は root だけ（devfs 0600 に加えて） | chmod で広げられない。bluetoothd の子は open せず親から fd を受ける（D16 (a)）ので、子の uid で開ける必要が無い。input-inject と同じ |
| Q14 | BTD_HID_MAX は 6 | session の数える link は 8（`BTD_LINKS_MAX`）。pairing 1 と、相手から来て断る前の接続 1 を残す |
| Q15 | SET_PROTOCOL（Report）は HIDBootDevice の device にだけ送る | HID 1.1.1 は boot protocol を持たない device に SET_PROTOCOL を送ることを許していない（**未確認**）。Report が既定なので送らなくても動く |
| Q16 | 名前は daemon が UTF-8 の境で 51 byte に切る（review M3 で 63 から）。kernel は 64 以上を EINVAL | 文字の規則を kernel に持たせない。design §5.2 [N16]。51 = 63 − `" Touchscreen"` の 12 |
| Q17 | **改めた（review S6）**: 相手からの BR/EDR の接続の Accept は role 0x00（master になる） | 2 台以上の device が相手から繋ぐと、slave のままでは scatternet になる。Role Change が失敗しても接続は続ける。**未確認**: HID 1.1.1 の推奨 |
| Q18 | L2CAP の channel の表は HID の device ごと（pair と同じ形） | 事実 15: pair は自分の `btd_l2cap`・`btd_reassembly` を持つ。共有の 1 表は 18 > 16 で上限の変更と identifier・Information の状態の混在が要る（§4.2） |
| Q19 | **決定（Q1、2026-10-08）**: `sleep.end` の Read Version のやり直し・firmware の load し直し（design §5.3）は **p003 の残件に分ける**（Sleep はベータ3）。p005 は `sleep.end` を hid.c に渡す口だけ | bluetoothd は `/dev/system` を今開いていない（事実 16）。transport（p003）の仕事で HID の受け入れとは別。p005 で両方作ると transport の firmware の経路（5330 だけで確かめられる）に依存する |
| Q20 | **判断待ち（Q1・ユーザー）**: i04（5330 で BR/EDR 1 台と LE 1 台の実物）を p006 の前の **必須の gate** にする（推奨）／任意のまま | review S12: B2（引き継ぎ）・B3（ATT server）・S1（Pending の流れ）・S2（再接続の直後の notification）は loopback では作り手が同じで見えず、実機でしか分からない。任意のままだと p006 の desktop の後の p008 で初めて分かる。要る物: ユーザーの device（design §9 の「情報のお願い」） |
| Q21 | i01 を **i01a（glue の refactor と USB の回帰）と i01b（`/dev/input/bridge` と UAPI）に分ける** | 危険の大きい refactor（全 USB の入力）の T1 の証拠を、新しい UAPI（Q2 の判断を含む）と分けて先に取る。i01a の diff は usb-hid.c と hid-input.[ch] と build の規則だけで review しやすい。選ばなかった案: 1 つのまま → T1 の FAIL がどちらの物か分かりにくく、Q2 の判断が refactor を止める |
| Q22 | `INPUT_BRIDGE_REPORT_MAX` は 512（= `SYSCALL_IO_CHUNK`、ID の byte を含む） | review S9: 513 以上の write は記憶の圧迫で 512 と残りに分割され、断片が report として decode され得る。512 以下なら分割は起きない（事実 31）。キーボード・マウスの report は十数 byte。大きい report（BR/EDR の最大 671）は daemon が捨てて数える。選ばなかった案: kernel が report ID ごとの厳密な長さで断片を断る → 長さ違いの padding の device を使えなくし、残りの断片が別の ID に化ける経路が残る |
| Q23 | **判断待ち（Q1）**: login の後に現れた `/dev/input/eventN` を seat の人に渡す（sessiond が `/dev/system` の INPUT の ADD を聞いて chown）と、compositor の `KWL_INPUT_MAX 16` の 32 への追従の行き先 | 事実 26・33。p005 の HID の入力は root の `evdev-probe` で kernel の node までを確かめる。compositor（user）が開けるかは sessiond の hotplug の仕事で、USB の hotplug も同じ既存の穴（bug の ticket の候補）。推奨: p006（desktop）の最初の作業、または sessiond の小さな Phase |
| Q24 | input bridge の build の knob は新しい `CONFIG_INPUT_BRIDGE`（Makefile の既定 y、amd64 の vmunix.mk だけが見る） | review S10(e): `CONFIG_DRIVER_USB_BT` は pcat でも既定 y で、それを条件にすると pcat の link が崩れる。pcat・arm64 には input bridge を入れない |
| Q25 | loopback は link ごとの状態に作り直す（i02 の仕事として見積もる） | review B7・S11: 今は 1 組の address と handle（事実 29）。p004 の試験は 01 の HID の link を開いたまま 07・05・06 を pairing するので、BR/EDR の link が 2 つ要る。選ばなかった案: p004 の試験を直す → 回帰の価値を減らす |
| Q26 | class・appearance が分からない device は pairing の後に HID を試さない（pair が切る） | review B7。試すには SDP の失敗の経路を loopback の 05〜07 に足すことになり、p004 の試験の時間と形が変わる。人が `bt connect` すれば SDP が確かめる |

## 自分での敵対的 review（design-reviewer の前に見つけた危険と扱い）

| # | 危険 | 扱い |
| --- | --- | --- |
| R1 | usb-hid の refactor が全ての USB のキーボード・マウス・tablet・touch を壊す | §2 の「振る舞いを変えない」の列挙、§7.1 の新旧一致の host 試験、§7.5 の QEMU の USB の試験。i01 を kernel だけの attempt にして T1 の結果を先に見る |
| R2 | syscall の bounce の 512 byte への落ち（記憶の圧迫）で setup の write が EINVAL | daemon が 3 回やり直す（§4.7）。p002 の既知の性質 |
| R3 | 相手からの BR/EDR の再接続で descriptor の前に DATA が来る | `.hid` の cache（Q6、§9.3）、無ければ待ち行列 32 と捨てて数える |
| R4 | handle の再利用（Disconnection Complete の直後の Connection Complete が同じ handle）で router が取り違える | router は session の `session_link_remove` の後に Disconnection Complete を受けて表から消す。host 試験に台本 |
| R5 | 電波から来る descriptor・report・SDP・ATT の壊れた値 | kernel の parser の fuzz（§7.2）、daemon の parser の fuzz（§7.4）、長さの上限（§4.8）。daemon は `_bluetooth` で動く（D16） |
| R6 | 悪い相手が Connection Request を大量に送る | router が bond 済み以外を Reject（HCI の command 1 つ）。page scan は bond 済みの HID がある時だけ |
| R7 | notification の洪水で daemon が詰まる | 1 report の write は kernel への 1 syscall（寝ない）。session の queue（32 KiB）が溢れれば捨てて数える（p004 §10.1） |
| R8 | KeyboardOnly のキーボード（passkey を打つ）の pairing の UI | pair.c は User Passkey Notification を `PASSKEY` で agent に出す（p004 S-c）。`bt pair` は表示だけ。Settings は p006。QEMU では host 試験だけ（loopback の 01 は DisplayYesNo のまま） |
| R9 | auto-connect の LE Create Connection と LE scan・PAIR の衝突（Command Disallowed） | hold/release（§4.9）。host 試験に順の台本。**未確認**の controller の振る舞いは 5330 で |
| R10 | S0ix の後に controller が link を保ったまま相手が消える | resume の Echo Request（§4.9、review S5 で「5 秒 report 無しで切る」から改めた）。**未確認** |
| R11 | HOGP の device が Secure Connections だけ、または authenticated の鍵を要求し、Just Works の bond では Insufficient Authentication | `security` で切り、Settings が再 pairing を促す材料（p006）。鍵の長さ 16 未満の device は使えない（D10） |
| R12 | sniff mode の遅れ | controller と相手の既定に任せる（design §4 の見積もり）。host から Sniff Subrating は送らない（記録） |
| R13 | `/dev/input/eventN` の持ち主と mode（0640、sessiond）: compositor が Bluetooth の node を開けられるか | **改訂 2 で事実 33 を確かめた**: sessiond は login の時の node だけ chown し、hotplug は聞かない → login の後に現れた node は compositor（user）が開けない（USB も同じ既存の穴）。p005 は kernel の node まで（root の `evdev-probe`）。行き先は Q23 |
| R14 | `bt-pair-p004.sh` の期待（01 の pairing の後に `l2cap=1`、07 の Just Works）が §6 の変更で変わる | 01・02・07 の pairing の流れは変えず、HID は暗号化の後の L2CAP の追加だけ。p004 の試験を回帰に入れる（§7.5 (7)） |
| R15 | daemon が落ちた時の key | kernel が fd を閉じ、input.c の unregister が離す（事実 4）。daemon に依らない |
| R16 | 2 つの bluetoothd の instance（再起動の重なり）が `/dev/input/bridge` を 6 枚使い切る | 親の `OPEN-HID` は kernel の上限で EBUSY。`/dev/bluetoothN` は 1 open なので 2 つ目の daemon は controller を持てない |
| R17 | UAPI の struct の大きさと rdev の取り違え（第 1 版は sizeof を 4320 と書き（正しくは 4324）、rdev を smartcard の `0x00100000` と重ねていた。2 回目の照合で見つけた） | `_Static_assert(sizeof(struct input_bridge_setup) == 4324U)` を input-bridge.c と host 試験に置く。rdev は事実 22 の表で `0x00130000` に。実装の attempt で `grep -rn 'DEVICE_BASE\|DEVICE_NUMBER' src` をやり直して表を更新する |
| R18 | 相手からの BR/EDR の接続（HIDReconnectInitiate）が page scan を立てた瞬間から来るが、hid.c の表（`wanted`）がまだ bond の読みの途中 | `btd_hid_refresh()` は bond を全部表に入れてから Write Scan Enable を送る順にする。router は表に無い address の Connection Request を Reject（相手は再試行する） |

## design-reviewer の結果（review-1、2026-10-08、529cbeb7c）と扱い

敵対的 review（Blocking 7・Should 13・Minor 12、[review-1.md](review-1.md)）。全部を改訂 2 で設計に入れた（§9 と §1〜§8 の該当の行）。「判断待ち」は Q1・ユーザーへ。

| ID | 指摘 | 扱い |
| --- | --- | --- |
| B1 | input device は system で 8 まで（`INPUT_DEVICE_MAX`）。input bridge が全部取れる、ENOSPC の扱いが無い | **§9.1 に反映**: 32 に上げる、`INPUT_BRIDGE_OPENS_MAX` 6、`ERROR input-full`、試験。compositor の 16 は Q23 |
| B2 | PAIRED の後に切って繋ぎ直す形と `hid=1` の条件で、NormallyConnectable=false の device が二度と繋がらない | **§9.2 に反映**: pair の `handoff` で切らずに同じ link で HID を始める、`.hid` の `candidate` で page scan と相手からの接続を受ける。Q4 の問いと推しを直した |
| B3 | 相手からの ATT の request に答えない | **§4.5・§9.14 に反映**: 最小の ATT server（Exchange MTU Response、Attribute Not Found、Request Not Supported）。loopback の 04 が Exchange MTU Request を送る |
| B4 | bond の file に descriptor を入れると 2048 byte の上限で bond が読めなくなる、再 pairing で消える | **§9.3 に反映**: 別の file `.hid`（`hidcache.[ch]`）。keys.c は変えない。再 pairing で `.hid` を消す |
| B5 | controller が消えた・reset の時に HID を片付けない（key が残る） | **§4.7・§9.5 に反映**: `btd_hid_lost()` を `btd_close` から。host 試験と QEMU の withdraw の試験 (9) |
| B6 | bond 済みの HID の address から来る pairing の event への答えが未定（鍵の上書き） | **§4.1・§9.6 に反映**: pair が進めている address 以外の pairing の event は router が Negative、Link Key Notification は保存しない。LE の SMP は Pairing Failed、Security Request は暗号化 |
| B7 | PAIR と CONNECT の排他が無い、loopback の link が 1 つ、p003・p004 の期待値（`devices=4`・再 pairing）が崩れる | **§4.7・§5・§6・§9.7 に反映**: hold と `busy`、OPEN の address への PAIR は先に切る、自動の接続は class・appearance が HID の device だけ、loopback を link ごとに作り直す（Q25）、04 は広告に出さない、p002〜p004 の script は変更なしで PASS を i02 の受け入れに |
| S1 | 暗号化の前の L2CAP request に即 Security Block | **§4.2・§9.8 に反映**: Pending → 自分から認証・暗号化 → 成功の Response。loopback の 02 の再接続がこの経路 |
| S2 | LE の再接続の直後の notification を失う（CCC は保たれる） | **§4.6・§9.12 に反映**: 待ち行列 32 と流し直し。Q6 を改めた |
| S3 | HOGP の上限（characteristic 17 で切る、HID service 1 つ、Read Blob の終わり） | **§4.6・§9.12 に反映**: 64・32、HID service 2 つ → device 2 つ、終わりの 3 条件 |
| S4 | LE の list の変更と initiating の衝突、LE Enhanced Connection Complete の mask、auto-connect の duty | **§4.9・§9.4 に反映**: hold/release を PAIRED・FORGET・refresh にも、Address Resolution の off/on、mask `{0x87, 0x03}` と counted event、1.28 秒/11.25 ms（**未確認**、i04 で測る）。session.c を file の表に |
| S5 | 永久の page、resume の「5 秒 report 無しで切る」 | **§4.9・§9.13 に反映**: 10 回で止めてきっかけで再開、Echo Request で生存の確認 |
| S6 | Accept の role | **Q17 を改めた**: 0x00（master）。**未確認**のまま |
| S7 | QoS Guaranteed の Unacceptable | **§4.2・Q10 を改めた**: 記録して受ける |
| S8 | write の文脈の kernel の stack | **§2 に反映**: 作業領域は `struct hid_input` に |
| S9 | bounce の分割で report が黙って 2 つになる | **§3・§9.9 に反映**: `INPUT_BRIDGE_REPORT_MAX` 512（Q22）、短い report は EINVAL |
| S10 | USB の回帰の穴（raw、send-key の送り先、抜き差し、register の情報、pcat・`CONFIG_DRIVER_USB_BT`・hidraw-describe） | **§2・§7.1・§7.5・§9.10 に反映**: raw の分岐は書き換えず ws161 の試験を回帰に、`input-send-event` の `device`、`device_add`/`device_del`、register の情報の比較、`CONFIG_INPUT_BRIDGE`（Q24） |
| S11 | 試験の作り手が同じ。btsnoop と tshark、loopback の振る舞いの追加 | **§6・§7.6・§9.11 に反映**: `-s PATH` の btsnoop（`snoop.[ch]`）、tshark は host に無い（事実 37。入れるかは Q1・T1）、loopback の 5 つの追加。CSR の dongle は**未確認** |
| S12 | 依存（p004 の i02 の PASS は成り立たない）、i04 の曖昧さ、Q2 の時期、実機の gate、i01 の分割 | **§9.16・attempt の表に反映**: 依存は p004 の cleared、i04 は 5330 の実機の gate（Q20、推奨: 必須）、Q2 は 1 か所に閉じて推しで作る（Q1 の指示）、i01 を i01a・i01b に分ける（Q21） |
| S13 | 「compositor の evdev に届く」の証拠 | **範囲の外・§9.16・Q23 に反映**: p005 は kernel の node まで（root の `evdev-probe`、`ls -ln` で持ち主を記録）。sessiond の hotplug は未実装（事実 33）で Q23 |
| M1 | ioctl の group の一覧の欠け | 事実 23 に 'G'・'f' |
| M2 | parser の EOPNOTSUPP | §3 の errno と §7.2 の判定に。FIDO は ENXIO |
| M3 | touch の名前が 63 を超える | §3: daemon が 51 byte で切る |
| M4 | HIDP の 1 + 1024、SDP の 1008 の理由 | §4.4: 1 + 512。§9.15: MaximumAttributeByteCount 640 |
| M5 | appearance は上位 10 bit | §9.2 |
| M6 | boot keyboard の descriptor は Appendix E.6 | §6 |
| M7 | report の write の EINVAL の扱い、長い report | §3・§4.7: 長い物は受ける、EINVAL は数えて捨てる |
| M8 | `input_bridge_device` に flags と report_max | §3 |
| M9 | publish の失敗の後の destroy、送られる途中の fd | §3・§4.10 |
| M10 | setup のやり直しは timer で | §4.7 |
| M11 | `response[16]` が足りない | §4.2: 48 |
| M12 | SUSPEND・EXIT_SUSPEND を sleep で送る | 範囲の外に記録（Sleep はベータ3） |

## attempt の区切り（QEMU で確かめられる単位。依存と危険で分ける。review S12 で改めた）

| attempt | 範囲 | 受け入れ（Q1 が判定） | 依存 |
| --- | --- | --- | --- |
| **i01a: kernel の glue の refactor と USB の回帰** | `hid-input.[ch]`（§2）、usb-hid.c の乗せ替えと規約の全文への合わせ（raw の分岐は触らない）、`input.c` の `INPUT_DEVICE_MAX` 32 と `drv_input_device_number()`、build の規則（amd64・pcat・arm64 に hid-input）、`evdev-probe`、host 試験 §7.1（新旧一致: emit と register の情報）、hid-report の fuzz §7.2、QEMU の `hid-usb-p005.sh`（§7.5: 4 つの USB の device、`input-send-event` の `device`、`device_add`/`device_del` の抜き差しと key の離し）と ws161 の hidraw の試験の回帰 | build warning 0（amd64 の製品の kernel `config/ci/config-amd64.mk`、試験の config `plan/ws143/tests/config-amd64-bt.mk`、`config/ci/config-pcat.mk`、arm64 の `config/ci/config-rpi4.mk`）、`python3 plan/tools/style-check.py` 0、host 試験 PASS（新旧一致、fuzz 60 秒で 0 件）、T1: `hid-usb-p005.sh` PASS と `plan/ws161/tests/fidoctl-p004.sh`（または hidraw の loopback の試験）PASS、`boot-test.sh` の login prompt | D3、Q1（決定前は i2c-hid に触らない） |
| **i01b: `/dev/input/bridge`** | `include/uapi/input-bridge.h`・`input-bridge.c`・devfs・vfs・`CONFIG_INPUT_BRIDGE`（§3）、`input-bridge-probe`、host 試験 §7.3、QEMU の `input-bridge-p005.sh`（§7.5） | build warning 0（同上）、style 0、host 試験 PASS、T1: `input-bridge-p005.sh` PASS（6 枚と 7 枚目の EBUSY、ENXIO、key の離し、ADD/REMOVE、`ls -ln` の記録）。Q2 の決定が無い間は `INPUT_BRIDGE_GET_DEVICE` を入れ、kernel の `input_bridge_ioctl`・daemon の `btd_bridge_numbers()`・試験の 1 関数ずつに閉じる | i01a（T1 の PASS）、Q2（無ければ推し） |
| **i02: bluetoothd の BR/EDR** | router（§4.1・§9.6）、l2cap の inbound・Pending・option・Echo（§4.2・§9.8）、sdp・hidp（§4.3・4.4）、att の server（§4.5 の最小の server は LE の link でだけ使うが純粋な部品はここで）、hid.c の BR/EDR の流れ・引き継ぎ・security・再接続・lost・排他（§4.7〜4.9、§9.2・9.5・9.7）、hidcache（§9.3）、privsep の `OPEN-HID`、pair.c の `handoff` と Reject の移動、`/dev/system` の購読、snoop、口と `bt`（§5）、loopback の link ごとの作り直しと 01・02（§6、Q25）、host 試験 §7.4 の BR/EDR の分、QEMU の `bt-hid-p005.sh` の (1)(2)(4)(6)(7)(8)(9)(10)(11) と (5) の 01・02 | build warning 0、style 0、host 試験 PASS、T1: `bt-hid-p005.sh` PASS と **`bt-loopback-p002.sh`・`bt-daemon-p003.sh`・`bt-pair-p004.sh` が変更なしで PASS**。Q4・Q5 の決定が無い間は推しで作り `btd_hid_policy_after_pair()`・`btd_hid_policy_after_disconnect()` で切り替えられる形 | i01b（T1 の PASS）、**p004 の cleared（最後の attempt が main に統合されている）** |
| **i03: LE の HOGP と再接続の残り** | hog（§4.6・§9.12）、hid.c の LE の流れ・SMP の答え、filter accept list・resolving list・auto-connect・hold/release・Address Resolution の off/on、session の LE mask と counted event（§9.4）、背景の scan の fallback、resume（`sleep.end` → Echo Request）、loopback の 04（Exchange MTU Request、LTK の照合）、host 試験 §7.4 の LE・resume の分、QEMU の (3) と (5) の 04 | 同上（p002〜p004 の回帰を含む）。resolving list の無い controller の fallback は host 試験だけ（loopback は持つ） | i02 |
| **i04: 5330 の実機の gate（Q20、推奨: 必須）** | ユーザーの BR/EDR の device 1 台と LE の device 1 台で `bt pair` → 引き継ぎ → `evdev-probe`、`bt disconnect`・`bt connect`、device の電源の切り入れでの再接続、btsnoop の記録。T1 の lock の下、account 入りの image（p004 の i03 と同じ条件）。USB の鍵があれば `hidraw-probe`（S10(a)、任意） | 接続と入力・再接続の観察を記録（実機の証拠として分けて書く）。btsnoop を host の tshark で読んで SDP・ATT・HIDP の解釈を照合（tshark を入れられれば） | i03、p004 の 5330 の attempt（i03）、device の有無 |

p005 を cleared にする条件: i01a〜i03 の T1 の PASS、仕様の値の照合の表（§8）、設計の §4.3〜4.6 の「未確認」の解消（または実機へ持ち越す物の明記）、Q1・Q2・Q4・Q5 の決定の反映（決定が無い間は推しの形で切り替えられること）。i04 を cleared の条件に入れるかは Q20（推奨: 入れる。入れないなら p008 の UAT が実機の受け入れ）。

## 確認

- 2026-10-08（P2、設計だけ）: code は書いていない。build・host 試験・QEMU・実機は **未実施**。読んだ file と行は §「事実」の表。
- 2026-10-08（P2、2 回目。Q1 は「file は作られていない」として最初からの作業を投入したが、第 1 版の file は worktree に未追跡で残っていた（最後の書き込み 08:04:04））: 第 1 版の §「事実」の 21 行の出典を全て code と照合し直した（`sed -n`・`grep` で行を読んだ。build・試験は無し）。直した物: 事実 5（input-inject の行）、10（hid-report.h の上限は 10〜16 行）、15（l2cap.h の行と表の持ち主）、16（main.c の行、`/dev/system` 未購読）、17（keys.h → keys.c の行、field の名前）、18（loopback の LE の接続の条件と行）、19（QMP と既存の道具）、§3 の sizeof（4320 → 4324）と rdev（`0x00100000` → `0x00130000`、smartcard と衝突）、§4.2 の表の持ち主の根拠（Q18）、§4.9 と範囲の外の `sleep.end` の Read Version（Q19）、§7.5 の QMP の未確認の解消。事実 22〜25、R17・R18 を足した。仕様の値（§4.3〜4.6 の **未確認**）は照合していない（仕様の PDF はこの作業では読んでいない）。
- design-reviewer: review-1（[review-1.md](review-1.md)、529cbeb7c、B7・S13・M12）。2026-10-08（P2、3 回目）: 全部を改訂 2 で設計に入れた（§9、§「design-reviewer の結果（review-1）と扱い」）。review の根拠の行（`INPUT_DEVICE_MAX`、`KEYS_TEXT_MAX`、`btd_close`、session の ERROR、loopback の `device[6]`、LE mask、syscall の write の loop、decode の長さの規則、pcat の vmunix.mk、`response[16]`、p003・p004 の試験の期待値、sessiond の seat.c、devfs の記憶）を code と照合して事実 26〜38 に足した。Q1 の決定（Q19、Q1・Q2・Q4・Q5 は推しで作って切り替えられる形、i01 から、T1 に USB の回帰）を反映。build・試験は無し。

- 2026-10-08（P2、q888 の i01a）: 下の §「i01a の記録」。

## i01a の記録（2026-10-08、P2、q888）

**作った物**:
- `include/drivers/generic/hid-input.h`・`src/drivers/generic/hid-input.c`（共有の HID glue、§2）。口は §2 のとおり。設計からの違い: `drv_hid_input_numbers()` は void（番号は -1 か N）。`drv_hid_input_report_ids()` を足した（i01b の `INPUT_BRIDGE_FLAG_REPORT_IDS` に使う）。identity の `touch_name` は呼び手が必ず渡す（usb-hid は名前の無い製品で「USB HID touchscreen」、有れば「<name> Touchscreen」と今のまま。NULL の既定は作らない）。`identity` の文字列は input.c が写すので glue は写しを持たない（log の行のための `physical_path` だけ持つ）。作業領域（decode の出力・pen と touch の出力・key の bit map）は `struct hid_input` の中（S8）。log の行は `hid-input: malformed input <physical_path> length= error=`（旧: `usb-hid: malformed input usbN device= interface=`。physical_path が usbN/portP/deviceD/interfaceI を含む）、pen・touch の drop は `hid-input: pen|touch report dropped`。最初の 16 回は今のまま（malformed と pen・touch の drop で共有の数え）。
- `src/drivers/usb/usb-hid.c`: glue を呼ぶ形に。`usb_hid_fetch_layout`・`usb_hid_identity`・`usb_hid_publish_report`・`usb_hid_unpublish`・`usb_hid_activate`（`usb_hid_activate_start`・`usb_hid_activate_undo` に分けて goto を無くした）・`usb_hid_free` を規約の全文に合わせた。**FIDO の raw の分岐の行（fetch_layout・publish_report・unpublish・activate）は変えていない**（`git diff -U0` に raw の行の変更が無いことを確かめた。identity の raw の名前の行だけは if の形を直した: 範囲の 4 か所の外）。触っていない関数（match・attach・detach・descriptor_length・endpoint_capacity・worker ほか）は規約に合わせていない（file 先頭の「XXX: Need coding style fitting.」を残す）。
- `src/drivers/generic/input.c`・`include/kern/input-device.h`: `INPUT_DEVICE_MAX` 8 → 32、`drv_input_device_number()`。
- build の規則: `platform/amd64/vmunix.mk` の `AMD64_HID_SOURCES`、`platform/pcat/vmunix.mk` の `PCAT_USB_CLASS_OBJS`、`platform/arm64/vmunix.mk` の `ARM64_USB_SOURCES` に hid-input。
- `userland/tests/evdev-probe/`（`-b`・`-n`・`-p`・`-N`・`-t`・`-r`・`-l`。§再開点の形から `-p`（place）・`-N`（新しい node だけ）・`-l`（数える）を足し、`-r` は「静かな時間」）。`plan/ws143/tests/config-amd64-bt.mk` に足した。
- host 試験: `plan/ws143/tests/hid-input-host-test.{sh,c}` と旧の写し `hid-input-old.{c,h}`（2026-10-08 の usb-hid.c、git a6988363c の report の扱いを文ごとに写した物。**比べる基準なので規約に合わせない**: style-check の対象外として扱う）。`hid-report-fuzz.sh`（同じ C の fuzz の mode を ASan・UBSan で。設計の「60 秒」は **固定の数 1000000 descriptor**（約 24 秒）に改めた: 時刻で止めると同じ seed でも回数が変わり再現しない）。旧の写しと kernel の file は freestanding（errno は kernel の番号）、試験の本体は host の libc。
- T1 の試験 `plan/ws143/tests/hid-usb-p005.sh`: 設計の `--qemu-extra` をやめ、QMP の `device_add` で USB のキーボード・マウス・tablet を足す（files-guest.sh の起動に手を入れない）。**`input-send-event` の `device` は使わない**（T1-129: この guest の text console に `device` の property が無く QEMU が abort する。`device` は表示の device を選ぶ引数で、USB の device は選べない）。QEMU は最後に足した USB キーボードに key を渡す（hid の keyboard の handler は登録の時に先頭になる）ので、足したキーボードの node で KEY_A を見る。tablet は QEMU が選ぶので、足した tablet に来なければ note だけ。

**確かめ（P2、host）**:
- `make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk BUILD=build/ws143-p005/amd64 build/ws143-p005/amd64/vmunix` exit 0、warning 0。
- `make -j16 ZEDBSD_CONFIG=plan/ws143/tests/config-amd64-bt.mk BUILD=build/ws143-p005/bt build/ws143-p005/bt/bin/evdev-probe build/ws143-p005/bt/vmunix` exit 0、warning 0（自分の source。openssl 等の package の既存の warning は別）。
- `config/ci/config-rpi4.mk`（BUILD=build/ws143-p005/rpi4）exit 0、warning 0。
- `config/ci/config-pcat.mk`（BUILD=build/ws143-p005/pcat）: hid-input.o・usb-hid.o は compile できたが **link が既存の理由で失敗**（`sandbox_create` などが未定義: pcat の vmunix.mk に `src/kern/sandbox.c` が無い。02cc81a10 以来。この Phase の変更ではない。Q1 に報告）。
- `python3 plan/tools/style-check.py`: hid-input.c・hid-input.h・hid-input-host-test.c・evdev-probe/main.c は 0 件。usb-hid.c は触った関数に 0 件（残りは触っていない関数の既存の指摘）。input.c の指摘は unregister の既存の物。
- `sh plan/ws143/tests/hid-input-host-test.sh`: PASS（160018 comparisons、1179517 events、0 failures。固定の 5 descriptor と 5330 の touchpad・Logitech の受信機 3 つ（if2 は旧も新も EOPNOTSUPP で断る）に 20000 report ずつ、register の情報（名前・path・uid・id・capability・axis・properties）の新旧一致、key の集約・ErrorRollOver・REL 0・touch だけ・ENOSPC からのやり直し・unpublish 2 回）。試験が誤りを捕まえることを、glue に故意の誤り（REL 0 の抑制を壊す）を入れた写しで確かめた（FAIL 5 件）。
- `sh plan/ws143/tests/hid-report-fuzz.sh`: PASS（1000000 descriptor、6942 受理、2395601 comparisons、7753406 events、0 failures、ASan・UBSan の報告なし、約 24 秒）。
- **未実施**: QEMU（T1: `hid-usb-p005.sh`、`plan/ws161/tests/fidoctl-p004.sh`（別の image `config-amd64-fidoctl.mk`）、`boot-test.sh`）。i2c-hid は触っていない（Q1）。実機は未実施。

## i01b の記録（2026-10-08、P2、q888）

ユーザーの決定（Q1 経由、2026-10-08 朝）: Q2 `INPUT_BRIDGE_GET_DEVICE` を足す（推しのとおり）。kernel では `input_bridge_ioctl()` の 1 か所が答える。

**作った物**:
- `include/uapi/input-bridge.h`（§3 の形。`struct input_bridge_setup` 4324 byte の `_Static_assert`、`struct input_bridge_device`、`INPUT_BRIDGE_GET_DEVICE`、group 'h'）。
- `src/drivers/generic/input-bridge.c`（cdev `bridge`、rdev `0x00130000`、root だけ、6 open、最初の write が setup、FIDO の page は ENXIO、続く write が report（0 か 512 超は EINVAL、宣言より短い物は EINVAL、長い物は受ける、decode の失敗は数えて size を返す）、read は EAGAIN、poll は POLLOUT だけ、close で unpublish と destroy）。名前が空なら `Bluetooth HID keyboard` などを kernel が付ける（bus が virtual なら `Virtual HID …`）。touch の device は `<name> Touchscreen`・`<path>/touch` を 64 byte に切る。
- `src/drivers/generic/input-bridge-setup.c`: setup の純粋な検査 `drv_input_bridge_setup_valid()`（host 試験のため node から分けた。設計の「input-bridge.c から切り出した純粋な関数」）。`include/drivers/generic/input-bridge.h`。
- glue に `drv_hid_input_report_short()`（report ID ごとの宣言の長さ。report の状態に `minimum_size` を持つ）。
- build: `Makefile` に `CONFIG_INPUT_BRIDGE ?= $(if $(filter amd64,$(ZEDBSD_PLATFORM)),y,n)` と `-DCONFIG_INPUT_BRIDGE`（設計の「既定 y、amd64 だけが見る」を、platform で既定を決める形にした: pcat・arm64 は n で flag も付かず、vfs.c の登録も入らない）。`platform/amd64/vmunix.mk` に input-bridge.c・input-bridge-setup.c と、USB_HID が n の時の hidraw-describe.c。HID の source の条件に `CONFIG_INPUT_BRIDGE`。`src/kern/vfs.c` の `#ifdef CONFIG_INPUT_BRIDGE` で登録、`src/kern/devfs.c` で 0600（既存の input-inject の行と同じ形の if の鎖に足した。鎖の既存の call-in-condition はそのまま）。
- `userland/tests/input-bridge-probe/`（`type`・`misuse`・`open`）。`config-amd64-bt.mk` に input-bridge-probe と systemevents。
- host 試験 §7.3: `hid-input-host-test.c` に setup の検査（正しい物、virtual、magic・version・USB の bus・descriptor 0 と 4097（4096 は可）・reserved・3 つの text の NUL 無し、63 byte の名前）と短い report の判定。
- T1 の試験 `plan/ws143/tests/input-bridge-p005.sh`（§7.5。6 枚の keyboard と PS/2・USB の device が同時に register できることは、旧の 8 を超えるので B1 の確かめになる。inject の device はこの image に無いので使わない）。

**確かめ（P2、host）**: `config/ci/config-amd64.mk`・`config-amd64-bt.mk`（vmunix、input-bridge-probe）・`config-rpi4.mk` は exit 0、warning 0。pcat は compile の error・warning 0、link は既知の sandbox の未定義で失敗（Q1 の既知）。style-check: input-bridge.c・input-bridge-setup.c・input-bridge.h（uapi・driver）・input-bridge-probe・試験の C は 0 件。`hid-input-host-test.sh` PASS（setup と短い report の検査を含む）、`hid-report-fuzz.sh` PASS。**未実施**: QEMU（T1: `input-bridge-p005.sh`）。

## i01c の記録（2026-10-08、P2、q888。ユーザーの決定（Q1 経由）: 「i2c-hid も共有の glue に乗せる」、推しと逆）

touchpad の経路の作り替え。host で旧と新の event 列を突き合わせ（i01a と同じ方法、5330 の touchpad の descriptor）、実機の回帰は UAT。設計 §5.2 の文言は元のまま。

**作った物**:
- glue: `struct hid_input_identity` に `flags`、`HID_INPUT_TOUCH_ONLY`（touch の device だけを登録し、main device（Precision Touchpad の mouse の collection）は登録しない。その report は main device が無いので何も出ない = 旧の「touch でない report は捨てる」と同じ）。`drv_hid_input_touch()`（layout の touch の情報）、`drv_hid_input_feature()`（feature report の field の位置、touchpad の mode の設定に使う）。
- `src/drivers/i2c/i2c-hid.c`: layout・touch の description・状態機械・decode の作業領域・input device の field を `struct hid_input *hidinput` に置き換えた。`read_report_descriptor` は `drv_hid_input_prepare`、`device_start` の touch の判定は `drv_hid_input_touch`、`set_feature` は `drv_hid_input_feature`、`publish` は identity（名前「VVVV:PPPP Touchpad|Touchscreen」、path は ACPI の path、unique_id は NULL、BUS_I2C、`HID_INPUT_TOUCH_ONLY`）で `drv_hid_input_publish`、`take_report` は `drv_hid_input_report`（時刻は読んだ時の `clock_milliseconds`）。I2C・ACPI・GPIO・IRQ の部分は変えていない。
- 振る舞いの違い（意図した物）: (1) glue の prepare は旧の parse より厳しい（report の数 0・32 超、capability 257 超、axis 65 超、report の長さ 0・1025 超で断る）。実の touchpad では起きない見込みで、fuzz では 0 件（`i2c stricter` で数える）。(2) 壊れた report と touch の状態機械の失敗に kernel の log の行（最初の 16 回、`hid-input: malformed input <path> …`）が出る（旧は黙って捨てた）。(3) touch の describe が失敗した時の errno は ENODEV（旧は describe の errno）。
- 試験: `hid-input-old.c` に旧の i2c-hid の parse・touch の判定・publish・take_report の写し（git aef0dead1）、`hid-input-host-test.c` に `compare_i2c_side_by_side`（旧の i2c-hid と、touch だけを出す glue を、登録の情報・main device が無いこと・event の列で突き合わせる。check と fuzz の両方）と、5330 の touchpad の指の report の生成（5 本の指の down・move・lift、端の外、Scan Time、Contact Count（時に誤り）、button）。`plan/ws159/tests/run-host-i2c-hid.sh`・`host-i2c-hid.c` を追従（hid-input.o を link、`kern_malloc`・`drv_input_device_unregister`・NULL の device の emit を捨てる stand-in。既存の壊れ（ws183-p001 以来の `kern_irq_*` の未定義）も stand-in を足して直した）。

**確かめ（P2、host）**: `config/ci/config-amd64.mk`・`config-amd64-bt.mk` の vmunix は exit 0、warning 0。style-check: i2c-hid.c・hid-input.[ch]・試験の C は 0 件（host-i2c-hid.c の setjmp の既存の 1 件は除く）。`hid-input-host-test.sh` PASS（200026 comparisons、1488468 events のうち i2c の突き合わせ 151044、0 failures、i2c stricter 0。5330 の touchpad は旧の i2c-hid と glue が 20000 report で一致、touch screen も一致）。`hid-report-fuzz.sh` PASS（1000000 descriptor、0 failures、i2c stricter 0）。`run-host-i2c-hid.sh` は line 74・sample 81・irq 75 checks で ok（phase006 の記録と同じ数）、ASan・UBSan でも ok。**未実施**: QEMU（i2c の device は QEMU に無い。boot-test の login prompt だけ T1 で）、実機（5330 の touchpad、UAT）。

## 再開点（次の担当がこの file だけで i01a に入れる形）

### i01a: kernel の glue の refactor と USB の回帰

1. 読む: この file の §2・§7.1・§7.2・§7.5 の `hid-usb-p005.sh`・§9.1・§9.10、`plan/coding-style.md` の全文、`src/drivers/usb/usb-hid.c`（事実 1・2 の行）、`include/kern/input-device.h`、`src/drivers/generic/input.c` 40・106・252〜376・654〜869 行、`plan/ws079/tests/run-hid-pen.sh`（freestanding の flag と stand-in の形）。
2. 作る順:
   1. `include/drivers/generic/hid-input.h`・`src/drivers/generic/hid-input.c`（§2 の口。usb-hid.c の 653〜797 の parse 以降・1012〜1167・1187〜1221・1427〜1559 の `info`・1588〜1653 を移す。作業領域は struct の中（S8）。振る舞いの列挙（§2）を変えない）。
   2. `input.c`: `INPUT_DEVICE_MAX` 32、`drv_input_device_number()`（header に宣言。HAL ではない）。
   3. `usb-hid.c`: glue を呼ぶ形に。raw（FIDO）の分岐（689〜703・1053〜1057・1203〜1205・1476〜1480 行）は**触らない**。名前の fallback は `drv_hid_input_kind()` で。動かす code と触った関数は規約の全文に合わせる。
   4. build の規則: `platform/amd64/vmunix.mk` の `AMD64_HID_SOURCES` と `platform/pcat/vmunix.mk` の `PCAT_USB_CLASS_OBJS` と `platform/arm64/vmunix.mk` の `ARM64_USB_SOURCES` に hid-input。
   5. `userland/tests/evdev-probe/`（`-b bluetooth|usb|virtual`、`-n NAME`、`-t MS`: node を待ち、EVIOCGID・EVIOCGNAME・EVIOCGBIT を出し、event を `EVDEV type=.. code=.. value=..` の行で出し、ENODEV で `EVDEV gone` と終わる。root で動く。`userland/tests/package.mk` と `plan/ws143/tests/config-amd64-bt.mk` の `ZEDBSD_USER_PROGRAMS` に足す）。
   6. host 試験 `plan/ws143/tests/hid-input-host-test.{sh,c}`（§7.1。旧の `usb_hid_publish_report`・`usb_hid_fetch_layout` の記述の部分の写しを試験の中に置き新旧一致。register の情報も比べる）、`hid-report-fuzz.{sh,c}`（§7.2。`-fsanitize` が freestanding で通らなければ stand-in の `kern_malloc` に赤帯）。
   7. T1 の script `plan/ws143/tests/hid-usb-p005.sh`（§7.5。QMP の `qmp_capabilities` → `input-send-event`（`device` 付き）・`device_add`・`device_del` を python の `socket`・`json` で送る小さな関数を script に持つ。guest は `--qemu-extra` で usb-kbd id=hidkbd・usb-tablet id=hidtab・usb-mouse id=hidmouse）。
3. 確かめ（自分で）: build warning 0 を `config/ci/config-amd64.mk`・`plan/ws143/tests/config-amd64-bt.mk`・`config/ci/config-pcat.mk`・`config/ci/config-rpi4.mk` の 4 つ（`BUILD=build/ws143-p005/<config>`、各 20 分の上限）、`python3 plan/tools/style-check.py` で触った file が 0、host 試験 2 つ PASS。
4. T1 に依頼（Q1 経由）: image は `plan/ws143/tests/build-bt-image.sh`。script は `hid-usb-p005.sh`、回帰に `plan/ws161/tests/fidoctl-p004.sh`（hidraw の class）と `plan/tools/boot-test.sh`（login prompt）。T1 の結果を待たずに i01b へ進んでよい（Phase の clearance は Q1 の判定まで待つ）。
5. 受け入れは §「attempt の区切り」の i01a の行。T1 の FAIL は §2 の危険 (a)〜(d) のどれかに当てて記録する。

### i01b 以降

- i01b: §3 と §7.3・§7.5 の `input-bridge-p005.sh`。Q2 の決定が無ければ `INPUT_BRIDGE_GET_DEVICE` を入れ、kernel・daemon・試験の 1 関数ずつに閉じる。`CONFIG_INPUT_BRIDGE`（§2 の配置、Q24）。
- i02（BR/EDR。p004 の cleared の後）→ i03（LE）→ i04（実機の gate、Q20）。
- Q1（調整役）へ: Q20・Q23 の判断、tshark を host に入れるか、Future Work（LED の出力、i2c-hid の乗せ替え、sessiond の hotplug の chown と `KWL_INPUT_MAX`）の登録。

## Q1 の判断（2026-10-08）

- Q20: i04（実機の短い確認）を p006 の前の必須の門にする（推しのとおり、ユーザーの device が要る）。
- Q23: sessiond の hotplug の穴は BUG-264 として立て、i01a の前に P2 の次の世代が再現と直しを行う（login の後の USB キーボードにも効くため優先）。
- S11 の tshark: btsnoop の外部の照合が要る時に host に入れる（sudo の package の導入は許可の範囲）。
- Future Work: LED の出力、i2c-hid の乗せ替え（Q1 の判断待ち）を F に記録。

## ユーザーの決定（2026-10-08 朝、クリックの回答）

- Q1: i2c-hid も共有の glue（hid-input）に乗せる（推しと逆）。touchpad の経路の作り替えなので実機の回帰が要る。F-084 は実施へ。
- Q2: INPUT_BRIDGE_GET_DEVICE を足す。
- Q4: ペアリングの後に自動で接続する。
- Q5: 人が切断した機器からの再接続は断る。
- B6（p004 の Q4）: account が無ければ bluetoothd は起動しない（暫定のまま確定）。

## T1-419（2026-10-08 Q1）

i01a: hid-usb-p005 PASS、boot-test PASS、fidoctl-p004 PASS（USB の HID の乗せ替えの回帰なし）。

## ユーザーの決定（2026-10-08 朝）: 名前の変更

`/dev/hid-host` → `/dev/input/bridge`、UAPI も揃える（include/uapi/hid-host.h → input-bridge.h、HID_HOST_* → INPUT_BRIDGE_*、struct hid_host_* → input_bridge_*、cdev・driver の file の名前も合わせる）。`/dev/btN` → `/dev/bluetoothN`（node の名前だけ。bluetooth.h・BT_IOC_* はそのまま）。i01c の前に行い、試験・文書・bluetoothd・bt-probe・hid-host-probe（→ 名前を合わせる）を追従させる。

**行った（P2、q888、2026-10-08）**: `include/uapi/hid-host.h` → `include/uapi/input-bridge.h`（`INPUT_BRIDGE_*`・`struct input_bridge_setup`・`struct input_bridge_device`・`INPUT_BRIDGE_GET_DEVICE`・group は 'h' のまま）、`include/drivers/generic/input-bridge.h`、`src/drivers/generic/input-bridge.c`・`input-bridge-setup.c`（cdev 名 `bridge`、rdev `0x00130000`）、`CONFIG_INPUT_BRIDGE`、devfs の `bridge_name()`（`/dev/input` に置き、0600。root からの `/dev/bridge` は見えない）、`userland/tests/input-bridge-probe`（行の頭は `BRIDGE`）、`plan/ws143/tests/input-bridge-p005.sh`。Bluetooth の node は cdev 名 `bluetoothN`（`bt-hci.c`）、devfs の `bluetooth_name()` は `bluetooth` と数字、bluetoothd の privsep の発見・`bt-probe` の既定（`/dev/bluetooth0`）・usage の文、試験の script（bt-loopback-p002・bt-daemon-p003・bt-pair-p004）、`config-amd64-bt.mk` の注記。文書: phase001〜005・ws.md・design.md の記述を追従した（この節と review の file は履歴として元のまま）。daemon の関数の名前は `btd_bridge_numbers()`（未実装、i02）。
確かめ: `config-amd64-bt.mk` の vmunix と input-bridge-probe・bt-probe・bluetoothd・bt・evdev-probe、`config/ci/config-amd64.mk` の vmunix は exit 0、warning 0。`hid-input-host-test.sh`・`hid-report-fuzz.sh`・`bt-daemon-host-test.sh`（daemon・pair・link）PASS。style-check 0（devfs.c の既存の指摘は除く）。QEMU は T1（`input-bridge-p005.sh` と bt-loopback-p002・bt-daemon-p003・bt-pair-p004 の回帰）。

## q896（ws143-p006）からの注記（2026-10-08、P1 の design review I4、Q1 が転記）

Bluetooth の off は今は daemon の flag だけ。p005 の自動の再接続・page scan・LE の自動接続は off を守ること（off の間は再接続しない、page scan を止める）。

## i02 の途中（2026-10-08 夜、P1 q904、BUG-267 の割り込みで区切った）

- 方針: i02 を 4 つの commit の単位に分ける: i02a 純粋な部品（sdp・hidp・att・hidcache・snoop と host 試験）→ i02b router・l2cap の拡張・pair の handoff → i02c hid.c・privsep・main・protocol・bt の CLI → i02d loopback の作り直しと bt-hid-p005.sh、T1 の依頼。
- ここまで（未試験、Makefile に未登録）: `userland/base/bluetoothd/sdp.[ch]`（ServiceSearchAttributeRequest の組み立て、continuation の繋ぎと 8 回の同じ continuation で protocol、data element の検査（深さ 8）、HID・PnP の record の読み）、`hidp.[ch]`（header）、`att.[ch]`（request の組み立て、PDU の解析、最小の server の答え）。host の cc で -Wall -Wextra -Werror の compile だけ通した。
- 次: hidcache・snoop、`bt-daemon-host-test.sh` に i02a の host 試験（§7.4 の sdp・hidp・att・att の server・hidcache・snoop）、Makefile への登録。

## i02a の記録（2026-10-08 夜、P1 q904）

- 純粋な部品（system call は hidcache・snoop の file の読み書きだけ）:
  - `sdp.[ch]`: ServiceSearchAttributeRequest（UUID 0x1124 か 0x1200、全属性、MaximumAttributeByteCount 0x0280）、Response の断片の繋ぎ（8 KiB まで、超えたら `descriptor`）、同じ continuation 8 回で `protocol`、data element の検査（長さ・型ごとの size・深さ 8）、HID の record（PSM 0x0011・0x0013 の確かめ、flags・subclass・country・名前・descriptor 4096 まで（超えたら E2BIG））、PnP の record。
  - `hidp.[ch]`: header の組み立てと解析、DATA の 512 byte 超は E2BIG（`oversize` の材料）。
  - `att.[ch]`: request の組み立て（MTU・範囲・Read/Blob・Write・Confirmation・Error）、PDU の解析（list の要素の数と長さの整合、format 1・2）、最小の server（MTU 185、発見と読みは Attribute Not Found、書きは Request Not Supported、壊れた request は Invalid PDU、command・response・notification・confirmation には答えない）。
  - `hidcache.[ch]`: `<address>-<型>.hid`（state・transport・flags・PnP・class・appearance・名前・descriptor の 128 byte の hex の行 32 まで）、一時 file → fsync → rename、欠け・重複・長さの不一致・state か transport の無い記録は EBADMSG、`btd_keys_list` は `.hid` を飛ばす（試験で確かめた）。
  - `snoop.[ch]`: btsnoop（`btsnoop\0`、version 1、datalink 1002）の header と record、file の open と追記。
- Makefile に 5 つの source を登録（main からはまだ呼ばない）。
- 試験: 新 `plan/ws143/tests/bt-hid-host-test.c`（`bt-daemon-host-test.sh` に追加）75 checks: §6 の loopback のキーボードの HID・PnP の record（1 回の応答と 60 byte の continuation、繰り返す continuation、別の interrupt PSM、descriptor 無し、4097 byte、9 段・8 段の入れ子、holder を越える element、別の transaction・Error Response・短い PDU・parameters を越える count）、HIDP、ATT の組み立て・解析・server、hidcache の書き読み（4096 byte の descriptor、短い最後の行、欠け・重複・切れ・size だけ・state 無し・key の重複、bond の横）、snoop の byte、固定の seed の fuzz 20000 回（SDP の応答・ATT の PDU）。ASan・UBSan で PASS。既存の bt-daemon 90・bt-pair 143・bt-link 44 も PASS。
- build: `make -j16 BUILD=build/p1-bt ZEDBSD_CONFIG=plan/ws143/tests/config-amd64-bt.mk build/p1-bt/bin/bluetoothd` warning 0。style-check: 新しい 10 file で指摘 0。
- 次（i02b）: router（§4.1）、l2cap の inbound・Pending・option・Echo・opened/closed（§4.2）、pair の handoff（§9.2）。その後 i02c（hid.c・privsep の OPEN-HID・main・protocol・bt）、i02d（loopback の作り直しと bt-hid-p005.sh、T1 の依頼）。

## i02b の途中の記録（2026-10-08 夜、P1 q904。BUG-270 の割り込みで区切った）

- 作った物:
  - `router.[ch]`（新、§4.1・§9.6）: session の handler。Connection Complete（LE の 0x01・0x0A も）で address の持ち主（pair が pairing 中の address → pair、hid の `claims` → hid、どちらでもなければ HCI Disconnect 0x13）を決め、handle → 持ち主の表（8）を持つ。ACL・handle の event（Auth Complete・Encryption Change・Key Refresh・Mode Change・Remote Features/Version・Max Slots・Supervision・LE の Update・Features・LTK Request）は持ち主へ、Disconnection Complete は持ち主へ渡してから表から消す。Connection Request は hid の `wants` なら hid、他は Reject 0x0F（pair の相手も今までどおり Reject）。Link Key Request は pair → hid（`wants`）→ Negative。pairing の event（IO Capability Request/Response、Confirmation、Passkey Request/Notification、Keypress、PIN、Simple Pairing Complete、Link Key Notification）は pair がその address を pairing している時だけ pair へ、他は Negative（IO 0x18・Confirmation・Passkey・PIN）、Link Key Notification は保存せず数える（`keys_dropped`）。hid の口は `struct btd_router_hid`（wants・claims・handle、NULL は「無い」）と `btd_router_assign`・`btd_router_owner`・`btd_router_clear`（`btd_close` から）。
  - `pair.[ch]`: Connection Request の Reject を router へ移した。`btd_pair_owns`、handoff（`btd_pair_handoff_fn`、`btd_pair_set_handoff`。設計の「`btd_pair_init` で渡す」は呼び手を変えないよう setter にした）。`pair_succeed` が鍵の保存の後に bond を読み直して handoff を呼び、1 なら切らずに handle を忘れ（l2cap・組み直しも捨てる）、PAIRED を報告する。
  - `l2cap.[ch]`（§4.2・§9.8）: accept の hook（`btd_l2cap_set_accept`、結果 success・pending・拒否、無ければ PSM not supported）、相手の source CID の検べ（0x0006・0x0007）、表が満ちれば No resources、Pending の channel（`BTD_CHANNEL_PENDING`、相手の identifier を保つ）と最終の答え `btd_l2cap_answer_pending`（success なら自分の Configure Request が続く）、option（Flush Timeout と QoS は記録して受ける、RFC は basic だけ、他の mode は Unacceptable で basic を返す、FCS は受ける、答えの buffer 48）、`btd_l2cap_echo` と Echo Response の照合、effect の `opened`・`closed`（理由 remote・local・refused）・`echo`。相手からの Disconnection・Configure は同じ handle の channel だけ。
  - `main.c`: handler を router に、`btd_close` で `btd_router_clear`。Makefile に router.c。
- 試験: `bt-link-host-test.c` の handler を router にし（「相手から始まる pairing の拒否」は router の数え）、`test_router` を足した（wants の device の Connection Request・Link Key Request は hid へ、他人は Reject・Negative、bond 済みの HID の IO・Passkey は Negative で Link Key Notification は捨てる、claims の接続の event・ACL・切断と表、誰のものでもない接続の Disconnect、handoff を断る（今までどおり切る）・受ける（切らずに hid の持ち物）。`OUT=build/p1-bthost sh plan/ws143/tests/bt-daemon-host-test.sh` → daemon 90・pair 143・link 56・hid 75 PASS（ASan・UBSan）。build: `make -j16 BUILD=build/p1-bt ZEDBSD_CONFIG=plan/ws143/tests/config-amd64-bt.mk build/p1-bt/bin/bluetoothd` warning 0。style-check: 触った file で 0。
- 未: l2cap の新しい口の host 試験（pending・option・echo・opened/closed）は i02c の前に bt-pair-host-test に足す。次は i02c（hid.c・privsep の OPEN-HID・main・protocol・bt）。

## i02b の記録（2026-10-08 夜、P1 q904）

- 上の「i02b の途中の記録」に続けて: l2cap の新しい口の host 試験 `test_l2cap_inbound`（`bt-pair-host-test.c`、18 checks: PSM 0x11 の accept と自分の Configure Request、Flush Timeout・QoS・FCS の受け、opened の通知、PSM 0x13 の Pending と `btd_l2cap_answer_pending` の success・Security Block、Pending の channel の Configure の断り、source CID 0x0001 の 0x0006・重複の 0x0007、hook が答えない PSM の 0x0002、ERTM の Unacceptable と basic の提示、別の handle からの Disconnection の無視と同じ handle の closed（remote）、Echo の identifier の照合、表が満ちた時の No resources）。privsep の `OPEN-HID`（`btd_privsep_open_bridge`。親は `/dev/input/bridge` を `O_RDWR|O_CLOEXEC` で開いて `OK /dev/input/bridge` と SCM_RIGHTS、自分の写しを閉じる。失敗は `ERR errno`。子の側の要求と答えの読みは `privsep_ask` に共通化し、`OPEN` の振る舞いは同じ）。privsep は host 試験が無い（fork と root が要る）ので QEMU（T1）で確かめる。
- 確かめ: `OUT=build/p1-bthost sh plan/ws143/tests/bt-daemon-host-test.sh` → daemon 90・pair 161・link 56・hid 75 PASS。`build/p1-bt/bin/bluetoothd`（config-amd64-bt.mk）warning 0。style-check 0。
- i02b の受け入れの残り: QEMU の p002〜p004 の回帰（router に替えた daemon で `bt-loopback-p002.sh`・`bt-daemon-p003.sh`・`bt-pair-p004.sh` が変更なしで PASS）は i02d の T1 の依頼にまとめる（早く確かめたい時は Q1 が先に T1 に流してよい: image は `plan/ws143/tests/build-bt-image.sh BUILD`）。

## i02c の再開点（次の担当がここから入る）

作る物（設計 §4.7〜§4.12・§5・§9.2・§9.3・§9.5・§9.7・§9.8）。BR/EDR だけ（LE・HOGP・auto-connect・resume は i03）。

1. `userland/base/bluetoothd/hid.[ch]`（新）:
   - 表 `struct btd_hid_device hid[BTD_HID_MAX 6]`（§4.7 の field。BR/EDR の分: address・state（IDLE・CONNECTING・AUTHENTICATING・ENCRYPTING・SDP・CHANNELS・SETUP・OPEN・CLOSING）・handle・encrypted・key_size・`btd_reassembly`・`btd_l2cap`（device ごと、§9.8 の accept の hook を `btd_l2cap_set_accept` で）・sdp/control/interrupt の local CID・`btd_sdp`・bridge の fd・`btd_hidcache`（descriptor）・setup の前の DATA の待ち行列 32・wanted・page の回数と次の時刻・counters（reports・malformed・oversize）・since・last_error・CONNECT の client）。
   - router の hook: `wants`（`.hid` の記録があり wanted の address、Q5 の `btd_hid_policy_after_disconnect`）、`claims`（自分が page した・accept した address）、`handle`（hid の packet: Connection Request → Accept 0x0409 role 0x00、Connection Complete、Link Key Request → bond の鍵で Reply、Authentication Complete、Encryption Change、Disconnection Complete、ACL → l2cap・SDP・HIDP）。
   - pair の handoff: `btd_hid_handoff`（`btd_pair_set_handoff` に渡す。§9.2 の判定 `btd_hid_policy_after_pair(class)`: scan の表（`session->devices`）の class の major が Peripheral 0x05 なら 1。表に作り、`.hid` を candidate で書き、`btd_router_assign(handle, BTD_OWNER_HID)`、L2CAP の SDP（PSM 1）へ）。
   - CONNECT の流れ（§4.7: Create Connection → Authentication Requested → Link Key Request Reply → Encryption → Read Encryption Key Size 16 → L2CAP PSM 1 → SDP HID（`btd_sdp_*`）と PnP → disconnect → PSM 0x11 → 0x13 → SET_PROTOCOL（boot_device の時）→ OPEN-HID → `struct input_bridge_setup` の write → `INPUT_BRIDGE_GET_DEVICE`（daemon の `btd_bridge_numbers()` の 1 関数に閉じる）→ `.hid` を confirmed で → OPEN、待ち行列の DATA を流す）。DATA（INPUT）は header を外して write、512 超は oversize、EINVAL は malformed。
   - 相手からの再接続（Connection Request → Accept、相手が認証しない時の L2CAP の Pending → Authentication Requested → Set Connection Encryption → Read Key Size → `btd_l2cap_answer_pending(SUCCESS)`、失敗・10 秒で SECURITY_BLOCK）。`.hid` の cache の descriptor で setup。
   - 切断（fd を close して kernel が key を離す）、DISCONNECT（L2CAP の interrupt → control の Disconnect、HCI Disconnect 0x13、wanted=0）、FORGET の時の VCU、lost（`btd_hid_lost`: fd を閉じ表を IDLE に、`btd_router_clear` は main）、timeout（§4.7）、PAIR・SCAN との排他（`btd_hid_hold`）、page scan（Write Scan Enable の bit 1、`.hid` が 1 つでもあれば。p006 の off（`btd_powered_off`）の間は page scan も再接続もしない、§「q896 からの注記」）、page の再試行 5→10→20→40→60 秒・10 回で paused。
2. `main.c`: `btd_hid_init`（router と pair の hook を渡す）、poll の timeout に `btd_hid_deadline`、`btd_hid_tick`、`btd_close` で `btd_hid_lost`、READY で `btd_hid_refresh`。口: `CONNECT`・`DISCONNECT`・`STATUS`（§5 の行の形）、SHOW の末尾 `hid=… page_scan=… le_auto=0`、PAIR の busy・同じ address の切断、FORGET の `.hid` の削除と VCU。
3. ~~`userland/base/bt/main.c`: `bt connect|disconnect|status`（§5）~~ 済み（2026-10-08 夜 P1: 最後の行 `BT CONNECT result=connected|error input=…|-`・`BT DISCONNECT result=ok|error`・`BT STATUS devices=N open=M`。daemon の verb が入るまでは `ERROR request` で error。build warning 0、style 0）。
4. host 試験（§7.4 の BR/EDR）: `bt-link-host-test.c` の偽の controller に HID の相手（SDP の record・channel・DATA）を足すか、新しい `bt-hidhost-host-test.c`。`/dev/input/bridge` は host に無いので、bridge の open と write は hook（privsep の代わりに socketpair か file）で受ける形にする。
5. その後 i02d: loopback の link ごとの作り直しと 01・02 の HID（§6）、`bt-hid-p005.sh`、T1 の依頼（p002〜p004 の回帰を含む）。

## i02c の途中の記録（2026-10-08 夜、P1 q904。context の上限で区切った）

- 作った物（e6d5523b8、build warning 0、host 試験は未）:
  - `hid.[ch]`（新、BR/EDR だけ）: 表 6、状態 IDLE・PAGING・AUTHENTICATING・ENCRYPTING・SDP・CHANNELS・HANDSHAKE・OPEN・CLOSING。CONNECT（bond の鍵が無ければ `not-bonded`、表が満ちれば `limit`、OPEN なら CONNECTED をそのまま、流れの途中・hold 中・他の page 中は `busy`）→ Create Connection → Authentication Requested → Link Key Request に bond の鍵 → Set Connection Encryption → Read Encryption Key Size 16 → L2CAP PSM 1 → SDP の HID record（無ければ `.hid` を消して `no-hid`、4096 超は `descriptor`）→ PnP → SDP を閉じて PSM 0x11 → 0x13 → boot device なら SET_PROTOCOL(report) と HANDSHAKE（2 秒で諦めて進む）→ OPEN-HID → `struct input_bridge_setup`（bus 5、名前は UTF-8 の境で 51 byte、physical_path `bluetooth/<controller>/<address>`、unique_id は address）→ `INPUT_BRIDGE_GET_DEVICE` → `.hid` を confirmed で → OPEN と CONNECTED の行、待ち行列（32 × 64 byte）の report を流す。相手からの接続: router の wants → Accept Connection Request（role 0x00）→ 相手の認証か、暗号化の前の channel の要求（l2cap の accept の hook が Pending、hid が Authentication Requested を始め、Encryption Change → Key Size 16 → `btd_l2cap_answer_pending(SUCCESS)`、失敗は SECURITY_BLOCK）→ descriptor の cache があれば相手の channel を待つ、無ければ SDP。interrupt の DATA(INPUT) を bridge に write（512 超は oversize、kernel の断りは malformed）。control の VCU で bond と `.hid` を消す（Q9）、GET/SET_REPORT・GET/SET_PROTOCOL は HANDSHAKE UNSUPPORTED。切断で bridge を閉じる（kernel が key を離す）、wanted で host が page する device は 5→10→20→40→60 秒で 10 回、後は paused。DISCONNECT（interrupt → control の L2CAP Disconnect、HCI Disconnect 0x13、wanted=0）、FORGET（VCU と切断、`.hid` を消す）、lost（全部の bridge を閉じて IDLE、wanted は保つ）、refresh（bond と `.hid` から表を作り、`.hid` が 1 つでもあれば Write Scan Enable 0x02）、handoff（class の major が Peripheral 0x05 の BR/EDR、`.hid` を candidate で書き router の持ち主を HID に、SDP から）、policy（Q4: pair の後に繋ぐ、Q5: 人の切断の後は戻さない）。
  - `main.c`: HID host の初期化と router の hook（wants・claims・handle）、READY で refresh（off でない時）、`btd_close` で lost、loop で hold（pairing か scan の間）と tick、timeout に deadline、口 `CONNECT`・`DISCONNECT`・`STATUS`、SHOW の CONTROLLER の行の末尾 `hid=N page_scan=0|1 le_auto=0`、PAIR は HID の流れの途中なら busy、FORGET で `btd_hid_forget`、client の close で `waits_connect` を下ろす（流れは続く）。**pair の handoff はまだ渡していない**（loopback が SDP に答えるのは i02d。今渡すと p004 の 01 の pairing の後に SDP が 10 秒待ちになり、p004 の試験の 01 の再 pairing が busy で落ちる）。
- 確かめ: `make … build/p1-bt/bin/bluetoothd`（config-amd64-bt.mk）warning 0、style-check 0、既存の host 試験（daemon 90・pair 161・link 56・hid 75）PASS。**hid.c の host 試験は未**。
- 次の担当の順: (1) hid.c の host 試験（`bt-link-host-test.c` の偽の controller に HID の相手を足す: L2CAP の Connection・Configure・Disconnection の答え、PSM 1 の SDP の応答（`bt-hid-host-test.c` の `build_hid_lists` の形）、PSM 0x11・0x13、DATA。bridge は hook の `open_bridge` に socketpair を渡し、write された setup と report を読む）: CONNECT の全部の流れ、not-bonded・limit・busy、key-missing・key-size、no-hid、相手からの接続と Pending、DATA の待ち行列、DISCONNECT・FORGET の VCU・lost、retry と paused。(2) i02d: loopback の link ごとの作り直しと 01・02 の HID（§6）、`btd_pair_set_handoff(&btd_pairing, btd_hid_handoff, &btd_hid_host)` を main.c に足す、`bt-hid-p005.sh`、T1 の依頼（p002〜p004 の回帰を含む）。

## i02c の記録（2026-10-08 夜、P1 q904 の新しい世代）

- host 試験 `plan/ws143/tests/bt-hidhost-host-test.c`（新、`bt-daemon-host-test.sh` に追加）71 checks: hid.c・router・session・l2cap・sdp を host の cc（ASan・UBSan）で、socketpair の偽の controller が HID の機器を 1 台演じる（page・認証・暗号化・鍵の長さ、L2CAP の channel の受けと要求、SDP の HID・PnP の record（continuation で分割）、HIDP の SET_PROTOCOL・HANDSHAKE・VCU・DATA）。bridge は socketpair（試験が setup と report を読む）、`INPUT_BRIDGE_GET_DEVICE` は試験の `ioctl`（event7）。部分: connect（全段・setup の byte・channel と一緒に来た report の待ち行列と流し・601 byte の report の oversize・GET_REPORT に UNSUPPORTED・DISCONNECT（interrupt → control → HCI Disconnect、wanted=0）・CONNECT のやり直し）、refusals（not-bonded・busy（流れの途中と hold）・key-missing・key-size 7・unreachable の後 waiting・no-hid・descriptor 4097・input-full（ENOSPC）・setup の EINVAL 2 回のやり直しと 4 回で諦め・page の timeout・handshake が来ない時の 2 秒の後の setup・待ち行列 32 と溢れ 8）、inbound（record の cache のある相手から: Accept の role 0x00、認証の前の channel の要求 → Pending → host の認証・暗号化 → 成功、SDP 無しで open、相手の切断で waiting（page しない）、相手が先に認証する形の再接続、DISCONNECT の後の Connection Request は Reject）、handoff（Peripheral の class は同じ link で SDP → open・record は confirmed、Computer の class は断り record 無し）、lifecycle（page の再試行 5・10・20・40・60 秒、10 回で paused、CONNECT で再開、表の 6 の後の limit、lost で bridge を閉じ waiting、相手の VCU（bond と record を消し VCU を返さない）、FORGET の VCU）。
- 試験が見つけて直した hid.c の誤り: (1) SDP・HIDP の frame を host 自身の local CID に送っていた（`hid_send` が local CID を channel の remote CID に写す形に）、(2) 新しい slot の record に address・型が無く `.hid` が別の名前で書かれていた、(3) 512 byte を超える DATA（hidp の E2BIG）を oversize に数えていなかった、(4) CONNECT の名前が bond の名前でなかった（§4.7: bond の name が先）。
- review で直した物: OPEN の link の Encryption Change（鍵の更新）で流れをやり直していた → AUTHENTICATING・ENCRYPTING の時だけ進む。相手から繋いだ device の SDP の後に自分で control を開き二重になり得た → 相手が求めた control を探して使う。相手の VCU に VCU を返していた → `hid_drop(unplug=0)`。流れの途中の device の Connection Request に答えなかった → Reject 0x0F。setup の EINVAL のやり直し（§4.7、100 ms × 3、tick から）が無かった → 状態 `BTD_HID_SETUP` を足した。
- 確かめ: `OUT=build/p1-bthost sh plan/ws143/tests/bt-daemon-host-test.sh` → daemon 90・pair 161・link 56・hid 75・hidhost 71 PASS。`make … build/p1-bt/bin/bluetoothd`（config-amd64-bt.mk）warning 0。style-check（hid.c・hid.h・試験）0。
- 残り（i02 の中、未）: `.hid` の bond の無い記録の refresh での削除（§4.9）、page の timeout の Create Connection Cancel（今は Connection Complete が後で来れば router が切る）。i02d へ。

## i02d の記録（2026-10-08 夜、P1 q904）

- daemon（454a18eb3・6e7ec702d）: `main.c` で `btd_pair_set_handoff(&btd_pairing, btd_hid_handoff, &btd_hid_host)`（Peripheral の class の device は PAIRED の後に同じ link で SDP → channel → open）。PAIR の前に同じ address の HID の device を離す `btd_hid_release()`（link を切り `.hid` を消し、bond は残す。pairing の引き継ぎが作り直す、review B7）。handoff の後に page scan を立てる（`hid_page_scan()`、§9.2: 相手から繋ぎ直す device のため。refresh と共通）。btsnoop: session に packet の trace の hook（`packet_trace`、書いた・読んだ H4 の packet）、`bluetoothd -s FILE`（子が開いて全 packet を `snoop.c` で追記、§9.11）。引数は `-f NODE`・`-s FILE` の組を順不同で。
- loopback（`src/drivers/generic/bt-hci-loopback.c`、6e7ec702d・037c48a42）: link ごとの状態 `struct loopback_link` を 3 つ（01 → 0x0040、02 → 0x0043、05・06・07 → 0x0045）。handle で来る command（Authentication Requested・Set Connection Encryption・Read Encryption Key Size・Disconnect）は handle の device、address で来る command は address の device を選ぶ（`loopback_select`）。IO・鍵の種類・暗号化は link ごと。01・02 は HID の相手: L2CAP（Connection・Configure（MTU 672 と Flush Timeout 0xFFFF を付ける）・Disconnection・Echo・Information）、SDP（HID と PnP の record を 60 byte の断片と 1 byte の continuation で。record は host の `sdp.c` で読めることを確かめた byte 列）、control の SET_PROTOCOL に HANDSHAKE SUCCESSFUL。01（boot keyboard）は interrupt が開いた直後に 'a'、200 ms で離す、300 ms から 'b' を押したまま、以後 3 秒ごとに 'a' の押し・離し。02（boot mouse、Just Works、reconnect_initiate=1・normally_connectable=0）は 1 秒ごとに X +5、最初の接続の 4 回の後に自分で切る（0x08）、1 秒後に page scan が立っていれば Connection Request、host の Accept の後すぐ暗号化の前に PSM 0x11 を求める（Pending の経路）、2 度目は切らない。pairing の後の 02 への page は Page Timeout。時刻の送りは worker（peers の mutex の下、10 ms ごと）。0xFC03 の withdraw と HCI Reset で接続は全て消える（Reset では bond の印は残す）。新しい mutex `peers`（command の経路と worker の時刻の送りが device の状態を触る。peers → lock の順）。04（LE の HOGP）は i03。
- QEMU の試験 `plan/ws143/tests/bt-hid-p005.sh`（§7.5 の (1)(2)(4)(5)(6)(9)(10)(11) と USB の node。(3) LE と (5) の 04 は i03）。
- 確かめ（P1、host）: `config-amd64-bt.mk` の `build/p1-bt/vmunix` と `bin/bluetoothd` は exit 0・warning 0。`bt-daemon-host-test.sh` → daemon 90・pair 161・link 56・hid 75・hidhost 75 PASS（hidhost に handoff の page scan、release、trace の hook を足した）。style-check（loopback・hid・main・session・試験）0。`sh -n bt-hid-p005.sh` OK。**未実施**: QEMU（T1: `bt-hid-p005.sh` と p002〜p004 の回帰）、tshark の照合（host に無い）、実機（i04）。
- 残り: `.hid` の bond の無い記録の refresh での削除、page の timeout の Create Connection Cancel。i03（LE の HOGP、loopback の 04）は Q1 の指示を待つ。

## Q1 の判定（2026-10-08 夜）

- i02（BR/EDR の HID host）は T1-464 で p002〜p005 の 4 本とも PASS（bt-hid-p005 1 m 17 s）→ i02 は cleared。Phase は i03（LE の HOGP）・i04（実機）まで in-progress。
