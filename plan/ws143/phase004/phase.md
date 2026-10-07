<!-- awesome-plan project=zedbsd record=ws143p004 -->

# ws143-p004: L2CAP・pairing（BR/EDR の SSP・LE の SMP）・暗号・鍵の保存・特権の分離・socket の口の権限

Phase ID: `ws143-p004`
Parent: [WS143](../ws.md)
Status: test-wait（i03 の直しを T1 に再依頼、Q1 経由。i02 は T1-405 で FAIL → uncleared（下の「T1-405 と i03」）。i01 は uncleared: P2 の context の都合で部品と host 試験まで）
Phase disposition: normal
Queue: q880-i01（P2、Q1 の投入「p004（L2CAP・SMP）。p003 と同じく試験の kernel の loopback で QEMU で確かめられる形に（loopback に要る答えを足してよい）。設計 → design-reviewer → 実装 → host 試験（fuzz を含む）」）、
q883-i02（P2、Q1 の投入: 再開点の順に、review による設計の改訂 → session の queue と初期化（B1・B3・B5）→ pair.[ch] → main.c の口と `bt` →
loopback・QEMU の試験・T1 の依頼。B6 はユーザーの決定待ち（どの案にも差し替えられる形）、S7 は Q1 の決定（試験の account））

## 範囲

[design.md](../design.md) §6.1 の L2CAP・SMP・SSP、§6.2・§6.3・§6.5・§6.6、§9 の D5・D8・D10・D16・D17。p003 の bluetoothd の上に足す。

i01 の範囲の外（理由と行き先）:
- **D5 の b2（自前の P-256）**: AX211 が b1 の command（LE Read Local P-256 Public Key・LE Generate DHKey）を持つかは i02 の 5330 で分かる
  （p003 の `bt show` の `p256`・`dhkey`）。持たない時だけ作る。i01 は b1 だけで、持たない controller は LE の pairing を断る（`ERROR no-p256`）。
- **PIN の legacy pairing（BR/EDR）**: D10 は legacy も受けて警告。PIN Code Request の答えには人の入力の PIN が要り、agent の口（§5）に
  passkey と同じ形で足せるが、i01 は PIN Code Negative Reply（断る）にして記録する（相手が SSP を持たない古い device だけが困る）。
- LE の peripheral の役（相手から接続される側）、LE Secure Connections の Out-of-Band、LE の private address（D11c、自分の IRK と RPA）:
  p005 の再接続（filter accept list と resolving list）と一緒に。
- HID・SDP・GATT（p005）、desktop（p006）。

## 詳細設計

### 1. 構成（`userland/base/bluetoothd/` に足す）

| file | 中身 | host の試験 |
| --- | --- | --- |
| `crypto.[ch]` | AES-128 の暗号化（FIPS-197、table の S-box、鍵の展開。復号は要らない）、AES-CMAC（RFC 4493）、SMP の関数（Core Vol 3 Part H §2.2: `e`、`c1`、`s1`、`f4`、`f5`、`f6`、`g2`、`ah`）。byte の順は SMP の約束（値は little-endian で運び、関数の中は MSB-first） | RFC 4493 の 4 つの例、FIPS-197 の例、SMP の各関数は python の `cryptography`（host の 43.0）で同じ式を独立に計算した値と照合（試験の script が作る） |
| `acl.[ch]` | 純粋: ACL の header（handle 12 bit、PB 2 bit、BC 2 bit、長さ）の組み立てと分解、L2CAP の basic frame（長さ・CID）、受けの組み直し（PB=0x02 の開始と 0x01 の続き、L2CAP の長さで完了、上限 `BTD_L2CAP_MAX` = 1024 byte、超えたら捨てて数える）、送りの分割（controller の ACL の長さ） | する（境界・壊れた長さ・fuzz） |
| `l2cap.[ch]` | 純粋に近い: BR/EDR の signalling（CID 1: Command Reject、Connection Request・Response、Configuration Request・Response（MTU だけ受け、他の option は Unacceptable で返す）、Disconnection Request・Response、Information Request・Response（feature mask 0、fixed channels の bit 1））と LE の signalling（CID 5: Connection Parameter Update Request に Accept、他は Command Reject）。channel の表（handle・local CID・remote CID・PSM・状態） | する（台本と fuzz） |
| `smp.[ch]` | LE の SMP の initiator（central）: Pairing Request（IO capability は agent の有無で DisplayYesNo か NoInputNoOutput、AuthReq = bonding・MITM・SC、最大の鍵の長さ 16、鍵の配り: initiator 0・responder は IdKey と EncKey）、Pairing Response の検査（鍵の長さ 16 未満は Pairing Failed 0x06 で断る）、SC の公開鍵の交換（自分の鍵は controller の LE Read Local P-256 Public Key）、相手の鍵が仕様の debug の鍵なら断る（0x0B）、DHKey は controller の LE Generate DHKey（status を検査）、Just Works・Numeric Comparison（g2 の 6 桁を agent に確かめる）・Passkey Entry（20 回の f4 の往復、agent の入力か表示）、`f5` で MacKey・LTK、`f6` の DHKey Check、LE Start Encryption、Encryption Change、相手の鍵の配り（Identity Information の IRK、Identity Address Information）。legacy（相手が SC を持たない）は D10 で受ける: TK（Just Works は 0、Passkey は agent）、`c1` の confirm、`s1` の STK、相手の LTK（Encryption Information・Master Identification）。timeout 30 秒（仕様）、失敗は Pairing Failed と切断 | する（台本の相手、python の値） |
| `pair.[ch]` | HCI の接続と pairing の状態機械: BR/EDR（Create Connection、Connection Complete、Authentication Requested、IO Capability Request → Reply、User Confirmation Request → agent → Reply・Negative Reply、User Passkey Request・Notification、Simple Pairing Complete、Link Key Request → 保存した鍵で Reply か Negative Reply、Link Key Notification（key type 3（debug）は切断、他は保存）、Authentication Complete、Set Connection Encryption、Encryption Change、Read Encryption Key Size（16 でなければ切断、KNOB）、PIN Code Request → Negative Reply）と LE（LE Create Connection、LE Connection Complete、SMP、切断）。Number Of Completed Packets で ACL の送りの数を数える | する（偽の controller が controller の側の SSP を演じる） |
| `keys.[ch]` | 鍵の保存（§3） | する（一時の folder） |
| `privsep.[ch]` | 特権の分離（§4） | しない（QEMU） |
| `main.c` | 口の request（§5）、agent、権限（§6） | しない（QEMU） |

