<!-- awesome-plan project=zedbsd record=ws143p005 -->

# ws143-p005: HID host（`/dev/hid-host`・共有の HID glue・SDP・HIDP・GATT/HOGP・再接続）

Phase ID: `ws143-p005`
Parent: [WS143](../ws.md)
Status: planning（詳細設計の第 1 版、2026-10-08 P2。同日の 2 回目の P2 が §「事実」の出典を code と照合し直し、誤りを直した（§「確認」）。design-reviewer 未実施。code は書いていない）
Phase disposition: normal
Queue: 設計だけ（P2、2026-10-08、Q1 の投入「ws143-p005 の詳細設計を書く。file は phase005/phase.md だけ、code は書かない」）。実装の attempt の区切りは §「attempt の区切り」。

## 範囲

[design.md](../design.md) §5.2（`/dev/hid-host`、D3「形を承認し struct は p005 で review」）、§6.1 の SDP・GATT・HID host、§6.3 の「忘れる」の Virtual Cable Unplug、§6.4（再接続と切断）、§10.1 の p005 の行（usb-hid の glue の共有の module への refactor と USB の回帰、`/dev/hid-host`、hid-report.c の fuzz、SDP・GATT client、HID host（BR/EDR と HOGP）、再接続、切断で key を離す）。p004 の bluetoothd（session・pair・l2cap・smp・keys・privsep・口）の上に足す。

Q1 の条件（2026-10-08）: 「HID は既存の usb-hid・hidraw・evdev の経路（WS161 の hidraw）と矛盾しない形で、入力は compositor の evdev に届くこと」。この設計は、Bluetooth の HID の入力を **既存の kernel の HID の層（hid-report.c の parser、hid-touch.c・hid-digitizer.c の状態機械、input.c の evdev）に流し、`/dev/input/eventN` として compositor が USB の device と同じ道で見つける**形にする（§「事実」の 7〜9 行）。

## 範囲の外（理由と行き先）

- **出力の report（キーボードの LED、HOGP の Output Report・Boot Output）**: kernel に出力の経路が無い（`include/kern/input-device.h` 64〜84 行の `input_device_info` に出力の callback は無く、hid-report.h に encoder も無い。design §5.2 [F6, N16]）。`/dev/hid-host` の read は EAGAIN、poll は POLLIN を言わない。evdev の EV_LED から作る経路は後の Phase（Future Work に登録を Q1 に依頼）。
- **i2c-hid の共有 glue への乗せ替え**: §「判断の記録」Q1。i2c-hid は touch の device しか出さず（`src/drivers/i2c/i2c-hid.c` 1019〜1030 行: touch が無ければ ENODEV、1330〜1370 行: touch の report だけ translate）、key の集約や pen を使わない。乗せ替えは 5330 の touchpad（実機だけで確かめられる）の回帰の危険があり、Bluetooth には益が無いので p005 ではしない。**design.md §5.2 の「USB・I2C・hid-host が共有する module」の文言と違う**ので、ユーザーの確認を求める（Q1）。
- **hidraw への Bluetooth の HID の公開**: §Q3。`/dev/input/hidrawN` は input でない raw の interface（FIDO）のための口で（`include/uapi/hidraw.h` 92〜95 行、`usb-hid.c` 689〜703 行は top の usage が FIDO の時だけ raw）、Bluetooth のキーボード・マウスは evdev へ流す。top の usage が FIDO の descriptor を `/dev/hid-host` に書いたら EOPNOTSUPP で断る（Bluetooth の FIDO は WS161 の範囲でも無い）。
- **A2DP・PAN・SCO、LE Audio**: design §7（別の WS、D1）。
- **desktop（Settings・system bar・pairing の窓・backend の口の `SUBSCRIBE` の event）**: p006。この Phase は socket の request（CONNECT・DISCONNECT・STATUS）と CLI `bt` まで。p006 が要る状態の変化の通知（接続・切断・電池）は、口の行の形をここで決めておき（§5）、SUBSCRIBE の配りは p006 で足す。
- **PIN の legacy pairing、`PASSKEY?`（こちらが打つ）、KeyboardDisplay の agent**: p004 の Q2・Q6 のまま（p006 の窓の後）。
- **LE の private address（D11c、自分の IRK と RPA）**: p004 は「p005 の再接続と一緒に」とした。この Phase は **相手の RPA の解決（controller の resolving list、相手の IRK は p004 が保存済み）** を扱い、**自分の RPA（LE Set Random Address・own address type 0x02/0x03）は i03 の後の判断**にする（§Q11）。理由: 自分の RPA を使うと bond 済みの相手が自分を見つけられない組（相手が自分の IRK を配られていない）が起き、鍵の配りの要求（p004 の smp.c は initiator の鍵を配らない: 「鍵の配り: initiator 0」）の変更が要る。
- **5330 の実機**: QEMU で確かめられる物までをこの Phase の attempt に置き、実機の相手（本物のキーボード・マウス）は p008 の UAT。i03 の後に 5330 で短い接続の確かめをする余地は残す（§attempt の i04）。
- **HID の boot protocol（SET_PROTOCOL boot、Boot Keyboard/Mouse Input Report の characteristic）**: report protocol だけを使う。boot だけの device は無い前提（HID 1.1.1 は report protocol を必須にしている。**未確認**: 版と節）。
- **resume の時の Read Version のやり直しと firmware の load し直し**（design §5.3、p003 が「p004 以降」とした物。事実 16）: この Phase は `/dev/system` の購読を足すが、HID の再接続の分だけを扱う。transport の分は p003 の残件か p008 の前の小さな Phase として Q1 に聞く（§Q19）。

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
| 17 | privsep: 親（root）は datagram の `OPEN`／`OPEN /dev/btN` に fd（SCM_RIGHTS）で答え、`OPEN` 以外は知らない。子は `_bluetooth`（80）。bond の file は `/var/db/bluetooth/<controller>/<address>-<型>` の `key=value`（type・name・link_key・link_key_type・ltk・ediv・rand・key_size・authenticated・secure・legacy・irk・identity・identity_type。知らない key は読み飛ばす）。`struct btd_bond` は `name[BTD_NAME_MAX]`（249）・`key_size`・`legacy`・`irk` を持つ | `privsep.c` 374〜399 行、`privsep.h` 23 行、`keys.c` 11〜13、318〜364、383、590〜591 行、`keys.h` 37〜65 行、`hci.h` 29 行、[phase004](../phase004/phase.md) §3・§4 |
| 18 | loopback の controller: BR/EDR の相手 0A:0B:0C:0D:0E:01（DisplayYesNo、Inquiry の「Loopback Keyboard」class 0x002540）・05・06・07（Just Works）は handle 0x0040、LE の 03 は 0x0041（pairing を断る）。Inquiry Result with RSSI の 02 は class 0x002580（マウス）。ACL は handle ごとに Number Of Completed Packets、L2CAP の Information Request と SMP の Pairing Request にだけ答える。**`loopback_frame` の payload は 16 byte まで**（`body[4+4+16]`、超えたら EINVAL）。LE Create Connection（0x200D）は address が 03（`LOOPBACK_DEVICE_MOUSE`、LE の広告の名前は「Loopback Mouse」appearance 0x03C2）の public の時だけ LE Connection Complete（19 byte、interval 0x0018、timeout 0x01F4）を返し、他の address は繋がない。LE Create Connection Cancel は status 0x02 の LE Connection Complete。HCI Disconnect は理由 0x16 の Disconnection Complete | `src/drivers/generic/bt-hci-loopback.c` 8〜60、120〜160、889、964〜972、1120〜1404（1340〜1389）、1480〜1566（1535〜1546）行 |
| 19 | 試験の道具: `bt-probe`（node は daemon と同時に開けない: EBUSY）、`hidraw-probe`、`peninject -d`（inject の evdev の node を待って event を出す）、`systemevents`（`/dev/system` の event を行で出す）。guest の QEMU は `-device usb-kbd,bus=xhci.0,port=3` を既に持ち、`--qemu-extra` で足せる。QMP は `-qmp unix:<runtime>/qmp.sock,server,nowait` で開いている（`boot-test.sh` が screendump に使う同じ socket）が、guest.py に汎用の QMP の command を送る関数は無い。`aat-input`・`touchinject`・`peninject` は inject の口の書き手で、任意の `/dev/input/eventN` を bus・名前で選んで読む道具は無い（`evdev-probe` を新しく作る理由） | `userland/tests/bt-probe/main.c` 8〜40、`hidraw-probe/main.c`、`peninject/main.c` 8〜33、`systemevents/main.c` 8〜33、`aat-input/main.c` 8〜20 行、`plan/tools/guest/guest.py` 159・190・209・211・222・368 行、`plan/tools/boot-test.sh` 15 行 |
| 20 | host 試験の形: bluetoothd の部品は host の cc で ASan・UBSan（`plan/ws143/tests/bt-daemon-host-test.sh`）、kernel の HID の file は freestanding（`-ffreestanding -nostdlibinc -fno-builtin -D__ZEDBSD__ -DKERN_USER_ABI_LP64`）で compile して host の試験と link（`plan/ws079/tests/run-hid-pen.sh`、`plan/ws159/tests/run-host-i2c-hid.sh`）。5330 の touchpad の descriptor は `plan/ws159/tests/latitude5330-linux/synaptics-06cb-ce65-rdesc.bin` | 各 file |
| 21 | kernel に AES-128 の block の暗号化がある（`wlan_aes128_encrypt_block`）。試験の kernel の config で link されるかは**未確認** | `src/kern/net/wifi/wlan-crypto.h` 73〜75 行、`Makefile` 697 行 |
| 22 | cdev の rdev の割り当て（既存）: console `0x00010000`・`0x00010010+n`・tty `0x00010020`、ptmx `0x00010001`、system `0x00010002`、pty `0x00020000+n`、input `0x00030000+N`、gpu `0x00090000`、audio `0x000A0000`、acpi `0x000b0000`、backlight `0x000d0000`、input-inject `0x000e0000`、hidraw `0x000f0000+N`、**smartcard `0x00100000`**、typec `0x00110000`、bt-hci `0x00120000`。`0x00130000` は空いている | `src/drivers/generic/console.c` 1024・1034・1042、`src/kern/tty.c` 1262、`src/drivers/generic/system-device.c` 158、`src/kern/devfs.c` 862・935、`src/drivers/gpu/gpu.c` 45、`src/drivers/audio/audio.c` 31、`src/drivers/acpi/acpi-dev.c` 34、`src/drivers/generic/backlight.c` 33、`input-inject.c` 44、`hidraw.c` 39、`smartcard.c` 42、`src/drivers/typec/typec-kern.c` 41、`src/drivers/generic/bt-hci.c` 49 行 |
| 23 | ioctl の group の文字（既存）: 'A' audio、'S' ccid、't' tty、's' system、'B' blkid、'c' console、'b' bluetooth、'E' evdev、'g' graphics、'L' backlight、'H' hidraw。'h' は未使用 | `include/uapi/*.h` の `*_IOC_GROUP` |
| 24 | `struct input_device` は header では opaque（`struct input_device;`）で、`number`（eventN の N）は input.c の中の field。外から番号を知る関数は無い | `include/kern/input-device.h` 12 行、`src/drivers/generic/input.c` 58・68 行 |
| 25 | SMP の Pairing Request の鍵の配り: initiator（自分）0、responder は EncKey と IdKey（p004 の記述どおり） | `userland/base/bluetoothd/smp.c` 155〜161 行 |

## 詳細設計

### 1. 構成（file の表）

