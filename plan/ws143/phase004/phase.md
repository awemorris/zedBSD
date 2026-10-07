<!-- awesome-plan project=zedbsd record=ws143p004 -->

# ws143-p004: L2CAP・pairing（BR/EDR の SSP・LE の SMP）・暗号・鍵の保存・特権の分離・socket の口の権限

Phase ID: `ws143-p004`
Parent: [WS143](../ws.md)
Status: uncleared（q880-i01 は P2 の context の都合で、部品と host 試験までで安全な地点に commit して終えた。再開点は「再開点」節）
Phase disposition: normal
Queue: q880-i01（P2、Q1 の投入「p004（L2CAP・SMP）。p003 と同じく試験の kernel の loopback で QEMU で確かめられる形に（loopback に要る答えを足してよい）。設計 → design-reviewer → 実装 → host 試験（fuzz を含む）」）

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

### 4. 特権の分離（D16 の (a)、D17）

- base の `etc/passwd`・`etc/group` に `_bluetooth`（uid・gid 80、home `/var/empty`、shell `/sbin/nologin`）。group `bluetooth`（81、D8 の
  許す人の group。今は空。wheel の人は §6 で別に許す）。
- 親（root）: `/run/bluetoothd.sock` を作り（0666）、`/var/db/bluetooth` を `_bluetooth` の 0700 で作り、子と socketpair（SOCK_SEQPACKET）を作り、
  fork する。子は `setgroups(0)`・`setgid(80)`・`setuid(80)` の後に `getuid() == 80` を確かめ（戻せたら終わる）、listener と socketpair の片方
  だけを持って動く。親は他を持たない小さな loop: 子の 1 行 `OPEN` か `OPEN /dev/btN` に、`/dev/bt` と数字 1〜2 桁だけの path を `O_RDWR` で
  開いて SCM_RIGHTS で渡す（失敗は errno の行）。子が終わったら親も終わる（service の `restart=on-failure` が起こし直す）。
- 子の `/dev/btN` の探し直し（p003 の `btd_open`）は親への `OPEN` に置き換える。`-f` も親に渡す。
- account が無い（古い install）時: 親は log に「`_bluetooth` が無いので分離しない」と出し、自分で動く（p003 と同じ形）。既存の install に
  account を足す仕組みは base に無い（新しい image で入る）。

### 5. 口の request（p003 の line の形に足す）

| request | 誰 | 答え |
| --- | --- | --- |
| `PAIR <address> <bredr|le-public|le-random>` | §6 の許す人 | 接続し、pairing し、鍵を保存し、切断する。途中で `CONFIRM <番号>`・`PASSKEY?`・`PASSKEY <番号>`（表示）を agent に送る。終わりに `PAIRED legacy=0|1 key_size=16` と `DONE`、失敗は `ERROR <理由>`（`timeout`・`rejected`・`key-size`・`debug-key`・`no-p256`・`pin-unsupported`・`busy`・`lost`）と `DONE` |
| `AGENT` | §6 の許す人 | この client を agent にする（同時に 1 つ。`PAIR` を送った client が agent が無い時はその client が agent）。agent は `CONFIRM`・`PASSKEY?` の行を受け、`YES`・`NO`・`PASSKEY <6 桁>` で答える。30 秒で答えが無ければ NO |
| `FORGET <address> <型>` | §6 の許す人 | 鍵を消す。`DONE` |
| `BONDS` | 誰でも | `BOND address=… type=… name="…" legacy=…` の行と `DONE` |
| `SCAN`・`SHOW`・`DEVICES` | p003 のまま（SCAN は §6 の許す人に広げる） | |

相手から始まる pairing（Connection Request・IO Capability Request で、こちらの `PAIR` の無い物）は断る（Reject Connection Request、IO
Capability Request Negative Reply、design §6.5）。

### 6. 権限（D8）

`getpeereid` の uid が、root、または seat の人（`/dev/gpu0` の持ち主で `_greeter` でない人、volumed と同じ）、または group `wheel` の人。
`_greeter` は断る（login の画面で pairing を許さない）。他は `ERROR permission`。CLI の `bt pair` は agent が居なければ自分が agent になる
（端末で `y/n` と passkey を聞く）。

### 7. 暗号（D5 b1、D10、KNOB）

- 相手の鍵の長さは BR/EDR は HCI_Read_Encryption_Key_Size、LE は Pairing Response の Maximum Encryption Key Size。どちらも 16 未満は断る。
- 仕様の debug の公開鍵（Core Vol 3 Part H §2.3.5.6.1 の X・Y）を相手が送れば断る（0x0B）。BR/EDR は Link Key Notification の key type 3。
- legacy（LE の SC 無し、BR/EDR の SSP 無しの PIN は i01 で断る）は保存の `legacy=1` と `PAIRED legacy=1` で知らせる（Settings の警告は p006）。

### 8. loopback の controller の追加（試験の kernel だけ、QEMU で確かめるため）