session（p003）は 1 つずつの command の流れのまま。接続と pairing の間の event は `session_dispatch` から `pair` に渡す。command は pairing の
途中でも 1 つずつ（答えまで最大 2 秒、loop を止める。p003 の P1 のまま）。

### 2. LE の SMP の値の向き（試験で取り違えない約束）

SMP の PDU の値（鍵、nonce、confirm、公開鍵の X・Y）は little-endian で運ばれる。crypto.c の関数は Core の式どおり MSB-first の 16 byte
を取る（`btd_aes`、`btd_cmac` は big-endian の block）。smp.c が PDU の値を反転してから渡し、結果を反転して送る。host の試験は python で
同じ向きの約束で計算する。

### 3. 鍵の保存（design §6.3、D9 system 共有）

- 場所: `/var/db/bluetooth/<自分の BD_ADDR>/<相手の address>-<型>`（例 `00:11:22:33:44:55/0A:0B:0C:0D:0E:01-bredr`）。folder は
  `_bluetooth` の 0700、file は 0600。
- 形: text の `key=value` の行（`type=bredr|le-public|le-random`、`name=`（escape 済み）、`link_key=` 32 桁の hex と `link_key_type=`（BR/EDR）、
  `ltk=`・`ediv=`・`rand=`・`key_size=`・`authenticated=`・`secure_connections=`（LE）、`irk=`・`identity=`（LE の相手の identity）、
  `legacy=1`（D10 の警告の印））。読みは知らない key を飛ばし、長さ・hex を検査する。
- 書き方: 同じ folder に `.<名前>.tmp` を `O_CREAT|O_EXCL`、write、`fsync`、`rename`、folder の `fsync`。
- 忘れる（FORGET）: file を unlink（`_bluetooth` の folder の中だけ。名前は address と型から作り、`/`・`..` を含まない）。

### 4. 特権の分離（D16 の (a)、D17。i02 で review B2・B6 を反映）

- base の `etc/passwd`・`etc/group`・`etc/shadow` に `_bluetooth`（uid・gid 80、home `/var/empty`、shell `/sbin/nologin`、password は `*`）と、
  D17 の決定どおり group `bluetooth`（81、空）。D8 の判定は §6 の root・seat の人・wheel で、group `bluetooth` は今は判定に使わない（review S6。
  i02 の review-2 BL3 で D17 の文言に合わせて作る形に戻した）。
- 親（root）: `/run/bluetoothd.sock` を作り（0666）、`/var/db`（無ければ 0755）と `/var/db/bluetooth`（`_bluetooth` の 0700。symlink や
  folder でない物は断る。uid・gid は `getpwnam` の値。review-2 M-i）を作り、子と
  **`socketpair(AF_UNIX, SOCK_DGRAM)`**（zedBSD の kernel に SOCK_SEQPACKET は無い。datagram は 1 送り 1 受けで境界を保ち、SCM_RIGHTS も
  運ぶ。review B2）を作り、fork する。子は `setgroups(0)`・`setgid(80)`・`setuid(80)` の後に `setuid(0)` が失敗すること（戻れない）を
  確かめて、listener と socketpair の片方だけを持って動く。親は他を持たない小さな loop: 子の datagram `OPEN`（最小の番号で開く物）か
  `OPEN /dev/btN`（`/dev/bt` と数字 1〜2 桁だけ）に、`O_RDWR|O_CLOEXEC` で開いて答える: `OK /dev/btN` と SCM_RIGHTS の fd、または
  `ERR <errno>`。親は送った fd の写しを必ず close する（node は同時に 1 つしか開けない）。node の path は親が自分の argv（`-f`）から取り、
  子からは受けない（review-2 M-i）。
- 寿命（review-2 BL1）: datagram の recv は相手の close で 0 を返さない（unix-socket.c の datagram の待ち）ので、親は 1 秒ごとの poll の間に
  `waitpid(WNOHANG)` で子の終わりを見て、子の終了の値で終わる。SIGTERM・SIGINT は SA_RESTART 無しの handler で受け、子に SIGTERM を送り、
  5 秒待って（来なければ SIGKILL）終わる。子は親の死を liveness の STREAM の socketpair の EOF（親は書かない）で知り、終わる。
- 子の `/dev/btN` の探し直し（p003 の `btd_open`）は親への `OPEN` に置き換える（`-f` の path も親に渡す）。
- **account が無い時（Q4、ユーザーの決定待ち、review B6）**: 扱いは 1 つの関数 `btd_privsep_no_account()` に閉じ、案 (a)〜(c) のどれでも
  そこだけを差し替える形にする。決定までの暫定は「起動を拒む」（log `no _bluetooth account; not starting`、終了の値 0。非 0 は init の
  `restart=on-failure` が 5 回起こし直すので、review-2 S-j）。理由: bluetoothd は既定の image にまだ入らず（p003 の P4）、既存の install で
  動いている物が無いので、暫定が利用者を困らせない。安全の側。**i03（5330 の実機）は「新しい image（account 入り）か Q4 の決定」に依存する。**

### 5. 口の request（p003 の line の形に足す。i02 で review S1・S8 を反映）

| request | 誰 | 答え |
| --- | --- | --- |
| `PAIR <address> <bredr|le-public|le-random>` | §6 の許す人 | 接続し、pairing し、鍵を保存し、切断する。途中で agent に `CONFIRM <番号>`（Numeric Comparison）・`CONSENT`（Just Works の同意、design §6.5 と D4）・`PASSKEY <番号>`（表示だけ、相手が打つ）を送る。終わりに `PAIRED address=… type=… authenticated=0|1 secure=0|1 legacy=0|1 key_size=16 stored=0|1` と `DONE`、失敗は `ERROR <理由>` と `DONE`。理由: `timeout`・`rejected`（相手か agent が断った）・`key-size`・`debug-key`・`reflection`・`check`（相手の値が合わない）・`no-p256`・`pin-unsupported`・`key-missing`（保存した鍵を相手が持たない、FORGET してからやり直す）・`key-type`・`bonded`（LE で bond 済み、FORGET してから）・`unreachable`・`busy`（pairing か scan の最中）・`lost`・`not-ready`・`encryption`・`protocol` |
| `AGENT` | §6 の許す人 | この client を agent にする（同時に 1 つ。同じ uid の AGENT は前を置き換え（前には `AGENT-END`）、別の uid の agent が居る間は `ERROR busy`、review-2 M-d）。答えは `AGENT ok` と `DONE`、その後 client は `CONFIRM <番号>`・`CONSENT`・`PASSKEY <番号>`・`AGENT-END` の行を受け、CONFIRM と CONSENT に `YES`・`NO` で答える。25 秒で答えが無ければ NO（SMP の 30 秒より前、review-2 S-k） |
| `FORGET <address> <型>` | §6 の許す人 | 鍵を消す。`DONE`（無ければ `ERROR not-bonded` と `DONE`） |
| `BONDS` | 誰でも | `BOND address=… type=… authenticated=… legacy=… name="…"` の行と `DONE`（鍵そのものは出さない） |
| `SCAN`・`SHOW`・`DEVICES` | p003 のまま（SCAN は §6 の許す人に広げる） | SHOW の CONTROLLER の行の末尾に `ssp=0|1 sc=0|1` |

