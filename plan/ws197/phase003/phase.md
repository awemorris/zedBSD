<!-- awesome-plan project=zedbsd record=ws197-p003 -->

# ws197-p003: MAP の MCE（MAS の client と MNS の server）、phone link の持ち主・記録・再接続、socket の PHONE（詳細設計）

Phase ID: `ws197-p003`
Parent: [WS197](../ws.md)
Status: planning（2026-10-10 P1: 詳細設計の第 1 版。design-reviewer の前。code は書かない）
Phase disposition: normal
Queue: Q1 の投入（2026-10-10「WS197 p003 MAP（MCE の MAS と MNS）を保留の branch agent/p1-ws197 で。まず詳細設計を書き design-reviewer を通してから実装」）
Branch: `agent/p1-ws197`（10/17 の公開まで main に merge しない。各 Phase の始めと merge 依頼の前に main を取り込む）
依存: [p002](../phase002/phase.md)（cleared 2026-10-10、T1-518 PASS）、[p001](../phase001/phase.md) の設計とユーザーの決定（2026-10-09「全部推しどおり」、Q16 は 2026-10-10 に (a)）
所有 path: `userland/base/bluetoothd/`（新しい file と、下の §2 の表の今の file の変更）、`plan/ws197/`

前提のユーザーの決定（p001 §11）:

- Q1 スマホは 1 台。Q2 持ち主だけが見る、bluetoothd は中継だけで message の中身を disk に書かない、保存は Phone の app の `~/Documents/Phone/`（p004）。
- Q4 最初の同期は過去 30 日、folder ごとに最大 500 通。Q5 MMS は範囲外。Q12 OBEX の認証は使わない。
- Q13 (a) Android で先に確かめ、iPhone の MAP は p006 の後。Q14 持ち主の logout で profile を切り、seat に戻ったら再接続。
- Q16 (a) MAP 1.1 を名乗り GOEP 1.1（RFCOMM）だけ。MapSupportedFeatures の交換、Extended Event Report 1.1、UTC Offset の形式、Persistent Message Handles は使わない。

## 0. 出典

- MAP 1.4.2 の PDF（2026-10-10 P1 が読んだ。複写は repository に入れない）。MAP 1.1 の本文は手元に無い。1.4.2 §2.1 は 1.1 の相手との後方互換を求め、1.1 の MCE が使う形（1.0 の listing・event・bMessage）は 1.4.2 の本文に「Version 1.0」として全部ある。この文書の節番号は MAP 1.4.2 の物。
- SDP の属性 ID と UUID は Bluetooth Assigned Numbers（ServiceName 0x0100（言語の base 0x0100 + 0x0000）、MAS 0x1132、MNS 0x1133、MAP の profile 0x1134、MASInstanceID 0x0315、SupportedMessageTypes 0x0316、MapSupportedFeatures 0x0317、OBEX 0x0008、RFCOMM 0x0003、L2CAP 0x0100）。手元に Assigned Numbers の文書が無いので「(Assigned Numbers、確かめる)」とし、実機の SDP の dump（p008）で照合する。p001 §4.4 の 0x0317 と同じ扱い。
- 他の OS の実装（BlueZ・Android・Apple）の code は読まない。

## 1. 範囲

この Phase で作る物（p001 §12 の p003 の行）:

1. 持ち主の記録 `.phone`（§3）と、`PAIR … phone=1` の handoff での書き込み、FORGET の規則、`PHONE LINK`・`PHONE SHOW`。
2. 全部の socket の client の出力の queue（`outq.c`）、client の枠 16 と予約、`PHONE SEND` の長さ付きの入力（§4）。
3. phone link の一生: 持ち主の在・不在（seat）、zedBSD からの page と backoff、スマホからの接続の受け、認証・暗号化・鍵の長さ、suspend の後（§5）。p002 の試しの `PHONE PROBE` は MAP に置き換えて消す。
4. MAP の XML（listing と event）と時刻（§6）、bMessage（§7）。
5. MAP の MAS の client（接続、folder、通知の登録、操作の queue、同期の page、1 通、既読、送信）と MNS の server（SDP の record、event）（§8）。
6. socket の PHONE の request と event、`PHONE SUBSCRIBE`（F-086 の一部）（§9）。
7. host の試験と fuzz、T1 への HID の回帰の依頼（§10）。

作らない物: Phone の app・compositor・libkeiland の API（p004）、PBAP（p005）、HFP（p006・p007）、通知の banner（p004）。MMS・EMAIL・IM（Q5）。送った SMS を app の保存と重ねる規則は p004（§9.6）。

## 2. 部品と file

| file | 新・変 | 内容 | i |
| --- | --- | --- | --- |
| `phonerec.c`・`.h` | 新 | `.phone` の記録の形・読み・書き・消し・探し（§3） | i01 |
| `outq.c`・`.h` | 新 | client ごとの出力の queue（§4） | i02 |
| `phone.c`・`.h` | 変 | 持ち主、在・不在、page と受け、認証、DLC の振り分け、MAP の配線、PROBE を消す（§5） | i01・i03・i06 |
| `mapxml.c`・`.h` | 新 | listing と event の XML、datetime（§6） | i04 |
| `bmsg.c`・`.h` | 新 | bMessage の読みと組み立て（§7） | i05 |
| `map.c`・`.h` | 新 | MAS の client、MNS の server、操作の queue（§8） | i06 |
| `phoneio.c`・`.h` | 新 | PHONE の行の読みと書き（escape、key=value、item の行）（§9） | i07 |
| `obex.c`・`.h` | 変 | 応答の各 packet の header を owner に渡す `response` の hook（§8.2） | i06 |
| `main.c`・`protocol.h`・`Makefile` | 変 | client の枠と出力の queue、長さ付きの入力、seat の見直し、PHONE の request、FORGET の規則、sleep.end | i01・i02・i03・i07 |

system call を持つのは `phonerec.c`（file）と `main.c` だけ。他は host の試験で build する（p002 と同じ）。

## 3. 持ち主の記録 `.phone`（p001 §8.2、S4）

