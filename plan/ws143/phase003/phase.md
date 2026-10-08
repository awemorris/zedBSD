<!-- awesome-plan project=zedbsd record=ws143p003 -->

# ws143-p003: bluetoothd（transport・HCI core・Intel の firmware の load・scan）と CLI `bt`

Phase ID: `ws143-p003`
Parent: [WS143](../ws.md)
Status: in-progress（q878、P2、2026-10-08 夜。attempt i01 は intelbt の package 以外）
Phase disposition: normal
Queue: q878-i01（P2、Q1 の投入「intelbt の firmware の id は T1-378（5330 が Linux の時）待ちなので、それ以外を。loopback（T1-384 の物）で QEMU で確かめられる形に」）

## 範囲

[design.md](../design.md) §3（load の手順）・§6.1 の transport・firmware・HCI core と scan、CLI `bt show`・`bt scan`、Read Local Supported
Commands の記録（D5 の b1 が使えるか）。

この attempt（i01）の範囲の外:
- firmware の package `userland/firmware/intelbt/`（5330 の CNVi・CNVR の id は T1-378 待ち。id が分かってから i02 で）。
- 実機の load と scan（T1 の 5330 の passthrough、T1-378 と一緒）。失敗の後の USB の reset の振る舞いの確認（design §3 の最後）も実機。
- 特権の分離（D16 a）・`_bluetooth` の account（D17）・socket の口の権限の細かい形（D8）は p004。p003 の bluetoothd は root で動き、
  変える操作（scan・電源）は root だけに許す（p004 で D8 の形にする）。
- `/dev/system` の POWER（`sleep.end`）・USB の event による load のやり直し（design §5.3）は p004 以降。p003 は `/dev/bluetoothN` の ENODEV で
  閉じ、2 秒ごとに `/dev/bt*` を探し直す。

## 詳細設計

### 1. 構成（`userland/base/bluetoothd/`、`userland/base/bt/`）

| file | 中身 | host の試験 |
| --- | --- | --- |
| `hci.h`・`hci.c` | 純粋: HCI の command の組み立て、event の分解（Command Complete・Command Status・Inquiry Result・Inquiry Result with RSSI・Extended Inquiry Result・Inquiry Complete・LE Meta の Advertising Report）、EIR・AD の名前と種類（Flags・名前・Class of Device・Appearance）、Read Local Supported Commands の bit、device の表（address と型で 1 つ、名前は長い方・完全の方を残す、RSSI は最後の値） | する |
| `intel.h`・`intel.c` | 純粋: Intel Read Version（0xFC05 0xFF）の TLV の分解、image の型（bootloader・operational）、firmware の file の名前 `ibt-%04x-%04x.sfi`・`.ddc`、.sfi の header の検査と Secure Send の断片の計画（design §3 の 3〜5）、boot の parameter、DDC の record の分割 | する |
| `session.h`・`session.c` | controller の session: 1 つの fd（`/dev/bluetoothN`、試験では SOCK_SEQPACKET の socketpair）に H4 の packet を読み書きし、command を 1 つずつ（答えまで 2 秒）送る。初期化の列、Intel の load、scan の状態機械。system call は read・write・poll・ioctl だけ（ioctl は fd が `/dev/bluetoothN` の時だけ。試験では無い物として飛ばす） | する（偽の controller） |
| `main.c` | daemon: `/dev/bt*` の発見と開け直し、socket `/run/bluetoothd.sock`、client の line の request、log（syslog と stderr） | しない（QEMU） |
| `protocol.h` | socket の line の形（下の §4） | — |
| `bluetoothd.service` | `type=daemon`、`command=/sbin/bluetoothd`、`after=syslogd`、`restart=on-failure`、`required=NO` | — |
| `userland/base/bt/main.c` | CLI `bt show`・`bt scan [SECONDS]`（socket の client） | しない（QEMU） |

image: この Phase では既定の config（config/ci）に入れない（D14 と同じく 5330 の既定の image に入れる時に決める。Q1 の判断）。試験の image
`plan/ws143/tests/config-amd64-bt.mk` に `bluetoothd bt` を足し、`etc/rc.conf` の services には足さない（試験の script が起こす）。

### 2. session の初期化の列

open の後、`BT_IOC_GET_INFO`（vid・pid・名前）。