- agent が居ない時は PAIR を送った client が agent を兼ねる（`CONFIRM` の行を PAIR の答えの途中に受け、`YES`・`NO` を書く）。agent が居れば
  そちらへ送る。agent は PAIR を送った人と同じ uid か seat の人だけ（review S6）。違えば PAIR の client が agent を兼ねる。
- PAIR の client が切れたら pairing をやめる（相手を切断する。review S8）。agent が切れたら答えは NO。
- **`PASSKEY?`（こちらが passkey を打つ形）は i02 では無い**（review S1）: bluetoothd は DisplayYesNo（agent が居る）か NoInputNoOutput を名乗るので、
  相手が passkey を表示する組（BR/EDR の User Passkey Request、LE の initiator が打つ側）は起こらない。キーボードの入力ができる agent（Settings の
  窓、p006）が来た時に KeyboardDisplay と `PASSKEY?` を足す（判断 Q6）。
- 相手から始まる pairing（Connection Request・IO Capability Request・Link Key Request・User Confirmation Request で、こちらの `PAIR` の
  無い物）は断る（Reject Connection Request 0x0F、IO Capability Request Negative Reply 0x18、Link Key Request Negative Reply、User Confirmation
  Request Negative Reply。design §6.5）。
- client の poll の timeout（review S8）: main の loop は session の queue に packet が残る時は 0、pairing の deadline・agent の 30 秒・scan の
  終わりの最も早い物まで待つ。

### 6. 権限（D8。i02 で review S6 を反映）

`getpeereid` の uid が、root、または seat の人（`/dev/gpu0` の持ち主で `_greeter` でない人、volumed と同じ）、または group `wheel` の人
（`getpwuid` と `getgrouplist` で確かめる。networkd の先例）。`_greeter` は断る（login の画面で pairing を許さない）。他は `ERROR permission`。
CLI の `bt pair` は agent が居なければ自分が agent になる（端末で `y/n` を聞く）。

### 7. 暗号（D5 b1、D10、KNOB）

- 相手の鍵の長さは BR/EDR は HCI_Read_Encryption_Key_Size、LE は Pairing Response の Maximum Encryption Key Size。どちらも 16 未満は断る。
- 仕様の debug の公開鍵（Core Vol 3 Part H §2.3.5.6.1 の X・Y）を相手が送れば断る（Pairing Failed 0x08 Unspecified Reason、review M1）。
  controller が返した自分の鍵が debug の鍵（controller が debug mode）の時も断る。相手の鍵の X が自分の鍵の X と同じ（reflection）も断る
  （review S4）。invalid curve（CVE-2018-5383）は controller の LE Generate DHKey の検査に頼る（残る危険として記録）。
- BR/EDR は Link Key Notification の key type: 3（debug）は断って切断、0・1・2（legacy の combination・unit）は断る（PIN は i02 も断る）、
  4・5（P-192）・7・8（P-256）は受け、6（changed combination）は前の type を保つ（前が無ければ断る）。保存は暗号化と鍵の長さの検査の後
  （review S5）。
- legacy（LE の SC 無し）は保存の `legacy=1` と `PAIRED legacy=1` で知らせる（Settings の警告は p006）。

### 8. loopback の controller の追加（試験の kernel だけ、QEMU で確かめるため。i02 で review S13 を反映）

controller の側の SSP（LMP の中）を演じる「相手」を loopback に足す。p002 の ACL の echo は handle 0x001 のまま残す。

- **mask と mode を覚える**: Set Event Mask（0x0C01）・LE Set Event Mask（0x2001）・Write Simple Pairing Mode（0x0C56）を覚え、既定は仕様の
  既定（mask `FF FF FF FF FF 1F 00 00`、LE `1F 00 …`、SSP off）。mask で落ちる event は出さない（Command Complete・Status、Number Of Completed
  Packets、vendor は落ちない）。SSP が off なら Link Key Request Negative Reply の後に IO Capability Request ではなく PIN Code Request を出す。
- 0x0405 Create Connection: Command Status。相手が 0A:0B:0C:0D:0E:01・05・06・07 なら Connection Complete（handle 0x0040、encryption off）、
  他は Connection Complete（status 0x04 Page Timeout）。
- 相手の役: 01 は DisplayYesNo（Numeric Comparison、番号 123456、鍵の type 8）、05 は鍵の type 3（debug）、06 は鍵の長さ 7、07 は
  NoInputNoOutput（Just Works、番号 0、鍵の type 7）。鍵は address から決まる固定の 16 byte。
- 0x0411 Authentication Requested: Command Status、Link Key Request。Link Key Request Reply（0x040B）は Command Complete の後、鍵が相手の
  固定の鍵と同じなら Authentication Complete（0）、違えば（0x06 PIN or Key Missing）。Negative Reply（0x040C）は Command Complete の後、
  SSP on なら IO Capability Request、off なら PIN Code Request。IO Capability Request Reply（0x042B）は Command Complete、IO Capability
  Response、User Confirmation Request。User Confirmation Request Reply（0x042C）は Command Complete、Simple Pairing Complete（0）、
  Link Key Notification、Authentication Complete（0）。Negative Reply（0x042D）は Command Complete、Simple Pairing Complete（0x05）、
  Authentication Complete（0x05）。PIN Code Request Negative Reply（0x040E）は Command Complete、Authentication Complete（0x06）。
- 0x0413 Set Connection Encryption: Command Status、Encryption Change（on）。0x1408 Read Encryption Key Size: 16（06 だけ 7）。
- 0x0406 Disconnect: Command Status、Disconnection Complete（reason 0x16）。
- ACL（handle 0x0040・0x0041）: 受けた packet ごとに Number Of Completed Packets（1）。0x0040 の L2CAP の signalling の Information Request
  （feature mask）に Information Response（fixed channels の bit）。0x0041 の SMP の Pairing Request に Pairing Failed（0x05 Pairing Not Supported）。
