<!-- awesome-plan project=zedbsd record=ws143p002 -->

# ws143-p002: `bt-usb` と `/dev/btN`（HCI の packet の char device）

Phase ID: `ws143-p002`
Parent: [WS143](../ws.md)
Status: in-progress（2026-10-08 P2、q860-i01）
Phase disposition: normal
Queue: q860-i01（P2、2026-10-08。Q1 の投入「p001 の記録を締め、HID の Phase から実装」）

## 範囲

[design.md](../design.md) §5.1・§5.3 の kernel の部分と、D2（UAPI `/dev/btN` の形、2026-10-05 ユーザー承認）。

- `include/uapi/bluetooth.h`（packet の型、境界、ioctl、struct）。
- class `src/drivers/generic/bt-hci.c`（`/dev/btN` の node、queue、read・write・poll・ioctl、寿命）と純粋な部分
  `src/drivers/generic/bt-hci-proto.c`（H4 の packet の検査、stream の組み直し、queue の ring。host の試験が compile する）。
- USB の transport `src/drivers/usb/usb-bt.c`（interface E0/01/01、普通の経路と Intel の bootloader の経路、URB と worker、reset、取り外し）。
- build の登録（`Makefile` の `CONFIG_DRIVER_USB_BT`、`platform/amd64/vmunix.mk`・`platform/pcat/vmunix.mk`、`src/kern/platform/pcat.c`）、
  devfs の node の mode（`src/kern/devfs.c`、`bt<n>` は root の 0600）。
- 試験の道具 `userland/tests/bt-probe`（`/dev/btN` で HCI_Reset・Read Local Version・Read BD_ADDR・Intel Read Version を送って答えを表示）。
- host の試験（`plan/ws143/tests/bt-hci-proto-host-test.{c,sh}`）。
- 5330 の descriptor と版の採取は T1 への依頼（Q1 経由、lock の下）。

範囲の外: firmware の load と HCI の初期化（p003 の bluetoothd）、`/dev/hid-host`（p005）。

## 詳細設計

### 1. resume の印（design §5.3 の改訂）

design §5.3 は `/dev/system` に resume の class（1 bit の UAPI の追加）を足す案だったが、2026-10-05 の ws052-p006 で
`KERN_SYSTEM_EVENT_POWER` の `sleep.end`（「After everything is resumed」、`src/kern/sleep.c` の `sleep_post`、
`docs/architecture/power-management.md` の Events の表）が入った。bluetoothd は POWER の class を購読し、subject `sleep.end` で
Read Version をやり直す。**UAPI の追加は要らないので足さない**（D2 で承認された bit は使わない。UAPI を小さく保つ）。

### 2. UAPI `include/uapi/bluetooth.h`

ioctl の group は `'b'`（未使用を `include/` の全ての `IOC_GROUP` と `_IO*('x'` で確かめた）。

| 名前 | 値・形 | 意味 |
| --- | --- | --- |
| `BT_PACKET_COMMAND`・`ACL`・`SCO`・`EVENT`・`ISO` | 0x01・0x02・0x03・0x04・0x05 | H4 の型。SCO・ISO は予約（write は EINVAL、read には来ない） |
| `BT_PACKET_NOTICE_RESET` | 0x80 | kernel の通知: `BT_IOC_RESET` が終わった（本体は無し、長さ 1 の packet）。0xFF（HCI の vendor event の code）は使わない |
| `BT_COMMAND_PACKET_MAX` | 1+3+255 | write の command の packet の最大（型の byte を含む） |
| `BT_EVENT_PACKET_MAX` | 1+2+255 | read の event の最大 |
| `BT_ACL_DATA_DEFAULT`・`BT_ACL_DATA_MAX` | 1021・4096 | ACL の data の長さの上限の既定と、`BT_IOC_SET_ACL_MAX` で設定できる最大 |
| `BT_ACL_DATA_MIN` | 27 | 設定できる最小（LE の最小の ACL の大きさ） |
| `BT_BUS_USB` | 1 | `bt_info.bus` |
| `BT_INFO_BOOTLOADER` | 0x0001 | `bt_info.flags`: bootloader の経路が有効 |
| `BT_TEXT_MAX` | 64 | 名前と place の大きさ（NUL を含む） |