1. vendor が Intel（0x8087）なら Intel Read Version（0xFC05、parameter 0xFF）。§3 の判断:
   - operational → 2 へ。
   - bootloader → firmware の load（§3）。成功したら 2 へ、失敗なら状態 `firmware-failed`（理由の文）で止まる。
   - file が無い → 状態 `firmware-needed`（file の名前を出す）で止まる。
   - どちらでもない・答えが壊れている → 状態 `unsupported`。
2. HCI_Reset（0x0C03）。
3. Read Local Version Information（0x1001）: HCI の版・manufacturer。
4. Read BD_ADDR（0x1009）。
5. Read Local Supported Commands（0x1002）: 64 byte。記録: LE Read Local P-256 Public Key（octet 34 bit 1）と LE Generate DHKey（octet 34 bit 2）
   の有無（D5 の b1）、LE Set Event Mask（octet 25 bit 0）、LE Set Scan Parameters・Enable（**octet 26 bit 2・3**、review B1 で直した。
   octet 25 bit 5・6 は Set Advertising Parameters と Read Advertising Physical Channel Tx Power）、Inquiry（octet 0 bit 0）。`bt show` に出す。
   Read Local Supported Features（0x1003）の page 0 の byte 4 bit 6（LE Supported (Controller)）も読む（答えが無ければ commands で判断）。
   LE の scan は「LE Supported」かつ octet 25 bit 0・octet 26 bit 2・3 が全て立つ時だけ（review S2）。
6. Read Buffer Size（0x1005）: ACL の長さ → `BT_IOC_SET_ACL_MAX`（範囲の外は既定のまま）。
7. Set Event Mask（0x0C01）= `03 80 00 00 02 40 00 20`（bit 0 Inquiry Complete、1 Inquiry Result、15 Hardware Error、33 Inquiry Result
   with RSSI、46 Extended Inquiry Result、61 LE Meta。Command Complete・Status は mask できない。review S1）。失敗は `error`。
   LE Set Event Mask（0x2001）= `02 00 …`（bit 1 Advertising Report）。失敗したら LE は使わない（`le=0`）。
   Hardware Error（0x10）を受けたら `error` にし、daemon が閉じて start し直す。
8. Write Inquiry Mode（0x0C45）= 2（Extended Inquiry Result）。
9. 状態 `ready`（電源は on。電源の on/off は p004 以降、今は常に on）。

各 command の答えが来なければ（2 秒）、または status が 0 でなければ、その段の名前と理由で `error` の状態。daemon は node を閉じて
2 秒後に start し直し、`error` が 3 回続いたら（review S5）やめる（`bt show` に理由。daemon の再起動で戻る）。5・6・8 の失敗は記録だけで
続ける（任意）。write の失敗: ENODEV は `lost`（node を閉じて探し直し）、他の errno は その段の `error`。

open の直後に `BT_IOC_GET_INFO` の flags に BOOTLOADER があれば（load の途中で daemon が落ちた）、`BT_IOC_SET_BOOTLOADER 0` で戻す（review S3）。
試験のために、session は ioctl を関数の表（`btd_control_fn`）で呼ぶ。daemon は ioctl(2)、host の試験は偽の controller の情報を返し、呼び出しを
記録する。

command の流れ: 一度に 1 つ。Command Complete・Command Status の opcode が送った物と違えば、それは別の（前の timeout の）答えとして捨てる。
答えを待つ間に来た他の event（Inquiry Result など）は scan の処理に渡す。Num_HCI_Command_Packets が 0 の Command Complete（controller の
command の窓が閉じている）の後は、次の command を Command Complete か Command Status の NOP（opcode 0）を待ってから送る。

### 3. Intel の firmware の load（design §3 の実装）

- TLV の分解（intel.c）: Read Version の Command Complete の parameter は status 1 byte の後に [type 1][length 1][value] の並び。使う type
  （FreeBSD iwmbtfw の `iwmbt_fw.h` の列挙で確かめた値。BSD-2-Clause の参照で、code は写さない）: 0x10 CNVi top（4）、0x11 CNVR top（4）、
  0x12 CNVi BT（4、hardware の variant は bit 16〜21）、0x1C image type（1: 0x01 bootloader、0x03 operational）、0x1D time stamp（2）、
  0x1E build type（1）、0x1F build number（4）、0x2E limited CCE（1）、0x2F SBE type（1）、0x30 OTP BD_ADDR（6）。知らない type は飛ばす。
  使う type の長さが違う・長さが残りを越える TLV は壊れた答え。