- 0x200D LE Create Connection: Command Status。相手が 0A:0B:0C:0D:0E:03（public）なら LE Connection Complete（handle 0x0041）、他は
  答えない（daemon の 10 秒の timeout と LE Create Connection Cancel（0x200E: Command Complete と LE Connection Complete status 0x02）の経路）。
- 0x2002 LE Read Buffer Size: 長さ 27・数 4（BR/EDR と別の pool、review B5 を QEMU で通す）。
- 0x2025・0x2026（P-256・DHKey）は loopback では作らない（相手が Pairing Request で断るので呼ばれない。判断 Q3）。

### 9. 試験

- host（`plan/ws143/tests/bt-daemon-host-test.sh` に足す）: crypto（RFC 4493・FIPS-197・python の値）、acl・l2cap の組み直しと signalling、smp の
  initiator を台本の相手（host の試験の中で crypto.c を使って相手の値を作る。python の値で crypto 自体は別に確かめる）で SC の Just Works・
  Numeric Comparison・Passkey、legacy の Just Works、鍵の長さ 7 の拒否、debug の鍵の拒否、reflection、DHKey の失敗、timeout。keys の書き読み・
  壊れた file・忘れる。fuzz（ACL・L2CAP・SMP・HCI の接続の event、固定の seed）。
- host（新、`bt-link-host-test.c`）: session の queue（command の待ちの間の event・ACL を積み、後で順に出す。上限の溢れ。review B3 の
  「Reply の間に次の event」）、初期化の列（mask の値、SSP・SC・LE Host の command、LE Read Buffer Size の 0 と非 0）、ACL の credit（BR/EDR と
  LE の pool、Number Of Completed Packets、credit が無い間は積む、LE の 27 byte の分割）。pair を偽の controller（BR/EDR の SSP の相手と、
  LE の SC Just Works の相手を crypto.c で演じる）で: Numeric Comparison の yes・no、Just Works、保存した鍵（Reply と key missing）、key type 3、
  鍵の長さ 7、PIN Code Request、unreachable、LE の SC Just Works で鍵（LTK・IRK・identity）の保存、LE の timeout と Cancel、相手から始まる
  pairing の拒否、途中の切断（lost）。
- QEMU（T1）: `plan/ws143/tests/bt-pair-p004.sh`: `_bluetooth` で子が動く（ps）、`bt pair 0A:0B:0C:0D:0E:01 bredr` が `CONFIRM 123456` に
  `y` で `PAIRED … authenticated=1`、`/var/db/bluetooth/...` に 0600 の鍵、2 度目の pair は保存した鍵（`stored=1`）、07 は Just Works
  （`authenticated=0`）、05 は `ERROR debug-key`、06 は `ERROR key-size`、`CONFIRM` に `n` で `ERROR rejected`、知らない address は
  `ERROR unreachable`、`FORGET` で消える、`bt bonds`、試験の account（wheel でも seat でもない）の `PAIR`・`SCAN` は `ERROR permission`、
  LE の 03 は `ERROR rejected`（loopback に SMP の相手が無い）、LE の知らない address は `ERROR timeout`、`bt-daemon-p003.sh` の回帰。
- 実機（i03 以降、p008 の UAT）: 本物の相手との pairing。HCI の event の byte の並び（IO Capability Response・User Confirmation Request・
  Link Key Notification・Read Encryption Key Size の答え・LE Connection Complete）と P-256 の HCI と SMP の byte の順は、偽の controller も
  loopback も daemon と同じ理解で作っているので、共通の誤りは見つからない（review-2 S-h）。外部の出典（btmon の記録など）との照合は
  i03 の実機で行う。

### 10. i02 の改訂（review の Blocking と Should の反映）

#### 10.1 session の packet の queue（review B3）

- `session_command`・`session_wait_vendor` は、待ちの間に来た答え以外の packet（event・ACL・kernel の notice の外）を session の中の queue
  （32 KiB の byte の環、各 packet の前に長さ 2 byte）に積み、その場では処理しない。reset の notice は今までどおり command を ECONNRESET で
  終える。queue が溢れたら新しい物を捨てて数え（`queue_dropped`、SHOW に出す）、log に出す。
- `btd_session_input` は queue を先に 1 つ取り出し、空なら node を読む。main の loop は `btd_session_pending()` が真の間 poll の timeout を 0 にする。
- pair（と scan）への配りは `session_dispatch` だけが行い、それは main の loop の `btd_session_input` からだけ呼ばれる。pair の handler は
  `session_command` を呼んでよい（待ちの間の packet は queue に積まれ、handler は入れ子に呼ばれない）。
- p003 の scan の結果も待ちの間は queue に積まれ、後で表に入る（振る舞いは同じ）。

#### 10.2 初期化（review B1・B5・S14）

p003 §2 の列を次にする（7 以降）:

7. Set Event Mask = `BF 80 E0 00 02 C0 2F 24`。bit n は event の code n+1（Core Vol 4 Part E §7.3.1）: Inquiry Complete・Inquiry Result・
   Connection Complete・Connection Request・Disconnection Complete・Authentication Complete・Encryption Change（bit 0〜5・7）、Hardware Error（15）、
   PIN Code Request・Link Key Request・Link Key Notification（21〜23）、Inquiry Result with RSSI（33）、Extended Inquiry Result・Encryption Key
   Refresh Complete（46・47）、IO Capability Request・Response・User Confirmation Request・User Passkey Request・Simple Pairing Complete（48〜51・53）、
   User Passkey Notification（58）、LE Meta（61）。失敗は `error`。
8. LE があれば LE Read Buffer Size（0x2002）: 長さと数。長さ 0 は BR/EDR と共有の pool。LE Set Event Mask = `87 01 00 00 00 00 00 00`（bit n は
   subevent n+1、Core §7.8.1: LE Connection Complete・Advertising Report・Connection Update Complete・Read Local P-256 Public Key Complete・
   Generate DHKey Complete）。失敗なら `le=0`。
9. Write Simple Pairing Mode（0x0C56）= 1。失敗は `ssp=0`（BR/EDR の pairing は PIN になり、断られる）を記録して続ける。
10. Write Secure Connections Host Support（0x0C7A）= 1。失敗は `sc=0` を記録して続ける。
11. LE があれば Write LE Host Support（0x0C6D）= `01 00`。失敗は記録だけ。
12. Write Inquiry Mode = 2。
- 9〜11 は supported commands の bit を見ずに送り、controller の Unknown HCI Command（0x01）などの断りを「無い」と読む（判断 Q9。bit の位置の
  取り違えを避ける。5330 の実の値は SHOW の記録で i03 に照合）。
