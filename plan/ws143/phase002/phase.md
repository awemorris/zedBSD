<!-- awesome-plan project=zedbsd record=ws143p002 -->

# ws143-p002: `bt-usb` と `/dev/btN`（HCI の packet の char device）

Phase ID: `ws143-p002`
Parent: [WS143](../ws.md)
Status: cleared（2026-10-08 Q1 判定、T1-384 QEMU PASS。5330 の passthrough は未実施）（旧: test-wait（T1-384）（2026-10-08 P2、q860-i01: 実装・review の反映・build・host 試験まで。T1 の bt-loopback-p002.sh と 5330 の passthrough を待つ））
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
  取り外しの後は ENODEV。`acl_data_max`（`BT_IOC_SET_ACL_MAX`、HCI_Read_Buffer_Size の host→controller の上限）は **write の検査だけ**に使う
  （2026-10-08 review S4。受けの ACL は組み立ての容量 `BT_ACL_DATA_MAX` まで）。
- read: 1 回の read は 1 packet（型の byte が先頭）。event と ACL は別の queue で、**transport が class に渡した順**（通し番号）に返す。
  これは interrupt IN と bulk IN の間の電波の上の順を保証しない（2 つの endpoint の受けの順は worker が処理した順。2026-10-08 review）。
  buffer が packet より短ければ EMSGSIZE（packet は残る）。`read(fd, buf, 0)` は 0（packet は取らない）。空なら寝る（O_NONBLOCK は EAGAIN）。
  取り外しの後は ENODEV。
- poll: packet があれば POLLIN、使える間は POLLOUT、取り外しの後は POLLHUP。
- ioctl: `BT_IOC_GET_INFO`・`BT_IOC_GET_STATS` は取り外しの後も最後の値を返す。動作を伴う `SET_BOOTLOADER`・`RESET`・`SET_ACL_MAX` は ENODEV。
  GET_INFO・GET_STATS は D2 の ioctl の種類の範囲（Q1 2026-10-08 の判断）。
- syscall の bounce（`src/kern/syscall.c` の `syscall_io_buffer`）: 512 byte を越える read・write は heap の buffer を使い、取れなければ
  512 byte の stack の buffer に落ちる。その時 512 byte を越える packet の read は EMSGSIZE、write は分割されて EINVAL になる（記憶の圧迫の時
  だけ。やり直せば通る）。`writev`・`readv` は iovec ごとに分かれるので 1 packet の口としては使えない（使わない）。