- file の名前: `/lib/firmware/intel/ibt-%04x-%04x.sfi`。数字は CNVi top・CNVR top のそれぞれを 16 bit に詰めた物:
  `((top & 0x0f000000) >> 16) | ((top & 0x0000000f) << 12) | ((top & 0x00000ff0) >> 4)`（FreeBSD の `iwmbt_get_fwname_tlv` と同じ規則）。
  5330 の値は T1-378 の後に照合する。`.ddc` は同じ名前の拡張子違い。
- 必須の TLV（review S13）: image type は常に必須（無ければ `unsupported`）。bootloader の時は CNVi top・CNVR top・CNVi BT・SBE type・
  limited CCE も必須。TLV の範囲（0x10〜0x4F）の type が 2 度出れば壊れた答え。
- 判断（design §3 の 2・3）: limited CCE が 0 でない → `unsupported`（理由「limited CCE」）。SBE type が 0（RSA）・1（ECDSA）以外 → `unsupported`。
  variant 0x15・0x16 → `unsupported`。variant 0x17 以上は header が RSA 644 + ECDSA 320、offset 644 の byte 0x06 と offset 652（644+8）の
  CSS の版 0x00020000、それ以前は RSA だけで offset 8 の CSS の版 0x00010000（review M1）。合わなければ `firmware-failed`。
- 計画（intel.c の純粋な関数が「送る断片の列」を作る）: Secure Send（0xFC09）の断片は [型 1 byte][data]、型 0x00 CSS、0x03 公開鍵、0x02 署名、
  0x01 command buffer。sbe_type 0: CSS 128（offset 0）、公開鍵 256（offset 128、128 ずつ 2 断片）、署名 256（offset 388、2 断片）。
  sbe_type 1: offset 644 から CSS 128、公開鍵 96、署名 96。本体は 964（variant 0x17 以上）か 644 から、HCI command の枠（opcode 2・長さ 1）を
  順に数え、溜まりが 252 に達したら 252 byte の断片、command の境で溜まりが 4 の倍数ならその分を送る。最後に 4 の倍数でない溜まりが
  残れば送らない（FreeBSD と同じ）。0xFC0E の command の parameter 4 byte を boot の parameter として記録。枠が file の終わりを越えれば
  `firmware-failed`（FreeBSD は検べない）。variant 0x14 以下は sbe_type 0 だけ（他は `unsupported`）。
- 実行（session.c）: `BT_IOC_SET_BOOTLOADER 1`、各断片を write し（bootloader の経路は bulk OUT）、答え（bulk IN から event として来る。
  FreeBSD は中身を見ない）を 2 秒まで待つ。答えは「次の event 1 つ」とし、vendor event 0xFF/0x06（download の完了）が先に来たらそれを
  記録して続ける。全部の後に 0xFF/0x06 を 5 秒まで待つ。`BT_IOC_SET_BOOTLOADER 0`、Intel Reset（0xFC01、`00 00 00 01` と boot の parameter
  の LE 4 byte）を送り、vendor event 0xFF/0x02（boot の完了）を 5 秒まで待つ。Reset の後に node が消えた（ENODEV、USB の re-enumerate）
  時は、探し直して開け直した node で Read Version から続ける（operational なら load 済み）。Read Version をやり直して operational を確かめ、
  `.ddc` があれば Intel Write DDC（0xFC8B）で record ごとに送る（record は [長さ L 1 byte][L byte]。command の parameter は長さの byte を
  含む L+1 byte。FreeBSD は L byte だけ写して L+1 を送る（最後の byte が 0）が、ここでは record の全部を送る）。Intel Set Event Mask
  （0xFC52、`87 0c 00 00 00 00 00 00`、FreeBSD と同じ）。2 へ。
- Secure Send の答え（review S4）: 答えが Command Complete（opcode 0xFC09）なら status を見て、0 でなければ `firmware-failed`。他の
  event は数えない（FreeBSD と同じく中身を問わない）。最初の 3 つの答えを hex で log に残し、i02 で形を確かめる。