- ACL の pool（review B5）: BR/EDR は Read Buffer Size の長さと数、LE は LE Read Buffer Size（0 なら BR/EDR の pool を共有）。session は送った
  packet を handle ごとに数え、Number Of Completed Packets（0x13、handle と数の組の並び）で返す。credit が無い時は送らずに session の送りの
  queue（16 frame）に積み、credit が戻った時に出す。分割は handle の種類の長さ（LE は 27 など）で、最初の packet の PB は BR/EDR が 0x00
  （non-flushable）、LE も 0x00、続きは 0x01。

#### 10.3 pair.[ch]（接続と pairing の状態機械）

- 同時に 1 つの pairing（`busy`）。scan の最中も `busy`（review S9）。session の handler として connection・pairing の event と ACL を受け、
  main.c へは callback（agent への問い、終わりの答え）で返す。deadline: 接続 10 秒（BR/EDR は Create Connection Cancel 0x0408、LE は LE Create
  Connection Cancel 0x200E を送り、Connection Complete を待つ）、pairing 全体 60 秒、SMP の 30 秒（仕様。過ぎたら Pairing Failed を送らずに切断）、
  agent 30 秒。
- BR/EDR: 保存した鍵があれば Link Key Request に Reply（`stored=1`）、Authentication Complete が 0x06 なら `key-missing`（鍵は消さない。
  上書きの攻撃を避け、人が FORGET する。review S11）。無ければ Negative Reply → IO Capability Request Reply（agent が居れば DisplayYesNo・
  Authentication Requirements 0x03 = MITM と dedicated bonding、居なければ NoInputNoOutput・0x02）→ IO Capability Response で相手の IO を
  覚える → User Confirmation Request: 両方が DisplayYesNo か KeyboardDisplay なら Numeric Comparison で agent に CONFIRM、他は Just Works で
  PAIR の要求を同意として受ける（判断 Q7。review S2）→ Link Key Notification を覚える（§7 の key type の規則）→ Authentication Complete →
  Set Connection Encryption → Encryption Change → Read Encryption Key Size が 16 → 鍵を保存 → L2CAP の Information Request（feature mask、
  2 秒、答えは log と `l2cap=` だけで失敗にしない。QEMU の L2CAP の引き金。review S10）→ Disconnect → `PAIRED`。
- LE: bond 済みなら `bonded`（判断 Q8）。controller に P-256・DHKey が無ければ `no-p256`。LE Create Connection（scan 0x60・0x30、interval
  0x18〜0x28、latency 0、supervision 5 秒、自分は public）→ LE Connection Complete → smp（自分は public の BD_ADDR、相手は address と型、
  乱数は `arc4random_buf`。review S4）→ smp の action を HCI・ACL へ（SEND は CID 6、READ_KEY は 0x2025 と LE meta 0x08、DHKEY は 0x2026 と
  0x09、CONFIRM は agent、SHOW は agent の PASSKEY、ENCRYPT は LE Enable Encryption 0x2019 と Encryption Change）→ DONE で鍵を保存（相手が
  identity address を配れば file の名前はそれ。review S11）→ Disconnect → `PAIRED`。CID 5 の Connection Parameter Update Request は
  l2cap.c の Accept の後に LE Connection Update（0x2013）を送る（review S9）。
- 相手からの切断（Disconnection Complete）は途中なら `lost`。

## 判断の記録

| ID | 判断 | 理由 |
| --- | --- | --- |
| Q1 | i01・i02 は b1 だけ。b2 は AX211 が b1 を持たない時に | 自前の P-256 は危険が大きい（design §6.6）。要るかは 5330 の SHOW（i03）で分かる |
| Q2 | BR/EDR の PIN の legacy は i02 も断る | agent の PIN の入力（p006 の窓）を足す前に、SSP の経路を固める。D10 の「受けて警告」は p006 以降で足す（記録） |
| Q3 | QEMU の LE の SMP は失敗の経路だけ（i02 で見直して保つ、review S12） | reviewer の指摘どおり loopback は DHKey を固定で返せ、kernel に AES もある。ただし SC の相手（f4・f5・f6 と鍵の配り）を試験の kernel に書くのは大きく、同じ確かめを host の `bt-link-host-test`（crypto.c で演じる LE の相手と pair.c・session.c の全体）で行う。QEMU の LE の相手は GATT の要る p005 で考える |
| Q4 | **判断待ち**（review B6）。暫定は「account が無ければ起動を拒む」、扱いは `btd_privsep_no_account()` の 1 か所 | 既存の install に account を足す仕組みが base に無い。D17 はユーザーの決定「足す（既存の install の更新を含む）」。選択肢: (a) 既存の install に account を足す仕組みを作る、(b) 分離できない時は起動を拒む、(c) D17 を改める。Q1 経由でユーザーに聞く |
| Q5 | PAIR は接続・pairing・切断まで（接続を保たない） | 接続を保つのは HID（p005）の仕事 |
| Q6 | `PASSKEY?`（こちらが打つ）と KeyboardDisplay は p006 へ（review S1） | DisplayYesNo では起こらない。入力のできる agent（Settings の窓）が来た時に足す |
| Q7 | **改めた（review-2 BL3）**: Just Works も agent に `CONSENT` で同意を聞く（BR/EDR は User Confirmation Request で、LE は暗号化の前に）。agent が居ない時（今の口では起こらない）は聞かずに進む | design §6.5（「Just Works でも人の同意を取る」）は D4 でユーザーが決めた形。i02 の最初の改訂の「PAIR の要求を同意とする」は決定と食い違っていた |
| Q8 | LE の bond 済みの相手への PAIR は `bonded`（BR/EDR は保存した鍵で確かめる） | 黙った上書き（review S11）を避ける。LE の LTK での再暗号化は再接続（p005）の仕事 |
| Q9 | Write Simple Pairing Mode・SC Host Support・LE Host Support は bit を見ずに送り、断りを「無い」と読む | supported commands の bit の位置の取り違え（p003 の review B1 の類）を避ける。結果は SHOW の `ssp`・`sc` |

## design-reviewer の結果（2026-10-08、q880-i01）と扱い

敵対的 review（Blocking 6・Should 14・Minor 15）。i01 で実装した部品（crypto・acl・l2cap・smp・keys）に関わる物はその場で直すか、
直し方を次の attempt の作業に書いた。設計（§1〜§9）の改訂は次の attempt の最初の作業にする（ここに挙げた物を §に入れてから pair・privsep に進む）。

