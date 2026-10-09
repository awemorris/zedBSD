<!-- awesome-plan project=zedbsd record=ws197-p003 -->

# ws197-p003: MAP の MCE（MAS の client と MNS の server）、phone link の持ち主・記録・再接続、socket の PHONE（詳細設計）

Phase ID: `ws197-p003`
Parent: [WS197](../ws.md)
Status: in-progress（2026-10-10 P1: 詳細設計の第 2.1 版。[review-1.md](review-1.md) の全部に答え、[review-2.md](review-2.md) で全部閉じた。review-2 の N1・N2・n1〜n9 を本文に入れた（各節の `[N1]`・`[n1]`）。i01〜i07 は GO、実装に入る）
Phase disposition: normal
Queue: Q1 の投入（2026-10-10「WS197 p003 MAP（MCE の MAS と MNS）を保留の branch agent/p1-ws197 で。まず詳細設計を書き design-reviewer を通してから実装」）
Branch: `agent/p1-ws197`（10/17 の公開まで main に merge しない。各 Phase の始めと merge 依頼の前に main を取り込む）
依存: [p002](../phase002/phase.md)（cleared 2026-10-10、T1-518 PASS）、[p001](../phase001/phase.md) の設計とユーザーの決定（2026-10-09「全部推しどおり」、Q16 は 2026-10-10 に (a)）
所有 path: `userland/base/bluetoothd/`（新しい file と、下の §2 の表の今の file の変更）、`plan/ws197/`

版: 2026-10-10 第 1 版（P1、d21a55cae）→ 同日 第 2 版（review-1 の指摘、各節の `[B1]`・`[M1]`・`[m1]` の印、4397bcb31）→ 第 2.1 版（review-2 の N1・N2・n1〜n9）。

前提のユーザーの決定（p001 §11）:

- Q1 スマホは 1 台。Q2 持ち主だけが見る、bluetoothd は中継だけで message の中身を disk に書かない、保存は Phone の app の `~/Documents/Phone/`（p004）。
- Q4 最初の同期は過去 30 日、folder ごとに最大 500 通。Q5 MMS は範囲外。Q12 OBEX の認証は使わない。
- Q13 (a) Android で先に確かめ、iPhone の MAP は p006 の後。Q14 持ち主の logout で profile を切り、seat に戻ったら再接続。
- Q16 (a) MAP 1.1 を名乗り GOEP 1.1（RFCOMM）だけ。MapSupportedFeatures の交換、Extended Event Report 1.1、UTC Offset の形式、Persistent Message Handles は使わない。

## 0. 出典

- MAP 1.4.2 の PDF（2026-10-10 P1 が読んだ。複写は repository に入れない）。MAP 1.1 の本文は手元に無い。1.4.2 §2.1 は 1.1 の相手との後方互換を求め、1.1 の MCE が使う形（1.0 の listing・event・bMessage）は 1.4.2 の本文に「Version 1.0」として全部ある。この文書の節番号は MAP 1.4.2 の物。
- Core 5.4（Vol 3 Part B §5.1 の SDP の属性、Vol 4 Part E §7.1 の HCI の command）。SDP の属性 ID のうち ServiceClassIDList 0x0001、ProtocolDescriptorList 0x0004、BrowseGroupList 0x0005、LanguageBaseAttributeIDList 0x0006、BluetoothProfileDescriptorList 0x0009、ServiceName（言語の base 0x0100 + offset 0x0000）は Core §5.1 で確かめた [m19]。
- Bluetooth Assigned Numbers の値（MAS 0x1132、MNS 0x1133、MAP の profile 0x1134、MASInstanceID 0x0315、SupportedMessageTypes 0x0316、MapSupportedFeatures 0x0317、OBEX 0x0008、RFCOMM 0x0003、L2CAP 0x0100）は手元に文書が無いので「(Assigned Numbers、確かめる)」。実機の SDP の dump（p008）で照合する。p001 §4.4 と同じ扱い。
- 他の OS の実装（BlueZ・Android・Apple）の code は読まない。

## 1. 範囲

この Phase で作る物（p001 §12 の p003 の行）:

1. 持ち主の記録 `.phone`（§3）と、`PAIR … phone=1` の handoff での書き込み、PAIR と FORGET の規則、`PHONE LINK`・`PHONE SHOW`。
2. 全部の socket の client の出力の queue（`outq.c`）、client の枠 16 と予約と世代、`PHONE SEND` の長さ付きの入力（§4）。
3. phone link の一生: 持ち主の在・不在（seat）、zedBSD からの page と backoff、スマホからの接続の受けと交差、認証・暗号化・鍵の長さ、切断の理由ごとの扱い、suspend の後（§5）。p002 の試しの `PHONE PROBE` は MAP に置き換えて消す。
4. MAP の XML（listing と event）と時刻（§6）、bMessage（§7）。
5. MAP の MAS の client（接続、folder、通知の登録、操作の queue、同期の page、既読、送信、失敗からの回復）と MNS の server（SDP の record、event）（§8）。
6. socket の PHONE の request と event、`PHONE SUBSCRIBE`（F-086 の一部）（§9）。
7. host の試験と fuzz、T1 への HID の回帰の依頼（§10）。

作らない物: Phone の app・compositor・libkeiland の API（p004）、PBAP（p005）、HFP（p006・p007）、通知の banner（p004）。MMS・EMAIL・IM（Q5）。送った SMS を app の保存と重ねる規則は p004（§9.6）。`PHONE GET`（1 通の取り直し）は作らない [M9]。

### 1.1 p001・p002 からの変更 [m21]

| 元 | 変更 | 理由 |
| --- | --- | --- |
| p001 §8.2.1 suspend に入る時に profile を閉じる | 何もしない。sleep.end で backoff を戻す（§5.6） | sleep.begin の後に user space が動く保証が無い |
| p001 §8.3 本文 64 KB | 受けの本文 16 KB（越えたら切る）、送信 8192 byte | SMS の連結でも数 KB。outq の上限と合わせる |
| p001 §8.3 `PHONE PAGE … after=` | `cursor=`（§8.5） | listing の offset と MAP の session を持つ |
| p001 §8.3 event の `source=` | `handle=`（session 付き）と `key=`（§8.3） | handle は session ごとに変わる |
| p001 §8.3 `PHONE GET` | 作らない | 1.1 では handle から時刻・folder を引けない [M9] |
| p001 §8.2 「持ち主の無い bond は phone=1 で pairing し直す（スマホがもう一度許可を求める）」 | 他人の記録のある address の PAIR は断る（§3.2） | 今の pair.c は保存の認証済みの鍵を使い、スマホに何も出ない [B1]。phone=1 の pairing は保存の鍵を使わない（§3.2）[N2] |
| p001 §8.2「持ち主＝PAIR の client」 | seat の人の PAIR だけが phone=1（§3.2） | SSH の root・wheel の pairing で持ち主が永久に不在になる [M4] |
| p002 §11・§14 の `PHONE PROBE` と「p008 で PROBE」 | 消す。p008 は MAP の操作で確かめる | MAP が同じ道を使う |
| p002 の `phone_ended` が HID の上限を 6 に戻す | 上限は記録の状態だけで決める（§3.4） | [M12] |

p004・p008 の行（WS197 の ws.md の表と p001 §12）の書き換えは Q1 に頼む。

## 2. 部品と file

| file | 新・変 | 内容 | i |
| --- | --- | --- | --- |
| `phonerec.c`・`.h` | 新 | `.phone` の記録の形・読み・書き・消し・探し・掃除（§3） | i01 |
| `outq.c`・`.h` | 新 | client ごとの出力の queue（§4） | i02 |
| `phone.c`・`.h` | 変 | 記録と持ち主、在・不在、page と受け、認証、DLC の振り分け、MAP の配線、PROBE を消す（§3・§5） | i01・i03・i06 |
| `mapxml.c`・`.h` | 新 | listing と event の XML、datetime（§6） | i04 |
| `bmsg.c`・`.h` | 新 | bMessage の読みと組み立て（§7） | i05 |
| `map.c`・`.h` | 新 | MAS の client、MNS の server、操作の queue（§8） | i06 |
| `phoneio.c`・`.h` | 新 | PHONE の行の読みと書き（escape、key=value、item の行）（§9） | i07 |
| `obex.c`・`.h` | 変 | 応答の各 packet の header を渡す `response` の hook、操作ごとの答えの時間（§8.2） | i06 |
| `pair.c` | 変 | PAIR の始めの検査の hook（他人の記録、§3.2） | i01 |
| `main.c`・`protocol.h`・`Makefile` | 変 | client の枠・世代・出力の queue、長さ付きの入力、seat の見直し、PHONE の request、PAIR・FORGET の規則、sleep.end | i01・i02・i03・i07 |

system call を持つのは `phonerec.c`・`keys.c`（file）と `main.c`。phone.c は keys の folder を受け取り、`btd_phonerec_*` と `btd_keys_read`（Link Key Request の答え、`hid_key_request` と同じ）を呼ぶ（hid.c と同じ形。host の試験は一時の folder を渡す）。account の名前（`getpwuid`）と seat は main が hook で渡す [M2]。

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

- 書きは一時 file・fsync・rename・folder の fsync（`hidcache.c` と同じ手順）。読みは全部の行が 1 回ずつあり値が範囲内の時だけ受け、他は file ごと**壊れた記録**（log）。
- `user` は uid の account の名前（1〜63 byte、`[A-Za-z0-9._-]`）。合わない名前の account は phone=1 を断る（`why=store`）[m13]。
- **有効な記録** ＝ 読めて、bond の file があり、bond の鍵が認証済みの型（0x05・0x08）で、uid の account があり名前が `user`。他は**無効**。
- 探し: `btd_phonerec_find(folder, controller, &record, &count)` は controller の folder の全部の `.phone` を読み、**有効な物**の数と最初の 1 つを返す。
- 掃除: controller を開いた時（`btd_open` の後、main が `btd_phone_load` を呼ぶ）、bond の無い `.phone` を消す（`hidcache_prune` と同じ）。有効な記録が 2 つ以上あれば全部を使わず log（壊れた状態、FORGET で直す）[M3]。

### 3.2 PAIR・handoff・LINK・FORGET [B1, M2, M3, M4, m13, m14]