### 3.1 形

`/var/db/bluetooth/<controller>/<address>-bredr.phone`、0600、`_bluetooth` の持ち物。bond の file の横（`hidcache.c` の `.hid` と同じ置き方。`btd_keys_list` は型の名前が `bredr.phone` で読めないので数えない）。text の行:

```
version 1
uid 1001
user alice
messages 1
contacts 1
calls 1
enabled 1
```

- 書きは一時 file・fsync・rename・folder の fsync（`hidcache.c` と同じ手順）。読みは全部の行が 1 回ずつあり値が範囲内の時だけ受け、他は file ごと無効（log、消さない）。
- `user` は uid の account の名前（63 byte まで、`[A-Za-z0-9._-]`）。uid の再利用の検出に使う（§3.3）。
- 1 台だけ（Q1）: controller の folder に `.phone` は 1 つ。`btd_phonerec_find(folder, controller, &record)` は 0 個なら ENOENT、2 個以上なら EEXIST（log。どれも使わない）。

### 3.2 書く時と消す時

| 出来事 | 記録 |
| --- | --- |
| `PAIR … phone=1` の handoff で phone が受ける（p002 §7.4 の 6） | 書く（uid は PAIR の client、`user` は main の hook で uid から引く、profile は全部 1、enabled 1）。**別の address の `.phone` があれば受けない**（`why=other-phone`）。書けなければ受けない（`why=store`） |
| `PHONE LINK ADDRESS on|off [profiles=m,c,h]` | enabled と profile を書き換える（持ち主と root だけ、§9.2） |
| `FORGET ADDRESS bredr` | `.phone` のある bond は持ち主と root だけ（p001 N2: D8 の例外）。bond と `.phone` を両方消し、つないでいれば切る |
| 記録の uid の account が無い、`user` が違う | 記録を無効として扱う（enabled 0 と同じ、file は残す）。`PHONE SHOW` に `owner=invalid`。直すのは FORGET と再 pairing |
| bond が無い、bond の鍵が認証済みの型（0x05・0x08）でない | 同じく無効 |

### 3.3 持ち主の在・不在（p001 §8.2、S10、S21）

- **在** ＝ 記録が有効で enabled、かつ seat の人（`/dev/gpu0` の持ち主、`_greeter` を除く）の uid が記録の uid で、その account の名前が `user`。
- main が 5 秒ごと（記録がある間だけ）と、持ち主の SUBSCRIBE の接続が切れた時に seat を見直し、`btd_phone_set_present(phone, present, now)` を呼ぶ。account の名前の照合（`getpwuid`）は main が行う（phone.c は system call を持たない）。
- 在 → 不在: MAP を閉じ（OBEX の Disconnect は送らず、DLC を閉じる）、ACL を切る（Q14）。page と page scan の要求を下ろす。
- 不在 → 在: すぐ page する（§5.2、backoff を戻す）。

### 3.4 HID の上限

記録が有効で enabled の間（在・不在に関わらず）HID の同時の接続は 5（`btd_hid_set_limit`、p002 §7.5 の「スマホとして使うが有効な間」）。記録が無いか無効か enabled 0 なら 6。

## 4. socket の出力の queue と入力（p001 §8.3、S9、S20）

### 4.1 出力の queue（`outq.c`）

- client ごとの byte の queue（malloc で伸ばす、上限 `BTD_OUTQ_MAX` 256 KB）。`btd_write` は書式の文字列を queue の後ろに足し、すぐ `send` を 1 回試す（non-blocking、EAGAIN は残す）。今の「1 秒まで poll で待つ」は消す（読まない client が daemon の loop を止めない）。
- loop の poll は queue に物がある client に POLLOUT を足し、書ける時に送る。
- 上限を越える追加は client を `dead` にする（今の「読まない client は閉じる」と同じ結果）。**例外: SUBSCRIBE の接続**（§9.4）は event を捨てて「落ちた」の印を立てる（接続は保つ）。
- 長さ付きの値（§9.1）は `btd_outq_append(q, bytes, length)` で、行の後に足す。
- 純粋な関数（append、`btd_outq_flush(q, send_fn, ctx)`、room、clear）にし、send を差し替えて host で試す。

### 4.2 client の枠

- `BTD_CLIENTS_MAX` 8 → 16。空きが 4 以下の時は、root と seat の人の uid の接続だけを受ける（compositor の分の予約、p001 S9）。poll の表は `4 + 16` に。

### 4.3 長さ付きの入力（`PHONE SEND`）

- 今の入力は 512 byte の行。`PHONE SEND to="…" length=N` の行を読んだら、client は「N byte を読む」mode に入り、その後の N byte（`BTD_PHONE_SEND_MAX` 8192 まで）を client の buffer（malloc、SEND の間だけ）に集めてから request を行う。N が上限を越える・0 の行は `ERROR length` で、N byte を読み捨てずに client を閉じる（流れの区切りが分からないため）。
- 待ちの request（PAGE・SEND）の間に来た行は今の `waits_*` と同じく受けない（読み捨て）。

## 5. phone link の一生（p001 §8.2、§8.2.1、§3.4）

### 5.1 状態

| 状態 | 意味 | claims（router） |
| --- | --- | --- |
| NONE | link 無し | 0 |
| PAGING | zedBSD が Create Connection を出した | 記録の address なら 1 |
| ACCEPTING | スマホの Connection Request に Accept を出した | 同 |
| SECURING | Connection Complete の後、認証・暗号化・鍵の長さの確かめ | 同 |
| READY | 使える（p002 の handoff の直後もここ） | 同 |
| CLOSING | Disconnect を出した | 同 |

p002 の `BTD_PHONE_NONE`・`READY`・`CLOSING` に PAGING・ACCEPTING・SECURING を足す。READY と CLOSING の意味は p002 のまま。

### 5.2 zedBSD からの page と backoff