| ID | 指摘 | 扱い |
| --- | --- | --- |
| B1 | p003 の初期化の event mask・LE event mask が接続・pairing の event を落とす。Write Simple Pairing Mode・SC Host Support を送らない。loopback は mask を見ないので QEMU では見つからない | **次の attempt**: session_core に Write Simple Pairing Mode=1、Write SC Host Support=1（supported の bit を見て）、mask の案 `BF 80 E0 00 02 C0 2F 24`・LE `87 01 00…`（Core Vol 4 Part E §7.3.1・§7.8.1 と照合）、Write LE Host Support の判断。loopback は mask と SSP mode を覚え、mask で落ちる event を出さず、SSP off なら PIN Code Request を出す |
| B2 | zedBSD の kernel に SOCK_SEQPACKET が無い（unix-socket.c は STREAM・DGRAM だけ） | **次の attempt**: 親子の socketpair は SOCK_DGRAM（SCM_RIGHTS は datagram でも扱える）。host の試験も同じ型 |
| B3 | command の待ちの中から pair が入れ子で呼ばれる、外側の答えが stray で捨てられる、ACL が捨てられる | **次の attempt**: command の待ちの間に来た答え以外の packet（event・ACL）を上限のある queue に積み、main loop が command の後に取り出す。pair は待ちの中から command を出さない。host の試験に「Reply の間に次の event」の台本 |
| B4 | 暗号の試験が独立でない | **i01 で一部対処**: f4・f5・f6・g2・ah・c1・s1 は Core Vol 3 Part H Appendix D と §2.2 の例の値で試す（bt-pair-host-test.c、python 43 でも同じ値）。台本の相手の P-256 の鍵と DHKey は python で独立に計算した値。**残り**: Appendix D の P-256 の sample の鍵の組と DHKey で SMP の流れ全体を再現する試験（乱数の口を仕様の Na・Nb に固定） |
| B5 | LE の ACL の分割と credit を BR/EDR の buffer で行う | **次の attempt**: 初期化で LE Read Buffer Size（0x2002、0 なら共有）、handle の種類ごとの長さと credit、送る PB は 0x00 |
| B6 | Q4 が D17（ユーザーの決定）と食い違う | **判断待ち**（判断の記録 Q4）。Q1 経由でユーザーへ |
| S1 | DisplayYesNo では `PASSKEY?` は起こらない | 次の attempt: 入力のできる agent なら KeyboardDisplay を名乗る（smp.c の IO は init の引数なので変更は呼び手だけ）。host の試験に組を足す |
| S2 | authenticated は方式から、BR/EDR の Just Works は「同意」として尋ねる、MITM の方針 | smp.c は方式から決めている（i01）。BR/EDR の判断は pair.c（次の attempt）。MITM: 認証の無い鍵も受け、bond に authenticated=0 と記録（design §6.2 の D10 と同じ「受けて記録」） |
| S3 | SMP の手順の細部 | smp.c が実装済み（Cb の向き、Passkey 20 回・毎回新しい Nai・順 Cai→Cbi→Nai→Nbi、Ea を先に・Eb を確かめる、7 octet の A・B、EDIV=Rand=0、LTK の反転）を host 試験で確かめた。鍵の配りの「要求の部分集合」の検査と「全部揃って PAIRED」は実装済み。HCI の P-256 の byte の順は i02 で照合 |
| S4 | 乱数の出所、reflection（相手の鍵＝自分の鍵）、controller の debug mode | 次の attempt: arc4random_buf を random_fn に渡す（smp.c は口を持つ）。reflection と自分の鍵が debug の鍵の検査を smp.c に足す。invalid curve（CVE-2018-5383）は controller の検査に頼る危険として記録 |
| S5 | link key の保存が早い | 次の attempt（pair.c）: 全部の検査の後に保存。key type 6 は前の type を保つ、0・1・2 は断る、P-192 と P-256 を区別 |
| S6 | D8 の判定（getgrouplist）、agent は PAIR の uid に限る、group bluetooth | 次の attempt: getpwuid＋getgrouplist（networkd の先例）、agent は PAIR を送った uid か seat の人。group `bluetooth` は判定に使わないなら作らない |
| S7 | p003 の試験（kei は scan を断られる）と矛盾 | Q1 の判断: 試験の config に wheel でも seat でもない試験の account を `--file` で足し、p003 の試験を直す |
| S8 | client の口（PAIR の client が agent になる）、切れた時、poll の timeout | 次の attempt（main.c） |
| S9 | LE Create Connection Cancel、Connection Update を実際に送る、SMP timeout は切断だけ、scan 中の PAIR は busy | 次の attempt（pair.c）。l2cap.c は update を effect で呼び手に返す（i01） |
| S10 | L2CAP の Configuration | **i01 で対処**: hint の bit は無視、知らない option は 0x0003 と type、MTU<48 は 0x0001 と最小、continuation の flag（host 試験）。Information の feature mask は fixed channels の bit（0x80）を立てる。Flush Timeout・QoS・RFC・FCS の受けは次の attempt。QEMU の L2CAP の引き金: PAIR の BR/EDR で Information Request を送る（次の attempt） |
| S11 | 鍵の file（残った tmp、identity address の名前、上書きの攻撃、/var/db が無い） | i01: tmp は O_TRUNC|O_NOFOLLOW（O_EXCL でない）。名前を identity address にする・上書きの検査・親が /var/db を作るのは次の attempt |
| S12 | Q3 の理由が半分違う（loopback は DHKey を固定で返せる、AES は kernel にある） | 次の attempt で Q3 を見直す（SC Just Works の相手を試験の kernel に作れば QEMU で LE の鍵の配りまで通せる） |
| S13 | loopback の再現度 | 次の attempt（§8 に足す） |
| S14 | 範囲と依存（p003 の i02 待ち、Q1・Q2 で i01 は clear しない） | 記録: p004 は D5 b2・D10 の PIN が残るので i01 では clear しない。p003 の申し送り（resume の load、Write LE Host Support、LE Read Buffer Size、active scan）は B1・B5 と次の attempt で扱う |
| M1〜M15 | 小さい物 | M1（debug の鍵の拒否の理由: i01 は 0x0B DHKey Check Failed）は次の attempt で 0x08 Unspecified に。M13（鍵の長さ 7〜16 の外は Invalid Parameters）: smp.c は 16 以外を全部 Encryption Key Size（0x06）で断る。7〜16 の外を Invalid Parameters（0x0A）にするのは次。M14（向き）は host 試験の debug の鍵で確かめた。他は次の attempt の設計の改訂で決める |