```c
struct bt_info {            /* BT_IOC_GET_INFO */
	uint16_t vendor;
	uint16_t product;
	uint16_t version;       /* bcdDevice */
	uint16_t bus;           /* BT_BUS_USB */
	uint32_t flags;         /* BT_INFO_* */
	uint32_t acl_data_max;  /* 今の ACL の data の上限 */
	uint32_t reserved[4];
	char name[BT_TEXT_MAX];          /* USB の product の文字列 */
	char physical_path[BT_TEXT_MAX]; /* "usb%u/port%u/device%u/interface%u"（usb-hid と同じ形） */
};

struct bt_stats {           /* BT_IOC_GET_STATS */
	uint64_t events_in;
	uint64_t acl_in;
	uint64_t commands_out;
	uint64_t acl_out;
	uint64_t malformed;     /* 捨てた壊れた受け（transfer の残り） */
	uint64_t stalls;        /* queue が満ちて IN を止めた回数 */
	uint64_t reserved[4];
};

#define BT_IOC_GET_INFO        _IOR('b', 0, struct bt_info)
#define BT_IOC_SET_BOOTLOADER  _IOW('b', 1, uint32_t)   /* 1 で入る、0 で出る */
#define BT_IOC_RESET           _IO('b', 2)
#define BT_IOC_SET_ACL_MAX     _IOW('b', 3, uint32_t)   /* BT_ACL_DATA_MIN..BT_ACL_DATA_MAX、外は EINVAL */
#define BT_IOC_GET_STATS       _IOR('b', 4, struct bt_stats)
```

file の操作:

- open: 同時に 1 つ（2 つ目は EBUSY）。node は root の 0600（D16 a: 特権の親が開けて fd を渡す）。
- write: 1 回の write は H4 の 1 packet。command は `size == 1 + 3 + plen`、ACL は `size == 1 + 4 + dlen` かつ `dlen <= acl_data_max`。
  それ以外は EINVAL。transfer が終わるまで寝る（command 1 秒、ACL 2 秒の timeout で ETIMEDOUT）。成功は size。
  取り外しの後は ENODEV。
- read: 1 回の read は 1 packet（型の byte が先頭）。event と ACL は別の queue で、届いた順（通し番号）に返す。buffer が packet より
  短ければ EMSGSIZE（packet は残る）。空なら寝る（O_NONBLOCK は EAGAIN）。取り外しの後は ENODEV。
- poll: packet があれば POLLIN、使える間は POLLOUT、取り外しの後は POLLHUP。

### 3. class `bt-hci`（`include/drivers/generic/bt-hci.h`）

transport が登録し、class が node `bt<N>`（最小の空き番号、16 まで）を出す。

```c
struct drv_bt_hci_ops {
	int (*send)(void *context, const uint8_t *packet, size_t length);  /* 検査済みの H4 の 1 packet、寝てよい */
	int (*set_bootloader)(void *context, int on);
	int (*reset)(void *context);
	void (*room)(void *context);   /* read が queue を空けた（transport は止めた IN を再開してよい）。lock を持たずに呼ぶ */
};
int drv_bt_hci_register(const struct drv_bt_hci_description *, const struct drv_bt_hci_ops *, void *context, struct drv_bt_hci **);
int drv_bt_hci_input(struct drv_bt_hci *, uint8_t type, const uint8_t *body, size_t length);  /* 0 か ENOSPC・EINVAL */
size_t drv_bt_hci_room(struct drv_bt_hci *, uint8_t type);   /* その queue の空きの byte */
void drv_bt_hci_notice(struct drv_bt_hci *, uint8_t type);   /* BT_PACKET_NOTICE_RESET を event の queue へ（予約の領域を使う） */
void drv_bt_hci_withdraw(struct drv_bt_hci *);  /* 読み手を ENODEV で起こし、走っている op を待ち、以後の op を断る */
void drv_bt_hci_release(struct drv_bt_hci *);   /* node を消し、登録の参照を返す（record は最後の参照で消える） */
```

- lock: `lock`（spin、queue・状態・読み手の waitq）と `op_lock`（mutex、send・set_bootloader・reset・withdraw を直列にする。
  write・ioctl はこれを取って ops を呼び、withdraw は取ってから ops を NULL にする。hidraw の output_lock と同じ形）。