| 出来事 | 規則 |
| --- | --- |
| `PAIR ADDRESS TYPE`（phone の有無に関わらず）の始め | その address に**有効な記録があり、その uid が client と違い、client が root でない**なら `ERROR owned`（pairing を始めない。他人が同じスマホの bond を作り直して鍵の型を変える事も防ぐ）。pair.c の始めに main が検査の hook を渡す（`btd_pair_set_check`、新） |
| `PAIR … phone=1` の始め | client の uid が seat の人（§3.3 の判定）でなければ `ERROR phone-seat`。root も同じ（持ち主は seat の人だけ）。**phone=1 の pairing の Link Key Request には保存の鍵があっても常に Negative Reply**（p002 §7.1 [m9] の「認証済みでない時だけ」を広げる）: SSP の数値比較をやり直させ、スマホの画面に確認を出す。持ち主の無い bond・無効な記録の bond を黙って取る道を閉じる [N2] |
| handoff で phone が受ける（p002 §7.4 の 1〜5 の後） | 6) 別の address の**有効な**記録があれば受けない（`why=other-phone`）。7) 同じ address の有効な記録の uid が違えば受けない（`why=owned`。始めの検査の後に記録が変わった時の守り）。8) 記録を書く（同じ uid の記録があれば profile を保ち enabled は 1（phone=1 で pairing し直した意図は「使う」）[n8]、無ければ全部 1、enabled 1）。書けなければ受けない（`why=store`）。9) 無効な他の `.phone` を消す。**記録を書いてから** route を移す（今の `btd_router_assign` の前）[M2] |
| `PHONE LINK ADDRESS on|off [profiles=m,c,h]` | 有効な記録の持ち主と root。enabled と profile を書き換える。記録の無い address は `ERROR not-phone`（今までの普通の bond は取れない） |
| `FORGET ADDRESS bredr` | `.phone` があり**有効**なら持ち主と root だけ（p001 N2: D8 の例外）、無効なら D8 の人。`.phone` を先に消し、次に bond（今の規則）。bond が無くても `.phone` を消せたら DONE。`.phone` の消しの失敗は `ERROR <理由>` で bond に触れない。つないでいれば切る |

phone.c は記録を自分で持つ: `btd_phone_load(phone)`（controller を開いた時、§3.1 の探しと掃除）、`btd_phone_link_set(phone, address, uid, enabled, profiles)`、`btd_phone_forget(phone, address, uid)`。account の照合は hook `account(context, uid, name, size)`（main が `getpwuid`）。

### 3.3 持ち主の在・不在（p001 §8.2、S10、S21）

- **在** ＝ 有効な記録で enabled、かつ seat の人（`/dev/gpu0` の持ち主、`_greeter` を除く。今の `btd_permitted` の判定を関数 `btd_seat_uid` に分ける）の uid が記録の uid。
- main が 5 秒ごと（記録がある間だけ）と、持ち主の SUBSCRIBE の接続が切れた時と、POWER の切り替えの時に `btd_phone_set_present(phone, present, now)` を呼ぶ。**POWER off の間は不在**として渡す [m15]。
- 在 → 不在: MAP を閉じ（DLC を閉じる）、ACL を切る（Q14）。page と page scan の要求を下ろす。自動の page の止め（§5.5）も解く。
- 不在 → 在: backoff を戻してすぐ page（§5.2）。

### 3.4 HID の上限 [M12]

- 有効な記録が enabled の間（在・不在に関わらず）HID の同時の接続は 5（`btd_hid_set_limit`）。記録が無い・無効・enabled 0 なら 6。上限は `btd_phone_load`・`link_set`・`forget`・handoff で記録が変わった時だけ決め、`phone_ended` は変えない（p002 の `phone.c:1103` の行を消す）。
- 上限を 5 にした時に HID が既に 6 台つないでいれば、HID は切らない（使用中の機器を落とさない）。phone は `btd_hid_link_count ≤ 5` の時だけ page と Accept を行い、他は `why=busy-links` で待つ（間隔の段は進めない。HID の 1 台が切れたら次の tick で page）。

## 4. socket の出力の queue と入力（p001 §8.3、S9、S20）

### 4.1 出力の queue（`outq.c`）

- client ごとの byte の queue（malloc で伸ばす、上限 `BTD_OUTQ_MAX` 256 KB）。`btd_write` は書式の文字列を queue の後ろに足し、すぐ `send` を 1 回試す（non-blocking、EAGAIN は残す）。今の「1 秒まで poll で待つ」は消す（読まない client が daemon の loop を止めない）。
- 今の `btd_write` の書式の buffer（`BTD_LINE_MAX + 4×BTD_NAME_MAX`、`main.c:1969`）より長い行（phoneio の item の行）は、書式を通さず `btd_outq_append` で直に足す [m4]。
- loop の poll は queue に物がある client に POLLOUT を足し、書ける時に送る。flush の後に `btd_map_pump`（§8.4 の room の待ちを起こす）[m16]。
- 上限を越える追加は client を `dead` にする（今の「読まない client は閉じる」と同じ）。**例外: SUBSCRIBE の接続**（§9.4）。
- `btd_client_close` は queue を free する。閉じる前に 1 回だけ flush を試す（`ERROR length` などを届けるため。届かなくてもよい）[m17, m18]。
- 純粋な関数（append、`btd_outq_flush(q, send_fn, ctx)`、room、clear）にし、send を差し替えて host で試す。

### 4.2 client の枠と世代 [B2]

- `BTD_CLIENTS_MAX` 8 → 16。空きが 4 以下の時は root と seat の人の uid の接続だけを受ける（compositor の分の予約、p001 S9）。poll の表は `4 + 16` に。
- client に**世代**（`btd_accept` ごとに 1 増やす 32 bit）を持たせる。map などへ渡す答えの宛先は **token ＝（枠, 世代）**。答えの hook は世代が合わなければ何もしない。`btd_client_close` は `btd_phone_cancel(phone, token)` を呼び、map の queue のその token の操作を消す（§8.4）。

### 4.3 長さ付きの入力（`PHONE SEND`）[M11]

- 行の読みで、`PHONE SEND` の行の**文法**（`to="…"` と `length=N`、N は 1〜8192 の 10 進）が正しければ、client はまず「N byte を読む」mode に入る。行の loop はそこで抜け、同じ `recv` の buffer の残りを本文の buffer（malloc、SEND の間だけ）へ移す。N byte が揃ってから request を行い、その時点で ERROR（not-ready、no-send、permission、number、待ちの間の行など）なら本文を捨てて答える。**本文の byte が要求の行として読まれる事は無い**。
- 文法の誤り（length が無い・範囲外・数字でない）は `ERROR length` を書いて client を閉じる（流れの区切りが分からないため）。
- 待ちの request（PAGE・READ・SEND）の間に来た `PHONE SEND` の行も、文法が正しければ N byte を読み捨ててから捨てる。

## 5. phone link の一生（p001 §8.2、§8.2.1、§3.4）

### 5.1 状態

| 状態 | 意味 | claims（router） |
| --- | --- | --- |
| NONE | link 無し | 0 |
| PAGING | zedBSD が Create Connection を出した | 記録の address なら 1 |
| CANCELLING | PAGING の守りの切れで Create Connection Cancel を出し、page の Connection Complete を待つ | 同 |
| ACCEPTING | スマホの Connection Request に Accept を出した（交差なら自分の page の Cancel も） | 同 |
| SECURING | Connection Complete の後、認証・暗号化・鍵の長さの確かめ（§5.4 の段） | 同 |
| READY | 使える（p002 の handoff の直後もここ） | 同 |
| CLOSING | Disconnect を出した | 同 |

phone は `page_outstanding`（自分の Create Connection の Connection Complete がまだ来ていない）を持つ。

### 5.2 zedBSD からの page と backoff [M6, m15]

- page する条件: 在（POWER on を含む、§3.3）、state NONE、自動の page を止めていない（§5.5）、HID の接続が 5 以下（§3.4）、controller が READY、pairing・scan の最中でない（main が HID と同じ `btd_hid_holding` の条件で phone の tick の page を止める）。
- 間隔: 最初はすぐ、page の失敗・短い link（§5.5）のたびに 30 s・60 s・120 s・240 s・480 s・600 s（以後 600 s）。**戻すのは**、link が READY で 2 分続いた時、MAP が ready になった時、不在 → 在、sleep.end、`PHONE LINK on`。READY になっただけでは戻さない。
- 手順: `btd_linkmgr_page_begin(lm, BTD_LINKMGR_PHONE, address, now)`（EBUSY なら 2 s 後、段は進めない）→ Create Connection（0x0405: address、packet type 0xCC18（HID の page と同じ、`hid.c:2427`）、page scan repetition mode R1、clock offset 0、Allow Role Switch 0x01）→ PAGING、`page_outstanding = 1`（12 s の守り。linkmgr の 15 s の満了より先に、CANCELLING の間に token が外れないため）[n3]。
- Command Status の失敗: NONE、`page_end`、次の段。Connection Complete の失敗（`page_outstanding` を 0 に）: NONE、次の段（page の終わりは router が `btd_linkmgr_connected` で行う）。
- **守りの切れ** [B3, n3]: PAGING で 12 s → Create Connection Cancel（0x0408、address）→ CANCELLING。command は同期で、待つ間の事象は queue に入って command の後に配られる（`session.c`）ので、Cancel の Command Complete の status（0x00・0x02・0x0B）は log だけにし、CANCELLING を出るのは Connection Complete だけ: 失敗なら NONE と次の段、成功なら（止める印が無ければ）SECURING へ進める（つながった link を捨てない）。CANCELLING も 5 s で NONE（log）。
- **止める**（在 → 不在、FORGET、`LINK off`）[n5]: PAGING は Cancel して CANCELLING に「止める」印、ACCEPTING・CANCELLING は印だけ立てる。印のある状態の成功の Connection Complete は Disconnect（0x16 の理由で page しない）。SECURING・READY は `phone_disconnect`。
- page scan: 在の間 `btd_linkmgr_want_scan(lm, BTD_LINKMGR_PHONE, 1)`、他は 0（自動の page を止めている間も、スマホからの接続は受けるので 1）。

### 5.3 スマホからの接続と交差 [B3, M1]