**i02 の扱い（2026-10-08 朝、q883）**: 設計の改訂として B1・B5・S14 → §10.2、B2・B6 → §4（B6 は Q4 のまま決定待ち）、B3 → §10.1、
S1 → §5・Q6、S2 → §10.3・Q7、S4 → §7・§10.3、S5 → §7、S6 → §4・§5・§6、S7 → §9（Q1 の決定: 試験の account）、S8 → §5、S9・S10 → §10.3、
S11 → §10.3・Q8、S12 → Q3、S13 → §8、M1・M13 → §7 と smp.c に入れた。

## design-reviewer の結果（2026-10-08 朝、i02 の改訂への review-2）と扱い

Blocking 3・Should 11・Minor 13。全てを i02 の実装と上の §に入れた。

| ID | 指摘 | 扱い |
| --- | --- | --- |
| BL1 | privsep の親子の寿命（datagram は EOF を返さない、service の停止で子が孤児、親の fd の写し） | §4: 親は waitpid を 1 秒ごと、SIGTERM・SIGINT を子へ、子は liveness の STREAM の EOF、親は送った fd を close。QEMU の試験 6 |
| BL2 | main loop が queue を取り出さない | main.c: node の revents に関わらず `btd_session_pending` なら取り出す（1 回の loop で 64 packet まで） |
| BL3 | Q7 が D4（§6.5）と食い違う、group bluetooth | Q7 を改め CONSENT を聞く。group bluetooth を作る（§4） |
| S-a | Encryption Change の 0x02 | pair.c は 0 でない値を on とする。loopback と偽の controller は SC の鍵で 0x02 を返す |
| S-b | authenticated・secure は key type から | pair.c は key type から（5・8 が authenticated、7・8 が secure）。loopback は host の IO と相手の IO で type を選ぶ |
| S-c | User Passkey Notification の枝、BR/EDR に KeyboardDisplay は無い | pair.c は `PASSKEY` を agent に送る。User Passkey Request は Negative Reply。host 試験に KeyboardOnly の相手。BR/EDR の Numeric の判定は DisplayYesNo だけ |
| S-d | BR/EDR の最初の PB | session.c: features の byte 6 bit 6（Non-flushable Packet Boundary Flag）があれば 0x00、無ければ 0x02。LE は 0x00 |
| S-e | cancel と成功の競合 | pair.c の CANCELLING: status 0 の Connection Complete は Disconnect して元の理由で終える。client が切れたら接続前は Cancel、後は Disconnect（`btd_pair_stop`） |
| S-f | close・reset で pair が終わらない | `btd_close` が `btd_pair_lost`（client に `ERROR lost`）。handler の中の ECONNRESET も lost |
| S-g | RPA の bond、identity の上書き | pair.c: 保存した IRK の `ah` で RPA を解決して `bonded`。pairing の後に identity の file があれば書かずに `bonded` |
| S-h | 作り手が同じ試験の共通の誤り | §9 に限界として記録。i03 の実機で照合 |
| S-i | queue の溢れ・reset の notice・Reset の後 | session.c: Number Of Completed Packets と接続の数えは待ちの中で処理（溢れで失わない、record に counted の印）。vendor の待ちの reset は ECONNRESET。HCI Reset の後に queue・links・frames を空に |
| S-j | 暫定の起動の拒否と restart | 終了の値 0。i03 の依存を §4 に |
| S-k | agent と SMP が同じ 30 秒 | agent を 25 秒 |
| M-a〜M-m | 小さい物 | M-b（stored key の 0x05 も key-missing）、M-d（AGENT の置き換え）、M-e（loopback: Reset で mask を戻す、LE の subevent は両方の mask、0x040B などは Status と BD_ADDR、0x1408 は Status・Handle・Key_Size）、M-f（mask に bit 25 Data Buffer Overflow を戻し数える。mask は `BF 80 E0 02 02 C0 2F 24`）、M-i（getpwnam の値、folder の検査、`-f` は親の argv）は実装した。M-a（§3 の O_EXCL の記述）・M-g（Create Connection の parameter: packet type 0xCC18、R1、clock offset 0、role switch 可、Disconnect の reason 0x13）・M-h（LE は `timeout`、BR/EDR は `unreachable` のまま）・M-l（SHOW の `sc` は host の bit の書き込みが通ったことだけ）・M-m（QEMU の LE は 27 byte の分割を通らない、credit と別の pool だけ）は記録。M-c（agent の有無）: 今の口では PAIR の client が常に agent を兼ねるので `agent=1`。答えられない client（backend）は p006 で口を足す。M-j は commit を揃えた。M-k（Appendix D の P-256 の sample で SMP 全体）は残り（p009 か i03） |

## 実装（i02、2026-10-08 朝、P2）

- `userland/base/bluetoothd/session.[ch]`: 待ちの間の packet の queue（32 KiB、record に長さと counted の印）、`btd_session_pending`・
  `btd_session_command`・`btd_session_send`、handler（`btd_handler_fn`）、初期化（§10.2 と review-2 の mask）、ACL の pool（BR/EDR と LE）と
  credit（Number Of Completed Packets、接続の数え、送りの queue 16 frame、PB）、Data Buffer Overflow の数え。
- `pair.[ch]`（新）: §10.3 と review-2 の全て。`smp.[ch]`: why、M1・M13、reflection、自分の debug の鍵。`l2cap.[ch]`: Information Request と
  その答えの effect。`keys.[ch]`: `btd_keys_list`。`acl.c`: 符号の変換。
- `privsep.[ch]`（新）、`main.c`（PAIR・AGENT・YES・NO・FORGET・BONDS、D8、queue の取り出し、deadline、client の dead の印）、`Makefile`。
- `userland/base/bt/main.c`: `bt pair|forget|bonds|agent`、端末で y/n。
- `userland/base/etc/passwd`・`group`・`shadow`: `_bluetooth`（80）と group `bluetooth`（81）。
- `src/drivers/generic/bt-hci-loopback.c`: §8 と review-2 M-e（試験の kernel だけ）。
- 試験: `plan/ws143/tests/bt-link-host-test.c`（新）、`bt-pair-host-test.c`（why と reflection）、`bt-daemon-host-test.sh`、
  `bt-pair-p004.sh`（新、QEMU）、`build-bt-image.sh`・`passwd`・`group`（新、試験の account btuser、S7）、`bt-daemon-p003.sh`・
  `bt-loopback-p002.sh`（T1-402 の直し: btuser と dmesg の差分）。