| file | 新規／変更 | 中身 | host の試験 |
| --- | --- | --- | --- |
| `include/drivers/generic/hid-input.h`、`src/drivers/generic/hid-input.c` | 新規 | **共有の HID glue**（§2）: report descriptor から layout・capability・axis・pen・touch の記述を作り、input device を登録し、report を decode して evdev に出す。transport（USB・hid-host）を知らない | する（freestanding、input の層の stand-in） |
| `src/drivers/usb/usb-hid.c` | 変更 | glue を呼ぶ。transport（descriptor の取得、SET_PROTOCOL、URB、worker、raw の hidraw）と名前の決めは残す。動かす code は規約の全文に合わせる（design §5.2 [N16]） | USB の回帰は build と QEMU（§7） |
| `include/uapi/hid-host.h` | 新規 | `/dev/hid-host` の UAPI（§3。D3 の形、struct はこの Phase の review） | — |
| `include/drivers/generic/hid-host.h`、`src/drivers/generic/hid-host.c` | 新規 | `/dev/hid-host` の cdev（§3） | する（setup の検査の純粋な部分だけ） |
| `src/kern/devfs.c` | 変更 | `hid-host` を 0600 に（`input-inject` と同じ行に足す） | — |
| `src/kern/vfs.c`、`platform/amd64/vmunix.mk` | 変更 | hid-host の登録（`CONFIG_DRIVER_USB_BT` か `CONFIG_BT_TEST_LOOPBACK`）、hid-input.c を HID の source に、HID の source を Bluetooth でも入れる | build |
| `plan/ws143/tests/hid-input-host-test.{sh,c}` | 新規 | glue の host 試験（§7） | — |
| `plan/ws143/tests/hid-report-fuzz.{sh,c}` | 新規 | hid-report.c・hid-touch.c・hid-digitizer.c・hid-input.c の fuzz（§7） | — |
| `userland/tests/evdev-probe/` | 新規 | guest で `/dev/input/eventN` を bus・名前で待ち、event を行で出す（§7） | — |
| `userland/tests/hid-host-probe/` | 新規 | root で `/dev/hid-host` にキーボードを作り report を書く（kernel だけの QEMU の確かめと、口の誤用の確かめ） | — |
| `userland/base/bluetoothd/router.[ch]` | 新規 | session の handler。接続の event と ACL を pair か HID の link に配る（§4.1） | する |
| `userland/base/bluetoothd/l2cap.[ch]` | 変更 | 相手からの Connection Request を policy で受ける、inbound の channel の表、Flush Timeout・QoS・RFC の option（§4.2） | する |
| `userland/base/bluetoothd/sdp.[ch]` | 新規 | 純粋: SDP client の PDU（ServiceSearchAttributeRequest の組み立て、Response の continuation と data element の解析、HID の record の属性）（§4.3） | する（台本と fuzz） |
| `userland/base/bluetoothd/hidp.[ch]` | 新規 | 純粋: HIDP の header（§4.4） | する |
| `userland/base/bluetoothd/att.[ch]` | 新規 | 純粋: ATT の PDU の組み立てと解析（§4.5） | する（台本と fuzz） |
| `userland/base/bluetoothd/hog.[ch]` | 新規 | HOGP の発見の状態機械（ATT の上。system call 無し、呼び手が送る）（§4.6） | する（台本の ATT server） |
| `userland/base/bluetoothd/hid.[ch]` | 新規 | HID host の核: device の表、BR/EDR と LE の接続の流れ、security の検査、`/dev/hid-host` への橋、再接続の policy と timer（§4.7〜4.9） | する（偽の controller、偽の hid-host の fd） |
| `userland/base/bluetoothd/privsep.[ch]` | 変更 | 子の `OPEN-HID` に親が `/dev/hid-host` を開けて fd で答える（§4.10） | しない（QEMU） |
| `userland/base/bluetoothd/keys.[ch]` | 変更 | bond の file に HID の field（§4.11） | する |
| `userland/base/bluetoothd/pair.[ch]` | 変更 | Connection Request の Reject と他 address の Negative Reply を router に移す。PAIRED の後の自動の CONNECT の hook（§4.12） | する（既存の試験の更新） |
| `userland/base/bluetoothd/main.c`、`protocol.h` | 変更 | CONNECT・DISCONNECT・STATUS、SHOW の `hid=`、`/dev/system` の POWER の購読、timer（§5） | しない（QEMU） |
| `userland/base/bt/main.c` | 変更 | `bt connect`・`bt disconnect`・`bt status` | しない（QEMU） |
| `src/drivers/generic/bt-hci-loopback.c` | 変更 | HID の相手: 01 の SDP・HIDP のキーボード、02 の HIDP のマウス（相手から再接続）、04 の LE の HOGP のマウス（ATT server）。frame の payload の上限を広げる（§6） | — |
| `plan/ws143/tests/bt-hid-host-test.{sh,c}`、`bt-hid-p005.sh`、`hid-usb-p005.sh`、`hid-host-p005.sh`、`config-amd64-bt.mk`・`build-bt-image.sh` | 新規／変更 | §7 | — |

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
size_t drv_hid_input_report_max(const struct hid_input *);   /* 最も長い input report（byte。ID の byte を含む）。usb-hid の buffer の大きさ、hid-host の write の検査 */
unsigned drv_hid_input_kind(const struct hid_input *);
int drv_hid_input_publish(struct hid_input *, const struct hid_input_identity *);   /* input device（と touch の device）を登録。何も作れなければ ENODEV */
void drv_hid_input_report(struct hid_input *, const uint8_t *report, size_t length, uint64_t milliseconds);   /* decode して evdev へ。壊れた report は数えて捨てる（kern_logf は最初の 16 回） */
unsigned drv_hid_input_malformed(const struct hid_input *);
int drv_hid_input_numbers(const struct hid_input *, int *event, int *touch_event);   /* 登録した eventN の番号（-1: 無い）。struct input_device は opaque（事実 24）なので input.c に unsigned drv_input_device_number(const struct input_device *) を足す */
void drv_hid_input_unpublish(struct hid_input *);   /* unregister（input.c が押されている key を離す、事実 4） */
void drv_hid_input_destroy(struct hid_input *);     /* layout と記憶を解放（unpublish の後） */
```

- `prepare` と `publish` を分けるのは、usb-hid が attach で parse し（buffer の大きさが要る）、input の層の準備の後（`drv_usb_hid_input_ready`）に登録するから。hid-host は setup の write で両方を続けて呼ぶ。
- **振る舞いは変えない**（規約「Style-only changes must preserve evaluation order, ownership, lifetime, error reporting, and observable behavior」）: report ID ごとの held と全 report の集約、`keyboard_error` の時は前の key の状態を保つ、EV_REL の 0 を出さない、EV_KEY 以外の値はそのまま、出した時だけ SYN_REPORT、touch と pen の振り分けの順（touch → pen → 普通）、「capability が SYN だけなら device を作らない」「何も作れなければ ENODEV」。これらを host の試験（§7.1）で、新旧の出力の一致として確かめる。
- 時刻: `drv_hid_input_report` は呼び手の時刻（usb-hid は URB の完了の時刻 `completed_milliseconds`、hid-host は write の時刻 `clock_milliseconds`）を取る（`drv_input_device_emit_at`、`input-device.h` 101〜107 行の約束）。
- lock: glue は自分の lock を持たない。呼び手が report と unpublish を直列にする（usb-hid は worker の thread 1 本、detach は worker の join の後に unpublish（438〜483 行）。hid-host は open ごとの mutex の下で write、close は最後の write の後（§3））。
- 配置: `src/drivers/generic/hid-input.c` を amd64 の `AMD64_HID_SOURCES`（`vmunix.mk` 268〜271 行）に足し、その条件に `CONFIG_DRIVER_USB_BT` と `CONFIG_BT_TEST_LOOPBACK` を加える（hid-host が要る）。arm64 の `ARM64_USB_SOURCES`（95 行）にも足す（usb-hid が呼ぶ）。rpi4 の Bluetooth（UART）は範囲の外なので hid-host は amd64 だけ。
- **USB の回帰の危険と確かめ方**: 危険は (a) key の集約の意味が変わる、(b) touch・pen の経路が変わる、(c) attach・detach の順（register が input ready の前に起きる、unpublish が 2 回呼ばれる）、(d) 名前の fallback が変わる。確かめ: (a)(b)(d) は §7.1 の host 試験で新旧の出力の byte 単位の一致（旧の usb_hid_publish_report を試験に写して並べて走らせる。写しは試験の中だけ）、(c) は code の review と QEMU（`hid-usb-p005.sh`: guest の usb-kbd に QMP の `send-key`、usb-tablet か usb-mouse を `--qemu-extra` で足して `input-send-event`。`evdev-probe -b usb` で event を読む。bus=3 の node が USB の数だけある）。実機（5330 に USB のキーボードを挿す）は p008 か T1 の空き（任意）。QEMU の規約「回帰試験では GPU を使わず framebuffer で login prompt だけ」は boot test の話で、この試験は guest の中の evdev の読みで判定する（console log は読まない）。

### 3. kernel: `/dev/hid-host`（D3 の形、struct の配置）

**UAPI `include/uapi/hid-host.h`**（D3 で承認した形: 最初の write が作成、続く write が input report、read が output report、close で消す。**形への追加は `HID_HOST_GET_DEVICE` の ioctl 1 つ**（§Q2、ユーザーの判断））:

```c
#define KERN_HID_HOST_IOC_GROUP	'h'          /* 未使用（事実 23: 'H' hidraw、'b' bluetooth、'E' evdev、's' system、'S' ccid …） */
#define HID_HOST_MAGIC		0x74686968U  /* "hiht" */
#define HID_HOST_VERSION	1U
#define HID_HOST_DESCRIPTOR_MAX	4096U        /* hid-report.h の HID_REPORT_DESCRIPTOR_SIZE_MAX */
#define HID_HOST_REPORT_MAX	1024U        /* hidraw と同じ（HIDRAW_REPORT_MAX）。ID の byte を含まない */
#define HID_HOST_TEXT_MAX	64U          /* input.c の INPUT_TEXT_MAX */
#define HID_HOST_OPENS_MAX	8U           /* 同時の device の数（bluetoothd の BTD_HID_MAX 6 + 余り） */

/* 最初の write: device の宣言。write の長さは sizeof ちょうど（4324 byte: 36 + 3×64 + 4096。field は自然に並び padding は無い。hid-host.c と host 試験に _Static_assert(sizeof(struct hid_host_setup) == 4324U) を置く）。 */
struct hid_host_setup {
	uint32_t magic;
	uint32_t version;		/* HID_HOST_VERSION */
	uint16_t bus;			/* BUS_BLUETOOTH か BUS_VIRTUAL だけ（他は EINVAL。USB を名乗れない） */
	uint16_t vendor;
	uint16_t product;
	uint16_t release;		/* input_id.version */
	uint32_t descriptor_size;	/* 1..HID_HOST_DESCRIPTOR_MAX */
	uint32_t reserved[4];		/* 0 */
	char name[HID_HOST_TEXT_MAX];		/* NUL で終わる。空なら kernel が種類から付ける（"Bluetooth HID keyboard" など） */
	char physical_path[HID_HOST_TEXT_MAX];	/* 例 "bluetooth/00:11:22:33:44:55/0A:0B:0C:0D:0E:01" */
	char unique_id[HID_HOST_TEXT_MAX];	/* 相手の address（再接続でも同じ、design §5.2） */
	uint8_t descriptor[HID_HOST_DESCRIPTOR_MAX];
};