- `btd_phone_wants(address)`: 在で address が記録の物なら、**state に関わらず** 1。router は Connection Request と Link Key Request の両方でこれを聞く（HID の wants の後）。READY の link でスマホが認証をやり直す時の Link Key Request も phone が答える（HID の wants が IDLE 以外で 1 なのと同じ、`hid.c:974-976`）。
- Connection Request（router が phone へ渡す、link type ACL だけ）:
  - NONE: HID の接続が 6 台なら Reject（下の吸い込み）[n5]。他は Accept Connection Request（0x0409、role 0x01 = peripheral のまま。スマホは他の機器の central であることが多く role switch の失敗で接続を落とさない。**未確認**: p008）→ ACCEPTING（15 s の守り。切れたら NONE と log。後で来る成功の Connection Complete は claims 0 で router が切る）。
  - PAGING（交差）[n4]: 先に自分の page の Create Connection Cancel（0x0408、同期で Command Complete まで数 ms）。その status が 0x0B（page の接続が既にでき Connection Complete は queue の中）なら Connection Request は Reject（下の吸い込み）して PAGING のまま（queue の成功の Connection Complete で SECURING）。他なら Accept（role 0x01）→ ACCEPTING（`page_outstanding` は 1 のまま）。Core 7.1.7: Cancel の時に baseband が既に接続を作っていれば controller が切って Success、page の Connection Complete は失敗（0x02 Unknown Connection Identifier）で来る。Cancel が受けた接続まで切る controller があるかは推測（p008）。
  - CANCELLING: Accept（Cancel は出し直さない）→ ACCEPTING（`page_outstanding` は 1 のまま）[n2]。
  - 他（ACCEPTING・SECURING・READY・CLOSING）: Reject Connection Request（0x040A、reason 0x0D）。
  - **Reject の吸い込み** [n1]: Core 7.1.9 で Reject の後にローカルの controller も Connection Complete（失敗）を出す。その status は Core が定めない（Reason が入るのは相手の Connection Complete）。phone は `reject_pending`（address と時刻）を覚え、その address の失敗の Connection Complete を status に関わらず 3 s の間に 1 回だけ吸う（state を変えない）。
- ACCEPTING の Connection Complete の振り分け: 成功 → SECURING（その handle）。失敗で `page_outstanding` が 1 かつ status が 0x02・0x04（Page Timeout）・0x0B のどれか → 自分の page の終わり（`page_outstanding = 0`、ACCEPTING のまま）。他の失敗 → Accept の失敗で NONE と次の段。SECURING 以後に来た失敗の Connection Complete で `page_outstanding` が 1 → page の終わりとして吸う。
- 交差の残る窓: router は最初に来たその address の Connection Complete で linkmgr の token を終える（`router.c:462-464`）ので、Cancel した page の Connection Complete がまだの間に HID が page を始めると Command Disallowed になり得る。HID はそれを unreachable として再試行する（p002 §6.2 の「調停しない事」と同じ扱い、窓は数 ms）。記録だけ。
- 同じ形の潜在の誤り（PAGING 中の Connection Request を Reject し、ローカルの Connection Complete を page の失敗と見る）は HID の `hid_request`（`hid.c:1259-1266`）にもある。WS143 の範囲なので Q1 に報告する（Bug にするかは Q1）。
- Link Key Request: 有効な記録で、bond の鍵が認証済みの型なら Link Key Request Reply、他は Negative Reply（鍵は読んだ後に 0 で消す、`hid_key_request` と同じ）。

### 5.4 認証・暗号化・鍵の長さ（SECURING）[M5]

Connection Complete（成功）→ route を PHONE に（`btd_router_assign`）、session の link の上限（p002 §4.2）、L2CAP の表を初期化して accept の hook、SECURING（全体 10 s の守り）。段:

| 段 | 入り | 出 |
| --- | --- | --- |
| WAIT_PEER（受けた接続だけ） | Connection Complete の後 | Encryption Change（on）→ KEY_SIZE。3 s 待っても来なければ AUTH |
| AUTH（page した接続はすぐここ） | Authentication Requested（0x0411）を出す | Authentication Complete 成功 → 暗号化が off なら ENCRYPT、on なら KEY_SIZE。0x23・0x2A（LMP の衝突）は 200 ms 後に 1 回やり直す。Command Status 0x0C（Command Disallowed: スマホが先に始めた）は WAIT_PEER に戻る。0x06（Key Missing）は `why=key-missing` で切る。他は `why=security` で切る |
| ENCRYPT | Set Connection Encryption（0x0413、on） | Encryption Change（on）→ KEY_SIZE、失敗 → 切る |
| KEY_SIZE | Read Encryption Key Size（0x1408） | 16 → READY、他 → `why=key-size` で切る |

- どの段でも Encryption Change（成功・on）が来たら KEY_SIZE へ進む（暗号化は phone が Link Key Request に答えた認証済みの bond の鍵で行われる）。暗号化が on になった後の Authentication Complete の失敗は捨てる。
- READY: Pending の channel（スマホが先に開けた SDP・RFCOMM）を成功で答え、MAP を始める（§8.1、messages の profile が on の時）。
- Key Missing（スマホが bond を消した）: 自動の page を止める（§5.5、スマホの側で pairing をやり直すまで繰り返しても無駄）。STATE `why=key-missing`。
- SECURING・ACCEPTING・CANCELLING でも `phone_disconnect` が handle を持つ時は Disconnect を出せるようにする（今は READY だけ、`phone.c:1053`）[B3]。
- p002 の handoff の link は暗号化と鍵 16 を確かめてあるので直接 READY（今のまま）。記録は handoff の中で先に書く（§3.2）。在なら MAP を始め、不在なら切る。

### 5.5 切断の理由と自動の page [M6]

| Disconnection Complete の reason | 扱い |
| --- | --- |
| 0x08 Connection Timeout（離れた） | 次の段の間隔で page |
| 0x15 Remote Device Terminated due to Power Off | 0x08 と同じ（スマホの再起動・電源の後に戻る）[N1] |
| 0x13 Remote User Terminated | 段を 1 つ進める（600 s まで）。**READY の後 2 分以内の 0x13 が 3 回続いた時だけ自動の page を止め**、STATE `why=peer-closed`（スマホの利用者が「切断」を押した、と見る）。止めを解くのは不在 → 在、sleep.end、`PHONE LINK on`。MAP は MCE（zedBSD）からしか始まらないので、止めると次の合図まで SMS が届かない。だから 1 回では止めない [N1]。スマホからの接続はいつでも受ける。スマホが出す reason の値は推測（p008） |
| 0x16 Local Host Terminated（自分で切った） | 切った理由のまま（不在・FORGET・LINK off は page しない、key-size・security は 600 s） |
| 他 | 次の段の間隔で page |

- READY の後 2 分より前に切れた link は「短い link」として段を 1 つ進める（MAP の許可が無く profile の無い ACL をスマホが切る場面で、すぐの page を繰り返さない）。
- Disconnection Complete・ACL の喪失・drop の知らせの回復（p002 §3.5）は今のまま。加えて MAP を失敗にし（§8.6）、NONE に戻す。

### 5.6 suspend と controller の喪失

- sleep.end（今の `btd_system_events`）: main が `btd_phone_resume(phone, now)` を呼び、backoff を戻し自動の page の止めを解く。生きている link には何もしない（死んだ link は supervision の timeout で Disconnection Complete が来る）。sleep.begin では何もしない（§1.1）。
- controller の喪失（`btd_phone_lost`）: 今のまま NONE。controller が戻った時に `btd_phone_load`。
- 通話中の suspend の扱いは範囲外（Q15、p006）。

### 5.7 PROBE を消す

p002 の `PHONE PROBE` と `struct btd_phone_probe`・`phone_probe_*` は消し、SDP の問い合わせ・RFCOMM の session・DLC の開けは §5.8 の一般の形にする。`bt-phone-link-host-test` の PROBE の場面は §10 の場面に置き換える（試験の直しは見つけた担当、AGENTS.md）。

### 5.8 phone.c と map.c の境

- SDP の問い合わせ: `phone_sdp_query(phone, uuid)` → 結果を `btd_map_sdp_done(map, sdp, error)` へ（1 度に 1 つ。p005・p006 も同じ口を使う）。
- RFCOMM: phone は session と DLC の表（dlci → 使い手 `PHONE_DLC_MAS`・`PHONE_DLC_MNS`）を持ち、`opened`・`data`・`writable`・`closed` を使い手の map の関数へ渡す。session を作るたびに（どちらが開けても）`btd_rfcomm_listen(rf, 16)` を呼ぶ（`btd_rfcomm_init` が server の表を空にするため）[m9]。`accept(16)` は MAP が CONNECTING 以降の時だけ 1。
- map から phone へは hook（`struct btd_map_hooks`）: `dlc_open(channel)`、`dlc_write`、`dlc_close`、`sdp_query(uuid)`、`emit(line, bytes, length)`（SUBSCRIBE の client へ）、`answer(token, line, bytes, length)`、`room(token)`（token の client の queue の空き、世代が合わなければ 0 でなく「居ない」）、`local_offset(now)`（zedBSD の UTC からのずれ、秒）。試験は hook を差し替える。
- `room` の待ちは loop の poll を 0 の timeout にしない（`btd_timeout` に入れない。outq の flush の後の `btd_map_pump` で起こす）。

## 6. XML と時刻（`mapxml.c`、p001 §7.1.1・§7.1.2、R18）

### 6.1 読む形

- listing（MAP §3.1.6.1、1.0）: root `MAP-msg-listing`、子 `msg` の空要素の列。event（§3.1.7.1、1.0）: root `MAP-event-report`、子 `event` 1 つ。version の属性は読み、`1.0` 以外でも続ける（知らない属性は無視、§3.1.6・§3.1.7 の「shall ignore」）。
- 字句: 先頭の UTF-8 の BOM、`<?xml …?>`、comment `<!-- … -->`、内部 subset の無い `<!DOCTYPE …>`（`[` があれば失敗）を飛ばす。要素の名前と属性の名前は `[A-Za-z_:][A-Za-z0-9_.:-]*`。`:` を含む属性は知らない属性として無視し、`:` を含む要素の名前（root と子）は失敗 [m11]。`=` の前後の空白を許す（§3.1.6.1 の例は `handle = "…"`）。値は `"` か `'`。実体は `&lt; &gt; &amp; &quot; &apos;` と `&#N;`・`&#xH;`（U+0000・surrogate・0x10FFFF 越えは失敗）を UTF-8 に。CDATA・処理命令（宣言以外）・`msg` の中身（入れ子の要素や文字）は失敗。
- 上限: 属性 32/要素、値 1024 byte（解いた後）、`msg` 1024 個、入力 64 KB。超えたら失敗（listing 全体）。
- 結果: 呼び手の配列に `struct btd_map_entry`（handle の 64 bit、datetime の文字列 24 byte（`YYYYMMDDTHHMMSS±hhmm` と NUL に余裕）[m1]、sender_name・sender_addressing・recipient_name・recipient_addressing（各 256 byte で切る）、type（SMS_GSM・SMS_CDMA・MMS・EMAIL・IM・他）、read、reception_status）。handle が無い・16 桁を越える・16 進でない `msg` は 1 件だけ捨てて数える（listing は続ける）。
- event: `struct btd_map_event`（type の名前（NewMessage・DeliverySuccess・SendingSuccess・DeliveryFailure・SendingFailure・MemoryFull・MemoryAvailable・MessageDeleted・MessageShift・他）、handle、folder、old_folder、msg_type）。
- 順: listing は新しい順と §3.1.6・§5.5.4.1 は言うが、§3.1.6.1 の例は古い順に並ぶ。map は順に頼らない（page は offset だけ、§8.5）。実機の順は p008 で見る。
- fuzz の対象（固定の seed、20 万回）。