- open の直後: open は両方の queue を空にするが、transport がその時に渡している途中の packet（open の前の受け）が直後に届くことはある。

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
int drv_bt_hci_input(struct drv_bt_hci *, uint8_t type, const uint8_t *body, size_t length);  /* 0 か ENOSPC・EINVAL。ENOSPC なら transport が持って room の後に再び */
void drv_bt_hci_count(struct drv_bt_hci *, const struct drv_bt_hci_counts *);  /* transport の malformed・stalls を足す */
void drv_bt_hci_notice(struct drv_bt_hci *, uint8_t type);   /* BT_PACKET_NOTICE_RESET を event の queue へ（予約の領域を使う） */
void drv_bt_hci_withdraw(struct drv_bt_hci *);  /* 読み手を ENODEV で起こし、走っている op を待ち、以後の op を断る */
void drv_bt_hci_release(struct drv_bt_hci *);   /* node を消し、登録の参照を返す（record は最後の参照で消える） */
```

- lock: `lock`（spin、queue・状態・読み手の waitq）と `op_lock`（mutex、send・set_bootloader・reset・withdraw を直列にする。
  write・ioctl はこれを取って ops を呼び、withdraw は取ってから ops を NULL にする。hidraw の output_lock と同じ形）。
- queue: event の ring 16 KiB と ACL の ring 32 KiB（別の上限: ACL が溢れても event は飢えない）。record は
  [長さ 2 byte][型 1][通し番号 4][本体]（header 7 byte）。event の ring は notice の record 4 つ分（28 byte、`BT_HCI_EVENT_RESERVE`）を予約し、
  普通の packet はその分を使えない（`drv_bt_hci_input` は ENOSPC）。notice は予約を使う。`drv_bt_hci_room` という関数は作らなかった
  （transport は ENOSPC で packet を持ち、`room` の callback で再び試す。2026-10-08 review S6）。
  ring の操作は純粋な関数（bt-hci-proto.c）。read は spin lock の中で 1 record を呼び手の kernel の buffer（syscall の bounce）に写し、
  user への copy は syscall の層が lock の外で行う。
- device の番号: `0x00120000 + N`（devfs は所有者と mode を番号で持つので、他の node と重ならない major。0x0011 は /dev/typec。2026-10-08 review B1）。
- release の順: node を外し（`cdev_unregister`）→ 番号を返す → 登録の参照を返す（次の controller が同じ `bt<N>` を出せる。review S5）。
- 取り外しの順: transport の detach が `drv_bt_hci_withdraw`（以後 send・reset は走らない）→ transport が URB を止め worker を終える
  → `drv_bt_hci_release`。release の後 transport は class に触らない。withdraw の後の `drv_bt_hci_input` は捨てる（安全）。

### 4. 組み直し（bt-hci-proto.c、純粋）

`struct bt_hci_assembler`: 受けの stream を header の長さで packet に組む。1 回の `feed(bytes, n)` が完成した packet を callback で渡し、
途中の packet を持ち越す（USB の transfer の境と packet の境は合わなくてよい）。

- mode EVENT: header 2 byte（code、plen）、本体 plen。
- mode ACL: header 4 byte（handle、dlen 16 bit LE）、`dlen > acl_data_max`（受けでは `BT_ACL_DATA_MAX`）は壊れた受け: その transfer の残りを捨て、組み立て中を
  捨て、malformed を数え、次の transfer の頭から組み直す（`feed` の戻り値で知らせ、transport が次の transfer で `reset_partial`）。
- bootloader の bulk IN は mode EVENT（Secure Send の答えは event）。経路を変える時（`BT_IOC_SET_BOOTLOADER`）に bulk の組み立て中を捨てる。

write の検査 `bt_hci_check_write(packet, size, acl_data_max)` と、bootloader の経路の判定 `bt_hci_is_secure_send(packet, size)`
（型 0x01 かつ opcode 0xFC09）も純粋な関数。

### 5. transport `usb-bt`

- match: interface の class E0/01/01 で、interrupt IN・bulk IN・bulk OUT の endpoint を持つ物（isochronous の interface 1 も同じ class
  なので endpoint で分ける）。score 100。class FF/01/01（vendor 固有）の Broadcom の dongle など、E0/01/01 でない controller は
  この Phase では取らない（必要になれば vid・pid の表を足す）。
- 送り: command は endpoint 0 の class の control（bmRequestType 0x20、bRequest 0、wValue 0、wIndex 0）。bootloader の経路の 0xFC09 と ACL は
  bulk OUT。送る前に自分の kmalloc の buffer に写す（syscall の stack の bounce を DMA に渡さない）。
- 受け: interrupt IN の URB 1 つ（buffer は wMaxPacketSize の packet の大きさ（`& 0x7ff`）ちょうど）と bulk IN の URB 1 つ（4096 byte、
  wMaxPacketSize の倍数）。completion（割り込みの文脈かもしれない）は印を立てて worker を起こすだけ（`kern_thread_wakeup`）。worker が
  drain し、組み直して class に渡す。class が ENOSPC を返したら、その packet と transfer の残りの byte を pipe が持ち、URB を出し直さない
  （stalls を 1 回数える）。read が空けると `room` の callback で worker が再び渡し、全部渡せた時に URB を出し直す（backpressure:
  controller の側で NAK、packet は捨てない）。
- 失敗: STALL は失敗として数え（`USB_BT_ERRORS_MAX` = 8 回続けば止めて log、review S3）、clear halt の後に出し直す。DISCONNECTED は
  その pipe を止める（detach が続く）。CANCELLED（reset・detach の取り消し）はそれらが次を決める。他の失敗は数えて出し直し、8 回続けば止める。
  COMPLETE でない終わりは全て、組み立て中の packet を捨てて次の packet を頭から組む（失われた byte の後の byte は前の packet に属さない。review S2）。
- reset（`BT_IOC_RESET`、ioctl の thread の文脈）: worker に pause を立て、両方の URB を cancel・drain し、worker が処理し終えるのを待ち
  （2 秒まで、`USB_BT_QUIESCE_MS`）、`drv_usb_device_reset()` を EBUSY なら 50 ms 置いて 5 回まで、組み立て中を捨て、**pause の間に**
  `drv_bt_hci_notice(RESET)` を積んでから pause を外して worker が出し直す（reset の後の event が notice より前に来ない。review S1）。
  ENOTSUP（root port でない）などはそのまま返す（notice は積まない）。
- detach: `drv_bt_hci_withdraw` → stopping（以後の submit を admit しない）→ admit 済みの submit を待つ → cancel・drain → worker を join →
  `drv_bt_hci_release` → 解放。drain が失敗したら（URB がまだ host controller の物）何も free せずに error を返し、USB の core が
  DETACH_PENDING で後で detach をやり直す（usb-hid と同じ。review B2）。

### 6. 試験

- host: `plan/ws143/tests/bt-hci-proto-host-test.sh`（ASan・UBSan）: event と ACL の組み直し（1 byte ずつ、境をまたぐ、1 回に複数）、
  長すぎる ACL の拒否と次の transfer からの回復、write の検査（短い・長い・SCO・未知の型・ACL の上限）、0xFC09 の判定、ring（wrap、通し番号の順、
  空き、notice の予約、満ち）。
- build: amd64 の kernel（warning 0）と bt-probe。
- loopback の controller（2026-10-08 review S7、ws161 の hidraw-loopback・smartcard-loopback と同じ形）: 試験の kernel だけの
  `src/drivers/generic/bt-hci-loopback.c`（`CONFIG_BT_TEST_LOOPBACK`、`/dev/bt0`、vendor 1209・product B7E5）。
  - 0xFC01: event・ACL・event・ACL の順に出してから Command Complete を返す。
  - 0xFC02 N: Command Complete の後に、index の付いた 255 byte の vendor event を N 個出す（queue より多い）。
  - 0xFC03: Command Complete の 300 ms 後に withdraw・release し（detach）、さらに 500 ms 後に登録し直す。
  - 0x1001・0x1009: 決まった値で答える。他の command は status 0 で答える。ACL は同じ packet で返す。
  - 渡すのは worker の thread。ENOSPC なら持って、room の callback を待つ。reset は渡しの mutex の下で待っている物を捨て、notice を積む。
- QEMU（T1）: `plan/ws143/tests/bt-loopback-p002.sh`（image は `plan/ws143/tests/config-amd64-bt.mk`）。
  - node: `/dev/bt0` が 0600 root で、log に loopback の名前がある。USB の controller が無い時は `/dev/bt1` も `usb-bt:` の log も無い。
  - `bt-probe -L`（class の試験）: 1 open・EBUSY、read の 0、2 つの queue の順、EMSGSIZE、ACL の往復、SCO の拒否、flood の backpressure
    （stalls、落とさない、順）、満ちた queue への reset の notice の位置、read の block 中の withdraw（ENODEV・POLLHUP・GET_INFO）と
    同じ名前での再登録。
  - ほかに `bt-probe` の普通の probe と、root 以外が開けないこと。
- 実機: 5330 の AX211 の Bluetooth の passthrough で `bt-probe -r -f /dev/bt1`（loopback の無い image なら `/dev/bt0`）。保留の T1-378 と一緒に流す。

## 確認

### 2026-10-08（P2、q860-i01 の再開）

- 前の世代の未 commit の作業は、2026-10-08 の緊急のラップアップで stash に退避していた（写しは main の plan/ws143/wip-20261008/）。q863・q864 の後に stash pop で戻した（競合なし）。checkpoint の commit は cfd196e70。
- kernel: `make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk vmunix`（worktree の build/amd64）は rc 0、warning 0。kernel include check PASS、amd64 vmunix check PASS。
- host: `OUT=<dir> sh plan/ws143/tests/bt-hci-proto-host-test.sh`（ASan・UBSan）は all checks passed。
- style: `python3 plan/tools/style-check.py` の新しい file の指摘（閉じ括弧の後の空行、段落の注記、split の呼び出し、試験の前方宣言）を直し、bt-hci.c・bt-hci-proto.c・usb-bt.c・試験・bt-probe は 0。devfs.c・pcat.c の残りの指摘は既存の部分のもの。
- `userland/tests/bt-probe`（commit 04ffcb8da）:
  - 動き: `/dev/btN` の info を出す。Intel の controller には Read Version（TLV、0xFC05 0xFF）を送る。続けて HCI_Reset・Read Local Version・Read BD_ADDR を送り、Command Complete を待つ。`-r` では `BT_IOC_RESET` の後に notice を待つ。最後に stats を出す。結果は `BT PASS` か `BT FAIL steps=N`。
  - build: `make ZEDBSD_CONFIG=plan/ws143/tests/config-amd64-bt.mk BUILD=build/amd64 build/amd64/bin/bt-probe` は rc 0、warning 0。
- 試験の image: `plan/ws143/tests/config-amd64-bt.mk`（CI の config に bt-probe を足した物）。
- design-reviewer の review: 2026-10-08 に実施中（§10.1。前の世代の記録に review が無かったため）。
- 自分の読みで見つけた競合（commit 25798861f）:
  - 問題: detach の `usb_bt_stop_transfers` が stopping を立てる直前に、worker が `usb_bt_arm` で armed を取った（admit した）場合、その submit は cancel の後に入りうる。その URB が pending のまま free される。
  - 直し: `submitting`（admit 済みで submit 中の数、lock の下）を足し、stop は stopping を立てた後でそれが 0 になるのを待ってから cancel・drain する（usb-hid の close_admission と同じ形）。
  - kernel の build は warning 0。

### 2026-10-08 design-reviewer の review の反映（P2）

review は cfd196e70 を見た物。detach の submit の競合は、25798861f で直してあることを review が確認した。

| 指摘 | 直し |
| --- | --- |
| B1 device の番号 0x00110000 が /dev/typec と同じ（devfs の chmod・chown は番号で持つ） | `BT_HCI_DEVICE_BASE` を 0x00120000 に（全ての `cdev_register*` を grep して空きを確かめた） |
| B2 drain の失敗を log だけして free へ進む | `usb_bt_stop_transfers` が drain の error を返し、detach は free せずに返す（core の DETACH_PENDING で再試行、usb-hid と同じ） |
| S1 reset の notice が re-arm の後 | pause の間に notice を積み、それから pause を外す |
| S2 COMPLETE でない終わりで組み立て中が残る | `usb_bt_pipe_restart`（held と組み立て中を捨てる）を COMPLETE 以外の全てで |
| S3 STALL を数えない | STALL も errors に数え、8 回で止める |
| S4 受けの ACL の上限に acl_data_max | 受けは `BT_ACL_DATA_MAX`。`drv_bt_hci_acl_data_max` を消した。acl_data_max は write の検査だけ |
| S5 release が番号を先に放す | `cdev_unregister` → 番号を返す → `cdev_release` |
| S6 文書と実装のずれ | この phase.md の §2・§3・§4・§5、design.md §5.1・§5.3、ws.md の p002 の行を直した。bt-hci.c の notice の comment（7 つ → 4 つ） |
| S7 試験 | loopback の controller、`bt-probe -L`、`bt-loopback-p002.sh`（上の §6） |
| Minor | withdraw の後の GET_INFO・GET_STATS は値を返す（comment と §2 に明記）、bounce の 512 byte と writev（§2）、`read(fd, buf, 0)` は 0（code と §2）、open の直後の古い packet（§2）、「届いた順」を「class に渡した順」に（§2）、Broadcom の FF/01/01（§5）、wMaxPacketSize の `& 0x7ff`、GET_INFO・GET_STATS は D2 の範囲（§2）、規約の指摘（3 節以上の条件の改行、連鎖の呼び出しの分け、unlock の直後の段落、join の loop の comment） |

確認（2026-10-08）:
- kernel（`make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk vmunix`）: rc 0、warning 0。include check PASS、vmunix check PASS。
- loopback 入りの kernel（`make ZEDBSD_CONFIG=plan/ws143/tests/config-amd64-bt.mk BUILD=build/ws143-bt vmunix`）: rc 0、warning 0。
- `bt-probe`（同じ config、`build/ws143-bt/bin/bt-probe`）: rc 0、warning 0。
- host: `bt-hci-proto-host-test.sh` は all checks passed（ASan・UBSan）。
- style-check: bt-hci.c・bt-hci-proto.c・usb-bt.c・bt-hci-loopback.c・bt-probe・試験は 0。
- i386 の pcat（`make ZEDBSD_CONFIG=config/ci/config-pcat.mk BUILD=build/ws143-pcat vmunix`）: usb-bt.c・bt-hci.c・bt-hci-proto.c は warning 0 で compile。
  link は既存の `sandbox_free`・`sandbox_answers`・`sandbox_permits`・`sandbox_deny`・`sandbox_create` の未定義で失敗する（WS143 と無関係、
  2026-10-02 ユーザー「i386 は当面 build も試験もしない」）。
- 未実施（T1）: `bt-loopback-p002.sh`。未実施（実機）: 5330 の passthrough の `bt-probe -r`（T1-378 と一緒）。
- HAL の変更は無い（Q1 の判定）。


## Q1 の判定（2026-10-08）

T1-384: bt-loopback-p002 PASS（ok 8 行）。5330 の Bluetooth の passthrough の bt-probe -r は T1-378 と一緒に未実施。