- page する条件: 在、state NONE、bond と記録が有効、controller が READY、pairing・scan の最中でない（HID と同じ `btd_hid_holding` の扱い: main が pairing・scan の間は phone の tick を page しない形で呼ぶ）。
- 間隔: 最初はすぐ、失敗のたびに 30 s・60 s・120 s・240 s・480 s・600 s（以後 600 s）。link が READY になったら戻す。不在から在、sleep.end（§5.5）、`PHONE LINK on` でも戻してすぐ。
- 手順: `btd_linkmgr_page_begin(lm, BTD_LINKMGR_PHONE, address, now)`（EBUSY なら 2 s 後、間隔の段は進めない）→ Create Connection（0x0405: address、packet type DM1・DH1・DM3・DH3・DM5・DH5（HID の page と同じ値）、page scan repetition mode R1、clock offset 0、Allow Role Switch 0x01）→ PAGING（15 s の守り、linkmgr の 15 s と同じ）。Command Status の失敗・Connection Complete の失敗は NONE と次の段（page の終わりは router が `btd_linkmgr_connected` で行う。同期の失敗は phone が `page_end`）。
- page scan: 記録が有効・enabled・在の間 `btd_linkmgr_want_scan(lm, BTD_LINKMGR_PHONE, 1)`、他は 0。

### 5.3 スマホからの接続

- `btd_phone_wants(address)`: 在、address が記録の物、state が NONE・PAGING・ACCEPTING・SECURING のどれか、で 1。router は Connection Request と Link Key Request の両方でこれを聞く（HID の wants の後、p002 のまま）ので、自分の page の後の Link Key Request も phone に来る。Connection Request の扱いは下の state ごと（NONE 以外の交差は断る）。
- Connection Request（router が phone へ渡す、link type ACL だけ）: state が NONE でない（自分の page と交差）なら Reject Connection Request（0x040A、reason 0x0D = 資源の不足）で断る（相手は自分の接続を続け、こちらの page の Connection Complete が来る）。NONE なら Accept Connection Request（role 0x01 = peripheral のまま。スマホは他の機器の central であることが多く、role switch の失敗で接続を落とさない。**未確認**: p008 で見る）→ ACCEPTING（15 s の守り）。
- Link Key Request（router が phone へ渡す。pair → HID → phone の順は p002 のまま）: 記録が有効で bond の鍵が認証済みの型なら Link Key Request Reply、他は Negative Reply（鍵は読んだ後に 0 で消す、`hid_key_request` と同じ）。

### 5.4 認証・暗号化・鍵の長さ（SECURING）

- Connection Complete（成功）→ route を PHONE に（`btd_router_assign`）、session の link の上限（p002 §4.2）、L2CAP の表を初期化して accept の hook、SECURING（10 s の守り）、**zedBSD から Authentication Requested（0x0411）**。page と受けのどちらも（p001 §3.4）。スマホも同時に認証を始めて LMP の衝突（status 0x23・0x2A）で失敗した時は 200 ms 後に 1 回だけやり直す。
- Authentication Complete（成功）→ 暗号化が未だなら Set Connection Encryption（0x0413、on）。Encryption Change（成功・on）→ Read Encryption Key Size（0x1408）→ 16 でなければ切る（`why=key-size`）。
- 16 なら READY: Pending の channel（スマホが先に開けた SDP・RFCOMM）を成功で答え、MAP を始める（§8.1、messages の profile が on の時）。
- 失敗（Key Missing 0x06 はスマホが bond を消した）→ 切る、STATE `why=key-missing`、間隔は 600 s へ（スマホの側で pairing をやり直すまで繰り返しても無駄）。
- p002 の handoff の link は既に暗号化と鍵 16 を確かめてあるので直接 READY（今のまま）。handoff の後に記録を書き（§3.2）、在なら MAP を始め、不在なら切る。

### 5.5 切断・suspend・controller の喪失

- Disconnection Complete・ACL の喪失・drop の知らせの回復（p002 §3.5 の phone の列）は今のまま。加えて MAP を失敗にし（§8.6）、NONE に戻して次の page を予定（スマホが離れた時の再接続）。
- sleep.end（`/dev/system` の POWER、今の `btd_system_events`）: main が `btd_phone_resume(phone, now)` を呼び、backoff を戻す（link が生きていれば何もしない。死んだ link は supervision の timeout で Disconnection Complete が来て、その後に page）。sleep.begin では何もしない（user space が動く時間の保証が無い）。
- 通話中の suspend の扱いは範囲外（Q15、p006）。

### 5.6 PROBE を消す

p002 の `PHONE PROBE` と `struct btd_phone_probe`・`phone_probe_*` は消し、SDP の問い合わせ・RFCOMM の session・DLC の開けは §8 の MAP が使う一般の形（§5.7）にする。`bt-phone-link-host-test` の PROBE の場面は §10 の MAP の場面に置き換える（試験の直しは見つけた担当、AGENTS.md）。

### 5.7 phone.c と map.c の境

- SDP の問い合わせ: `phone_sdp_query(phone, uuid)` → 結果を `btd_map_sdp_done(map, sdp, error)` へ（1 度に 1 つ。p005・p006 も同じ口を使う）。
- RFCOMM: phone は session と DLC の表（dlci → 使い手 `PHONE_DLC_MAS`・`PHONE_DLC_MNS`）を持ち、`opened`・`data`・`writable`・`closed` を使い手の map の関数へ渡す。`accept(server_channel)` は MNS の channel（16）で messages が動いている時だけ 1。
- map から phone へは hook（`struct btd_map_hooks`）: `dlc_open(channel)`、`dlc_write`、`dlc_close`、`sdp_query(uuid)`、`emit(line, bytes, length)`（SUBSCRIBE の client へ）、`answer(token, line, bytes, length)`（request の client へ）、`room(token)`（client の queue の空き、§8.4）、`local_offset(now)`（zedBSD の UTC からのずれ、秒）。試験は hook を差し替える。

## 6. XML と時刻（`mapxml.c`、p001 §7.1.1・§7.1.2、R18）

### 6.1 読む形