### 6.2 datetime（MAP §3.1.6 の datetime、OBEX の time の形）[m1, m2, m20]

- 読み: `YYYYMMDDTHHMMSS` と、任意の `±hhmm`（MAP 1.4.2 §3.1.10 の形）か `Z`（UTC、+0000 と同じ）。末尾の NUL は許す。範囲（月 1〜12、日はその月の日数、時 0〜23、分 0〜59、秒 0〜60）外は失敗。
- 読めない datetime の entry は、時刻を受けた時の zedBSD の時刻にし（`zone=received`）、key を作らない（§8.3）。
- UNIX 秒へ: 文字列の offset、無ければ MSETime の offset（listing の答えの App Parameters 0x19、§5.5.4.12。1.1 の相手が付けるかは未確認）、無ければ zedBSD の `local_offset`（スマホと同じ地域と仮定、p001 §7.1.2）。どれを使ったかを item の `zone=phone|mse|local|received` で残す。暦の計算は純粋な関数（days-from-civil）。
- FilterPeriodBegin の書き: UNIX 秒をスマホの local time（上と同じ順で選んだ offset）で `YYYYMMDDTHHMMSS`（offset と NUL を付けない。1.1 の形）。
- 試験の正解: 手で計算した値（`20071213T130510+0900` → 1197518710、`20240229T235960` の閏日と 60 秒、`19700101T000000Z` → 0）を試験に書く。

## 7. bMessage（`bmsg.c`、MAP §3.1.3、p001 §7.1.1、R9）

### 7.1 読み（GetMessage の答え）

- 行は CRLF（LF だけも受ける）。構造の行は前後の空白を除いて比べる（§3.1.3 の例は字下げしてある）。`BEGIN:BMSG` で始まり `END:BMSG` で終わる。入れ子（BENV）は 3 段まで（§3.1.3）、4 段目は失敗。
- 読む物: `STATUS`（READ・UNREAD）、`TYPE`、`FOLDER`、originator の vCard（BENV の外の最初の BEGIN:VCARD〜END:VCARD）の `TEL`・`N`・`FN`、**一番外の** BENV の最初の recipient の vCard の `TEL`・`N`・`FN`（MAP の図 3.1: 外側が最終の受け手。SMS は 1 段）[m12]、BBODY の `CHARSET`・`ENCODING`・`LENGTH`、最初の `BEGIN:MSG` の中身。vCard の行の parameter（`TEL;TYPE=CELL:`）は `:` の前を飛ばす。2.1 の quoted-printable の名前は解かない（名前は listing の sender_name を先に使う。vcard.c は p005）。
- **本文の範囲**（LENGTH、§3.1.3: 最初の `BEGIN:MSG` の `B` から最後の `END:MSG<CRLF>` の CRLF まで）: 次の順に試し、最初に合った物を使う（`form=` として数える）。
  1. 仕様: `start = BEGIN:MSG の位置`、`end = start + LENGTH`。`end` の直前が `END:MSG\r\n`（LF だけの入力では `END:MSG\n`）。
  2. 本文だけ: `BEGIN:MSG` の行の後から LENGTH byte の直後が `\r\nEND:MSG` か `END:MSG`。
  3. 最後の CRLF を数えない: `start + LENGTH` の直前が `END:MSG`。
  4. どれも合わない: `BEGIN:MSG` の後の最初の、行頭の `END:MSG` の行まで（log に `form=scan`）。
- 本文 ＝ `BEGIN:MSG` の行の後から `END:MSG` の行の前の改行の前まで。行頭の `/END:MSG` は `/` を 1 つ除く（`//END:MSG` → `/END:MSG`、§3.1.3）。
- 本文の charset: CHARSET が UTF-8 か無い（Charset=UTF-8 を頼んだ時の SMS の形 1）時だけ受け、UTF-8 として検査（不正な列は U+FFFD）。ENCODING があり CHARSET が無い（PDU の native の形）は失敗（頼んでいない形、数える）。
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
- 本文の行頭の `END:MSG`（と `/END:MSG`…）は `/` を 1 つ足す（§3.1.3）。本文の改行は CRLF に揃える。
- LENGTH ＝ `BEGIN:MSG\r\n` ＋ 本文 ＋ `\r\nEND:MSG\r\n` の byte 数（形 1）。
- 番号は `[0-9+*#]`、1〜32 文字。他は `ERROR number`。本文は UTF-8 で 1〜8192 byte、NUL を含まない。
- 試験: 手で書いた byte の列（BNF から、LENGTH を手で数えた値）と一致。読みに通して戻る（往復は補い。手の正解が主）。

## 8. MAP の MCE（`map.c`）

### 8.1 始め（READY かつ messages が on、§6.4.3）

1. MNS の SDP の record（§8.7）は phone link が READY の間ずっと登録してある（§8.7）。
2. SDP で MAS（0x1132）を問い合わせ、record を順に見て SupportedMessageTypes（0x0316）が SMS_GSM（bit 1）か SMS_CDMA（bit 2）を持つ最初の物を選ぶ（p001 §4.2）。RFCOMM の channel、MASInstanceID（0x0315）、MapSupportedFeatures（0x0317。無ければ 0x0000001F、§7.1.1 の注 1）を持つ。無ければ `why=no-mas`（やり直さない）。
3. RFCOMM の DLC（MAS の channel）→ OBEX Connect（Target MAS、App Parameters 無し、Q16 (a)）。**Connect の答えの時間は 60 s**（スマホが許可の確認を出している間に待たせる機種のため、推測）[M7]。答えが 0xC1・0xC3 なら `why=permission`、他の失敗は `why=refused`。
4. SetPath を 2 回: `telecom`、`msg`（flags 0x02 = 作らない、§5.3 表 5.7）。以後 folder は `telecom/msg` に留め、listing・送信は子の folder の名前で指す（§5.5.2・§5.8.2）。既読は handle で指す（handle は MSE の全体で一意、§3.1.1）。
5. 通知: MapSupportedFeatures に bit 0（通知の登録）と bit 1（通知）があれば Put `x-bt/MAP-NotificationRegistration`（App Parameters NotificationStatus 0x0E = 1、End of Body 0x30、§5.2）。答えの後、スマホが MNS に接続してくる（§8.7）。無ければ通知無しで続け、STATE に `notify=0`。
6. READY: STATE `messages=ready send=<1 if bit 3> notify=<1 if MNS が開いた>`。送信は bit 3（Uploading）がある時だけ受ける。

### 8.1.1 失敗からのやり直し [M7]

- 一時の失敗（timeout、DLC の切断、0xD3 Service Unavailable、`refused`）: 同じ link の上で 30 s・60 s・120 s…最大 600 s の間隔で §8.1 の 2 からやり直す。
- 許可の拒否（`permission`）: 600 s ごとに 1 回やり直す（利用者がスマホで「許可」を押した後に拾う）。`PHONE LINK on` でもすぐやり直す。
- `no-mas`: やり直さない（次の link で）。
- MAP が ready になったら間隔を戻す（§5.2 の backoff も戻す）。
- MNS の DLC だけが閉じた時（MAS は生きている）: MAP §4.1 で MSE の通知は off になるので、NotificationRegistration（on）を 1 回出し直す。MNS が 30 s で戻らなければ STATE `notify=0`（受信は次の同期で拾う）。

### 8.2 OBEX の変更

- MAP §6.3.2: 何 packet かに分かれた Get の答えは、Body 以外の header を最初の packet に置く（listing の ListingSize・MSETime）。今の `done` は最後の答えの header だけを渡す（`obex.c:1215-1216`）ので、`struct btd_obex_events` に `response(context, operation, code, headers, length)` を足し、client の各答えの packet ごとに（Body を含む header の列のまま）呼ぶ（NULL なら呼ばない。p002 の使い手は NULL のまま）。
- `btd_obex_set_timeout(ob, ms)`（新）: 次の操作から答えの時間を変える（既定 10 s のまま、MAS の Connect だけ 60 s）。

### 8.3 MAP の handle とセッションと key [M9, m8]

- handle は MAP の session の間だけ有効（§3.1.1: Persistent Message Handles の無い時）。MAS の接続ごとに `session`（daemon の起動時の 32 bit の乱数（main の `btd_random`）から 1 ずつ増やす）を付け、socket には `handle=<session 8 桁の 16 進>.<handle 16 桁の 16 進>` で出す。READ の handle が今の session のでなければ `ERROR stale`。
- 重複の鍵 `key`（app の Source、p001 §8.6）: 64 bit の FNV-1a（dir の 1 文字、`|`、スマホの datetime の文字列（変換の前の、listing の値のまま）、`|`、相手の番号（`[0-9+*#]` 以外を除いた物、出どころは listing の addressing を先に、無ければ vCard の TEL）、`|`、本文）の 16 桁の 16 進。handle を含めない（session で変わる）。
- **listing の datetime の無い item**（§8.6 の LOCATE で見つからない、§6.2 の読めない datetime）は `key=-` と `partial=1` を出す。app は peer・本文・時刻の幅で重ねる（§9.6）。

### 8.4 操作の queue（p001 §7.1.0）[B2, m5, m16]

MAS の OBEX は 1 度に 1 つの操作。全部を 1 本の queue（24 個まで）で順に行う。

| 操作 | OBEX | 使い道 |
| --- | --- | --- |
| COUNT | Get `x-bt/MAP-msg-listing`、Name＝folder、MaxListCount 0x01 = 0、LIST と同じ filter（FilterMessageType・FilterPeriodBegin）（§5.5.4.1・§5.5.4.13） | その folder の ListingSize（filter の後の件数）と MSETime |
| LIST | Get 同、Name＝folder、MaxListCount＝n、ListStartOffset 0x02＝o、FilterMessageType 0x03 = 0x0C（EMAIL と MMS を除く。IM の bit 4 は 1.1 の相手には予約なので立てない）、FilterPeriodBegin 0x04（§6.2）、ParameterMask 0x10 = 0x0000117E（bit 1 datetime・2 sender_name・3 sender_addressing・4 recipient_name・5 recipient_addressing・6 type・8 reception_status・12 read）、body の上限 64 KB | 同期の page |
| LOCATE | Get 同、Name＝folder、MaxListCount 32、FilterPeriodBegin＝今から 1 時間前（§6.2 の offset。listing の順に頼らない）[n6]、ParameterMask 同 | live の event の時刻と名前（event 1.0 に datetime は無い、§3.1.7.1） |
| GET | Get `x-bt/message`、Name＝handle（16 桁、§5.6.2）、Attachment 0x0A = 0、Charset 0x14 = 1（UTF-8）、body の上限 64 KB | 1 通の本文 |
| UNREAD | Put `x-bt/messageStatus`、Name＝handle、StatusIndicator 0x17 = 0、StatusValue 0x18 = 0、End of Body 0x30 | GET でスマホが既読にした物を戻す |
| READ | 同、StatusValue 1 | `PHONE READ` |
| PUSH | Put `x-bt/message`、Name `outbox`、Charset 1、End of Body＝bMessage。答えの Name が新しい handle（§5.8.2） | `PHONE SEND` |
| REGISTER | §8.1 の 5 | MNS の出し直し（§8.1.1） |