- Intel Reset は普通の command の流れの外（Command Complete を待たない）。write の失敗（ENODEV 以外）は許し、0xFF/0x02 を 5 秒待つ。
- 自動のやり直しはしない（design §3 の失敗の経路、review B3）: daemon は load を送った controller を「USB の vid:pid と `usbB/portN`」
  （`physical_path` の device の部分の前。USB の address は re-enumerate で変わるので使わない）で覚える。その controller が再び bootloader で
  現れたら load せず `firmware-failed`（「still in the bootloader after a load」）に固定する（daemon の再起動まで）。Reset の後に node が
  消えた時は 10 秒まで再出現を待ち、来なければ `firmware-failed`（「did not come back」）。p004 で resume（`sleep.end`）の後は
  この記録を消す（S0ix で bootloader に戻る時の load）。
- 失敗の後は bootloader の経路を必ず戻す（`BT_IOC_SET_BOOTLOADER 0`、review S3）。
- load は daemon の loop を止める（断片ごとに最大 2 秒、file は 4 MiB まで。review M3: 非同期の状態機械は p004 以降。その間 client は待つ）。

### 4. socket の口（`/run/bluetoothd.sock`、line の形、volumed の形）

- request の 1 行は 512 byte まで、`\n` で終わる。答えの行は名前の escape の後で 1 行 約 1.5 KB まで（review S6、1 行を黙って捨てない）。
  request: `SHOW`、`SCAN <seconds>`（1〜30）、`DEVICES`。SCAN の答えを待つ client の他の行は無視する（1 client に 1 request、review S8）。