- listing（MAP §3.1.6.1、1.0）: root `MAP-msg-listing`、子 `msg` の空要素の列。event（§3.1.7.1、1.0）: root `MAP-event-report`、子 `event` 1 つ。version の属性は読み、`1.0` 以外でも続ける（知らない属性は無視、§3.1.6・§3.1.7 の「shall ignore」）。
- 字句: 先頭の UTF-8 の BOM、`<?xml …?>`、comment `<!-- … -->`、内部 subset の無い `<!DOCTYPE …>`（`[` があれば失敗）を飛ばす。要素の名前と属性の名前は `[A-Za-z_:][A-Za-z0-9_.:-]*`。`=` の前後の空白を許す（§3.1.6.1 の例は `handle = "…"`）。値は `"` か `'`。実体は `&lt; &gt; &amp; &quot; &apos;` と `&#N;`・`&#xH;`（U+0000・surrogate・0x10FFFF 越えは失敗）を UTF-8 に。CDATA・処理命令（宣言以外）・namespace の扱い・入れ子の要素（`msg` の中身）は失敗。
- 上限: 属性 32/要素、値 1024 byte（解いた後）、`msg` 1024 個、入力 64 KB。超えたら失敗（listing 全体が失敗）。
- 結果: 呼び手の配列に `struct btd_map_entry`（handle の 64 bit、datetime の文字列 20 byte、sender_name・sender_addressing・recipient_name・recipient_addressing（各 256 byte で切る）、type（SMS_GSM・SMS_CDMA・MMS・EMAIL・IM・他）、size、read、sent、reception_status）。必須の handle が無い・16 桁を越える・16 進でない `msg` は 1 件だけ捨てて数える（listing は続ける）。
- event: `struct btd_map_event`（type の名前（NewMessage・DeliverySuccess・SendingSuccess・DeliveryFailure・SendingFailure・MemoryFull・MemoryAvailable・MessageDeleted・MessageShift・他）、handle、folder、old_folder、msg_type）。
- fuzz の対象（固定の seed、20 万回）。

### 6.2 datetime（MAP §3.1.6 の datetime、OBEX の time の形）

- 読み: `YYYYMMDDTHHMMSS` と、任意の `±hhmm`（MAP 1.4.2 §3.1.10 の形。1.1 の相手は付けない見込みだが、付いていれば使う）。範囲（月 1〜12、日はその月の日数、時 0〜23、分 0〜59、秒 0〜60）外は失敗。
- UNIX 秒へ: offset があればそれ、無ければ **MSETime の offset**（listing の答えの App Parameters 0x19 `YYYYMMDDTHHMMSS±hhmm`、§5.5.4.12。1.1 の相手が付けるかは未確認）、それも無ければ zedBSD の `local_offset`（スマホと同じ地域と仮定、p001 §7.1.2）。どれを使ったかを item に `zone=phone|mse|local` で残す。暦の計算は純粋な関数（days-from-civil）。
- FilterPeriodBegin の書き: UNIX 秒をスマホの local time（上と同じ順で選んだ offset）で `YYYYMMDDTHHMMSS`（offset を付けない。1.1 の形）。
- 試験の正解: 手で計算した値（例 `20071213T130510+0900` → 1197518710、`20240229T235960` の閏日と 60 秒、`19700101T000000+0000` → 0）を試験に書く。

## 7. bMessage（`bmsg.c`、MAP §3.1.3、p001 §7.1.1、R9）

### 7.1 読み（GetMessage の答え）

- 行は CRLF（LF だけも受ける）。構造の行は前後の空白を除いて比べる（§3.1.3 の例は字下げしてある）。`BEGIN:BMSG` で始まり `END:BMSG` で終わる。入れ子（BENV）は 3 段まで（§3.1.3 「maximum level … three」）、4 段目は失敗。
- 読む物: `STATUS`（READ・UNREAD）、`TYPE`、`FOLDER`、originator の vCard（BENV の外の最初の BEGIN:VCARD〜END:VCARD）の `TEL`・`N`・`FN`、最も内の BENV の recipient の vCard の最初の `TEL`、BBODY の `CHARSET`・`ENCODING`・`LENGTH`、最初の `BEGIN:MSG` の中身。vCard の行の parameter（`TEL;TYPE=CELL:`）は `:` の前を飛ばす。2.1 の quoted-printable の名前は p005 の vcard.c まで解かない（N・FN は表示の補いで、listing の sender_name を先に使う）。
- **本文の範囲**（LENGTH、§3.1.3 の bmessage-body-content-length-property: 最初の `BEGIN:MSG` の `B` から最後の `END:MSG<CRLF>` の CRLF まで）: 次の順に試し、最初に合った物を使う（どれかを `form=` として数える）。
  1. 仕様: `start = BEGIN:MSG の位置`、`end = start + LENGTH`。`end` の直前 9 byte が `END:MSG\r\n`（LF だけの入力では 8 byte の `END:MSG\n`）。
  2. 本文だけ: `BEGIN:MSG` の行の後から LENGTH byte の直後が `\r\nEND:MSG` か `END:MSG`。
  3. 最後の CRLF を数えない: `start + LENGTH` の直前 7 byte が `END:MSG`。
  4. どれも合わない: `BEGIN:MSG` の後の最初の、行頭の `END:MSG` の行まで（log に `form=scan`）。
- 本文 ＝ `BEGIN:MSG` の行の後から `END:MSG` の行の前の改行の前まで。行頭の `/END:MSG` は `/` を 1 つ除く（`//END:MSG` → `/END:MSG`、§3.1.3）。
- 本文の charset: CHARSET が UTF-8 か無い（GetMessage で Charset=UTF-8 を頼んだ時の SMS の形 1）時だけ受け、UTF-8 として検査（不正な列は U+FFFD に置き換え）。ENCODING があり CHARSET が無い（PDU の native の形）は `ERROR native`（頼んでいない形、数えて捨てる）。
- 上限: 入力 64 KB、本文 16 KB（越えたら切って `truncated=1`、UTF-8 の文字の途中で切らない）。fuzz の対象。

### 7.2 組み立て（PushMessage、§3.1.3 の BNF と §5.8）