- Type の header は NUL 終わりの ASCII（p002 の `PHONE_LISTING_TYPE` と同じ扱い、(IrOBEX、確かめる)）。App Parameters は big-endian（§6.3.1）。文字列の値（FilterPeriodBegin）は NUL を付けない。
- 答えの code: 0xA0 で成功。0xC4 は `ERROR not-found`、0xD3 は `ERROR unavailable`、0xC3・0xC1 は `ERROR permission`、他は `ERROR refused`。timeout・DLC の切断は MAP の失敗（§8.6 の最後）。
- LIST の body が 64 KB を越えた（EMSGSIZE で Abort）時は、同じ offset で n を半分にしてやり直す（n＝1 でも越えれば `ERROR too-large`）[m5]。
- **token**（§4.2）: request の操作は token を持つ。`btd_phone_cancel(token)` で、queue のその token の操作を消す。実行中の操作は OBEX の上では終わりまで行い、答えを捨て、PAGE は次の操作に進まない。
- **live の操作を先に**: PAGE は sub-op（COUNT・LIST・GET・UNREAD）を 1 つずつ queue に入れ、1 つ終わるごとに次を入れる。live の event の操作（LOCATE・GET・UNREAD）は queue の前に入れる（PAGE の sub-op の間に割り込む）[m16]。
- queue が満ちた時: 新しい request は `ERROR busy`、live の event の取得は捨てて SUBSCRIBE に `PHONE DROPPED`（app が同期をやり直す、p001 S7）。
- **client の速さ**: PAGE の GET を出す前に `room(token)` が 32 KB 未満なら待つ（outq の flush の後の `btd_map_pump` で見直す）。待ちが 30 s を越えたら PAGE を `ERROR slow` で終える（MAS を止め続けない）。client が居なくなった token は cancel と同じ。

### 8.5 同期の page（`PHONE PAGE messages`）[M10, m6]

- 引数: `since=<UNIX 秒>`、`cursor=<文字列>`（初回は空）、`count=<1..32>`。folder は `inbox`（dir=in）と `sent`（dir=out）の順（outbox・draft・deleted は読まない）。
- cursor ＝ `<session>.<since の 16 進>.<folder の番号>.<offset>`。session か since が request と違えば `ERROR stale-cursor`（app は自分の目印から最初からやり直す、p001 §8.4）。
- folder の最初（offset 0）では COUNT を出し、その folder の ListingSize と MSETime の offset を覚える（MSETime は MAP の session の間、最初に得た物を FilterPeriodBegin の書きに使う。COUNT の FilterPeriodBegin は、まだ MSETime が無い最初の 1 回は zedBSD の `local_offset` で書き、MSETime の offset が違えば同じ COUNT を 1 回やり直す）。
- LIST（n＝count、o＝offset）→ 各 entry に GET → item を `answer` で出す（§9.3）→ listing で `read=no` の entry は GET の後に UNREAD（p001 S6、S23）。type が SMS でない entry は飛ばす（数える）。
- 次: ListingSize があれば `o + k < min(ListingSize, 500)` かつ k ＝ n で同じ folder の `o + k`、ListingSize が無い答え（1.1 の相手が付けない時）では k ＝ n かつ `o + k < 500` で同じ folder、他は次の folder の 0、最後の folder の後は `more=0`（Q4）。
- 答えの最後: `PHONE PAGE-END cursor=… more=0|1 count=<出した item の数> skipped=<飛ばした数>`、`DONE`。途中の失敗は `ERROR …`、`DONE`（app は PAGE-END が来た時だけ目印を進める、p001 S7）。
- 限界（記録）: offset で進むので、page の間に届いた SMS で 1 件ずれて重複し（key で除く）、消された SMS で 1 件飛ぶ。app の 24 時間の重なり（p001 §8.4）と live の event で補う。

### 8.6 live の event（MNS）[M8, m7]

| event（1.0） | folder | 行い |
| --- | --- | --- |
| NewMessage | `…/INBOX`・`…/SENT`（大文字・小文字を問わず最後の部分） | LOCATE（見つからなければ 2 s 後に 1 回だけやり直す）→ GET → INBOX で LOCATE が `read=no`、か見つからなかった時は UNREAD → SUBSCRIBE へ `PHONE MESSAGE`。見つからない item は時刻が受けた時の zedBSD の時刻（`zone=received`）、`key=-`、`partial=1`。msg_type が SMS でなければ捨てる。他の folder は捨てる |
| MessageShift | folder が SENT、old_folder が OUTBOX | 自分の PUSH の handle なら `PHONE SENT … state=sent`（§下の 1 回だけ）。他（スマホで打った SMS）は NewMessage と同じ |
| SendingSuccess・DeliverySuccess・SendingFailure・DeliveryFailure | — | 自分の PUSH の handle（MAP の session の間、32 個まで覚える）なら `PHONE SENT request=… handle=… state=sent|delivered|failed` |
| MessageDeleted | — | `PHONE MESSAGE-GONE handle=…`（app が使うかは p004） |
| MemoryFull・MemoryAvailable・他 | — | log（数だけ） |

- PUSH の答え（Name＝handle）より先に MNS の event が来る場合（MAP §6.3.2 で Put の答えの header は最後の packet、MNS は別の DLC）: PUSH の間に来た知らない handle の Sending・Delivery・MessageShift の event は 8 個まで 10 s 保留し、PUSH の答えの handle と照らす。
- `PHONE SENT` は handle と state ごとに 1 回だけ（MessageShift と SendingSuccess の両方で sent を 2 回出さない）。
- MNS の Put（§5.1）: Type が `x-bt/MAP-event-report`、App Parameters の MASInstanceID 0x0F が自分の MAS の物（違えば 0xA0 で受けて捨てる。1 つの MNS に全部の MAS の event が来る、§3.1.7.2）。MASInstanceID が無ければ、自分の MAS の物とみなす（1.0 の MSE、§5.1.3 では必須だが受ける側は寛容に）。body を §6.1 で読む。読めない body は 0xC0 Bad Request。Type が違えば 0xD1 Not Implemented。
- MAP の失敗（MAS の DLC の切断、timeout、link の喪失）: queue の request は全部 `ERROR lost`、STATE `messages=failed why=…`、MNS の DLC も閉じる。§8.1.1 でやり直す。

### 8.7 MNS の server と SDP の record（MAP §7.1.2、Q16 (a)）[m9, m10]

- RFCOMM の server channel 16（p001 §5.4）。phone の `accept(16)` は MAP が CONNECTING 以降の間だけ 1。DLC が開いたら OBEX の server（`btd_obex_init(…, BTD_OBEX_SERVER)`）、`target` は MNS の UUID（`bb582b41-420c-11db-b0de-0800200c9a66`、§6.3 表 6.5）の時だけ 1。MNS の DLC は 1 つ（2 つ目は DM）。
- 登録は phone が持つ db（今の `const struct btd_sdps_db *` を const でなくし、main の `btd_records` を渡す）で `btd_sdps_register`・`unregister` を呼ぶ [n7]。record は phone link が READY になった時に登録し（pairing の直後や接続の直後にスマホが SDP を引く機種のため、MAP の始めを待たない。messages が on の時だけ）、phone link の終わりと messages の off で外す。
- record（属性 ID の昇順、`btd_sdps_register` の形）:

| 属性 | 値 |
| --- | --- |
| 0x0001 ServiceClassIDList | UUID16 0x1133 |
| 0x0004 ProtocolDescriptorList | (L2CAP 0x0100)、(RFCOMM 0x0003, uint8 16)、(OBEX 0x0008) |
| 0x0005 BrowseGroupList | 0x1002 |
| 0x0006 LanguageBaseAttributeIDList | uint16 0x656E（"en"）、uint16 0x006A（UTF-8 の MIBEnum 106）、uint16 0x0100（Core §5.1.8: ServiceName を持つ record は should） |
| 0x0009 BluetoothProfileDescriptorList | (0x1134, uint16 0x0101) |
| 0x0100 ServiceName | "Keiland MNS" |

- MapSupportedFeatures（0x0317）と GoepL2CapPsm（0x0200）は出さない（Q16 (a)、p002 §1）。MSE は MapSupportedFeatures の無い MCE に 0x0000001F を仮定する（§7.1.2 の注 1）。
- 試験の正解は手で書いた record の byte の列（data element の型 byte と長さを含む）。自前の `sdp.c` で読み戻すのは補い（両側が同じ誤りなら通るため）。

## 9. socket の PHONE（p001 §8.3）

### 9.1 行の形（`phoneio.c`）[m3]

- 値の文字列は `"…"`。中の `"` と `\` は `\"`・`\\`、0x20 未満と 0x7F は `\xHH`。名前と番号は escape の前に 128 byte で切る（UTF-8 の文字の途中で切らない）。daemon が書く 1 行は改行を含めて 2047 byte 以下（compositor の読みの入れ物は 2048 byte で、満ちると閉じる、`bluetooth-zedbsd.c:732,760`）。
- 長さ付きの値: 行の最後の `length=N` の後、改行の次の N byte が中身（改行を付けない）。
- request の引数は `key=value` を空白で区切る（value は引用符の文字列か空白の無い語）。知らない key は `ERROR argument`。
- 純粋な関数（escape、取り出し、item の行）にして host で試す。

### 9.2 request（持ち主と root だけ。`SHOW` は D8 の人も）

| request | 答え |
| --- | --- |
| `PHONE SHOW` | `PHONE address=… owner=<uid>|invalid mine=0|1 enabled=0|1 profiles=m,c,h present=0|1 link=none|paging|securing|ready|closing（ACCEPTING は securing、CANCELLING は paging に寄せる）[n9] messages=off|connecting|ready|failed send=0|1 notify=0|1 why=…`（記録が無ければ行無し）、`DONE`。持ち主でない D8 の人には `mine=0` と address・enabled だけ |
| `PHONE LINK ADDRESS on|off [profiles=m,c,h]` | §3.2。on: 在ならすぐ page、自動の page の止めを解き、MAP の失敗のやり直しを戻す。off: 切る（記録は残す）。`DONE` |
| `PHONE SUBSCRIBE` | §9.4。答えは `DONE` の後に event の行が続く |
| `PHONE PAGE messages since=N cursor=C count=N` | §8.5 |
| `PHONE READ handle=H` | `DONE` |
| `PHONE SEND to="…" length=N` ＋ N byte | §4.3 の後、`PHONE SENT request=<n> handle=H state=pushed`、`DONE`（PushMessage の成功まで待つ）[n9] |
| `PHONE DROP ADDRESS` | p002 のまま（root、試験の道具） |