controller の側の SSP（LMP の中）を演じる「相手」を loopback に足す。p002 の ACL の echo は handle 0x001 のまま残す。

- 0x0405 Create Connection（相手 0A:0B:0C:0D:0E:01）: Command Status、Connection Complete（handle 0x0040、encryption off）。
- 0x0411 Authentication Requested: Command Status、Link Key Request。Link Key Request Negative Reply なら IO Capability Request、
  Reply（host の IO capability）の後に IO Capability Response（相手は DisplayYesNo）、User Confirmation Request（番号 123456）、Reply なら
  Simple Pairing Complete（0）・Link Key Notification（固定の鍵、type 5 = authenticated P-192。鍵の type 3 を返す別の address 0A:0B:0C:0D:0E:05 も持つ）・
  Authentication Complete。Negative Reply なら Simple Pairing Complete（0x05）・Authentication Complete（0x05）。Link Key Request Reply なら
  Authentication Complete（0）。
- 0x0413 Set Connection Encryption: Command Status、Encryption Change（on）。0x1408 Read Encryption Key Size: 16（0A:0B:0C:0D:0E:06 だけ 7）。
- 0x0406 Disconnect: Command Status、Disconnection Complete。
- ACL（handle 0x0040）: L2CAP の相手（Information Request に Response、Connection Request（PSM 1）に success、Configuration の往復、
  Disconnection Request に Response）。
- 0x200D LE Create Connection: Command Status、LE Connection Complete（handle 0x0041、相手 0A:0B:0C:0D:0E:03 public）。LE の SMP の相手は
  i01 では作らない（kernel に AES-CMAC と固定の P-256 の鍵の組が要る。下の残り）。LE の Pairing Request には Pairing Failed（0x05 Pairing Not
  Supported）を返し、daemon の失敗の経路を通す。
- 0x2025 LE Read Local P-256 Public Key・0x2026 LE Generate DHKey: Command Status と固定の値（host の試験は偽の controller で同じ形を
  演じる。loopback の値は daemon の経路の確かめだけ）。

### 9. 試験

- host（`plan/ws143/tests/bt-daemon-host-test.sh` に足す）: crypto（RFC 4493・FIPS-197・python の値）、acl・l2cap の組み直しと signalling、smp の
  initiator を台本の相手（host の試験の中で crypto.c を使って相手の値を作る。python の値で crypto 自体は別に確かめる）で SC の Just Works・
  Numeric Comparison・Passkey、legacy の Just Works、鍵の長さ 7 の拒否、debug の鍵の拒否、DHKey の失敗、timeout。pair の BR/EDR の台本
  （SSP の全部の経路、key type 3、鍵の長さ 7、Link Key Request の保存した鍵）。keys の書き読み・壊れた file・忘れる。fuzz（ACL・L2CAP・SMP・
  HCI の接続の event、固定の seed）。
- QEMU（T1）: `plan/ws143/tests/bt-pair-p004.sh`: `_bluetooth` で子が動く（ps）、`bt pair 0A:0B:0C:0D:0E:01 bredr` が agent の `CONFIRM 123456` に
  `y` で `PAIRED`、`/var/db/bluetooth/...` に 0600 の鍵、2 度目の pair は保存した鍵で（Link Key Request Reply）、debug の鍵の相手は `ERROR debug-key`、
  鍵の長さ 7 の相手は `ERROR key-size`、`FORGET` で消える、root でない・seat でない人の `PAIR` は `ERROR permission`、LE の pair は
  `ERROR rejected`（loopback に SMP の相手が無い）、`bt-daemon-p003.sh` の回帰。
- 実機（i02 以降、p008 の UAT）: 本物の相手との pairing。

## 判断の記録

| ID | 判断 | 理由 |
| --- | --- | --- |
| Q1 | i01 は b1 だけ。b2 は AX211 が b1 を持たない時に | 自前の P-256 は危険が大きい（design §6.6）。要るかは i02 で分かる |
| Q2 | BR/EDR の PIN の legacy は i01 で断る | agent の PIN の入力を足す前に、SSP の経路を固める。D10 の「受けて警告」は i02 以降で足す（記録） |
| Q3 | QEMU の LE の SMP は失敗の経路だけ | loopback に LE の SMP の相手を作るには kernel に AES-CMAC と固定の P-256 の鍵の組が要る。LE の SMP は host の試験（python で照合した crypto と台本の相手）で確かめる |
| Q4 | **判断待ち**（review B6）。案は「`_bluetooth` が無い install では分離しない（log と `bt show`）」 | 既存の install に account を足す仕組みが base に無い。ただし D17 はユーザーの決定「足す（既存の install の更新を含む）」なので、phase が変えられない。選択肢: (a) 既存の install に account を足す仕組みを作る、(b) 分離できない時は起動を拒む、(c) D17 を改める。Q1 経由でユーザーに聞く |
| Q5 | PAIR は接続・pairing・切断まで（接続を保たない） | 接続を保つのは HID（p005）の仕事 |

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

## 確認

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

## 再開点（次の attempt）

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