- answer:
  - `STATE <state> [reason]`（state: `none`（controller が無い）、`starting`、`ready`、`scanning`、`firmware-needed <file>`、`firmware-failed <理由>`、
    `unsupported <理由>`、`error <段>`）。
  - `CONTROLLER node=/dev/bluetooth0 vendor=1209 product=b7e5 name="…" address=00:11:22:33:44:55 hci=12 manufacturer=65535 le=1 p256=1 dhkey=1`。
  - `DEVICE address=… type=bredr|le-public|le-random rssi=-40 class=0x002540 appearance=0x03c1 name="…"`。名前と USB の product の文字列は、
    printable ASCII と正しい UTF-8（U+00A0 以上、overlong・surrogate・U+10FFFF 超を除く）以外の全ての byte（制御、DEL、C1、壊れた列、
    `"`、`\`）を `\xNN` にする（review S6。端末への escape の差し込みを防ぐ）。`bt` は escape の形のまま出す。
  - 表は 64 台まで（`BTD_DEVICES_MAX`）。溢れは新しい物を捨てて数え、`DROPPED <n>` を DONE の前に出す（review S7）。
  - `DONE` で終わる。失敗は `ERROR <理由>` と `DONE`（理由: `permission`、`seconds`、`no-controller`、`not-ready`、`busy`、`lost`、errno の文）。
- SCAN: root だけ（`getpeereid` の uid 0。他は `ERROR permission`）。scan の表を空にして BR/EDR の Inquiry（GIAC、長さ = seconds / 1.28 を
  切り上げ、最大 0x30）と、LE があれば LE Set Scan Parameters（**passive**、interval・window 0x0010、own address public（passive は
  SCAN_REQ を出さないので自分の address を周りに出さない）。active scan と private address（D11c）は p004、review S9）と LE Set Scan
  Enable（重複の除去は off）を始める。LE の scan が断られたら（Inquiry と同時に許さない controller）Inquiry だけで続ける。seconds の後に
  止め（Inquiry が自分で終わっていれば Cancel は送らない。Cancel の Command Disallowed は失敗にしない。LE Set Scan Enable 0）、`DEVICE` の
  行と `DONE` を返す。ready でない時は `ERROR not-ready`、scan 中の別の SCAN は `ERROR busy`、scan 中に node が消えたら `ERROR lost`。
  scan 中に client が切れても scan は続ける（表は DEVICES で読める）。
- SHOW・DEVICES は誰でも（読むだけ）。DEVICES は最後の scan の表。
- client は 8 まで。1 行を読む間は待たない（poll）。

### 5. CLI `bt`

`bt show`・`bt scan [SECONDS]`（既定 8）・`bt devices`。daemon の行をそのまま出し、最後に試験の行 `BT SHOW state=…`・`BT SCAN devices=N`。
終了の値: 成功 0、`ERROR` 1、daemon が無い 2、使い方の誤り 64。

daemon の node の選び方（review S10）: `bluetoothd -f /dev/bluetoothN` で指定、無ければ最小の番号で開けられる物。loopback 入りの image で
5330 の実機（i02）を試す時は `-f /dev/bluetooth1`。`bt show` に node を出す。別の process が開けている（EBUSY）時は log を 1 度だけ出す。

### 6. loopback の controller の追加（`src/drivers/generic/bt-hci-loopback.c`、試験の kernel だけ）

QEMU で scan の経路を通すため（WS143 の試験の source）。Command Complete の return の上限を 32 から 252 byte に、Command Status の関数を足した（review S14）:
- 0x1002 Read Local Supported Commands: 64 byte、octet 0 の 0x03（Inquiry・Cancel）、25 の 0x01、26 の 0x0C、34 の 0x06。
- 0x1003 Read Local Supported Features: byte 4 の 0x40（LE Supported）。
- 0x1005 Read Buffer Size: ACL 1021・SCO 0・ACL の数 8。
- 0x0401 Inquiry: Command Status（0x0F）の後、Extended Inquiry Result（0A:0B:0C:0D:0E:01、class 0x002540、RSSI −40、"Loopback Keyboard"、
  plen 255）、Inquiry Result with RSSI（0A:0B:0C:0D:0E:02、class 0x002580、RSSI −60、名前なし）、Inquiry Complete。
- 0x200C LE Set Scan Enable（enable 1）: Command Complete の後、LE Advertising Report 2 つ（0A:0B:0C:0D:0E:03 public "Loopback Mouse"・
  appearance 0x03C2・RSSI −50、4A:0B:0C:0D:0E:04 random・名前なし・RSSI −70）。address は 4 つとも違う。
- 0xFC03 に任意の 2 byte の遅れ（ms、10 秒まで）。遅れの間も worker は packet を渡し続ける（review B2 (a)）。`bt-probe -W MS` が送って
  すぐ閉じる。daemon が node を持っている間に node が消える経路（本物の ENODEV）を QEMU で通す。
- Read Local Version の HCI の版は今の 0x0C（5.3）のまま（LE あり）。

### 7. 試験

- host: `plan/ws143/tests/bt-daemon-host-test.{c,sh}`（ASan・UBSan）: hci.c（event の分解、EIR・AD の名前、表の統合、壊れた長さ）、intel.c
  （TLV の分解と壊れた TLV、file の名前、判断、合成の .sfi で断片の列（252 と 4 の倍数の規則、boot の parameter）、header の不一致）、
  session.c（偽の controller: socketpair の上で台本の event を返す thread。初期化の列、Intel の bootloader の load の全部の段（断片の数、
  vendor event、Reset、Read Version のやり直し、DDC）、firmware の file が無い時の `firmware-needed`、答えの来ない command の timeout、
  別の opcode の答えの捨て、scan の結果）。
- build: amd64 の kernel（loopback 入りの config も）・bluetoothd・bt は warning 0、style-check 0。
- QEMU（T1）: `plan/ws143/tests/bt-daemon-p003.sh`（image は `config-amd64-bt.mk`、script が `/sbin/bluetoothd` を直接起こす。rc には足さない、
  review M10）: `bt show` が `ready` と loopback の address・`le=1 p256=1 dhkey=1`、`bt scan 3` がちょうど 4 台（各 address・型・RSSI・class・
  appearance・名前を断定、review S14）、root でない人は scan が `ERROR permission`・show は可、daemon を止めて `bt-probe -W 3000`、すぐ
  daemon を起こして ready、3 秒後の withdraw で `closed (lost`、再登録の後に ready に戻る（start が 2 回）、daemon を止めて `bt-probe -L`（p002 の回帰）。
- 実機（i02、T1-378 と一緒）: 5330 の passthrough で load と scan。**前提は D13（host の btusb・btintel の blacklist と電源の入れ直し）**:
  Linux の host が先に operational の firmware を load するので、それが無いと bootloader の経路を通らず「load の確認」が operational の検出に
  なる（review S11）。DDC の送り方（record の全部）と HCI_Reset が DDC・0xFC52 を戻さないか（review M2・M4）、Secure Send の答えの形
  （trace）も i02 で確かめる。
- **この Phase は i01 では clear できない**: Intel の load の全体は同じ理解から書いた偽の controller でしか確かめていない。i02（intelbt の
  package と 5330 の実機、D13）まで in-progress。

## 判断の記録

| ID | 判断 | 理由 |
| --- | --- | --- |
| P1 | command は一度に 1 つ（Num_HCI_Command_Packets の窓を使わない） | 初期化と scan は直列で足りる。p004 以降で接続が増えたら窓を使う |
| P2 | socket は volumed と同じ text の line | `bt` と backend の両方が読みやすく、networkd の binary の frame より小さい。backend（p006）で足りなければ改める |
| P3 | 変える操作は p003 では root だけ | D8 の形（seat の人・wheel）は p004 の特権の分離と一緒に入れる |
| P4 | 既定の image に入れない | firmware の package と D14 が揃ってから |
| P5 | LE の scan は passive | active は自分の public の address を広告者に出す（D11c は private address）。private address と active は p004 |
| P6 | load を試した controller は daemon の命の間は再び load しない | 失敗の後の無限の load を防ぐ（review B3）。resume の後の再 load は p004 で記録を消して行う |

## p004 への申し送り（review M11・M12・S11）

- D5（b1/b2）の判断の材料: 5330 の Read Local Supported Commands の octet 34（i02、または T1-378 の Linux の btmon）。
- dual-mode の相手に Write LE Host Support（0x0C6D）・LE Read Buffer Size（0x2002）が要るか。
- node の探し直しと open は main.c に閉じてある（特権の親（D16 a）に移しやすい形）。
- 古い Intel（8087:0a2b・0aaa・0025 など、TLV でない Read Version）は p003 では `unsupported`（image type の TLV が無い）。

## design-reviewer の review（2026-10-08 夜、i01 の着手時）

Blocking 3・Should 14・Minor 13。全てをこの phase.md と実装に反映した（上の各節の「review …」）。主な物: B1 supported commands の
LE scan の bit（octet 26 bit 2・3）、B2 本物の ENODEV を QEMU で通す（0xFC03 の遅れと `bt-probe -W`）、B3 load の無限の繰り返しを防ぐ記録、
S1 event mask の値、S3 bootloader の経路の戻しと ioctl の表、S4 Intel Reset と Secure Send の答え、S6 UTF-8 の escape、S9 passive scan、
S12 fuzz と境界の試験、S13 必須の TLV、S14 loopback の Command Status と 252 byte。M13（ws.md の p003 の行）は Q1 の projection。

## 実装（i01、2026-10-08 夜、P2）

- `userland/base/bluetoothd/`: `hci.[ch]`（command・event・answer・scan の event・EIR/AD・supported の bit・UTF-8 の escape）、`intel.[ch]`
  （TLV・file の名前・loadable・断片の計画・DDC の record）、`session.[ch]`（command の流れ、start、Intel の load、core の set-up、scan、
  ioctl の表 `btd_control_fn`）、`main.c`（daemon: node の発見と `-f`、load の記録、失敗の数、socket、client）、`protocol.h`、`Makefile`、
  `bluetoothd.service`（rc には足さない）。
- `userland/base/bt/`（`main.c`、`Makefile`）。
- `src/drivers/generic/bt-hci-loopback.c`（§6）、`userland/tests/bt-probe/main.c`（`-W MS`）。
- `plan/ws143/tests/config-amd64-bt.mk` に `bluetoothd bt`、`bt-daemon-host-test.{c,sh}`（新）、`bt-daemon-p003.sh`（新、QEMU）。

## 確認

| コマンド | 結果 |
| --- | --- |
| `sh plan/ws143/tests/bt-daemon-host-test.sh`（ASan・UBSan） | `PASS (90 checks)`、fuzz 20000 回（固定の seed） |
| `make -j16 ZEDBSD_CONFIG=plan/ws143/tests/config-amd64-bt.mk BUILD=build/p2-bt build/p2-bt/vmunix build/p2-bt/bin/{bluetoothd,bt,bt-probe}` | rc 0、warning 0、kernel include check・amd64 vmunix check PASS |
| `python3 plan/tools/style-check.py`（bluetoothd の全 file、bt、loopback、bt-probe、host 試験） | 0 |

未実施: QEMU の `bt-daemon-p003.sh`（T1 に依頼）、i02 の全部（intelbt の package、5330 の実機）。