- messages が ready でない時の PAGE・READ・SEND は `ERROR not-ready`。SEND は send=0 の時 `ERROR no-send`。
- 1 つの client が待つ request は 1 つ（PAGE・READ・SEND の答えまで他の行を受けない、今の `waits_*` の形）。

### 9.3 item の行

```
PHONE MESSAGE handle=<s>.<h> key=<16 hex>|- folder=inbox|sent dir=in|out time=<UNIX 秒> zone=phone|mse|local|received datetime="<スマホの文字列>" peer="<番号>" name="<名前>" read=0|1 partial=0|1 truncated=0|1 length=<n>
<n byte の UTF-8 の本文>
```

- peer と name: dir=in は sender_addressing と sender_name（無ければ originator の vCard の TEL と FN・N）、dir=out は recipient_addressing と recipient_name（無ければ recipient の vCard）。
- read は listing の `read`（GET の前の値。UNREAD で戻すので、スマホの状態と同じ）。

### 9.4 SUBSCRIBE（F-086 の一部）

- `PHONE SUBSCRIBE` を送った接続は、以後 event の行だけを受ける（request の行は読み捨て）。持ち主と root だけ、同時に 2 つまで。
- event: `PHONE STATE …`（§9.2 の SHOW の行と同じ中身、変わった時）、`PHONE MESSAGE …`（§9.3）、`PHONE SENT …`、`PHONE MESSAGE-GONE …`、`PHONE DROPPED`。
- 出力の queue（§4.1）で、event を足すと 192 KB を越える時は足さず「落ちた」の印を立てる。queue が 64 KB 以下に減った時に、先に `PHONE DROPPED` を足す。app は DROPPED で同期と `PHONE SHOW` をやり直す（§9.6）。
- F-086（状態の変化の通知の全体）の Future Work の行の更新は Q1 に頼む（この WS は phone の event だけ）。

### 9.5 log

message の本文・番号・名前は log に書かない（p001 R22）。件数、response code、`why`、bMessage の `form=` だけ。

### 9.6 p004 に渡す事

- `PHONE SEND` した SMS は app が自分で保存する。同じ SMS が後の PAGE（sent の folder）で `key` 付きで来るので、app は「同じ peer・同じ本文・時刻が ±10 分」の自分の送信の item と重ねる。`partial=1`（key 無し）の item も同じ規則で重ねる（p004 の store の設計で決める）。
- 本文 16 KB の上限、`truncated`。DROPPED の後の同期と SHOW のやり直し。

## 10. 試験

### 10.1 host（`plan/ws197/tests/`、ASan・UBSan、`bt-phone-host-test.sh` に足す）[m10]

試験の期待値（tag、ParameterMask `00 00 11 7E`、FilterMessageType `0C`、opcode、record の byte）は仕様の表から手で書いた定数にし、実装の macro を使わない。

| 試験 | 内容（正解の出典） |
| --- | --- |
| `bt-phonerec-host-test` | 形と読みの検査（行の欠け・重複・範囲外、名前の文字）、書き・読み・消し、find の有効な 0・1・2 個、無効な記録（bond 無し、鍵の型、account 無し、名前違い）、掃除、`btd_keys_list` が `.phone` を数えない |
| `bt-outq-host-test` | 部分の send、EAGAIN、上限で dead、SUBSCRIBE の落ちた印と 64 KB での DROPPED、長さ付きの値、close の前の flush |
| `bt-mapxml-host-test` | §3.1.6.1 と §3.1.7.1 の例（空白付きの `=`）、実体と文字参照、DOCTYPE、comment、`:` の名前、上限、壊れた入力、datetime（§6.2 の手の値、`Z`、NUL）、fuzz 20 万回 |
| `bt-bmsg-host-test` | 手で書いた bMessage（形 1〜4 の LENGTH、`/END:MSG`、3 段の BENV と外側の recipient、4 段は失敗、native の形）、組み立ての手の正解、fuzz 20 万回 |
| `bt-map-host-test` | map.c を hook で: 台本の MSE（OBEX の答えの byte を手で書く。要求は `btd_obex_header_next` で読み、header と App Parameters の tag・値を手の定数と照合）。始め（Connect の 60 s、Connect の答えが 20 s 後、SetPath 2 回、通知の登録の filler 0x30 と tag 0x0E）、COUNT（filter 付き）と MSETime、LIST の App Parameters の byte、ListingSize の無い答え、GET と UNREAD、PAGE の cursor（session・since の違い）と 500 の上限と folder の移り、`room` の待ちと 30 s、cancel（PAGE の途中で token を消す → 何も書かれない）、live の割り込み、LIST の 64 KB で n の半分、MNS の Put（MASInstanceID の違い・無い、壊れた XML に 0xC0）、NewMessage の LOCATE・やり直し・GET・UNREAD、SEND と PUSH の答えの前の SendingSuccess の保留、SENT が 1 回、queue の満ち、各 error code、失敗のやり直しの間隔、MNS だけの切断と出し直し、stale の handle |
| `bt-phone-link-host-test`（直す） | PROBE の場面を消し、偽の controller で: handoff で記録を書く（`other-phone`、`owned`、`store`、同じ uid で profile を保つ）、記録を書いてから route、在で page → Connection Complete → 認証 → 暗号化 → 鍵 16 → READY、受けの接続でスマホが先に暗号化（自分の Authentication Requested が 0x0C → READY）、WAIT_PEER の 3 s、交差（PAGING 中の Connection Request → Accept と Cancel、page の Connection Complete 0x02 を吸う）、READY 中の Connection Request を Reject し、Core 7.1.9 のローカルの Connection Complete（0x0D）を吸う、守りの切れの Cancel、READY の Link Key Request に鍵、Key Missing で自動の page の止め、0x13 の切断で page しない、短い link で段が進む、2 分で戻す、不在で切る、POWER off、HID の上限 5・6 と busy-links、MNS の record が SDP の server で手の byte と一致 |
| `bt-phoneio-host-test` | escape、引数、item の行、2047 byte |
| main の配線（`bt-phone-host-test` の中の小さな試験、main.c は host で build しないので、token・長さ付きの入力の処理は main の関数を `phoneio.c` の純粋な関数に寄せて試す） | SEND の行と本文が同じ buffer に来る、not-ready の SEND の本文の中の `SHOW` が実行されない、token の世代の違いで答えが捨てられる |

偽の controller は Core 7.1.7（Cancel）・7.1.8（Accept）・7.1.9（Reject の後のローカルの Connection Complete）の事象の順を台本に入れる。

WS143 の host の試験（`bt-daemon-host-test.sh`）も流す（main.c・pair.c・obex.c を変えるため）。

### 10.2 QEMU（T1）

保留の branch の image で WS143 の HID の回帰 4 本（p002 §12.2 と同じ、T1-518 の組）。main.c の client と出力の変更で compositor の Bluetooth の画面が壊れないことを `bt-daemon-p003.sh` の SHOW・BONDS で見る。MAP の相手は QEMU に無い（dongle 無し、p001 §9.2）ので MAP は host の試験だけ。

### 10.3 実機（p008）

Android で: pairing（phone=1、seat の人）→ スマホの「メッセージへのアクセス」の許可 → 最初の同期（30 日）→ 受信の event（スマホで未読のまま）→ 既読 → 送信 → 離れて戻る（再接続）→ スマホで「切断」（page しない）→ logout と login。iPhone は Q13 (a) で p006 の後。

## 11. 実装の順（保留の branch の WIP commit の単位）

| i | 内容 | 確かめ |
| --- | --- | --- |
| i01 | `phonerec.c`、phone の記録（load・link_set・forget・handoff の 6〜9）、PAIR の始めの検査（pair.c の hook、`owned`・`phone-seat`）、`PHONE LINK`・`PHONE SHOW`、FORGET の規則、HID の上限（§3） | host（phonerec、phone-link の handoff）、build |
| i02 | `outq.c`、main の client の枠 16・予約・世代、non-blocking の出力、長さ付きの入力（§4） | host（outq、phoneio の入力の部分）、build、WS143 の host の試験 |
| i03 | phone link の一生（§5: 在・不在と seat の見直し、page と backoff、受けと交差、SECURING の段、切断の理由、sleep.end）、PROBE を消す、DLC の振り分け（§5.8） | host（phone-link）、build |
| i04 | `mapxml.c` と datetime（§6） | host、fuzz |
| i05 | `bmsg.c`（§7） | host、fuzz |
| i06 | `obex.c` の `response` の hook と timeout、`map.c`（§8）、phone の配線、MNS の record | host（map、phone-link の MNS）、build |
| i07 | `phoneio.c`、main の PHONE の request と SUBSCRIBE と event、token の cancel（§9）、`protocol.h` | host（phoneio、main の配線）、build |
| i08 | T1 への依頼（§10.2、Q1 へ）、style-check、phase.md の記録 | T1 |

各 i の後に `bt-phone-host-test.sh` と WS143 の `bt-daemon-host-test.sh` を流し、target の bluetoothd の build（`config/current-uat.mk`、`BUILD=build/p1-uat`、target を名指す）を warning 0 にして WIP commit、SHA を「ws197 branch」と明記して Q1 に送る。

## 12. 受け入れ

- §10.1 の host の試験と fuzz が全部 PASS、WS143 の host の試験が PASS、target の bluetoothd の build warning 0、style-check の変更箇所 0。
- T1 の HID の回帰（§10.2）が PASS。
- 実機の MAP（§10.3）は p008。この Phase では未実施と書き、それだけで uncleared にしない（p001 §1 の 4、p002 §14 と同じ）。

## 13. 危険と未確認