```
BEGIN:BMSG
VERSION:1.0
STATUS:READ
TYPE:SMS_GSM
FOLDER:
BEGIN:BENV
BEGIN:VCARD
VERSION:2.1
N:
TEL:<番号>
END:VCARD
BEGIN:BBODY
CHARSET:UTF-8
LENGTH:<n>
BEGIN:MSG
<本文>
END:MSG
END:BBODY
END:BENV
END:BMSG
```

- 改行は全部 CRLF。originator は付けない（BNF で任意）。FOLDER は空（§3.1.3「the MCE should send an empty bmessage-folder-property」）。TYPE は MAS の SupportedMessageTypes が SMS_GSM を持てば SMS_GSM、SMS_CDMA だけなら SMS_CDMA。recipient の vCard 2.1 は VERSION と N が必須、SMS は TEL（§3.1.3 の vCard）。
- 本文の行頭の `END:MSG`（と `/END:MSG`…）は `/` を 1 つ足し、LENGTH に足す（§3.1.3）。本文の改行は CRLF に揃える（LF だけの改行を CRLF に）。
- LENGTH ＝ `BEGIN:MSG\r\n` ＋ 本文 ＋ `\r\nEND:MSG\r\n` の byte 数（形 1）。
- 番号は `[0-9+*#]`、1〜32 文字。他は `ERROR number`。本文は UTF-8 で 1〜8192 byte、NUL を含まない。
- 試験: 手で書いた byte の列（BNF から、LENGTH を手で数えた値）と一致。読みに通して戻る（往復だけでは対称の誤りを捕まえないので、手の正解が主）。

## 8. MAP の MCE（`map.c`）

### 8.1 始め（READY かつ messages が on、§6.4.3）

1. MNS の SDP の record を登録（§8.7）。
2. SDP で MAS（0x1132）を問い合わせ、record を順に見て SupportedMessageTypes（0x0316）が SMS_GSM（bit 1）か SMS_CDMA（bit 2）を持つ最初の物を選ぶ（p001 §4.2）。その RFCOMM の channel、MASInstanceID（0x0315）、MapSupportedFeatures（0x0317。無ければ 0x0000001F、§7.1.1 の注 1）、profile の版を持つ。無ければ `why=no-mas`。
3. RFCOMM の DLC（MAS の channel）→ OBEX Connect（Target MAS、App Parameters 無し、Q16 (a)）。答えが 0xC1・0xC3（Unauthorized・Forbidden: スマホで許可が無い）なら `why=permission`、他の失敗は `why=refused`。
4. SetPath を 2 回: `telecom`、`msg`（flags 0x02 = 作らない、§5.3 表 5.7）。以後 folder は `telecom/msg` に留め、listing・送信は子の folder の名前で指す（§5.5.2・§5.8.2）。1 通・既読は handle で指す（handle は MSE の全体で一意、§3.1.1）。
5. 通知: MapSupportedFeatures に bit 0（通知の登録）と bit 1（通知）があれば Put `x-bt/MAP-NotificationRegistration`（App Parameters NotificationStatus 0x0E = 1、End of Body 0x30、§5.2）。答えの後、スマホが MNS に接続してくる（§8.7）。無ければ通知無しで続け、STATE に `notify=0`。
6. READY: STATE `messages=ready send=<1 if bit 3>`。送信は bit 3（Uploading）がある時だけ受ける。

各段の失敗は MAP を FAILED にして STATE で知らせ、**同じ link の間は繰り返さない**（スマホの許可を待つ時に繰り返しで相手を叩かない）。`PHONE LINK on` と次の link で始め直す。

### 8.2 OBEX の変更

MAP §6.3.2: 何 packet かに分かれた Get の答えは、Body 以外の header を最初の packet に置く（listing の ListingSize・MSETime）。今の `done` は最後の答えの header だけを渡すので、`struct btd_obex_events` に `response(context, operation, code, headers, length)` を足し、client の各答えの packet ごとに（Body を含む header の列のまま）呼ぶ（NULL なら呼ばない。p002 の使い手は NULL のまま）。

### 8.3 MAP の handle とセッション

- handle は MAP の session の間だけ有効（§3.1.1: Persistent Message Handles が無い時。1.1 の MCE は使えない）。MAP の session（MAS の接続）ごとに番号 `session`（daemon の起動時の 32 bit の乱数（main の `btd_random`）から 1 ずつ増やす）を付け、socket には `handle=<session 8 桁の 16 進>.<handle 16 桁の 16 進>` で出す。READ・GET・SEND の答えの handle が今の session のでなければ `ERROR stale`。
- 重複の鍵 `key`（app の Source、p001 §8.6）: 64 bit の FNV-1a（`dir` の 1 文字、`|`、スマホの datetime の文字列（変換の前、offset 付きならそれも）、`|`、相手の番号（空白と `-` を除いた物）、`|`、本文）の 16 桁の 16 進。handle を含めない（session で変わる）。timezone の推定の差で変わらないよう、変換の前の文字列を使う。

### 8.4 操作の queue（p001 §7.1.0）

MAS の OBEX は 1 度に 1 つの操作。全部を 1 本の queue（16 個まで）で順に行う。

| 操作 | OBEX | 使い道 |
| --- | --- | --- |
| COUNT | Get `x-bt/MAP-msg-listing`、Name＝folder、MaxListCount 0x01 = 0（§5.5.4.1） | MSETime と ListingSize（§8.5） |
| LIST | Get 同、Name＝folder、MaxListCount＝n、ListStartOffset 0x02＝o、FilterMessageType 0x03 = 0x0C（EMAIL と MMS を除く。IM の bit 4 は 1.1 の相手には予約なので立てない）、FilterPeriodBegin 0x04（§6.2）、ParameterMask 0x10 = bit 1・2・3・4・5・6・8・12（datetime、sender_name、sender_addressing、recipient_name、recipient_addressing、type、reception_status、read）= 0x0000117E、body の上限 64 KB | 同期の page |
| LOCATE | Get 同、Name＝folder、MaxListCount 8、ParameterMask 同 | live の event の時刻と名前（event 1.0 には datetime が無い、§3.1.7.1） |
| GET | Get `x-bt/message`、Name＝handle（16 桁、§5.6.2）、Attachment 0x0A = 0、Charset 0x14 = 1（UTF-8）、body の上限 64 KB | 1 通 |
| UNREAD | Put `x-bt/messageStatus`、Name＝handle、StatusIndicator 0x17 = 0、StatusValue 0x18 = 0、End of Body 0x30 | GET でスマホが既読にした物を戻す（§8.5） |
| READ | 同、StatusValue 1 | `PHONE READ` |
| PUSH | Put `x-bt/message`、Name `outbox`、Charset 1、End of Body＝bMessage。答えの Name が新しい handle（§5.8.2） | `PHONE SEND` |