- queue: event の ring 16 KiB と ACL の ring 32 KiB（別の上限: ACL が溢れても event は飢えない）。record は
  [長さ 2 byte][型 1][通し番号 4][本体]。event の ring は notice の record のために 16 byte を予約し、`drv_bt_hci_room` はその分を引いて答える。
  ring の操作は純粋な関数（bt-hci-proto.c）。read は lock の中で 1 record を呼び手の kernel の buffer（syscall の bounce）に写す。
- 取り外しの順: transport の detach が `drv_bt_hci_withdraw`（以後 send・reset は走らない）→ transport が URB を止め worker を終える
  → `drv_bt_hci_release`。release の後 transport は class に触らない。withdraw の後の `drv_bt_hci_input` は捨てる（安全）。

### 4. 組み直し（bt-hci-proto.c、純粋）

`struct bt_hci_assembler`: 受けの stream を header の長さで packet に組む。1 回の `feed(bytes, n)` が完成した packet を callback で渡し、
途中の packet を持ち越す（USB の transfer の境と packet の境は合わなくてよい）。

- mode EVENT: header 2 byte（code、plen）、本体 plen。
- mode ACL: header 4 byte（handle、dlen 16 bit LE）、`dlen > acl_data_max` は壊れた受け: その transfer の残りを捨て、組み立て中を
  捨て、malformed を数え、次の transfer の頭から組み直す（`feed` の戻り値で知らせ、transport が次の transfer で `reset_partial`）。
- bootloader の bulk IN は mode EVENT（Secure Send の答えは event）。経路を変える時（`BT_IOC_SET_BOOTLOADER`）に bulk の組み立て中を捨てる。

write の検査 `bt_hci_check_write(packet, size, acl_data_max)` と、bootloader の経路の判定 `bt_hci_is_secure_send(packet, size)`
（型 0x01 かつ opcode 0xFC09）も純粋な関数。

### 5. transport `usb-bt`

- match: interface の class E0/01/01 で、interrupt IN・bulk IN・bulk OUT の endpoint を持つ物（isochronous の interface 1 も同じ class
  なので endpoint で分ける）。score 100。
- 送り: command は endpoint 0 の class の control（bmRequestType 0x20、bRequest 0、wValue 0、wIndex 0）。bootloader の経路の 0xFC09 と ACL は
  bulk OUT。送る前に自分の kmalloc の buffer に写す（syscall の stack の bounce を DMA に渡さない）。
- 受け: interrupt IN の URB 1 つ（buffer は wMaxPacketSize ちょうど）と bulk IN の URB 1 つ（4096 byte、wMaxPacketSize の倍数）。
  completion（割り込みの文脈かもしれない）は印を立てて worker を起こすだけ（`kern_thread_wakeup`）。worker が drain し、組み直して
  class に渡し、class の queue の空きが予約（組み立ての最大 + URB の buffer + record の header の分）以上の時だけ URB を出し直す。
  足りなければ止めて stalls を数え、`room` の callback で worker が再び見る（backpressure: controller の側で NAK、packet は捨てない）。
- STALL は clear halt の後に出し直す。CANCELLED・DISCONNECTED はその endpoint を止める。他の失敗は数えて出し直し、8 回続けば止めて log。
- reset（`BT_IOC_RESET`、ioctl の thread の文脈）: worker に pause を立て、両方の URB を cancel・drain し、worker が処理し終えるのを待ち
  （1 秒まで）、`drv_usb_device_reset()` を EBUSY なら 50 ms 置いて 5 回まで、組み立て中を捨て、pause を外して worker が出し直し、
  `drv_bt_hci_notice(RESET)`。ENOTSUP（root port でない）などはそのまま返す。
- detach: `drv_bt_hci_withdraw` → stopping → cancel・drain → worker を join → `drv_bt_hci_release` → 解放。

### 6. 試験

- host: `plan/ws143/tests/bt-hci-proto-host-test.sh`（ASan・UBSan）: event と ACL の組み直し（1 byte ずつ、境をまたぐ、1 回に複数）、
  長すぎる ACL の拒否と次の transfer からの回復、write の検査（短い・長い・SCO・未知の型・ACL の上限）、0xFC09 の判定、ring（wrap、通し番号の順、
  空き、notice の予約、満ち）。
- build: amd64 の kernel（warning 0）と bt-probe。
- QEMU（T1、WS の区切り）: QEMU に Bluetooth の controller は無いので、boot と「controller が無い時に node が無い」だけ。実機は 5330 の
  passthrough（p003 の firmware の load と一緒に）。

## 確認

（実施の記録）