| 項目 | 内容 | いつ |
| --- | --- | --- |
| MAP 1.1 の MCE への相手の振る舞い | Android・iPhone が MapSupportedFeatures の無い MCE に通知・送信を出すか、MSETime・ListingSize を付けるか | p008 |
| 厳しい 1.4 の MSE | MSE の record の bit 19（Connect の MapSupportedFeatures）が 1 の時、表 6.9 の C.1 は Connect に MapSupportedFeatures を必須にする。1.1 の MCE は後方互換で救われる見込みだが、Connect を断る MSE があり得る [m22]。あれば Q16 の見直しをユーザーに尋ねる | p008 |
| GetMessage が既読にするか | 機種差。UNREAD で戻す（害は無い） | p008 |
| Accept の role 0x01 | スマホとの接続で role を変えない選択が通るか | p008 |
| スマホの認証の振る舞い | 受けの接続で自分から認証・暗号化するか（WAIT_PEER の 3 s）、profile の無い ACL を切るか、許可の画面の間 Connect を待たせるか | p008 |
| listing の順 | 新しい順と仕様は言うが例は古い順。map は順に頼らない（LOCATE は 1 時間の filter で絞る） | p008 |
| SDP の属性 ID | Assigned Numbers の値（§0）を手元で確かめていない | p008 の SDP の dump |
| handle の寿命 | session ごとに変わる前提（`stale`）。Persistent が使えない 1.1 の制限 | — |
| 同期の取りこぼし | page の間の受信・削除で 1 件ずれる（§8.5） | p004 の重なりと live |
| 時刻 | offset の無い datetime は MSETime か zedBSD の timezone で推定（`zone=`） | p008 |
| 交差の窓 | 交差で Cancel した page の Connection Complete の前に HID が page を始めると Command Disallowed（§5.3） | p008 |
| HID の同じ潜在の誤り | `hid_request` の PAGING 中の Reject（§5.3）。BUG-282 | i03 で同じ形の直しを当てられるか見る（Q1 2026-10-10） |

## 14. 見積もり

p001 の p003 の 16 LW と骨格 +5 LW（p001 §12 の見直し）に対し、第 2 版の内訳: i01 3、i02 2、i03 5、i04 2、i05 2、i06 7、i07 2、i08 1、計 **24 LW**（review-1 の持ち主の規則・交差・SECURING の段・MAP のやり直しで +3）。

## Event

- 2026-10-10: 第 1 版（P1、d21a55cae）。MAP 1.4.2 の §3.1.3・§3.1.6・§3.1.7・§5.1〜§5.9・§6.3・§6.4・§7.1 を読んで書いた。
- 2026-10-10: design-reviewer（agent adc3ca142b525c859）の review → [review-1.md](review-1.md)（blocker 3・major 12・minor 22。i04・i05 は GO、他は直してから）。
- 2026-10-10: 第 2 版（P1、4397bcb31）。全部に答えた（各節の印）。Core 7.1.7（Create Connection Cancel）と §5.1.8（LanguageBaseAttributeIDList）を読み足した。
- 2026-10-10: 第 2 版の再確認（agent a8b0127cb85b80091）→ [review-2.md](review-2.md): review-1 は全部閉じた。新しい major 2（N1: 0x13・0x15 で page を止めると戻らない、N2: 持ち主の無い bond を phone=1 で黙って取れる）と minor 9。**i02・i04〜i07 は GO、i01・i03 は N2・N1 を書けば GO**（再 review は要らない）。第 2.1 版で全部を本文に入れた。
- 2026-10-10: Q1: hid.c の同じ形の潜在の誤りは [BUG-282](../../bugs/BUG-282.md)（tracking、ベータ3、WS143）。i03 で linkmgr・交差の直しと同じ形が HID にも当てられるなら一緒に直し、ticket に記録する（ws197 branch）。

## 実装の進み（ws197 branch）