- Type の header は NUL 終わりの ASCII（p002 の `PHONE_LISTING_TYPE` と同じ扱い、(IrOBEX、確かめる)）。App Parameters は big-endian（§6.3.1）。
- 答えの code: 0xA0 で成功。0xC4 Not Found（handle が無い）は `ERROR not-found`、0xD3 Service Unavailable（通話中など）は `ERROR unavailable`、0xC3・0xC1 は `ERROR permission`、他は `ERROR refused`。timeout・DLC の切断は MAP を FAILED にする（§8.6）。
- queue が満ちた時: 新しい request は `ERROR busy`、live の event の取得は捨てて SUBSCRIBE に `PHONE DROPPED`（app が同期をやり直す、p001 S7）。
- **client の速さ**: PAGE の GET を 1 通ずつ出す前に `room(token)` が 32 KB 未満なら待つ（loop の次の回で見る）。PAGE は MAS の queue を占めるので、待ちが 30 s を越えたら PAGE を `ERROR slow` で終える（live の event を止め続けない）。

### 8.5 同期の page（`PHONE PAGE messages`）

- 引数: `since=<UNIX 秒>`、`cursor=<文字列>`（初回は空）、`count=<1..32>`。folder は `inbox`（dir=in）と `sent`（dir=out）の順（outbox・draft・deleted は読まない）。
- cursor ＝ `<session>.<folder の番号>.<offset>`。session が違えば `ERROR stale-cursor`（app は自分の目印から最初からやり直す、p001 §8.4）。
- その MAP の session で最初の page なら、最初の folder に COUNT を 1 回出し MSETime の offset を覚える（§6.2）。
- LIST（n＝count、o＝offset）→ 各 entry に GET → item を `answer` で出す（§9.3）。listing で `read=no` だった entry は、GET の後に UNREAD を出す（p001 S6、S23: GetMessage が既読にする機種がある。既読にしない機種には害が無い）。type が SMS でない entry は飛ばす（数える）。
- 次: `o + k < min(ListingSize, 500)` かつ k ＝ n なら同じ folder の `o + k`、他は次の folder の 0、最後の folder の後は `more=0`（Q4 の folder ごと 500 通）。
- 答えの最後: `PHONE PAGE-END cursor=… more=0|1 count=<出した item の数> skipped=<飛ばした数>`、`DONE`。途中の失敗は `ERROR …`、`DONE`（app は PAGE-END が来た時だけ目印を進める、p001 S7）。
- 限界（記録）: listing は新しい順の offset なので、page の間に届いた SMS で 1 件ずれて重複し（key で除く）、消された SMS で 1 件飛ぶ。app の 24 時間の重なり（p001 §8.4）と live の event で補う。

### 8.6 live の event（MNS）

| event（1.0） | folder | 行い |
| --- | --- | --- |
| NewMessage | `…/INBOX`・`…/SENT`（大文字・小文字を問わず最後の部分） | LOCATE → GET → SUBSCRIBE へ `PHONE MESSAGE`（§9.3）。LOCATE で見つからなければ time は受けた時の zedBSD の時刻（`zone=received`）。msg_type が SMS でなければ捨てる。他の folder（outbox・draft）は捨てる |
| MessageShift | folder が SENT、old_folder が OUTBOX | 自分の PUSH の handle なら `PHONE SENT … state=sent`。他（スマホで打った SMS）は NewMessage と同じ |
| SendingSuccess・DeliverySuccess・SendingFailure・DeliveryFailure | — | 自分の PUSH の handle（MAP の session の間、32 個まで覚える）なら `PHONE SENT request=… handle=… state=sent|delivered|failed`。知らない handle は捨てる |
| MessageDeleted | — | `PHONE MESSAGE-GONE handle=…`（app が使うかは p004） |
| MemoryFull・MemoryAvailable・他 | — | log（数だけ） |

- MNS の Put（§5.1）: Type が `x-bt/MAP-event-report`、App Parameters の MASInstanceID 0x0F が自分の MAS の物（違えば 0xA0 で受けて捨てる。1 つの MNS に全部の MAS の event が来る、§3.1.7.2）、body を §6.1 で読む。読めない body は 0xC0 Bad Request。Type が違えば 0xD1 Not Implemented。
- MAP の失敗（MAS の DLC の切断、timeout、link の喪失）: queue の request は全部 `ERROR lost`、STATE `messages=failed why=…`、MNS の DLC も閉じる。

### 8.7 MNS の server と SDP の record（MAP §7.1.2、Q16 (a)）

- RFCOMM の server channel 16（p001 §5.4）。phone の `accept(16)` は messages が動いている（MAP が CONNECTING 以降）間だけ 1。DLC が開いたら OBEX の server（`btd_obex_init(…, BTD_OBEX_SERVER)`）、`target` は MNS の UUID（`bb582b41-420c-11db-b0de-0800200c9a66`、§6.3 表 6.5）の時だけ 1。MNS の DLC は 1 つ（2 つ目は DM）。
- record（属性 ID の昇順、`btd_sdps_register` の形）:

| 属性 | 値 |
| --- | --- |
| 0x0001 ServiceClassIDList | UUID16 0x1133 |
| 0x0004 ProtocolDescriptorList | (L2CAP 0x0100)、(RFCOMM 0x0003, uint8 16)、(OBEX 0x0008) |
| 0x0005 BrowseGroupList | 0x1002 |
| 0x0009 BluetoothProfileDescriptorList | (0x1134, uint16 0x0101) |
| 0x0100 ServiceName | "Keiland MNS" |