## 確認

q883-i02（2026-10-08 朝、P2、host と build だけ）:

- `OUT=build/tmp/p2-btd-i02 sh plan/ws143/tests/bt-daemon-host-test.sh`（gcc、ASan・UBSan、-Werror）: `bt-daemon-host-test: PASS (90 checks)`、
  `bt-pair-host-test: PASS (143 checks)`、`bt-link-host-test: PASS (44 checks)`。
- `make -j16 ZEDBSD_CONFIG=plan/ws143/tests/config-amd64-bt.mk BUILD=build/p2-bt build/p2-bt/vmunix build/p2-bt/bin/bluetoothd build/p2-bt/bin/bt`:
  rc 0、warning 0（-Werror）、kernel include check・amd64 vmunix check PASS。
- `python3 plan/tools/style-check.py`（bluetoothd の全 file、bt、loopback、host 試験）: 0。
- **未実施**: QEMU（T1 に依頼: bt-loopback-p002.sh・bt-daemon-p003.sh・bt-pair-p004.sh、image は build-bt-image.sh）、実機（i03）。

q880-i01:

q880-i01（2026-10-08、P2、host だけ）:

- `OUT=build/tmp/p2-btd-host sh plan/ws143/tests/bt-daemon-host-test.sh`（gcc、ASan・UBSan、-Werror）: p003 の `bt-daemon-host-test: PASS (90 checks)`、
  p004 の `bt-pair-host-test: PASS (140 checks)`（fuzz 20000 回ずつ）。
  - crypto: FIPS-197 C.1、RFC 4493 の 4 例、Core の f4・g2・f5（MacKey・LTK）・f6・c1・s1・ah。
  - smp: SC の Just Works・Numeric Comparison（yes・no）・Passkey Entry（20 回、bluetoothd の confirm を毎回 f4 で照合）、legacy の Just Works
    （Mconfirm・STK・responder の LTK・EDIV・Rand・IRK・identity）、鍵の長さ 7、debug の鍵、confirm の誤り、DHKey の失敗、空の PDU、相手の Pairing Failed。
  - acl・l2cap・keys は上の review の S10・S11 の i01 の分を含む。
- `clang -Wall -Wextra -Wshadow -Wconversion -Werror` で crypto・acl・l2cap・smp・keys を単独に compile して warning 0。
- `python3 plan/tools/style-check.py`（新しい file と試験）: 指摘 0。
- **未実施**: bluetoothd の Makefile への追加（新しい部品はまだ daemon に link していない。daemon の build は p003 のまま）、zedBSD の build、QEMU（T1 への依頼は無い）、実機。

## T1-405 と i03（2026-10-08、P2、Q1 の投入「p004 の新しい attempt、test-wait を外して uncleared の記録」）

T1-405（tree ecda54bc2、build-bt-image.sh、p002 → p003 → p004、新しい guest で 2 回とも同じ）: bt-loopback-p002 PASS。bt-daemon-p003 FAIL
（`it started twice on the controller (got '3', want '2')`）。bt-pair-p004 FAIL（`the child killed ends the parent (got '2', want '0')`、
他の約 40 行は ok、`the parent told to end ends the child` は ok）。q883-i02 はこれで **uncleared**。

i03 の直し:
- p003 の数え方: 子の起動の log `BLUETOOTHD READY state=ready uid=80` も `state=ready ` に当たって 3 になった（試験の前提のずれ）。数えるのを
  `: /dev/btN state=ready ` の行（start の結果の行）に限った（T1-405 の log では 2）。
- privsep の親: 子が死ぬと親の datagram の socket は poll で読める（相手の close で read_shutdown）のに、datagram の recv は EOF を返さずに
  待ち続ける（unix-socket.c、review-2 BL1 の指摘の続き）。親が `privsep_answer` の recv で止まり、waitpid に戻らなかった（子は zombie で残り
  数が 2）。recv を `MSG_DONTWAIT` にした（読める物が無ければすぐ戻り、次の round の waitpid が子の終わりを見る）。試験に kill の後の
  `ps` の表示を足した。
- 確認: bluetoothd の build（rc 0、warning 0）、style-check 0。QEMU は T1 に再依頼。

## 再開点（i02 の後）

- T1 の結果（bt-loopback-p002.sh・bt-daemon-p003.sh・bt-pair-p004.sh）を Q1 が判定する。FAIL は同じ Phase の次の attempt で直す。
- p004 を clear に残る物: D5 b2（5330 の `p256`・`dhkey` 次第）、D10 の PIN の legacy（Q2、p006 の PIN の入力の後）、Q4（ユーザーの決定）、
  i03 の 5330 の実機（新しい image）。B4 の残り（Appendix D の P-256 の sample で SMP の流れ全体）。

## 再開点（i01 の後、記録）

i01 で commit した物: `userland/base/bluetoothd/{crypto,acl,l2cap,smp,keys}.[ch]`、`plan/ws143/tests/bt-pair-host-test.c`、
`plan/ws143/tests/bt-daemon-host-test.sh`（p004 の試験を足した）、この phase.md。daemon（main.c・session.c・Makefile）は変えていない。

順に:
1. 上の review の表の「次の attempt」を §1〜§9 に入れて設計を改訂する（B1・B2・B3・B5 は Blocking）。B6（Q4）はユーザーの判断を待つ（それまで privsep は
   account の有る時の経路だけを作り、無い時の扱いを判断の後に足す）。
2. session の packet の queue（B3）と初期化（B1・B5）、その host の試験。
3. `pair.[ch]`: HCI の接続（BR/EDR・LE）、BR/EDR の SSP（IO Capability・User Confirmation・Link Key Request/Notification・Authentication Complete・
   暗号化・鍵の長さの検査の後の保存）、LE の SMP の orchestration（smp.c の action を HCI・ACL へ）、ACL の送りの分割と credit（B5）。
4. privsep（親が `/dev/btN` を開いて SCM_RIGHTS、子は `_bluetooth`、SOCK_DGRAM）、`userland/base/etc/passwd`・`group` に `_bluetooth`（uid 80）。
5. main.c の口: PAIR・AGENT・FORGET・BONDS、D8 の判定、poll の timeout。`bt pair/forget/bonds/agent`。Makefile の BLUETOOTHD_SOURCES に新しい file。
6. loopback（§8 と S13）、`plan/ws143/tests/bt-pair-p004.sh`、build（`plan/ws143/tests/config-amd64-bt.mk`）、T1 への依頼。