| i | commit | 状態 |
| --- | --- | --- |
| i01 記録・PAIR の検査・LINK・SHOW・FORGET・HID の上限 | この commit | 新 `phonerec.c`・`.h`（§3.1: 形・読み・書き・消し・list・valid・prune）。phone: `btd_phone_init` に keys の folder と hook（account）、`btd_phone_load`（掃除、有効が 1 つならそれ、0 なら最初の無効の物を SHOW 用に、2 つ以上は EEXIST）、`btd_phone_pair_check`（`busy`・`phone-seat`・`owned`）、`btd_phone_link_set`、`btd_phone_forget`、`btd_phone_show`、handoff の 6〜9（`phone_take_record`: 記録を書いてから route）、HID の上限は記録だけで決める（`phone_limit`、`phone_ended` から外した）。pair.c: phone=1 の pairing の Link Key Request は保存の鍵があっても常に Negative Reply（N2、使わなくなった `pair_key_authenticated` を消した）。main: `btd_account`（getpwuid）、`btd_seated`、controller の READY で `btd_phone_load`、PAIR の始めの検査、FORGET は `.phone` を先に（bond が無くても `.phone` を消せたら DONE）、`PHONE SHOW`・`PHONE LINK`。**設計の補い**: §3.2 の「pair.c の始めの検査の hook」は、main が `btd_pair_start` の前に `btd_phone_pair_check` を呼ぶ形にした（同じ loop の中で同じ結果、pair.c の API を増やさない）。`.phone` の行は `key value`。試験: 新 `bt-phonerec-host-test`（34 checks）、`bt-phone-link-host-test` に records（world ごとに mkdtemp の folder、+38 checks、計 79）、`bt-pair-phone-host-test` の保存の認証済みの鍵の期待値を N2 に直した。bt-phone-host-test.sh（10 本）PASS、WS143 の bt-daemon-host-test.sh PASS、target の bluetoothd の build（`ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/p1-uat`）rc 0・warning 0、style-check（変えた file）0 |
| i02 出力の queue・client の枠・長さ付きの入力 | a7709b6a6（途中、BUG-275 の割り込みで中断）、この commit | 新 `outq.c`・`.h`（§4.1: 上限 256 KB、超える追加は丸ごと ENOBUFS、send を差し替えられる flush）、`phoneio.c`・`.h`（引数の読み `btd_phoneio_next`（語と `"…"`、`\"`・`\\`・`\xHH`、NUL は不可）、`btd_phoneio_send_length`、client の入力の reader `btd_phoneio_input_*`: 行と PHONE SEND の本文を分け、本文の byte を行として読まない、本文の後の改行は無い（§9.1））。main: client の枠 16 と予約 4（空きが 4 以下なら root と seat の人だけ）、世代（`btd_accept` ごと）、出力の queue（`btd_write` は queue に足して 1 回 send、poll の POLLOUT で flush、close の前に 1 回 flush、1 秒の poll の待ちを消した）、入力は `btd_phoneio_input`（malformed の PHONE SEND は `ERROR length` の後に閉じる、待ちの間の SEND の本文は読んで捨てる）、`PHONE SEND` は i07 まで `ERROR not-ready`。**設計の補い**: §4.3 の入力の扱いを main でなく `phoneio.c` の純粋な reader にした（host で試せるように、review-1 M11 の試験）。試験: 新 `bt-outq-host-test`（21 checks）、`bt-phoneio-host-test`（34 checks: 本文の中の改行と `SHOW`・`FORGET` が要求にならない、1 byte ずつ・7 byte ずつ、閉じる要求の後は読まない、malformed、長すぎる行）。bt-phone-host-test.sh（12 本）PASS、WS143 の host 試験 PASS、target の build warning 0、style-check 0 |
| i03（途中、2026-10-10 Q1 の割り込み BUG-283・WS199・WS200 で中断、**この commit は build しない**） | この commit | phone.h: 状態（PAGING・CANCELLING・ACCEPTING・SECURING）と securing の段、時間、backoff、`struct btd_phone_profile`（ready・ended・sdp_done・accept・opened・data・writable・closed・open_failed）、`btd_phone_set_profile`・`set_seat`・`resume`・`sdp_query`・`dlc_open`・`dlc_write`・`dlc_close`、probe と answer の hook を消した。phone.c: 新しい関数を差し込み、probe・OBEX の関数を消した。**再開点**: (1) phone.c の先頭の定数（`PHONE_CREATE_CONNECTION` 0x0405・`PHONE_CANCEL_CONNECTION` 0x0408・`PHONE_ACCEPT_CONNECTION` 0x0409・`PHONE_REJECT_CONNECTION` 0x040A・`PHONE_LINK_KEY_REPLY` 0x040B・`PHONE_LINK_KEY_NEGATIVE` 0x040C・`PHONE_AUTHENTICATION` 0x0411・`PHONE_SET_ENCRYPTION` 0x0413・`PHONE_READ_KEY_SIZE` 0x1408、事象 0x03・0x04・0x06・0x17、status 0x02・0x04・0x06・0x0B・0x0C・0x23・0x2A、reason 0x0D・0x16、role 0x01、packet type 0xCC18、`phone_backoff_ms[]` = {0, 30000, 60000, 120000, 240000, 480000, 600000}）を足し、listing・target・headers の定数を消す。phone.h に `BTD_PHONE_MNS_CHANNEL` 16。(2) 前方宣言の list を直す。(3) `phone_notice`・`phone_lost_frame`・`btd_phone_drop` の `phone_disconnect` に `BTD_PHONE_AFTER_STEP`、handoff の終わりで `phone->ready_since`・`phone_presence`・`phone_profile_ready`（不在なら切る）、`btd_phone_load`・`link_set`・`forget` の後に `phone_presence`（link_set・forget の明示の切断を消す）。(4) main: `btd_phone_init` の引数、PROBE・`btd_probed`・`waits_probe` を消し、5 秒ごとの `btd_phone_set_seat`（POWER off は seat 無し）、sleep.end で `btd_phone_resume`。(5) `bt-phone-link-host-test` の PROBE の場面を profile の試験に置き換え、§10.1 の lifecycle の場面を足す。(6) BUG-282: `hid_request` の PAGING 中の交差に同じ形（Cancel を先に、Accept、cancel した page の失敗の Connection Complete を吸う）を当てるか見る |
| i03（続き、2026-10-10 夜、Q1 の指示で WS199 の設計へ移るため再び中断） | この commit | (1)〜(4) の一部を済ませた: phone.c の定数（command・事象・status・reason・role・packet type・`phone_backoff_ms`）と前方宣言、probe の定数・target を消した、`phone_notice`・`phone_lost_frame`・`btd_phone_drop` の `phone_disconnect` に after、handoff の終わりで ready_since・presence・profile（不在なら切る）、load・link_set（on で止めを解く）・forget は `phone_presence`。main: `btd_phone_init` の引数、PHONE PROBE・`btd_probed`・`waits_probe`・`btd_probe_client` を消した（PHONE は SHOW・LINK・DROP）。target の bluetoothd の build rc 0・warning 0（host の試験は PROBE を使うので未更新で通らない）。**再開点**: (4) の残り（main の 5 秒ごとの `btd_phone_set_seat`（`btd_seated` の uid、POWER off は seat 無し）、sleep.end で `btd_phone_resume`）、(5) `bt-phone-link-host-test` の PROBE の場面を profile の試験に置き換え・§10.1 の lifecycle の場面、(6) BUG-282、style-check |
| i03（残り、2026-10-10 深夜 P1 の新しい世代） | この commit | main と merge（e4ebf2dbf、衝突なし）。(4) main: 5 秒ごとの `btd_seat_check`（`/dev/gpu0` の持ち主、greeter と POWER off は seat 無し、POWER の切り替えですぐ見直す、poll の deadline に入れた）→ `btd_phone_set_seat`、sleep.end で `btd_phone_resume`（2b60a79e1）。**直した**: `phone_schedule` が最初の失敗の後も段 0（すぐ）で page していた → 段を先に進めてからその段の待ち（最初の失敗は 30 s、§5.2）。(5) `bt-phone-link-host-test`: PROBE の場面を消し、偽の controller が Create Connection Cancel・Read Encryption Key Size に Command Complete、Authentication Requested に指定の status で答える。新しい場面 profile（handoff で owner が不在なら切る、在なら profile に ready 1 回、SDP の問いで MAS の channel 5、DLC の open、不在で切れて ended と closed）、page（Create Connection → 認証 → 鍵の Reply → 暗号化 → 鍵 16 → READY、READY 中の Link Key Request、鍵 7 で最長の待ち、HID 6 台で page を止めて 2 s）、inbound（Accept、スマホの暗号化で READY、WAIT_PEER 3 s の後の認証の 0x0C で待ちに戻る、0x23 の衝突で 200 ms 後にやり直し）、crossing（Cancel → Accept、0x02 を吸う、Cancel 0x0B で Reject と自分の Connection Complete を吸う、READY 中の Request を Reject）、guard（12 s で Cancel、0x02 で次は 30 s）、keys（Key Missing で止め、sleep.end で戻る）、reasons（0x08 で段 1、2 分で段 0、短い 0x13 が 3 回で peer-closed、LINK on で戻る）、absent（PAGING で不在 → Cancel、つながった link を切る、在に戻ると page）。notices は DLC で RFCOMM の session を作る形に。(6) BUG-282: HID への同じ形の直しは router の token の終わりも要るので i03 では当てず、ticket に記録（ベータ3 の WS143）。style-check（phone.c・main.c）0 | `bt-phone-host-test.sh` PASS（phone-link 187 checks）、WS143 `bt-daemon-host-test.sh` PASS、target の bluetoothd（`config/current-uat.mk`、`BUILD=build/p1-uat`）warning 0。**再開点**: T1 の WS143 の HID の回帰（Q1 が依頼）→ main へ merge。次は i04（`mapxml.c` と datetime、§6） |
| i04 mapxml と datetime（§6） | この commit | main と merge（fef7cd9dc、衝突なし）。新 `mapxml.c`・`.h`: listing（`btd_mapxml_listing`: root `MAP-msg-listing` の `msg` を呼び手の配列へ、handle の欠け・17 桁以上・16 進でない物は skipped、配列の外は dropped、1024 通を越えると E2BIG）と event（`btd_mapxml_event`: `event` がちょうど 1 つ、type が無い・handle が壊れていれば EINVAL）。字句は §6.1 の通り（BOM、宣言、comment、subset の無い DOCTYPE、`:` の属性は無視・`:` の要素は失敗、`=` の前後の空白、実体と文字参照を UTF-8 に、値の TAB・CR・LF は空白、CDATA・処理命令・`msg` の中身は失敗、root の後の NUL は許す）。上限: 入力 64 KB、属性 32、値 1024 byte（解いた後）、名前 64 byte。text の欄は 256 byte で UTF-8 の文字の途中で切らない（`btd_mapxml_utf8_cut`、bmsg・phoneio も使う）。datetime（§6.2）: `btd_mapxml_time_parse`（`±hhmm`・`Z`・末尾の NUL、閏日、60 秒）、`btd_mapxml_time_unix`（自分の offset → MSETime → zedBSD の順、`BTD_MAP_FROM_*`）、`btd_mapxml_days_from_civil`、`btd_mapxml_time_format`（FilterPeriodBegin の形、年は 4 桁だけ）。**設計の補い**: listing の `read` は yes・no・無しの 3 値（無しは `BTD_MAP_READ_UNKNOWN`、扱いは i06 の map）、知らない子の要素（空の物）は無視、datetime が 23 byte を越える物は空にする（読めない datetime と同じ扱い）。Makefile に mapxml.c。試験: 新 `bt-mapxml-host-test`（144 checks: 例の listing と event、handle、字句、参照、全部の切り口、上限、UTF-8 の切り、event、datetime の手の値（1197518710、1791621245、1709251200、0、日 11017・-719162）、format、fuzz 20 万回） | `bt-phone-host-test.sh` PASS（13 本）、WS143 `bt-daemon-host-test.sh` PASS、target の bluetoothd（`config/current-uat.mk`、`BUILD=build/p1-uat`）rc 0・warning 0、style-check（mapxml.c・.h・試験）0。**再開点**: i05（`bmsg.c`、§7） |
| i05 bMessage（§7） | この commit | 新 `bmsg.c`・`.h`: 読み `btd_bmsg_parse`（§7.1: CRLF・LF、構造の行は前後の空白を除いて大文字・小文字を問わず比べる、BENV は 3 段まで、STATUS・TYPE・FOLDER、originator の vCard と一番外の BENV の最初の recipient の vCard の TEL（parameter を飛ばす）・FN（無ければ N を「名 姓」に）、本文は LENGTH の形 1〜3 を順に試し、合わなければ最初の END:MSG の行（行頭の物、無ければ空白の後の物）、本文の行頭の `/…END:MSG` の `/` を 1 つ除く、CHARSET が UTF-8 か無い時だけ（ENCODING だけの native の形と他の charset は EINVAL）、不正な列と NUL は U+FFFD、16 KB で文字の途中で切らず `truncated`、2 つ目の MSG の part は読み飛ばす、入力 64 KB）。組み立て `btd_bmsg_build`（§7.2: 手の BNF の形、改行は全部 CRLF（lone CR・LF も）、行頭の `/*END:MSG` に `/` を足す、LENGTH は形 1、TYPE は SMS_GSM・SMS_CDMA、番号は `btd_bmsg_number_ok`（`[0-9+*#]` 1〜32）、本文は 1〜8192 byte の UTF-8 で NUL 無し（`btd_bmsg_utf8_ok`）、出力の上限 `BTD_BMSG_BUILD_MAX`）。**設計の補い**: 受けの本文の NUL も U+FFFD にする（socket の長さ付きの値と app の文字列を守る）。Makefile に bmsg.c。試験: 新 `bt-bmsg-host-test`（90 checks: 受けの例（LENGTH 34 を手で数えた）、形 1〜4（12・14・32・999・`1x`）、escape（LENGTH 44）、LF だけ（30）、構造に似た本文（41）、3 段の BENV と外側の recipient、4 段は失敗、charset、U+FFFD、16 KB の切り、拒否の各形、組み立ての手の byte 列（LENGTH 38）と CRLF（29）と往復、最悪の escape と改行、UTF-8 の検査、fuzz 20 万回） | `bt-phone-host-test.sh` PASS（14 本）、WS143 `bt-daemon-host-test.sh` PASS、target の bluetoothd rc 0・warning 0、style-check（bmsg.c・.h・試験）0。**再開点**: i06（`obex.c` の response の hook と timeout、`map.c`、phone の配線、MNS の record、§8） |
| i06（途中、2026-10-10 Q1 の割り込み（T1-523 の FAIL、agent/p1）で中断。**merge しない**、i05 の 7ed73b31b までが merge の対象） | この commit | obex: `response` の hook（Get・Put・SetPath の各答えの packet の header を全部）と `btd_obex_set_timeout`（client の答えの待ち、既定 10 s）、`bt-obex-host-test` に 2 本（2079 checks）。phoneio: `btd_phoneio_quote`（`"…"`、`\"`・`\\`・`\xHH`、128 byte で文字の途中で切らない）、`BTD_PHONEIO_OUT_MAX` 2047・`BTD_PHONEIO_TEXT_MAX` 128（phoneio の試験は mapxml.c も link）。新 `map.c`・`.h`（§8 全部を書いた: 始め（SDP の MAS の選び、Connect 60 s、SetPath 2 回、登録）、失敗とやり直し（30 s〜600 s、permission 600 s、no-mas）、操作の queue（COUNT・LIST・LOCATE・GET・UNREAD・READ・PUSH・REGISTER、live を先に）、PAGE（cursor、COUNT の zone のやり直し、LIST の半分、room の待ちと slow、PAGE-END）、live の event（LOCATE と 2 s 後のやり直し、GET、UNREAD）、PUSH と held の event と SENT の 1 回、MNS の server（target、Put、MASInstanceID、0xC0・0xD1）、item の行と key、DLC の閉じは次の tick に回す（RFCOMM の event の中で閉じない））。build は通る（warning 0、style-check 0）、Makefile にはまだ入れていない。**再開点**: (1) `plan/ws197/tests/bt-map-host-test.c` を書く（§10.1 の map の行。台本の MSE: map の dlc_write の OBEX の packet を読んで手の定数と照合し、手の答えの byte を `btd_map_data` で返す。期待の値: key `23ef6660912108a2`（`i|20231115T090000+1100|+15551234|Hello`）、時刻 1699999200、since 1700000000 = 0x6553f100、FilterPeriodBegin `20231114T221320`（offset 0）・`20231115T091320`（+1100））。(2) phone の配線: `struct btd_phone_profile` の opened を (dlci, server_channel, ours) に、MNS の SDP record の登録（phone.c、db を const でなく）、`btd_phone_profile_ok`（MAP の ready で backoff を戻す、map の `up` の hook）、SHOW の present を本当の値に。(3) Makefile に map.c、target の build、bt-phone-link の試験に MNS の record の手の byte。(4) i07 で main の hook（answer・emit・room は client を同期で閉じない事）|
| T1-524 の FAIL の直し（2026-10-10 P1） | この commit | bt-hid-p005 の FAIL（再起動の後に HOG の自動の接続が来ない、withdraw の前に keyboard の node が出ない、戻った時に keyboard・HOG が開かない）の原因: i03 で `btd_timeout` に足した seat の見直しの期限 `btd_seat_check_at` が、最初の見直しの前は 0 で、`earliest = 0`（期限無し）にして HID と phone の期限を消し、poll が descriptor を無限に待った（controller が READY でない間も同じ）。HID の tick（page と LE の auto-connect）が I/O の来る時まで走らない。直し: phone の期限と seat の見直しは controller が READY の間だけ数え（loop が tick するのも READY の間だけ）、まだの見直し（0）は「今すぐ」とした。phone の期限も READY の間だけ（READY でない間の空回りを防ぐ）。bt-phone-host-test.sh PASS、WS143 の bt-daemon-host-test.sh PASS、target の bluetoothd warning 0、style-check 0。QEMU は T1 の再試験待ち |