- MapSupportedFeatures（0x0317）と GoepL2CapPsm（0x0200）は出さない（Q16 (a)、p002 §1）。MSE は MapSupportedFeatures の無い MCE に 0x0000001F を仮定する（§7.1.2 の注 1）。
- 登録は MAP の始め（§8.1 の 1）、外すのは phone link の終わり。SDP の server は phone の link の上だけで答えるので（p002）、他の機器には見えない。

## 9. socket の PHONE（p001 §8.3）

### 9.1 行の形（`phoneio.c`）

- 値の文字列は `"…"`。中の `"` と `\` は `\"`・`\\`、0x20 未満と 0x7F は `\xHH`。名前と番号は escape の前に 128 byte で切る（UTF-8 の文字の途中で切らない）。daemon が書く 1 行は 2048 byte 以下（compositor の `BT_INPUT_MAX` と同じ）。
- 長さ付きの値: 行の最後の `length=N` の後、改行の次の N byte が中身（改行を付けない）。
- request の引数は `key=value` を空白で区切る（value は引用符の文字列か空白の無い語）。知らない key は `ERROR argument`。
- 純粋な関数（escape、取り出し、item の行）にして host で試す。

### 9.2 request（持ち主と root だけ。`SHOW` は D8 の人も）

| request | 答え |
| --- | --- |
| `PHONE SHOW` | `PHONE address=… owner=<uid>|invalid mine=0|1 enabled=0|1 profiles=m,c,h present=0|1 link=none|paging|securing|ready messages=off|connecting|ready|failed send=0|1 notify=0|1 why=…`（記録が無ければ行無し）、`DONE`。D8 の人で持ち主でない者には `mine=0` と address・enabled だけ |
| `PHONE LINK ADDRESS on|off [profiles=m,c,h]` | 記録の持ち主と root。on: enabled 1（と profile）、在ならすぐ page。off: enabled 0、つないでいれば切る（記録は残す）。記録が無い address は `ERROR not-phone`（今までの普通の bond は取れない、p001 §8.2）、`DONE` |
| `PHONE SUBSCRIBE` | §9.4。答えは `DONE` の後に event の行が続く |
| `PHONE PAGE messages since=N cursor=C count=N` | §8.5 |
| `PHONE GET handle=H` | `PHONE MESSAGE …` 1 つ、`DONE` |
| `PHONE READ handle=H` | `DONE` |
| `PHONE SEND to="…" length=N` ＋ N byte | `PHONE SENT request=<n> handle=H state=queued`、`DONE`（PushMessage の成功まで待つ） |
| `PHONE DROP ADDRESS` | p002 のまま（root、試験の道具） |

- messages が ready でない時の PAGE・GET・READ・SEND は `ERROR not-ready`。SEND は send=0 の時 `ERROR no-send`。
- 1 つの client が待つ request は 1 つ（PAGE・GET・READ・SEND の答えまで他の行を受けない、今の `waits_*` の形）。daemon 全体でも MAS の queue（§8.4）が順にする。

### 9.3 item の行

```
PHONE MESSAGE handle=<s>.<h> key=<16 hex> folder=inbox|sent dir=in|out time=<UNIX 秒> zone=phone|mse|local|received datetime="<スマホの文字列>" peer="<番号>" name="<名前>" read=0|1 truncated=0|1 length=<n>
<n byte の UTF-8 の本文>
```

- peer と name: dir=in は sender_addressing と sender_name（無ければ originator の vCard の TEL と FN・N）、dir=out は recipient_addressing と recipient_name（無ければ recipient の vCard）。
- read は listing の `read`（GET の前の値。UNREAD で戻すので、スマホの状態と同じ）。

### 9.4 SUBSCRIBE（F-086 の一部）

- `PHONE SUBSCRIBE` を送った接続は、以後 event の行だけを受ける（request の行は読み捨て）。持ち主と root だけ、同時に 2 つまで。
- event: `PHONE STATE …`（§9.2 の SHOW の行と同じ中身、変わった時）、`PHONE MESSAGE …`（§9.3）、`PHONE SENT …`、`PHONE MESSAGE-GONE …`、`PHONE DROPPED`。
- 出力の queue（§4.1）で、event を足すと 192 KB を越える時は足さず「落ちた」の印を立て、空いた時に先に `PHONE DROPPED` を足す。
- F-086（状態の変化の通知の全体）の Future Work の行の更新は Q1 に頼む（この WS は phone の event だけ）。

### 9.5 log

message の本文・番号・名前は log に書かない（p001 R22）。件数、response code、`why`、bMessage の `form=` だけ。

### 9.6 p004 に渡す事

- `PHONE SEND` した SMS は app が自分で保存する。同じ SMS が後の PAGE（sent の folder）で `key` を付けて来るので、app は「同じ peer・同じ本文・時刻が ±10 分」の自分の送信の item と重ねる（p004 の store の設計で決める）。
- 本文 16 KB の上限、`truncated`。

## 10. 試験

### 10.1 host（`plan/ws197/tests/`、ASan・UBSan、`bt-phone-host-test.sh` に足す）