/* HID_HOST_GET_DEVICE: 作った device の番号（/dev/input/eventN の N、-1 は無い）と、layout が断った report の数。 */
struct hid_host_device {
	int32_t event;
	int32_t touch_event;
	uint32_t malformed;
	uint32_t reserved[5];
};
#define HID_HOST_GET_DEVICE	_IOR(KERN_HID_HOST_IOC_GROUP, 0, struct hid_host_device)
```

**file の操作**（`src/drivers/generic/hid-host.c`、cdev 名 `hid-host`、rdev は **`0x00130000`**（事実 22 の表の次の空き。第 1 版の `0x00100000` は smartcard と衝突していた）、`cdev_register("hid-host", …)` は `input-inject` と同じ形）:

- open: **root だけ**（`cred_is_superuser`。devfs の 0600 とは別に、chmod で広げられないため。input-inject と同じ）。同時に `HID_HOST_OPENS_MAX` まで、1 open = 1 device。open の状態は `struct hid_host_open { struct mutex lock; struct hid_input *input; unsigned declared; uint64_t reports; }`。超えたら EBUSY。
- write（最初）: 長さが `sizeof(struct hid_host_setup)` でなければ EINVAL。magic・version・bus・descriptor_size・reserved・文字列の NUL（`strnlen < 64`）を検査（純粋な `hid_host_setup_valid()`、host で試験）。top の usage が FIDO（`drv_hidraw_describe` で見る）なら EOPNOTSUPP（§Q3）。`drv_hid_input_prepare` → `drv_hid_input_publish`（identity: setup の文字列、`id.bustype = bus`）。失敗はその errno（parser の EINVAL・E2BIG・ENOMEM、何も作れない ENODEV）。成功は size。**2 度目の setup は無い**（宣言の後の write は全部 report）。
- write（続き）: 1 回の write は 1 つの input report（layout が report ID を使うなら先頭の byte が ID。hidraw の read と同じ約束）。長さ 0 か `HID_HOST_REPORT_MAX + 1` 超は EINVAL。`drv_hid_input_report_max` を超える長さも EINVAL（descriptor に無い長さ）。decode に失敗した report は捨てて数える（`malformed`。write は size を返す。壊れた report で daemon を止めない）。宣言の前の report は EINVAL。
- read: 出力の report の経路が無いので EAGAIN（O_NONBLOCK の有無に関わらず。寝ない。範囲の外）。将来 LED を足す時は「read は 1 つの output report（ID の byte が先頭）」の形で、poll の POLLIN を足す。
- poll: POLLOUT 常に（宣言の後も前も write できる）、POLLIN は言わない、POLLHUP は無い（device を消すのは自分の close だけ）。
- ioctl: `HID_HOST_GET_DEVICE`（宣言の前は event = -1）。他は ENOTTY。
- close: `drv_hid_input_unpublish`（input.c が押されている key を全部離す、事実 4）→ `destroy` → open の枚数を返す。**切断で key が残らない**（design の受け入れ 3）は、daemon が切断で fd を close することで満たす（§4.7）。
- 境界と寿命: setup は open の中で 1 回、device は open と同じ寿命（daemon が落ちれば kernel が fd を close して device が消え、key が離れる）。write は open の mutex で直列、close は file の最後の参照で呼ばれるので write と重ならない。report の decode は write の process の文脈で走る（sleep しない、spin lock は input.c の中だけ）。
- 権限と privsep（D16 (a)）: node は root の 0600（devfs の規則に `hid-host` を足す）。bluetoothd の **特権の親が開けて SCM_RIGHTS で子に渡す**（§4.10）。子（`_bluetooth`）は open しない。乗っ取られた子が任意のキーボードを作れる危険は design §5.2 [N9] のまま（防ぐのは電波の相手の解析の上限と、bond 済み・暗号化・鍵の長さ 16 の link だけを受けること。§4.8）。
- 名前: Bluetooth の名前（248 byte まで、`BTD_NAME_MAX 249`）は **daemon が UTF-8 の境で 63 byte に切る**（kernel は 64 以上を EINVAL で断るだけ。切る規則を kernel に持たせない）。
- syscall の bounce（p002 §2 の注）: 512 byte を超える write は heap の buffer を使い、取れなければ 512 byte の stack に落ちて write が分割される（`src/kern/syscall.c` 85 行 `SYSCALL_IO_CHUNK 512`）→ setup（4324 byte）と長い report は EINVAL になり得る（記憶の圧迫の時だけ）。daemon は setup の EINVAL を 3 回まで 100 ms おきにやり直す（§4.7）。
- 試験の口 `/dev/input-inject` との違い: inject は固定の形の device だけ、hid-host は descriptor から作る。inject は試験の kernel だけ、hid-host は製品。

### 4. bluetoothd

#### 4.1 router（session の handler の分配）

今は `btd_session.handler = btd_pair_handle` で、pair が自分の物でない packet を捨てるか断る（事実 14）。HID の link が増えるので、handler を `btd_router_handle` にし、router が配る:

- 表: `handle → owner`（owner は pair か `hid[i]`）。Connection Complete・LE Connection Complete で、address が pair の相手なら pair、bond 済みの HID の address か HID が始めた接続なら hid、どれでもなければ **その接続を切る**（pair.c の `pair_disconnect_other` の役を router に移す）。Disconnection Complete で表から消す（session の `session_link_remove` と同じ順: router は session の後に呼ばれるので、同じ handle の次の Connection Complete と取り違えない）。
- address で届く event（Connection Request、Link Key Request、PIN Code Request、IO Capability Request・Response、User Confirmation Request、User Passkey Request・Notification、Simple Pairing Complete、Link Key Notification、Authentication Complete は handle）: pair の相手の address なら pair、bond 済みの HID の address なら hid、他は router が断る（Connection Request は Reject 0x0F、Link Key Request は Negative Reply、PIN Code Request は Negative Reply、IO Capability Request は Negative Reply 0x18、User Confirmation Request は Negative Reply。pair.c の 538〜545 行の Reject と「他 address の Negative Reply」はここへ移す）。
- ACL: handle の owner へ。owner が無ければ `ignored`。
- Encryption Change・Encryption Key Refresh Complete・Authentication Complete（handle 付き）: handle の owner へ。
- LE Meta: LE Connection Complete は上の規則、LE Read Local P-256 Public Key Complete・LE Generate DHKey Complete は pair、Advertising Report は session（scan）。LE Connection Update Complete は owner。
- pair.c の変更は「Connection Request を見ない」「他 address の Negative Reply をしない」の 2 点と、Connection Complete で自分の相手でない時に切らない（router がする）。host 試験の bt-link-host-test の「相手から始まる pairing の拒否」は router の試験に移す。

#### 4.2 L2CAP の拡張（inbound の channel と option）

- 相手からの Connection Request: `btd_l2cap` に policy の callback `int (*accept)(void *context, uint16_t handle, uint16_t psm, uint16_t *result)` を持たせる。無ければ今の PSM not supported。HID の link は hid.c が渡す: PSM 0x0011・0x0013 で、link が暗号化済み・鍵 16・bond 済み・HID の状態が「channel を待つ」なら 0（成功）、暗号化の前なら **Security Block（0x0003）**、他は PSM not supported（0x0002）。受けた channel は表に入れ（remote CID、local CID を割り当て）、Connection Response（成功）に続けて自分の Configure Request（MTU 672）を送る。相手の Configure Request は今の `signal_configure_request` で受け、両方の done で OPEN。表が満ちれば **No resources（0x0004）**。
- option: Flush Timeout（0x02、2 byte）は値を記録して受ける（controller の flush の設定は変えない。HID 1.1.1 は interrupt channel に flush timeout を推奨するが、受けるだけで動く。**未確認**: 相手が自分の側の flush の実施を期待するか）。QoS（0x03、22 byte）は service type が Best effort（0x01）か No traffic（0x00）なら受け、Guaranteed（0x02）は Unacceptable で Best effort を返す。Retransmission and Flow Control（0x04）は mode が Basic（0x00）なら受け、他は Unacceptable で Basic を返す。FCS（0x05）は受ける（Basic mode では意味が無い）。知らない hint でない option は今のまま Unknown。p004 の S10 の残り。
- channel の open・close の通知: `btd_signal_effect` に `opened`（local CID）と `closed`（local CID、理由）を足し、hid.c が channel の状態の変化を知る（今は表を見るだけで通知が無い）。
- channel の表は持ち主ごと（`struct btd_l2cap` と `struct btd_reassembly` を HID の device ごとに 1 つ。pair は今も自分の物を持ち pairing ごとに init し直す、事実 15）。1 link の channel は SDP 1 + control 1 + interrupt 1 の 3 つ（SDP は HID の channel の前に閉じるので同時は 2〜3）。選ばなかった案: daemon で 1 つの表を pair と hid が共有する → 6 device × 3 = 18 が `BTD_CHANNELS_MAX` 16 を超えて上限を上げる変更が要り、`next_identifier` と Information Request の状態も link をまたいで混ざる。持ち主ごとなら既存の API（handle 付きの `btd_l2cap_signal`・`btd_l2cap_connect`・`btd_l2cap_drop`）をそのまま使える（§Q18）。

#### 4.3 SDP client（`sdp.[ch]`、純粋）

- 役: HID の service record から、HID の report descriptor、HIDReconnectInitiate、HIDNormallyConnectable、HIDVirtualCable、HIDBootDevice、HIDCountryCode、PnP（Device ID の record: VendorID・ProductID・Version）を読む。PSM は HID の規約の固定値（control 0x0011、interrupt 0x0013。ProtocolDescriptorList も読むが固定値と違えば `no-hid`）。
- PDU: ServiceSearchAttributeRequest（PDU ID 0x06）、Response（0x07）、ErrorResponse（0x01）。header は PDU ID 1 byte・TransactionID 2 byte・ParameterLength 2 byte（big-endian。SDP は big-endian、HCI・L2CAP は little-endian）。Request の parameter: ServiceSearchPattern（data element sequence の UUID 0x1124（HID）か 0x1200（PnP Information））、MaximumAttributeByteCount（送る値は 0x03F0 = 1008: L2CAP の MTU 672 の相手でも 1 回で返せる長さ。相手は小さく返してよい）、AttributeIDList（data element sequence の uint32 の range 0x0000FFFF = 全属性。全部読んで要る物を選ぶ。record は小さい）、ContinuationState（1 byte の長さ + 0〜16 byte）。Response: AttributeListsByteCount 2 byte、AttributeLists の断片、ContinuationState。断片を繋いで（上限 `BTD_SDP_MAX` 8 KiB）、ContinuationState の長さが 0 になるまで Request を繰り返す（同じ continuation を返し続ける相手は 8 回で `protocol`）。**未確認**: PDU ID・parameter の並び・MaximumAttributeByteCount の下限（Core 5.4 Vol 3 Part B §4.7.1・§4.7.2 と照合）。
- data element: 先頭 1 byte の type（上位 5 bit）と size index（下位 3 bit）、size index 5・6・7 は続く 1・2・4 byte が長さ。型: nil 0、uint 1、sint 2、UUID 3、text 4、bool 5、sequence 6、alternative 7、URL 8。解析は長さの検査、入れ子の深さ 8 まで、1 要素の長さが残りを超えたら `malformed`。属性は「uint16 の ID、値」の組の並び（1 record = 1 sequence、AttributeLists は record の sequence の sequence）。
- HID の属性の ID（**未確認**: HID 1.1.1 §5.3.4 と照合。Bluetooth SIG の Assigned Numbers の HID の service の attribute）: ServiceClassIDList 0x0001、ProtocolDescriptorList 0x0004、AdditionalProtocolDescriptorLists 0x000D（interrupt の PSM）、HIDDeviceSubclass 0x0202、HIDCountryCode 0x0203、HIDVirtualCable 0x0204、HIDReconnectInitiate 0x0205、HIDDescriptorList 0x0206（sequence の sequence: {uint8 の class descriptor type 0x22、text の descriptor の byte 列}）、HIDBatteryPower 0x0209、HIDRemoteWake 0x020A、HIDNormallyConnectable 0x020D、HIDBootDevice 0x020E。PnP Information（0x1200）の VendorID 0x0201、ProductID 0x0202、Version 0x0203、VendorIDSource 0x0205（**未確認**: Device ID Profile 1.3 と照合）。
- 口: `btd_sdp_request(struct btd_sdp *, uint16_t uuid, uint8_t *out, size_t size, size_t *length)`、`btd_sdp_input(struct btd_sdp *, const uint8_t *pdu, size_t length)` → `BTD_SDP_MORE`（次の request を送る）・`BTD_SDP_DONE`・`BTD_SDP_FAILED`、`btd_sdp_hid(const struct btd_sdp *, struct btd_hid_record *)`（descriptor は `btd_sdp` の buffer を指す。4096 超は `descriptor`）。

#### 4.4 HIDP（`hidp.[ch]`、純粋）

- header 1 byte: 上位 4 bit が message type、下位 4 bit が parameter。type: HANDSHAKE 0x0、HID_CONTROL 0x1、GET_REPORT 0x4、SET_REPORT 0x5、GET_PROTOCOL 0x6、SET_PROTOCOL 0x7、DATA 0xA。HID_CONTROL の parameter: SUSPEND 0x3、EXIT_SUSPEND 0x4、VIRTUAL_CABLE_UNPLUG 0x5。DATA の parameter（report type）: OTHER 0、INPUT 1、OUTPUT 2、FEATURE 3。SET_PROTOCOL の parameter: Boot 0、Report 1。HANDSHAKE の result: SUCCESSFUL 0、NOT_READY 1、ERR_INVALID_REPORT_ID 2、ERR_UNSUPPORTED_REQUEST 3、ERR_INVALID_PARAMETER 4、ERR_UNKNOWN 0xE、ERR_FATAL 0xF。**未確認**: 値（HID 1.1.1 §7.3・§7.4 と照合）。
- control channel: SET_PROTOCOL（Report）は **HIDBootDevice が true の device にだけ**送り（HID 1.1.1 は boot protocol を持つ device だけに SET_PROTOCOL を許す。**未確認**）、HANDSHAKE を 2 秒待つ。ERR_UNSUPPORTED_REQUEST は無視して進む。相手からの HID_CONTROL VIRTUAL_CABLE_UNPLUG は「相手が unpair を求めた」: link を切り、bond を消す（design §6.3 の逆向き。§Q9）。SUSPEND・EXIT_SUSPEND は記録だけ。相手からの GET_REPORT・SET_REPORT・GET_PROTOCOL（host への要求）は HANDSHAKE ERR_UNSUPPORTED_REQUEST。
- interrupt channel: 相手からの DATA（INPUT）の header を外して `/dev/hid-host` へ write（layout が report ID を使うなら相手の report の先頭に ID があるまま。HIDP は ID を report の中に持つ）。DATA（OTHER・OUTPUT・FEATURE）は無視して数える。host から interrupt channel に送る物は無い（出力は範囲の外）。
- 長さ: 1 frame は HIDP の header 1 byte + report ≤ 1024（`HID_HOST_REPORT_MAX`）。超えたら捨てて数える。

#### 4.5 ATT client（`att.[ch]`、純粋）

- PDU の opcode（**未確認**: Core 5.4 Vol 3 Part F §3.4 と照合）: Error Response 0x01（request opcode、handle、error code）、Exchange MTU Request 0x02／Response 0x03、Find Information Request 0x04／Response 0x05（format 1 = 16 bit UUID、2 = 128 bit）、Read By Type Request 0x08／Response 0x09（length、{handle、value} の並び）、Read Request 0x0A／Response 0x0B、Read Blob Request 0x0C／Response 0x0D、Read By Group Type Request 0x10／Response 0x11（length、{handle、end group handle、value} の並び）、Write Request 0x12／Response 0x13、Write Command 0x52、Handle Value Notification 0x1B、Handle Value Indication 0x1D／Confirmation 0x1E。error code: Invalid Handle 0x01、Read Not Permitted 0x02、Insufficient Authentication 0x05、Request Not Supported 0x06、Insufficient Authorization 0x08、Attribute Not Found 0x0A、Insufficient Encryption Key Size 0x0C、Insufficient Encryption 0x0F。
- MTU: 既定 23。Exchange MTU で `BTD_ATT_MTU` 185 を求める（LE の ACL の data 27 byte で 7 packet。session の分割と組み直し（1024）の中）。相手の値と小さい方。
- 1 つの request が出ている間は次を出さない（ATT の規則）。request の timeout 30 秒（ATT の transaction timeout。過ぎたら link を切る）。
- 口: `btd_att_build_*`（request の組み立て）、`btd_att_parse(const uint8_t *pdu, size_t length, struct btd_att_pdu *)`（opcode ごとに長さを検査して field を指す。Response の list は要素の数と長さの整合を検査）。Notification・Indication は呼び手に渡し、Indication には Confirmation を返す。

#### 4.6 HOGP の発見（`hog.[ch]`、ATT の上の状態機械。system call 無し）

状態の順（HOGP 1.0 §4・GATT の手順。**未確認**: 版と節、UUID の値は Bluetooth SIG Assigned Numbers と照合）:

1. Exchange MTU。
2. primary service の発見: Read By Group Type（UUID 0x2800）を 0x0001 から末尾の handle まで繰り返す（Attribute Not Found で終わり）。要る service: HID 0x1812（1 つ以上あり得る: 複数の HID service は最初の 1 つだけを使い、数を記録）、Battery 0x180F、Device Information 0x180A。HID が無ければ `no-hid`。
3. HID service の characteristic: Read By Type（UUID 0x2803）を service の範囲で繰り返す。要る物: Report Map 0x2A4B、Report 0x2A4D（複数）、Protocol Mode 0x2A4E、HID Information 0x2A4A、HID Control Point 0x2A4C。Boot Keyboard Input Report 0x2A22・Boot Mouse Input Report 0x2A33 は無視（report protocol だけ）。characteristic は 16 まで（超えたら `protocol`）。
4. 各 Report の descriptor: Find Information を value handle + 1 から次の characteristic の宣言 − 1 まで。Report Reference 0x2908（value 2 byte: report ID、report type 1 = Input・2 = Output・3 = Feature）、Client Characteristic Configuration 0x2902。Report Reference を Read。
5. Report Map を Read、応答が MTU − 1 ちょうどなら Read Blob を offset で繰り返す（4096 まで。超えたら `descriptor`）。External Report Reference 0x2907（他の service の characteristic を report として使う形）は非対応（記録して無視）。
6. Device Information の PnP ID 0x2A50（Vendor ID Source 1 byte、Vendor ID 2、Product ID 2、Product Version 2）を Read（無ければ vendor・product は 0）。
7. Protocol Mode があれば Write Command で 0x01（Report Protocol Mode）。
8. `/dev/hid-host` に setup を書く（§4.7。descriptor は Report Map）。
9. 各 Input Report の CCC に Write Request で 0x0001（notification）。Battery Level 0x2A19 を Read し CCC があれば notification。
10. OPEN。Handle Value Notification（handle → Report Reference の ID と type）: **Report Map が report ID を使うか**は、Report Reference の ID が 1 つでも 0 でなければ「使う」と見て、notification の値の先頭に ID を付けて write する（HOGP では notification に ID が無い。design §5.2 [F7]、§Q12）。全部 0 なら付けない。知らない handle の notification は数える。Battery Level の notification は STATUS の `battery=` に出す。
- security: 暗号化（LE Enable Encryption、§4.7）の **後**に 1 から始める。それでも Insufficient Authentication（0x05）・Insufficient Encryption（0x0F）・Insufficient Encryption Key Size（0x0C）が返れば `security` で切る（bond が相手の要求（authenticated）に足りない: Settings が「もう一度 pairing」と出す材料。p006）。
- Service Changed（GATT 0x2A05 の indication）: Confirmation を返し、link を切って再接続（発見をやり直す）。
- 口: `btd_hog_start(struct btd_hog *)` → `BTD_HOG_SEND`（`out` を ATT で送る）／`BTD_HOG_SETUP`（descriptor と PnP が揃った）／`BTD_HOG_OPEN`／`BTD_HOG_FAILED`（why）。`btd_hog_input(struct btd_hog *, const uint8_t *pdu, size_t length)` → 同じ action の bit と、notification の時は `BTD_HOG_REPORT`（`report`・`report_length`、ID を付けた後）。`btd_hog_tick(now)` で transaction の timeout。

#### 4.7 HID host の核（`hid.[ch]`）: device の表と接続の流れ

- 表: `struct btd_hid_device hid[BTD_HID_MAX]`（6。session の link の数 8 のうち pairing 1 と余り 1 を残す）。field: address・type、transport（`BTD_HID_BREDR`／`BTD_HID_LE`）、state（IDLE・CONNECTING・AUTHENTICATING・ENCRYPTING・SDP・CHANNELS・SETUP・OPEN・CLOSING）、handle、`connected`、`encrypted`、`key_size`、`btd_reassembly`・`btd_l2cap`（BR/EDR）、channel の local CID（sdp・control・interrupt）、`btd_sdp`・`btd_hog`・`btd_att` の状態、`/dev/hid-host` の fd（-1）、`struct btd_hid_record`（descriptor 4096・flags・vid・pid・version・名前）、report の待ち行列（setup の前に来た DATA を 32 まで、§4.8）、`wanted`（再接続する）、`reconnect_due`・`retry_ms`、counters（reports・malformed・dropped）、`battery`（-1）、`since_ms`、`last_error`。
- **CONNECT（host から）BR/EDR**: bond を読む（無ければ `not-bonded`。`hid=1` の印は無くてもよい: 最初の CONNECT が SDP で HID を確かめ、印を付ける）→ Create Connection（pair.c と同じ parameter。packet type 0xCC18、R1、clock offset 0、role switch 可）→ Connection Complete → Authentication Requested → Link Key Request に保存した鍵で Reply → Authentication Complete（0x06 なら `key-missing`、他の失敗は `security`）→ Set Connection Encryption（on）→ Encryption Change（0 でない値を on）→ Read Encryption Key Size（16 でなければ `key-size` で切る、KNOB）→ L2CAP connect PSM 0x0001 → SDP（§4.3。HID の record が無ければ `no-hid`）→ L2CAP disconnect（SDP）→ L2CAP connect PSM 0x0011 → PSM 0x0013（両方 OPEN）→ SET_PROTOCOL（HIDBootDevice の時）→ **setup を `/dev/hid-host` へ**（fd は親から §4.10、名前は bond の `name`、無ければ SDP の ServiceName（**未確認**: 属性 0x0100 + language base）、無ければ空で kernel の種類の名前。UTF-8 の境で 63 byte に切る。`physical_path` は `bluetooth/<controller>/<address>`、`unique_id` は address の文字列、bus `BUS_BLUETOOTH`）→ `HID_HOST_GET_DEVICE` で eventN → bond に HID の field を書く（§4.11）→ OPEN、`CONNECTED` の答え。
- **CONNECT LE**: bond（LTK。無ければ `not-bonded`）。auto-connect（§4.9）が出ていれば LE Create Connection Cancel で止めてから、LE Create Connection（pair.c の parameter と同じ、相手の address・型は bond の identity）→ LE Connection Complete → LE Enable Encryption（0x2019: handle、rand 8、ediv 2、LTK 16。SC の bond は rand・ediv 0）→ Encryption Change（status 0x06 PIN or Key Missing は `key-missing`、他の失敗は `security`。bond の key_size が 16 でなければ始めから `key-size`）→ ATT/HOGP（§4.6）→ setup → OPEN。接続の parameter: 相手の Connection Parameter Update Request は l2cap.c の Accept（p004）に LE Connection Update を送る。
- **相手から（BR/EDR、HIDReconnectInitiate な device）**: Connection Request（address、class、link type ACL）→ router が bond 済みの HID（`hid=1`）で `wanted` の device だけ受ける（Accept Connection Request 0x0409、role 0x01 = slave のまま。**未確認**: HID 1.1.1 が host の master を勧めるか。role switch は相手の自由）→ Connection Complete → 相手が認証する: Link Key Request → Reply（鍵が無ければ Negative、相手が切る）→ Encryption Change（on）→ Read Encryption Key Size（16 でなければ切る）→ 相手の L2CAP Connection Request PSM 0x0011・0x0013 を §4.2 の policy で受ける（暗号化の前の request は Security Block）→ 両方 OPEN → setup（bond の cache の descriptor。無ければ SDP を先にし、その間の DATA は 32 まで待ち行列、超えたら捨てて数える）→ OPEN。`CONNECTED` の行は STATUS（と p006 の event）で見える。
- **LE の相手から**: LE の peripheral の役（相手が central になる）は範囲の外（HOGP の device は peripheral）。再接続は §4.9 の auto-connect。
- **切断**: Disconnection Complete（理由を記録: 0x08 supervision timeout、0x13 remote user、0x16 local、0x05 authentication failure …）→ `/dev/hid-host` の fd を close（kernel が key を離す）→ channel の表・ATT の状態を捨てる → `wanted` なら再接続の予定（§4.9）。**DISCONNECT（人から）**: BR/EDR は HID_CONTROL は送らず（VCU は unpair の意味）L2CAP の Disconnect（interrupt → control）→ HCI Disconnect（0x13）。LE は HCI Disconnect。`wanted = 0`（§Q5）。
- **FORGET の時に接続中**: BR/EDR は control channel に HID_CONTROL VIRTUAL_CABLE_UNPLUG を送ってから切る（design §6.3）。LE は切るだけ。filter accept list・resolving list から消す。
- setup の write が EINVAL（bounce の落ち、§3）なら 100 ms おきに 3 回。他の errno（E2BIG・ENODEV・EOPNOTSUPP）は `descriptor` で切る。kernel が断る descriptor（電波から来る）で daemon は止まらない。
- timeout: 接続 10 秒（Create Connection Cancel／LE Create Connection Cancel）、認証・暗号化 10 秒、SDP 10 秒、channel 10 秒、HOGP の発見 30 秒（ATT の transaction 30 秒とは別）、全体 60 秒。過ぎたら `timeout` で切る。
- 1 つの device に同時に 1 つの流れ（CONNECT 中の CONNECT は `busy`）。別の device の CONNECT は並ぶ（BR/EDR の Create Connection は controller が 1 つずつ: 2 つ目は Command Disallowed になり得るので、daemon は **BR/EDR の page を 1 つずつ**（待ち行列）、LE の接続も 1 つずつ）。SCAN と PAIR の間は新しい CONNECT を始めない（`busy`。既に OPEN の device はそのまま）。

#### 4.8 security の検査（design §6.4、[F3, N4, N5]）

HID の channel（BR/EDR）と HOGP の発見（LE）を始める前に、全て満たす: (1) bond がある（鍵の file）、(2) link が暗号化されている（Encryption Change on）、(3) 鍵の長さ 16（BR/EDR は Read Encryption Key Size、LE は bond の `key_size`）。満たさない link で来た L2CAP の Connection Request は Security Block、ATT は送らない。legacy の bond（`legacy=1`）は D10 で受ける（STATUS に `legacy=1`）。相手の descriptor・report は全部 kernel の parser に渡す前に長さだけ daemon が検査し（4096・1024）、中身は kernel の parser（fuzz 済み、§7.2）が見る。待ち行列（setup 前の DATA）は 32 report で捨てる。SDP の record は 8 KiB、ATT の MTU は 185、HOGP の characteristic は 16、report は 16、notification の handle の表は 16。

#### 4.9 再接続（design §6.4、受け入れ 3）

- 起動（controller が READY）と bond の変化で `btd_hid_refresh()`: bond ごとに `hid=1` なら表に入れ `wanted = 1`。
  - BR/EDR: bond 済みの HID が 1 つでもあれば **Write Scan Enable に page scan（bit 1）を立てる**（inquiry scan は pairing の mode の間だけ、D11b。pair.c／main.c の pairing の mode と or で合成する関数 `btd_scan_enable_update()` を session に置く）。`hid_reconnect_initiate=0`（host から）か `hid_normally_connectable=1` の device は daemon が page する: 起動時すぐ、失敗したら 5 秒 → 10 → 20 → 40 → 60 秒（上限）で繰り返す（`unreachable` は普通: 相手が寝ている）。`hid_reconnect_initiate=1` の device は待つだけ（page scan で受ける）。
  - LE: bond 済みの HOGP の device を **filter accept list**（LE Add Device To Filter Accept List 0x2011。identity address）に入れ、IRK のある bond は **resolving list**（LE Add Device To Resolving List 0x2027: peer identity address・peer IRK・local IRK（0 で可）、LE Set Address Resolution Enable 0x202D = 1、LE Set Resolvable Private Address Timeout は既定）に入れ、**LE Create Connection を Initiator_Filter_Policy = 1（filter accept list）で出しておく**（auto-connect。相手が directed/undirected advertising をすれば controller が繋ぐ）。LE Connection Complete の address は resolving list で identity に戻る（**未確認**: 戻るのは LE Enhanced Connection Complete（subevent 0x0A）の時で、普通の LE Connection Complete は RPA のまま返す controller があるか。実装では両方の subevent を受け、RPA なら p004 の `pair_resolved_bond` と同じ ah で bond を探す）。resolving list が無い controller（Read Local Supported Commands の bit、**未確認**: AX211 の値。loopback は持つ）では、相手が RPA を使う bond の再接続は **背景の passive scan**（Advertising Report の address を ah で解決 → LE Create Connection 直接）に落とす。
  - auto-connect の LE Create Connection が出ている間は LE scan・LE の PAIR・LE の CONNECT（直接）ができない（Command Disallowed になり得る）ので、SCAN・PAIR・CONNECT の前に LE Create Connection Cancel（→ LE Connection Complete status 0x02 を待つ）を送り、終わった後に出し直す（`btd_hid_le_hold()`／`_release()`）。**未確認**: scan と initiating の同時を許す controller の範囲（Core 5.4 は許すが、controller の実装次第）。
- supervision timeout・相手の切断（0x08・0x13・0x16 以外）→ `wanted` のまま → BR/EDR は上の page の規則、LE は auto-connect を出し直す。
- resume: main.c が `/dev/system` を開いて `KERN_SYSTEM_EVENT_POWER` を購読し（bluetoothd は今 `/dev/system` を開いていない、事実 16。design §5.3 が `sleep.end` に求める **Read Version のやり直しと firmware の load し直し（S0ix で bootloader に戻る時）と、受けて渡していない report の捨て**のうち、この Phase は HID の分（report の捨て = 切断で fd を close、再接続）だけを作り、Read Version のやり直しは p003 の transport の残件として Q1 に行き先を聞く（§Q19）。購読の口は 1 つにし、`btd_hid_resume()` と後の `btd_session_resume()` を同じ event から呼ぶ形にする）、`sleep.end` で `btd_hid_resume()`: OPEN の device の最後の report からの時間を見ず、**全ての OPEN の link に Read RSSI（0x1405）か Read Link Quality を送らず**、単に supervision timeout の Disconnection Complete を待つ（design §5.3: 相手が切る）。ただし 5 秒の間に Disconnection Complete も report も無い BR/EDR の link は HCI Disconnect して page に戻す（S0ix で controller の link の状態だけ残り相手が居ない時のため。**未確認**: AX211 の S0ix の後の link の状態）。LE は auto-connect を出し直す。re-enumerate（新しい `/dev/btN`）は p003 の「新しい controller」の経路で `btd_hid_refresh()` が走る。
- DISCONNECT の後（`wanted = 0`）: 相手からの Connection Request は Reject（§Q5）。CONNECT で `wanted = 1` に戻る。daemon の再起動で `wanted` は 1 に戻る（記憶しない）。
- 鍵の離し: 切断の経路で fd を close するだけ（kernel が離す、事実 4）。daemon が落ちた時も kernel が fd を閉じるので残らない。

#### 4.10 privsep の拡張

- 子 → 親の datagram に `OPEN-HID` を足す。親は `/dev/hid-host` を `O_RDWR|O_CLOEXEC` で開けて `OK /dev/hid-host` と SCM_RIGHTS で答え、自分の写しを close する。失敗は `ERR <errno>`（EBUSY: 8 枚、EPERM）。親は枚数を数えない（上限は kernel の `HID_HOST_OPENS_MAX`。子が閉じたかを親は知れないので、親の側の数えは誤る）。
- 親の変更はこの 1 verb だけ。親の `OPEN` の path の検査（`/dev/bt` と 1〜2 桁）は変えない。

#### 4.11 bond の file の HID の field（keys.c）

`key=value` の行を足す（知らない key は読み飛ばす規則のまま。p004 の試験は通る）: `hid=1`、`hid_transport=bredr|le`、`hid_reconnect_initiate=0|1`、`hid_normally_connectable=0|1`、`hid_virtual_cable=0|1`、`hid_boot_device=0|1`、`hid_vendor=`・`hid_product=`・`hid_version=`（hex 4 桁）、`hid_country=`、`hid_descriptor_<n>=`（n は 0 から、1 行に 128 byte = hex 256 文字、最大 32 行で 4096 byte。n は連続で、欠けや重複は壊れた file として HID の field を捨てる。BR/EDR だけ。LE は毎回発見するので書かない、§Q6）、`hid_descriptor_size=`、`hid_name=`（SDP の ServiceName、escape 済み）。**keys.c の読みは 1 行 600 byte の buffer**（`userland/base/bluetoothd/keys.c:383`）なので descriptor は 1 行に置けず、分ける。書きは一時 file と rename（既存の `btd_keys_write`）。`btd_keys_list` の `struct btd_bond` に HID の field を足す（表は 64 × 4 KiB = 256 KiB になるので、descriptor は `btd_bond` に持たず `btd_keys_read_hid()` で別に読む）。

#### 4.12 PAIR の後の自動の CONNECT（§Q4）

`PAIRED` の hook（main.c の `btd_paired`）で、相手が HID らしい時（BR/EDR: scan の表の class の major device class が Peripheral（bit 8〜12 = 0x05）。LE: appearance の上位 6 bit が HID（0x03C0〜0x03FF）。表に無ければ **試す**）に `btd_hid_connect(address, type, 自動)` を始める。PAIR の client には `PAIRED` と `DONE` を今までどおり返す（自動の CONNECT の結果は STATUS と p006 の event）。pairing の流れ（pair.c）は変えない: PAIR は切断で終わり、CONNECT が新しく繋ぐ（相手が切断の直後の page に答えない時は §4.9 の再試行に乗る）。

### 5. 口の request と CLI

| request | 誰 | 答え |
| --- | --- | --- |
| `CONNECT <address> <bredr|le-public|le-random>` | D8 の許す人（`btd_permitted`） | 繋いで setup まで。`CONNECTED address=… type=… transport=hid|hog input=/dev/input/eventN [touch=/dev/input/eventM] name="…" legacy=0|1 vendor=XXXX product=XXXX` と `DONE`。失敗は `ERROR <理由>` と `DONE`: `not-bonded`・`busy`（この device の流れが進行中、または SCAN・PAIR の最中）・`limit`（表が満ちた、6）・`unreachable`・`timeout`・`key-missing`・`key-size`・`security`（認証・暗号化の失敗、ATT の 0x05/0x0F/0x0C）・`no-hid`（SDP に HID の record が無い、GATT に HID service が無い）・`descriptor`（kernel が断った、4096 超）・`protocol`・`lost`・`not-ready`・`permission`。既に OPEN なら `CONNECTED` をそのまま返す |
| `DISCONNECT <address> <型>` | 同 | 切って `DONE`（Disconnection Complete の後、3 秒）。`ERROR not-connected` |
| `STATUS` | 誰でも | bond 済みの HID の device ごとに `HID address=… type=… transport=hid|hog state=idle|connecting|open|waiting input=/dev/input/eventN|- name="…" reconnect=auto|off battery=NN|- since=<秒> last=<理由>|-` と `DONE`。`waiting` は切れていて再接続を待つ |
| `SHOW` | p003 のまま | CONTROLLER の行の末尾に `hid=<open の数> page_scan=0|1 le_auto=0|1` |
| `FORGET` | p004 のまま | 接続中なら VCU（BR/EDR）と切断の後に消す |

- 1 client に 1 request の規則のまま（CONNECT の client は `waits_connect`）。CONNECT の client が切れても流れは続ける（結果は STATUS）。
- CLI: `bt connect ADDRESS [TYPE]`（最後の行 `BT CONNECT result=connected|error input=/dev/input/eventN`）、`bt disconnect ADDRESS [TYPE]`（`BT DISCONNECT result=ok|error`）、`bt status`（daemon の行と `BT STATUS devices=N open=M`）。終了の値は p003 と同じ。
- p006 のための備え: 状態の変化の行（`CONNECTED`・`DISCONNECTED address=… reason=…`・`BATTERY address=… level=NN`）の形をここで固定し、`SUBSCRIBE` の配りは p006。

### 6. 試験の kernel の loopback の controller の相手

`bt-hci-loopback.c` に足す（QEMU で確かめるため。`loopback_frame` の payload の上限 16 byte を 256 byte に広げ、SDP・ATT の応答を出せるようにする）:

- **0A:0B:0C:0D:0E:01（Loopback Keyboard、既存の DisplayYesNo の相手）に HID のキーボードを足す**: 接続は p004 のまま（handle 0x0040）。暗号化の後の L2CAP: Connection Request PSM 0x0001 → Connection Response（remote CID 0x0040、成功）→ 自分の Configure Request（MTU 672、Flush Timeout 0xFFFF を付けて option の受けを通す）と相手の Configure Request への Response。SDP の ServiceSearchAttributeRequest に **固定の byte 列の record**（ServiceClassIDList {0x1124}、ProtocolDescriptorList {{L2CAP, PSM 0x0011}, {HIDP}}、AdditionalProtocolDescriptorLists {{{L2CAP, 0x0013}, {HIDP}}}、HIDDescriptorList {{0x22, boot keyboard の descriptor 63 byte（HID 1.11 Appendix B.1 の形を自分で書く。report ID 無し、8 byte の report）}}、HIDReconnectInitiate false、HIDNormallyConnectable true、HIDVirtualCable true、HIDBootDevice true）を、MaximumAttributeByteCount が record より小さければ **continuation で 2 回に分けて**返す（continuation の経路を通す）。PnP の record（0x1200: vendor 0x1209、product 0x4B42、version 0x0100）。PSM 0x0011・0x0013 の Connection Request を受ける（CID 0x0041・0x0042）。control の SET_PROTOCOL に HANDSHAKE SUCCESSFUL。interrupt が開いて 100 ms 後に DATA INPUT `00 00 04 00 00 00 00 00`（'a' 押す）、200 ms 後に全部 0（離す）、300 ms 後に `00 00 05 00 …`（'b' を押したまま）。以後 3 秒ごとに 'a' の押し・離しを繰り返す（'b' は押したまま。切断で kernel が離すのを見る）。host からの L2CAP Disconnect・HCI Disconnect に答える。
- **0A:0B:0C:0D:0E:02（class 0x002580 のマウス、Inquiry Result with RSSI の既存の address）を BR/EDR の HID のマウスにする**: pairing は NoInputNoOutput（Just Works、07 と同じ流れ、鍵 type 7）、handle 0x0043。SDP の record は HIDReconnectInitiate **true**、HIDNormallyConnectable false、descriptor は boot mouse（3 byte: button・X・Y、report ID 無し）。channel が両方開いた 500 ms 後から 1 秒ごとに DATA INPUT `00 05 00`（X +5）。**接続から 4 秒後に自分で切る**（Disconnection Complete、理由 0x08 supervision timeout）、その 1 秒後に **Connection Request（address 02、class 0x002580、link type ACL）** を出し、host の Accept Connection Request（0x0409）に Command Status と Connection Complete（handle 0x0043）、続けて Link Key Request（host が Reply）→ Encryption Change（on）、そして自分から L2CAP Connection Request PSM 0x0011、0x0013（host の Response と Configure を受ける）→ DATA を再開（切断は 1 回だけ。2 度目の接続は切らない）。Reject（0x040A）が来たら何もしない（`wanted = 0` の試験）。
- **0A:0B:0C:0D:0E:04（Loopback HOG Mouse、新規。LE の public）**: **pre-bonded**（§Q7: 試験が bond の file を root で書く。固定の LTK `00 11 22 … FF`、ediv 0、rand 0、key_size 16、legacy 0）。LE Set Scan Enable の広告の報告に 04 を足す（appearance 0x03C2、名前「Loopback HOG Mouse」）。LE Create Connection（直接の address 04、または filter policy 1 で表に 04 がある時）に LE Connection Complete（handle 0x0044）。LE Enable Encryption（0x2019）には鍵を見ずに Encryption Change（on、0x01）。LE Add Device To Filter Accept List・Resolving List・Set Address Resolution Enable・LE Create Connection Cancel は Command Complete（Cancel は status 0x02 の LE Connection Complete も）。ATT server（固定の表）: 0x0001〜0x0005 GAP（Device Name）、0x0010 primary HID 0x1812（終わり 0x0020）: 0x0011/0x0012 HID Information、0x0013/0x0014 Report Map（マウス、report ID 1、約 50 byte → MTU 23 なら Read Blob が要る）、0x0015/0x0016 Report（Input）+ 0x0017 CCC + 0x0018 Report Reference {1, 1}、0x0019/0x001A Protocol Mode、0x001B/0x001C HID Control Point、0x0030 primary Battery 0x180F: 0x0031/0x0032 Battery Level 80 + 0x0033 CCC、0x0040 primary Device Information 0x180A: 0x0041/0x0042 PnP ID {1, 0x1209, 0x4842, 0x0100}。Exchange MTU は 23 のまま（Read Blob を通す）。CCC 0x0017 に 0x0001 が書かれたら 1 秒ごとに Handle Value Notification（0x0016、`00 05 00`: button 0、X +5、Y 0。ID 無し）。Write Request に Write Response、Read By Group Type・Read By Type・Find Information・Read・Read Blob を表どおり、無い物は Error Response Attribute Not Found。暗号化の前の HID の Read には Error Response Insufficient Encryption（0x0F）（security の順の試験）。
- **03（既存の LE、pairing を断る）はそのまま**（p004 の試験）。
- 変えない物: 05・06・07、Information Request の答え、SMP の Pairing Request への断り（03）、Inquiry と広告の報告の既存の内容（01 の class 0x002540、02 の class 0x002580 は HID に合う）。

### 7. 試験

#### 7.1 host: kernel の glue（`plan/ws143/tests/hid-input-host-test.sh`、新しい `.c`）

freestanding で `hid-report.c`・`hid-digitizer.c`・`hid-touch.c`・`hid-input.c` を compile（`run-hid-pen.sh` の flag）、試験は `drv_input_device_register`・`_unregister`・`_emit_at`・`drv_input_device_number`・`kern_*`・`kern_logf` の stand-in（emit を記録）を持つ。試験の中に **旧の `usb_hid_publish_report`（2026-10-08 の usb-hid.c 1031〜1167 行）と `usb_hid_fetch_layout` の記述の部分の写し**を置き（試験だけ。製品の code は glue）、同じ descriptor と report の列で新旧の emit の列が byte 単位で一致することを確かめる: boot keyboard（複数 key の押し・離し・ErrorRollOver の keyboard_error）、report ID 2 つのキーボード（集約: 片方の report の key を他方の report で離さない）、マウス（EV_REL 0 の抑制、button）、pen（digitizer の経路）、touch（`plan/ws159/tests/latitude5330-linux/synaptics-06cb-ce65-rdesc.bin` の touchpad の report、touch の device が別に登録される）、capability が SYN だけ（device が作られない、ENODEV）、prepare → publish → unpublish → destroy の寿命、`report_max`、`numbers`。

#### 7.2 host: hid-report.c の fuzz（`plan/ws143/tests/hid-report-fuzz.sh`、`.c`、design §5.2 [F6]）

ASan・UBSan（freestanding の file を `-fsanitize=address,undefined` で compile できるかは**未確認**: `-ffreestanding` と sanitizer の組み合わせは clang で通る見込み。通らなければ試験の側の stand-in の `kern_malloc` に赤帯を付ける）。固定の seed、1 回 60 秒まで。(a) ランダムな descriptor（item の形を守るランダム: short item の tag・type・size、long item、collection の深さと ID の数を上限の周りで振る）→ `drv_hid_input_prepare` は 0・EINVAL・E2BIG・ENOMEM だけを返し、成功なら `report_max ≤ 1025`、capability の数と axis の数は上限の中。(b) 本物の descriptor（boot keyboard・boot mouse・5330 touchpad・pen・multitouch）の変異（byte の置換・挿入・削除・切り詰め）。(c) parse の成功した layout にランダムと変異の report（長さ 0〜1025）を `drv_hid_input_report` → 落ちない、emit の type・code が capability の中、EV_KEY の code ≤ KEY_MAX。回数と見つけた物を出力。回帰に使うので Q1 に master の Tools への登録を依頼する。

#### 7.3 host: `/dev/hid-host` の純粋な検査

`hid_host_setup_valid()`（magic・version・bus・descriptor_size・reserved・NUL）と report の長さの検査を `hid-host.c` から切り出した純粋な関数として、hid-input-host-test に足す（cdev・file・cred の stand-in は作らない。cdev の経路は QEMU）。

#### 7.4 host: bluetoothd（`bt-daemon-host-test.sh` に足す、新しい `bt-hid-host-test.c`）

- sdp: request の組み立ての byte、応答の解析（§6 の固定の record、continuation 2 回、同じ continuation を返し続ける相手、入れ子 9 段、長さが残りを超える要素、uint の size の違い、HIDDescriptorList が無い、4097 byte の descriptor）、fuzz。
- hidp: header の組み立てと解析、HANDSHAKE の値、DATA の type、1025 byte の DATA。
- att: 各 PDU の組み立てと解析（list の要素の数の整合、format 1・2、Error Response）、fuzz。
- hog: 台本の ATT server（§6 の表と、変種: Report Map 3 回の Blob、Protocol Mode 無し、Report Reference が全部 0（ID を付けない）、HID service 無し（`no-hid`）、Insufficient Encryption（`security`）、Service Changed、characteristic 17 個、transaction の timeout）→ action と setup の内容、notification に ID が付く・付かない。
- l2cap: inbound の Connection Request（policy の accept・Security Block・PSM not supported・No resources）、Configure Request の Flush Timeout・QoS（best effort・guaranteed）・RFC（basic・ERTM）・FCS、`opened`・`closed` の effect。
- router: handle・address の配り、知らない相手の Connection Request の Reject、bond 済み（wanted）の Accept、wanted=0 の Reject、Disconnection Complete と同じ handle の再利用。pair.c の既存の試験（bt-link-host-test）の「相手から始まる pairing の拒否」を router に移す。
- hid（偽の controller の台本 + 偽の `/dev/hid-host` = socketpair の片方を読む）: BR/EDR の CONNECT の全段（setup の byte、report の write、close）、key-missing、key-size 7、no-hid、descriptor 4097、setup の EINVAL 3 回のやり直し、DATA の待ち行列 32 と溢れ、相手からの接続（Connection Request → … → channel → setup）、VCU（bond が消える）、DISCONNECT（wanted=0 → 次の Connection Request は Reject）、再接続の backoff の時刻（tick）、LE の CONNECT（LE Enable Encryption の parameter、Encryption Change 0x06 → key-missing、HOGP → setup → notification → write）、auto-connect の hold/release（SCAN の前の Cancel と後の出し直し）、resume（sleep.end の後 5 秒の規則）。
- keys: HID の field の書き読み、知らない key の読み飛ばし（p004 の file がそのまま読める）、descriptor の 32 行の分割と、欠けた行・重複した行・`hid_descriptor_size` と合わない時の捨て方。

#### 7.5 QEMU（T1。image は `build-bt-image.sh`。`config-amd64-bt.mk` に `evdev-probe hid-host-probe` を足す）

- `hid-host-p005.sh`（kernel だけ、i01）: `hid-host-probe` が root で `/dev/hid-host` にキーボード（boot の descriptor、bus 5、名前「Probe Keyboard」）を作り、`evdev-probe -b bluetooth -t 5000` が `/dev/input/eventN` を見つけ（EVIOCGID の bustype 5、EVIOCGNAME）、probe が 'a' の press・release、'b' の press を書き、evdev が KEY_A 1/0・KEY_B 1 を読む。probe が close → evdev は KEY_B 0・SYN と、その後の read の ENODEV（node が消えた）。誤用: setup の前の report（EINVAL）、壊れた magic（EINVAL）、descriptor_size 4097（EINVAL）、FIDO の descriptor（EOPNOTSUPP）、9 枚目の open（EBUSY）、`runas btuser` の open（EACCES か EPERM）、read（EAGAIN）、`HID_HOST_GET_DEVICE` の番号と `evdev-probe` の node が同じ。`systemevents -c input -n 2` が ADD と REMOVE（subject eventN、detail `bus=5`）を出す。
- `hid-usb-p005.sh`（USB の回帰、i01）: guest の `-device usb-kbd`（既存）と `--qemu-extra "-device usb-mouse,bus=xhci.0,port=4"`。`evdev-probe -b usb` が 2 つの node（bus 3）を数え、T1 の QMP で `send-key`（keys `a`）と `input-send-event`（rel の x +5）を送り、evdev が KEY_A 1/0 と REL_X 5 を読む。QMP の socket は guest.py が `-qmp unix:<runtime>/qmp.sock,server,nowait` で開いている（事実 19。`boot-test.sh` の screendump と同じ socket）が、guest.py に汎用の command の口は無いので、T1 の script が `qmp_capabilities` → command の 2 行を socket に書く小さな手順（python の `socket` と `json`）を持つ。guest の usb-kbd の node を `evdev-probe` が開いている間、compositor の読みは要らない（この image は Files の image で desktop は無い）。
- `bt-hid-p005.sh`（i02 以降。bt-pair-p004.sh の形）: (1) `bt pair 01`（CONFIRM に y、p004 のまま）→ `bt status` が自動の CONNECT の後 `state=open input=/dev/input/eventN transport=hid`（§4.12）、`evdev-probe -b bluetooth` が KEY_A 1/0、KEY_B 1 を読む、`bt disconnect 01` → evdev が KEY_B 0 と ENODEV、`bt status` が `reconnect=off`。`bt connect 01` → open。(2) `bt pair 02`（CONSENT に y）→ 自動の CONNECT → open、evdev が REL_X 5 → 4 秒後に loopback が切る → `state=waiting` → 1 秒後に相手から再接続 → open、REL_X が続く（`bt status` の `since` が新しい）。`bt disconnect 02` の後に loopback の Connection Request は Reject（loopback は 1 回しか再接続しないので、この確かめは host 試験）。(3) 試験が root で `/var/db/bluetooth/00:11:22:33:44:55/0A:0B:0C:0D:0E:04-le-public` を書く（`type=le-public ltk=… ediv=0 rand=0000000000000000 key_size=16 authenticated=0 secure=1 legacy=0`、0600、owner 80）→ `bt connect 0A:0B:0C:0D:0E:04 le-public` → `transport=hog input=…`、evdev が REL_X 5 を 1 秒ごと、`bt status` に `battery=80`。`bt disconnect 04`。(4) 権限: `runas btuser bt connect …` は `ERROR permission`、`runas btuser bt status` は通る。(5) daemon の再起動（kill → `/sbin/bluetoothd &`）: 01（host から）が自動で open、04（auto-connect の filter accept list、loopback が LE Connection Complete）が open、02 は `waiting`（相手から来るまで）。(6) `bt forget 01`（接続中: VCU → 切断 → file が消える）。(7) 回帰: `bt-pair-p004.sh`・`bt-daemon-p003.sh`・`bt-loopback-p002.sh` が PASS のまま（01・02 の pairing の振る舞いは変えない。p003 の Inquiry の名前・class はそのまま）。(8) `hid-usb-p005.sh` と同じ guest で USB の node が残っていること（bus 3 の node の数）。
- 合否: 各 `ok` と最後の `bt-hid-p005: PASS`。1 回の試験は 3 分以内を目標（loopback の待ち時間は秒の単位）。

#### 7.6 実機

- p008 の UAT（本物の BR/EDR と LE のキーボード・マウス、再接続、suspend）。i04 で 5330 に手元の device があれば短い接続（`bt connect` → `evdev-probe`）を T1 の lock の下で試す（任意）。
- 共通の誤りの限界（p004 の review-2 S-h と同じ）: loopback・偽の controller・daemon は同じ理解で作るので、SDP の byte の順、HIDP の header、ATT の PDU、Report Reference の順、LE Enable Encryption の parameter の順の取り違えは QEMU では見つからない。実機（p008）で btmon などの外部の記録と照合する。

### 8. ライセンスと参照

- 新しい code は全て Zlib（`plan/coding-style.md` §13 の header）。
- 参照は Bluetooth Core 5.4（Vol 3 Part A L2CAP、Part B SDP、Part F ATT、Part G GATT、Vol 4 Part E HCI）、HID Profile 1.1.1、HOGP 1.0、HID 1.11（descriptor の形）、Bluetooth SIG Assigned Numbers、Device ID Profile 1.3。**値の多くに「未確認」を付けた**（§4.3〜4.6）。実装の attempt の最初に仕様の PDF で照合して表にする（照合の結果はこの phase.md に書く）。
- Linux の GPL の code（BlueZ の kernel 部、hidp、uhid、hid-core）は読まない。BlueZ の userland（GPL/LGPL）も写さない。FreeBSD の `bthidd`・`sdpd`（BSD-2-Clause）は手順の確認の参照に使ってよいが code は写さない（design の冒頭の iwmbtfw と同じ扱い。使った時は commit・path・sha256 を記録する）。
- 既存の zedBSD の code の移動（usb-hid.c → hid-input.c）は同じ著作者の Zlib の中。

## 判断の記録

| ID | 判断 | 理由 |
| --- | --- | --- |
| Q1 | **判断待ち（ユーザー）**: i2c-hid は共有 glue に乗せない（USB と hid-host だけ）。design.md §5.2 の「USB・I2C・hid-host が共有する module」の文言からの縮小 | i2c-hid は touch の device だけを出し、key の集約・pen・名前の規則を使わない（事実の表の i2c-hid の行）。乗せ替えは 5330 の touchpad（実機だけ）の回帰の危険に見合う益が無い。選ばなかった案: (b) 乗せる（touch だけの経路を glue に通す。回帰は p008 の実機で）→ Bluetooth の Phase に無関係の危険を足す。Future Work に「i2c-hid を hid-input に乗せる」を登録（Q1 に依頼） |
| Q2 | **判断待ち（ユーザー）**: `HID_HOST_GET_DEVICE`（eventN の番号と malformed の数）の ioctl を D3 の形に足す | 無いと daemon が自分の作った node を知れず、`CONNECTED input=…`・STATUS・試験が `/dev/system` の INPUT の event と `bus=5 name=` の照合に頼る（競合し、同じ名前の device が 2 つあると見分けられない）。選ばなかった案: (b) ioctl 無し、daemon が `/dev/system` の event を見る → 上の弱さ。(c) write の返り値に番号を乗せる → write の約束（size を返す）を壊す |
| Q3 | Bluetooth の HID は hidraw に出さない。FIDO の descriptor は hid-host が EOPNOTSUPP | hidraw は「input でない raw の interface」の口（ws161 U1）。キーボードを hidraw にも出すと、grab していない読み手に打鍵が見える口が増える。Bluetooth の FIDO は WS161 の範囲でも無い。将来要れば `hid_host_setup` に flag を足して hidraw に出す（reserved を使う） |
| Q4 | **判断待ち（ユーザー、製品の振る舞い）**: PAIRED の後に HID らしい device へ自動で CONNECT する | 利用者は pairing の後にすぐ使えることを期待する（Settings の「device を足す」の後に「接続」を押させない）。選ばなかった案: PAIR が接続を保って HID を始める → pair.c の Q5（接続を保たない）を変え、pairing の失敗の経路と HID の経路が絡む |
| Q5 | **判断待ち（ユーザー、製品の振る舞い）**: 人が DISCONNECT した device は、CONNECT・再 pairing・daemon の再起動まで、相手からの再接続を Reject する（`wanted=0`） | 「切断」の直後に device が自分で繋ぎ直すのは人の意図に反する。選ばなかった案: 相手からの接続は常に受ける（bond 済みなので）→ 切断がすぐ戻る。記憶しない（再起動で戻る）のは、電源の入れ直しで使えなくなる事故を避けるため |
| Q6 | BR/EDR は SDP の結果（descriptor・flags・PnP）を bond の file に cache し、相手からの再接続で使う。LE は毎回 GATT の発見 | 相手からの BR/EDR の再接続は channel が開いた直後に DATA が来る（descriptor が無いと待ち行列と遅れ）。LE は CCC を書くまで notification が来ないので待ちが無く、handle の cache は firmware の更新で狂う（Service Changed の扱いが要る）。cache が無い bond（p004 で作った物）は SDP を先にして待ち行列 32 |
| Q7 | QEMU の LE の HOGP の相手は pre-bonded（試験が bond の file を書く）。loopback に SMP の相手は作らない | kernel に AES-128 はある（事実 21）が legacy の c1・s1 を試験の kernel に書くのは大きく、SC は P-256 が要る。LE の pairing の全体は host 試験（p004 の bt-link-host-test）で確かめ済み。p004 の Q3 と同じ結論 |
| Q8 | 出力の report（LED）は作らない。read は EAGAIN、poll は POLLIN を言わない | kernel に出力の経路が無い（事実の表）。形は「read は 1 つの output report」と決めておく |
| Q9 | 相手からの VIRTUAL_CABLE_UNPLUG で bond を消す | HID 1.1.1 の virtual cable の意味（device が unpair を求める。**未確認**: §7.4.x）。消さないと、相手が鍵を捨てた後に host が繋ぎに行き続ける |
| Q10 | L2CAP の Flush Timeout は受けて記録だけ、QoS は best effort だけ、RFC は basic だけ | HID の device は interrupt channel に Flush Timeout を求める物があり、今の「Unknown option」の断りでは繋がらない（事実 15）。controller の flush timeout を書く（Write Automatic Flush Timeout）のは相手の送りの話ではなく、自分の送りは無いので意味が無い |
| Q11 | LE の再接続は controller の filter accept list + resolving list の auto-connect。無い controller では背景の passive scan。自分の RPA（D11c）は i03 の後に判断 | 電波と電力の面で auto-connect が普通の形。自分の RPA は相手に自分の IRK を配る鍵の配りの変更が要り、p004 の smp.c の「initiator は鍵を配らない」を変える。D11c の「使う」は保つが、この Phase では相手の RPA の解決まで |
| Q12 | HOGP の notification に付ける report ID は Report Reference から。1 つでも ID が 0 でなければ付ける、全部 0 なら付けない | Report Map が ID を使うか daemon は parse せずに知れない（kernel の parser に任せる）。HOGP の Report Reference は Map の ID をそのまま持つ（ID を使わない Map では 0）。**未確認**: HOGP 1.0 §4.x の文言 |
| Q13 | kernel の `/dev/hid-host` の open は root だけ（devfs 0600 に加えて） | chmod で広げられない。bluetoothd の子は open せず親から fd を受ける（D16 (a)）ので、子の uid で開ける必要が無い。input-inject と同じ |
| Q14 | BTD_HID_MAX は 6 | session の数える link は 8（`BTD_LINKS_MAX`）。pairing 1 と、相手から来て断る前の接続 1 を残す |
| Q15 | SET_PROTOCOL（Report）は HIDBootDevice の device にだけ送る | HID 1.1.1 は boot protocol を持たない device に SET_PROTOCOL を送ることを許していない（**未確認**）。Report が既定なので送らなくても動く |
| Q16 | 名前は daemon が UTF-8 の境で 63 byte に切る。kernel は 64 以上を EINVAL | 文字の規則を kernel に持たせない。design §5.2 [N16] |
| Q17 | 相手からの BR/EDR の接続の Accept は role 0x01（slave のまま） | role switch は相手の自由で、host が master を取る理由が無い。**未確認**: HID 1.1.1 の推奨 |
| Q18 | L2CAP の channel の表は HID の device ごと（pair と同じ形） | 事実 15: pair は自分の `btd_l2cap`・`btd_reassembly` を持つ。共有の 1 表は 18 > 16 で上限の変更と identifier・Information の状態の混在が要る（§4.2） |
| Q19 | **判断待ち（Q1）**: `sleep.end` の Read Version のやり直し・firmware の load し直し（design §5.3）の行き先 | bluetoothd は `/dev/system` を今開いていない（事実 16）。この Phase が購読を足すので一緒に作れるが、transport（p003）の仕事で HID の受け入れとは別。推奨: p003 の残件の attempt（または p008 の前の小さな Phase）に分け、p005 は `sleep.end` を hid.c に渡す口だけを作る。選ばなかった案: p005 で両方 → p005 が transport の firmware の経路（5330 だけで確かめられる）に依存する |

## 自分での敵対的 review（design-reviewer の前に見つけた危険と扱い）

| # | 危険 | 扱い |
| --- | --- | --- |
| R1 | usb-hid の refactor が全ての USB のキーボード・マウス・tablet・touch を壊す | §2 の「振る舞いを変えない」の列挙、§7.1 の新旧一致の host 試験、§7.5 の QEMU の USB の試験。i01 を kernel だけの attempt にして T1 の結果を先に見る |
| R2 | syscall の bounce の 512 byte への落ち（記憶の圧迫）で setup の write が EINVAL | daemon が 3 回やり直す（§4.7）。p002 の既知の性質 |
| R3 | 相手からの BR/EDR の再接続で descriptor の前に DATA が来る | bond の cache（Q6）、無ければ待ち行列 32 と捨てて数える |
| R4 | handle の再利用（Disconnection Complete の直後の Connection Complete が同じ handle）で router が取り違える | router は session の `session_link_remove` の後に Disconnection Complete を受けて表から消す。host 試験に台本 |
| R5 | 電波から来る descriptor・report・SDP・ATT の壊れた値 | kernel の parser の fuzz（§7.2）、daemon の parser の fuzz（§7.4）、長さの上限（§4.8）。daemon は `_bluetooth` で動く（D16） |
| R6 | 悪い相手が Connection Request を大量に送る | router が bond 済み以外を Reject（HCI の command 1 つ）。page scan は bond 済みの HID がある時だけ |
| R7 | notification の洪水で daemon が詰まる | 1 report の write は kernel への 1 syscall（寝ない）。session の queue（32 KiB）が溢れれば捨てて数える（p004 §10.1） |
| R8 | KeyboardOnly のキーボード（passkey を打つ）の pairing の UI | pair.c は User Passkey Notification を `PASSKEY` で agent に出す（p004 S-c）。`bt pair` は表示だけ。Settings は p006。QEMU では host 試験だけ（loopback の 01 は DisplayYesNo のまま） |
| R9 | auto-connect の LE Create Connection と LE scan・PAIR の衝突（Command Disallowed） | hold/release（§4.9）。host 試験に順の台本。**未確認**の controller の振る舞いは 5330 で |
| R10 | S0ix の後に controller が link を保ったまま相手が消える | 5 秒の規則（§4.9）。**未確認** |
| R11 | HOGP の device が Secure Connections だけ、または authenticated の鍵を要求し、Just Works の bond では Insufficient Authentication | `security` で切り、Settings が再 pairing を促す材料（p006）。鍵の長さ 16 未満の device は使えない（D10） |
| R12 | sniff mode の遅れ | controller と相手の既定に任せる（design §4 の見積もり）。host から Sniff Subrating は送らない（記録） |
| R13 | `/dev/input/eventN` の持ち主と mode（0640、sessiond）: compositor が Bluetooth の node を開けられるか | USB の node と同じ規則（devfs の `event*`、事実 6 の sessiond の仕組み）。bus で区別しない。QEMU の試験で `evdev-probe` は root で読むが、compositor の読みは p006 の desktop の試験で見る |
| R14 | `bt-pair-p004.sh` の期待（01 の pairing の後に `l2cap=1`、07 の Just Works）が §6 の変更で変わる | 01・02・07 の pairing の流れは変えず、HID は暗号化の後の L2CAP の追加だけ。p004 の試験を回帰に入れる（§7.5 (7)） |
| R15 | daemon が落ちた時の key | kernel が fd を閉じ、input.c の unregister が離す（事実 4）。daemon に依らない |
| R16 | 2 つの bluetoothd の instance（再起動の重なり）が `/dev/hid-host` を 8 枚使い切る | 親の `OPEN-HID` は kernel の上限で EBUSY。`/dev/btN` は 1 open なので 2 つ目の daemon は controller を持てない |
| R17 | UAPI の struct の大きさと rdev の取り違え（第 1 版は sizeof を 4320 と書き（正しくは 4324）、rdev を smartcard の `0x00100000` と重ねていた。2 回目の照合で見つけた） | `_Static_assert(sizeof(struct hid_host_setup) == 4324U)` を hid-host.c と host 試験に置く。rdev は事実 22 の表で `0x00130000` に。実装の attempt で `grep -rn 'DEVICE_BASE\|DEVICE_NUMBER' src` をやり直して表を更新する |
| R18 | 相手からの BR/EDR の接続（HIDReconnectInitiate）が page scan を立てた瞬間から来るが、hid.c の表（`wanted`）がまだ bond の読みの途中 | `btd_hid_refresh()` は bond を全部表に入れてから Write Scan Enable を送る順にする。router は表に無い address の Connection Request を Reject（相手は再試行する） |

## attempt の区切り（QEMU で確かめられる単位。依存と危険で分ける）

| attempt | 範囲 | 受け入れ（Q1 が判定） | 依存 |
| --- | --- | --- | --- |
| **i01: kernel** | `hid-input.[ch]`（§2）、usb-hid.c の乗せ替えと規約の全文への合わせ、`include/uapi/hid-host.h`・`hid-host.c`・devfs・vfs・vmunix.mk（§3）、`evdev-probe`・`hid-host-probe`、host 試験 §7.1〜7.3、hid-report の fuzz、QEMU の `hid-host-p005.sh`・`hid-usb-p005.sh` | build warning 0（amd64 の製品の kernel、試験の config、`config/ci/config-pcat.mk`、arm64 の USB HID 入りの build）、`python3 plan/tools/style-check.py` 0、host 試験 PASS（新旧一致、fuzz 60 秒で 0 件）、T1 の 2 つの script PASS。Q2 の決定が無い間は `HID_HOST_GET_DEVICE` を入れて実装し、断られたら外す（試験は `/dev/system` の event に落とす） | D3、Q1（決定前は i2c-hid に触らない）、Q2 |
| **i02: bluetoothd の BR/EDR** | router（§4.1）、l2cap の inbound と option（§4.2）、sdp・hidp（§4.3・4.4）、hid.c の BR/EDR の流れ・security・再接続（§4.7〜4.9 の BR/EDR）、privsep の `OPEN-HID`、keys の field、pair.c の変更と自動の CONNECT、口と `bt`（§5）、loopback の 01・02（§6）、host 試験 §7.4 の BR/EDR の分、QEMU の `bt-hid-p005.sh` の (1)(2)(4)(6)(7)(8) と (5) の 01・02 | build warning 0、style 0、host 試験 PASS、T1 PASS、p002〜p004 の試験の回帰 PASS。Q4・Q5 の決定が無い間は推奨の形で実装し、1 か所（`btd_hid_policy_*`）で差し替えられる形にする | i01（T1 の PASS）、p004 の i02 の T1 の PASS（pairing の経路） |
| **i03: LE の HOGP と再接続の残り** | att・hog（§4.5・4.6）、hid.c の LE の流れ、filter accept list・resolving list・auto-connect・hold/release、背景の scan の fallback、resume（`sleep.end`）、loopback の 04、host 試験 §7.4 の LE・resume の分、QEMU の (3) と (5) の 04 | 同上。resolving list の無い controller の fallback は host 試験だけ（loopback は持つ） | i02 |
| **i04: 5330（任意、短い）** | 手元の device があれば `bt connect` と `evdev-probe`（T1 の lock の下、p004 の i03 と同じ image の条件: account 入りの image か Q4（p004）の決定）。無ければ p008 へ | 接続と入力の 1 回の観察を記録（実機の証拠として分けて書く） | i03、p004 の i03、device の有無 |

p005 を cleared にする条件: i01〜i03 の T1 の PASS、仕様の値の照合の表（§8）、設計の §4.3〜4.6 の「未確認」の解消（または実機へ持ち越す物の明記）、Q1・Q2・Q4・Q5・Q19 の決定の反映。i04 は cleared の条件に入れない（p008 の UAT が実機の受け入れ）。

## 確認

- 2026-10-08（P2、設計だけ）: code は書いていない。build・host 試験・QEMU・実機は **未実施**。読んだ file と行は §「事実」の表。
- 2026-10-08（P2、2 回目。Q1 は「file は作られていない」として最初からの作業を投入したが、第 1 版の file は worktree に未追跡で残っていた（最後の書き込み 08:04:04））: 第 1 版の §「事実」の 21 行の出典を全て code と照合し直した（`sed -n`・`grep` で行を読んだ。build・試験は無し）。直した物: 事実 5（input-inject の行）、10（hid-report.h の上限は 10〜16 行）、15（l2cap.h の行と表の持ち主）、16（main.c の行、`/dev/system` 未購読）、17（keys.h → keys.c の行、field の名前）、18（loopback の LE の接続の条件と行）、19（QMP と既存の道具）、§3 の sizeof（4320 → 4324）と rdev（`0x00100000` → `0x00130000`、smartcard と衝突）、§4.2 の表の持ち主の根拠（Q18）、§4.9 と範囲の外の `sleep.end` の Read Version（Q19）、§7.5 の QMP の未確認の解消。事実 22〜25、R17・R18 を足した。仕様の値（§4.3〜4.6 の **未確認**）は照合していない（仕様の PDF はこの作業では読んでいない）。
- design-reviewer: **未実施**（Q1 が起こす。結果はこの file の「design-reviewer の結果」の節に足し、Blocking は実装の前に設計へ反映する）。

## 再開点

1. design-reviewer の指摘を反映し、Q1・Q2・Q4・Q5 をユーザーに（Q1 経由で）聞く。Q19（`sleep.end` の Read Version の行き先）は Q1 の判断。
2. i01 を Queue に（kernel だけ。T1 に `hid-host-p005.sh`・`hid-usb-p005.sh`）。
3. i02（BR/EDR）→ i03（LE）→ i04（任意）。