| 試験 | 内容（正解の出典） |
| --- | --- |
| `bt-phonerec-host-test` | 形と読みの検査（行の欠け・重複・範囲外）、書き・読み・消し、find の 0・1・2 個、`btd_keys_list` が `.phone` を数えない |
| `bt-outq-host-test` | 部分の send、EAGAIN、上限で dead、SUBSCRIBE の落ちた印と DROPPED、長さ付きの値 |
| `bt-mapxml-host-test` | §3.1.6.1 と §3.1.7.1 の例（空白付きの `=`）、実体と文字参照、DOCTYPE、comment、上限、壊れた入力、datetime（§6.2 の手の値）、fuzz 20 万回 |
| `bt-bmsg-host-test` | 手で書いた bMessage（形 1〜4 の LENGTH、`/END:MSG`、3 段の BENV、4 段は失敗、native の形）、組み立ての手の正解、fuzz 20 万回 |
| `bt-map-host-test` | map.c を hook で: 台本の MSE（試験の中で OBEX の答えの byte を手で書く。要求は `btd_obex_header_next` で読んで、§5 の表の header と App Parameters の tag・値を照合）。始め（Connect、SetPath 2 回、通知の登録の filler 0x30 と tag 0x0E）、COUNT と MSETime、LIST の App Parameters の byte、GET と UNREAD、PAGE の cursor と 500 の上限と folder の移り、`room` の待ちと 30 s、MNS の Put（MASInstanceID の違い、壊れた XML に 0xC0）、NewMessage の LOCATE・GET、SEND と SendingSuccess、queue の満ち、各 error code、MAP の失敗で全部 `ERROR lost`、stale の handle と cursor |
| `bt-phone-link-host-test`（直す） | PROBE の場面を消し、偽の controller で: handoff で記録を書く（`why=other-phone`、`why=store`）、在で page → Connection Complete → 認証 → 暗号化 → 鍵 16 → READY → MAP の SDP の問い合わせ、交差の Connection Request を断る、スマホからの Connection Request を Accept（role 0x01）、Link Key Request の鍵、Key Missing、backoff の間隔、不在で切る、HID の上限 5・6、MNS の record が SDP の server で読める（今の `sdp.c` の client で読み戻す） |
| `bt-phoneio-host-test` | escape、引数、item の行、2048 byte |

WS143 の host の試験（`bt-daemon-host-test.sh`）も流す（main.c・obex.c を変えるため。obex は WS197 の file だが p002 の試験も）。

### 10.2 QEMU（T1）

保留の branch の image で WS143 の HID の回帰 4 本（p002 §12.2 と同じ、T1-518 の組）。main.c の client と出力の変更で compositor の Bluetooth の画面が壊れないことを、WS143 の `bt-desktop`（あれば）か `bt-daemon-p003.sh` の SHOW・BONDS で見る。MAP の相手は QEMU に無い（dongle 無し、p001 §9.2）ので MAP は host の試験だけ。

### 10.3 実機（p008）

Android で: pairing（phone=1）→ スマホの「メッセージへのアクセス」の許可 → 最初の同期（30 日）→ 受信の event → 既読 → 送信 → 離れて戻る（再接続）→ logout と login。iPhone は Q13 (a) で p006 の後。

## 11. 実装の順（保留の branch の WIP commit の単位）

| i | 内容 | 確かめ |
| --- | --- | --- |
| i01 | `phonerec.c`、handoff での記録（§3.2）、`PHONE LINK`・`PHONE SHOW`（main）、FORGET の規則、HID の上限（§3.4） | host（phonerec、phone-link の handoff）、build |
| i02 | `outq.c`、main の client の枠 16 と予約、non-blocking の出力、長さ付きの入力（§4） | host（outq）、build、WS143 の host の試験 |
| i03 | phone link の一生（§5: 在・不在と seat の見直し、page と backoff、受け、SECURING、sleep.end）、PROBE を消す、DLC の振り分けの形（§5.7） | host（phone-link）、build |
| i04 | `mapxml.c` と datetime（§6） | host、fuzz |
| i05 | `bmsg.c`（§7） | host、fuzz |
| i06 | `obex.c` の `response` の hook、`map.c`（§8）、phone の配線、MNS の record | host（map、phone-link の MNS）、build |
| i07 | `phoneio.c`、main の PHONE の request と SUBSCRIBE と event（§9）、`protocol.h` | host（phoneio）、build |
| i08 | T1 への依頼（§10.2、Q1 へ）、style-check、phase.md の記録 | T1 |

各 i の後に `bt-phone-host-test.sh` と WS143 の `bt-daemon-host-test.sh` を流し、target の bluetoothd の build（`config/current-uat.mk`、`BUILD=build/p1-uat`、target を名指す）を warning 0 にして WIP commit、SHA を「ws197 branch」と明記して Q1 に送る。

## 12. 受け入れ

- §10.1 の host の試験と fuzz が全部 PASS、WS143 の host の試験が PASS、target の bluetoothd の build warning 0、style-check の変更箇所 0。
- T1 の HID の回帰（§10.2）が PASS。
- 実機の MAP（§10.3）は p008。この Phase では未実施と書き、それだけで uncleared にしない（p001 §1 の 4、p002 §14 と同じ）。

## 13. 危険と未確認

| 項目 | 内容 | いつ |
| --- | --- | --- |
| MAP 1.1 の MCE への相手の振る舞い | Android・iPhone が MapSupportedFeatures の無い MCE に通知・送信を出すか、MSETime を付けるか | p008 |
| GetMessage が既読にするか | 機種差。UNREAD で戻す（害は無い） | p008 |
| Accept の role 0x01 | スマホとの接続で role を変えない選択が通るか | p008 |
| 認証の衝突 | スマホも同時に認証を始める時（0x23・0x2A で 1 回やり直す） | p008 |
| SDP の属性 ID | Assigned Numbers を手元で確かめていない（§0） | p008 の SDP の dump |
| handle の寿命 | session ごとに変わる前提（`stale`）。Persistent が使えない 1.1 の制限 | — |
| 同期の取りこぼし | page の間の受信・削除で 1 件ずれる（§8.5） | p004 の重なりと live |
| 時刻 | offset の無い datetime は MSETime か zedBSD の timezone で推定（`zone=`） | p008 |

## 14. 見積もり

p001 の p003 の 16 LW と骨格 +5 LW（p001 §12 の見直し）に対し、内訳: i01 2、i02 2、i03 4、i04 2、i05 2、i06 6、i07 2、i08 1、計 **21 LW**。

## Event

- 2026-10-10: 第 1 版（P1）。MAP 1.4.2 の §3.1.3・§3.1.6・§3.1.7・§5.1〜§5.9・§6.3・§6.4・§7.1 を読んで書いた。
