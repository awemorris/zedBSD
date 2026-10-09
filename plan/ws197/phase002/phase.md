<!-- awesome-plan project=zedbsd record=ws197-p002 -->

# ws197-p002: RFCOMM・OBEX・SDP の server と、bluetoothd の受け・送り・handoff の直し（詳細設計）

Phase ID: `ws197-p002`
Parent: [WS197](../ws.md)
Status: in-progress（2026-10-10 P1: 詳細設計の第 2.1 版は design-reviewer を通った（[review-2.md](review-2.md)、i01〜i08 は GO）。実装中）
Phase disposition: normal
Queue: Q1 の投入（2026-10-10「WS197 p002（RFCOMM と OBEX）を、10/17 まで main に入れない保留の branch で始める。§12 の条件 T1〜T4 を先に詳細設計に書いて design-reviewer を通し、通ってから実装」）
Branch: `agent/p1-ws197`（main 43e55052c から。10/17 まで main に merge しない。各 Phase の始めと merge 依頼の前に main を取り込む）
依存: [p001](../phase001/phase.md)（設計、cleared 候補）、ユーザーの決定（2026-10-09「全部推しどおり」）、WS143 p005 i02・i03（main に入っている。T1 の再試験は WS143 の側）
所有 path: `userland/base/bluetoothd/`（新しい file と、下の §3〜§7 の範囲の WS143 の file の変更）、`plan/ws197/`

版: 2026-10-10 第 1 版（P1）→ 同日 第 2 版（[review-1.md](review-1.md) の B1・M1〜M6・m1〜m18 を反映、各節の `[B1]` などの印）。各節の `[T1]`〜`[T4]`・`[N3]`〜`[N5]` は [review-3](../phase001/review-3.md) の条件。

## 0. 出典

- 仕様は Bluetooth SIG と ETSI の公開の PDF を読んだ（2026-10-10、P1。複写は repository に入れない）:
  Core 5.4（Vol 3 Part B SDP、Vol 4 Part E HCI）、RFCOMM 1.2（TS 07.10 の部分集合）、3GPP TS 27.010 v13.0.0（= GSM 07.10、ETSI TS 127 010）、MAP 1.4.2、PBAP 1.2.3。
- IrOBEX 1.5（OBEX 本体）は公開されていない。OBEX の値は MAP・PBAP の本文（Target の UUID、Application Parameters、filler）と、台本の相手・実機の trace で確かめる。「(IrOBEX、確かめる)」と書いた値はその扱い。
- 他の OS の実装（BlueZ・Android・Apple）の code と、BlueZ の tree の中の PICS の file は読まない。

## 1. Q16 の確かめ（§4.5）の結果

| 仕様 | 本文 | 結論 |
| --- | --- | --- |
| MAP 1.4.2 §9 | 「The MCE-MAS and MSE-MNS clients shall implement GOEP v2.0 or later and follow the GOEP SDP Interoperability Requirements」。§2.1 「The MAP v1.2 profile requires backwards compatibility with previous versions of MAP that were based on GOEP v1.1 … When OBEX over RFCOMM is used (GOEP v1.1), then only a subset of features shall be used」。表 9.1: 片方が RFCOMM の channel だけを出す時は GOEP v1.1（IrOBEX 1.2 over RFCOMM） | MAP 1.2 以降を名乗る MCE は GOEP 2.0（OBEX over L2CAP、ERTM）が必須 |
| PBAP 1.2.3 §9 | 「The Phone Book Client Equipment (PCE) shall implement GOEP v2.0 or later」、§2.1 に同じ後方互換 | PBAP 1.2 の PCE も GOEP 2.0 が必須 |
| MAP 1.4.2 表 7.2 の注 | MNS の record に MapSupportedFeatures が無ければ、MSE は 0x0000001F（通知の登録・通知・browsing・uploading・delete）を仮定する | MAP 1.1 の MNS の record（MapSupportedFeatures 無し）でも MSE は送信・通知を扱う |

→ p001 §4.5 の条件が成り立った。**Q16 の決定（ユーザー、Q1 経由 2026-10-10、クリック「(a) 1.1 で RFCOMM だけ（推し）」）: MAP 1.1・PBAP 1.1 を名乗り GOEP 1.1（RFCOMM）だけ。L2CAP ERTM と GOEP 2.0 は Future Work。** OBEX の Connect と SDP の record の版を code にしてよい（Q1）。

(a) の帰結（p003・p005 の設計へ）: MNS の record は MAP 0x0101 の版、MapSupportedFeatures と GoepL2CapPsm を出さない。OBEX の Connect に MapSupportedFeatures・PbapSupportedFeatures の App Parameters を付けない。PBAP の Folder Version Counters・Database Identifier は使わない。MAP の event は 1.0 の形（送り主の名前と時刻が無い → Get で読む）。
p002 の中で Q16 に関わるのは OBEX の Connect の App Parameters だけで、p002 では付けない。SDP の record の版は p003（MNS）・p005（PCE）・p006（HF）が登録する時に決まり、p002 の daemon は record を 1 つも登録しない（§7.1）。

## 2. 範囲

この Phase で作る物:

1. session の受けの drop の印と回復（[T1]）、送りの link ごとの上限と round-robin（[T2]、N5）。
2. router の phone の持ち主と hook、SCO の要求を断る、drop の知らせの配り、接続の照合（[T1]）。
3. L2CAP の channel の持ち主の移し替え（[T3]）、pairing の `phone=1` と uid、phone の handoff（[T3]、N4）。
4. 接続の調停 `linkmgr.c`（BR/EDR の page scan と page、[N3]）。
5. RFCOMM（`rfcomm.c`）、OBEX（`obex.c`）、SDP の server（`sdps.c`）と client の一般化（`sdp.c`）。
6. phone link の輸送の部分（`phone.c` の p002 の分: 1 台の link の L2CAP・SDP の server・RFCOMM の session・OBEX の client の試し）と、btsnoop の phone の payload を伏せる事。
7. host の試験と fuzz、T1 への HID の回帰の依頼（[T4]）。

作らない物（後の Phase）: 持ち主の記録 `.phone` と再接続（p003）、出力の queue と SUBSCRIBE（p003）、MAP・PBAP・HFP の profile（p003・p005・p006）、SCO（p007）。LE の接続（HID の LE の auto-connect と pair の LE）は linkmgr に入れない（§6.1）。

## 3. session の受けの drop（[T1]）

### 3.1 今

`session_enqueue`（session.c:1563-1611）は同期の command の待ちの間に来た packet を 32 KB の ring（`BTD_QUEUE_BYTES`）に入れ、入らない物を区別なく捨てて `queue_dropped` を数えるだけ。接続の事象（Connection・Disconnection Complete）は捨てる前に session の link の数えに入るので（1584-1586）、捨てられると router と持ち主は接続の始まり・終わりを知らない。kernel は捨てない（`include/uapi/bluetooth.h` の冒頭: 読まない間は controller が持つ）ので、drop は session の ring と、owner の L2CAP の組み立て（`btd_reassembly_feed`、続きの断片の欠け）だけ。

### 3.2 入れる規則（ring の余白の予約）[m1, m2]

record を 4 つの class に分ける。

| class | 何 | 入る条件（`free` = 32 KB − 使用） | 捨てた時の印 |
| --- | --- | --- | --- |
| 数えた事象 | Connection・Disconnection Complete（`SESSION_RECORD_COUNTED`） | `need ≤ free` | `counted_lost` |
| 事象 | 上と scan の report 以外の事象（Encryption Change、Authentication Complete、L2CAP に関わらない接続の事象、pairing の事象） | `need ≤ free − BTD_QUEUE_COUNTED_RESERVE`（1 KB） | `events_dropped`（§3.4 の EVENT） |
| ACL | ACL の packet | `need ≤ free − BTD_QUEUE_EVENT_RESERVE`（4 KB）、かつその link が封の中でない（§3.3） | その link の印と封 |
| scan の report | Inquiry Result（0x02・0x22・0x2F）、LE の Advertising Report（LE Meta の 0x02・0x0D） | ACL と同じ条件 | `scan_dropped` を数えるだけ（印も知らせも無い。scan の表は欠けても害が無い） |

- 積む時に処理して積まない物: Number Of Completed Packets（今どおり）、Hardware Error（0x10）と Data Buffer Overflow（0x1A）（`session_dispatch` と同じ処理を enqueue で行い、積まない。今は ring が満ちると捨てられ得る、session.c:690-707）[m1]。
- 数えた事象の 1 KB の予約: LE の scan の洪水でも Connection・Disconnection Complete は入る。それでも入らない時（1 回の待ちに約 70 の接続の事象）は `counted_lost`（§3.5 の照合で回復）。

### 3.3 印と封（link ごと）[B1, M1, M2]

- `struct btd_link_count` に足す: `address`（Connection Complete の address、[m3]）、`sealed`、`skip_continuing`、`drop_flags`（`BTD_DROP_SIGNAL`・`BTD_DROP_DATA`・`BTD_DROP_UNKNOWN`）、`drop_cid`（最初に捨てた data の CID）、`drop_count`、`notice_after`（§3.4）、`last_cid`。
- **封**: link H の ACL を 1 つ捨てたら H を封じる。封の間は H の ACL を全部捨てて数える。ring に積む時も、ring を通らず直に読んで dispatch する時も同じ（`session_read` の後、`session_dispatch` の前の共通の判定）。**封は H の知らせを handler に渡す直前に解く**（待ちの境では解けない）。他の link は封じない（phone の洪水で keyboard の link を巻き込まない）。
- **続きの断片を捨てる**: H の知らせを渡した後、H の ACL は first の断片（boundary が continuing でない物）が来るまで続きの断片を捨てる（`skip_continuing`、数えるが印は立てない）。持ち主の組み立て（`btd_reassembly_feed`）は知らせを受けた時に途中の frame を捨てる（`reassembly.active = 0`、§3.5）。この 2 つで、捨てた frame の続きが別の frame の頭につながらない。
- **`last_cid`** は着いた順にだけ更新する: node から読んだ時（`session_read` の直後）に、ACL の first の断片なら その L2CAP の CID（4 byte 未満なら 0 = 不明）。ring から取り出した packet の dispatch では更新しない [M2]。
- 捨てた ACL の印: first の断片なら その CID、続きの断片なら `last_cid`。CID が 0x0001・0x0005 なら `SIGNAL`、0 なら `UNKNOWN`、他は `DATA` と `drop_cid`（最初の 1 つ）。数えていない handle の ACL は印を付けない（持ち主が居ない）。
- session 全体: `events_dropped`、`counted_lost`、`scan_dropped`。
- `session_link_add`・`session_link_remove` は record の drop の状態（sealed・skip_continuing・drop_*・notice_after・last_cid）と送りの上限（frame_limit・inflight_limit）を初期値に戻す（handle を使い直した新しい link が古い封や phone の上限を引き継がない）。知らせを待つ間に Disconnection Complete で link が消えたら、その link の知らせは出さない（持ち主は切断を受ける）[review-2 の 1]。

### 3.4 知らせ（捨てた所より前の物を全部渡した後）[B1, M3, m4]

- ring の record に通し番号の考えを入れる: `enqueued`（積んだ record の数）と `dequeued`（取り出した数）を数える（ring は FIFO なので番号を record に書かない）。H の最初の drop の時に `H.notice_after = enqueued`（その時までに積んだ物）。期日の比較は unsigned の差（`(uint32_t)(dequeued − notice_after) < 0x80000000`）で、数が回っても誤らない [review-2 の 2]。session 全体の印も同じ（`events_notice_after`）。
- `btd_session_input` は ring から取り出す前に、`dequeued ≥ notice_after` の知らせがあれば先にそれを 1 つ渡す（印を消し封を解いてから handler を呼ぶ: handler の中の command の待ちで同じ link に立つ新しい印を後で消して失わない）。ring が空の時は全部の知らせが期日。
- 知らせは合成の packet `[BTD_PACKET_DROP (0xF0), handle 下位, handle 上位, flags, cid 下位, cid 上位, count 下位, count 上位]`（8 byte）。session 全体は handle 0xFFFF、flags `BTD_DROP_EVENT`・`BTD_DROP_COUNTED`。0xF0 は bluetoothd の中だけの値で `session.h` に定義する（UAPI の kernel の知らせ（0x80 以上の 1 byte の packet）とは長さで区別でき、node からは来ない）。btsnoop には書かない [m4]。
- `btd_session_pending` は ring に物があるか、期日の知らせがある時 1 [M3]。daemon の loop は pending の間 poll で待たない（今の main.c の 0 の timeout の規則のまま）。
- 結果: 持ち主は「捨てた物より前の物」を全部見た後に、封の間の後の物を見る前に知らせを受ける。封の中の後の物は捨てられているので、知らせの前に後の物が渡る事は無い。

### 3.5 持ち主の回復（段階）[M6]

| 知らせ | router | phone（p002） | HID（WS143 の同じ危険の直し） | pair |
| --- | --- | --- | --- | --- |
| handle 0xFFFF・`COUNTED` | **照合**: session の数えた link（handle と address、`btd_session_links`）と自分の route（handle と address）を比べる。route にあって session に無い（か address の違う）handle は Disconnection Complete（status 0、reason 0x08）を合成して持ち主に渡し route を消す。session にあって route に無い handle は Disconnect（route の無い接続は今も切る規則）[m3] | — | — | — |
| handle 0xFFFF・`EVENT` | 全部の route の持ち主に、その route の handle を付けて渡す | 認証・暗号化・鍵の長さの答えを待つ状態なら ACL を切る。他は数えるだけ | 状態が PAGING・AUTHENTICATING・ENCRYPTING・SDP（BR/EDR）か LE の暗号化の待ちなら `hid_fail(…, "lost-packets")`。他は数えるだけ | pairing 中なら `btd_pair_stop("lost-packets")` |
| `SIGNAL` | 持ち主へ | ACL を切る（作り直し。p002 は再接続しない） | `hid_fail(…, "lost-packets")` | pairing を止める |
| `DATA`・CID が RFCOMM・SDP の channel | 持ち主へ | その L2CAP の channel だけを閉じる（Disconnection Request）。RFCOMM なら DLC と進行中の OBEX の操作が失敗、SDP なら query の失敗 | — | — |
| `DATA`・その他の CID | 持ち主へ | ACL を切る | `hid_fail`（control・interrupt・LE の ATT 0x0004 のどれでも。key-up の欠けで押されたままの key を残さない。bridge を閉じると key は離れる（WS143 の hid-input、確かめる）） | 持ち主の link の DATA（SMP 0x0006 を含む）なら何でも `btd_pair_stop` |
| `UNKNOWN` | 持ち主へ | ACL を切る | `hid_fail` | pairing を止める |

- 全ての持ち主は、handle の知らせを受けた時にその link の組み立てを捨てる（`reassembly.active = 0`、§3.3）。HID・pair の組み立ての扱いはこの 1 行だけ変える。
- 組み立ての欠け（`btd_reassembly_feed` の -1）は持ち主が自分の `reassembly.dropped` で見る。phone は RFCOMM の channel の frame の欠けを `DATA` と同じに扱う。HID は今のまま（WS143 の範囲）。
- p001 §5.3 の credit の合計の上限（8 KB、§8.5）で、phone だけで ring の 4 KB の余白を越えて送らせない（credit の無い相手は断る、§8.4）。

## 4. session の送り（[T2]、N5）

### 4.1 今

`btd_session_send` は全 link で 16 frame（`BTD_SEND_FRAMES`）を越えると ENOBUFS（session.c:452-456）、`session_flush` は一番古い frame から順に送り、その link の pool に buffer が無いと後の全部の link が待つ（1889-1904）。HID は ENOBUFS を捨てる（hid.c:2521・3152）。

### 4.2 変更

- `struct btd_link_count` に `frames`（表に待つその link の frame の数）、`frame_limit`（既定 `BTD_SEND_FRAMES`）、`inflight_limit`（既定 0 = pool の総数）を足す。
- `btd_session_set_link_limits(session, handle, frame_limit, inflight_limit)`（新）。phone は link ごとに frame 8（`BTD_SEND_PHONE_FRAMES`）、in-flight は `max(1, pool->total − 2)`（pool は `session_pool(link)`。LE が BR/EDR の pool を共有する時（`le_shared`）も同じ総数から 2 を残す: 残る 2 は LE の HID と他の BR/EDR の link が使える）。
- `btd_session_send`: link の `frames ≥ frame_limit` か全体 16 で ENOBUFS。phone が 8 を使っても HID 等に 8 が残る。
- `btd_session_link_room(session, handle)`（新）: その link が今入れられる frame の数（phone の queue が session へ移す量）。
- `session_flush` を round-robin に: `session->flush_next`（link の表の添字）から link を 1 つずつ見て、その link の一番古い frame の次の 1 packet（first か continuing）を送れるか（pool に空きがあり、`inflight_limit` があれば `outstanding < inflight_limit`）。送れたら次の link へ。全 link が 1 周して何も送れなければ終わり。表の中の frame の順（同じ link の中）は保つ。別の handle の断片が間に入るのは HCI で許される。
- 試験（host、偽の controller）: phone の link が frame 8 と in-flight を使い切って buffer を待つ間に、HID の link の signalling（6 frame）が ENOBUFS にならず、phone の後ろで待たずに送られる。pool の総数 1・2・3 で in-flight の上限が 1 以上。LE の共有の pool。

## 5. router（[T1]、§3.3 の router の行）

- `BTD_OWNER_PHONE`（3）と `struct btd_router_phone { context; wants; claims; handle; }`（`struct btd_router_hid` と同じ形）、`btd_router_set_phone`。
- 接続の持ち主の判定（`router_owner_of`）: pair → HID の claims → phone の claims。
- route に address を足す（`struct btd_route` の `address`。Connection Complete の address。照合 [m3] と SCO の断り [M4] が使う）と、その ACL の暗号が AES-CCM か（Encryption Change（0x08）の Encryption_Enabled が 0x02、Encryption Change v2（0x59）も同じ。route を持つ handle の事象を持ち主へ渡す時に router が記録する）。
- Connection Request（event 0x04、address・class 3 byte・link type）: **link type が ACL（0x01）の時だけ** HID の wants → phone の wants。SCO（0x00）・eSCO（0x02）は p007b までは全部断る: **Reject Synchronous Connection Request（0x042A、BD_ADDR と Reason）**、reason は同じ address の ACL の route が AES-CCM なら 0x0E（Core Vol 4 Part E 7.1.28: AES-CCM で暗号化した link への SCO は reason 0x0E で断る shall）、他は 0x0D（資源の不足）。Core 7.7.4 は SCO・eSCO の要求に Accept・Reject Synchronous で答えるとする [M4]。今の code は link type を見ず HID の wants に渡すので、SCO を HID が受ける事を防ぐ直しにもなる。
- Synchronous Connection Complete（0x2C）・Changed（0x2D）は明示して捨てる（数える）。Connection Complete（0x03）は link type が ACL の物だけを持ち主の判定（`router_connected`）と linkmgr の hook へ渡し、SCO の link type の成功した Connection Complete（断った後に来るはずは無い）は Disconnect する [M4]。
- Link Key Request: pair → HID → phone → negative。
- 合成の 0xF0 の知らせ（§3.4）: handle が route にあれば持ち主へ、0xFFFF は §3.5 の行どおり。
- `btd_router_reconcile`（新、§3.5）: session の数えた link の handle の一覧（`btd_session_links`（新）で読む）と route を比べる。
- page の結果の hook（[N3]）: `router_connected` は持ち主へ渡す前に、`btd_linkmgr_connected(lm, address, status)` を呼ぶ（link type が ACL の BR/EDR の Connection Complete の全部、成功も失敗も）。router は linkmgr の pointer を持つ（`btd_router_set_linkmgr`）。

## 6. linkmgr（[N3]、p001 §3.6）

### 6.1 範囲

BR/EDR の page scan（Write Scan Enable 0x0C1A）と BR/EDR の page（Create Connection 0x0405）だけ。LE の Create Connection と auto-connect は今の HID の調停（`hid_le_disarm` で 1 つずつ）のまま: phone は BR/EDR だけで LE を使わないので、LE を linkmgr に入れる利得が無い。

### 6.2 API と規則

- `btd_linkmgr_init(lm, session)`、`btd_linkmgr_reset(lm)`（controller の start の後: 状態を知らないとして次の want で必ず書く）。
- `btd_linkmgr_want_scan(lm, who, on)`: who は `BTD_LINKMGR_HID`・`BTD_LINKMGR_PHONE`。誰かが on なら page scan（0x02）、誰も居なければ off（0x00）。値が変わった時だけ書く。失敗は次の want か tick で書き直す。
- `btd_linkmgr_page_begin(lm, who, address)`: BR/EDR の page が 1 つも出ていなければ 0（呼び手が Create Connection を出す）、出ていれば EBUSY。`btd_linkmgr_page_end(lm, who, address)`: 呼び手が Create Connection の失敗（同期の答えの status）や自分の締め切りで呼ぶ。`btd_linkmgr_connected`（router から、§5）は address が一致すれば成功・失敗とも終わり。`btd_linkmgr_tick`: 15 秒を越えた page は終わりにして log（呼び手の終わりの呼び忘れの守り）。
- HID の変更（WS143 の code、小さく）: `hid_page_scan` は `btd_linkmgr_want_scan(lm, HID, 1)`。`hid_page` は先に `page_begin`、EBUSY なら `retry_at = now + 2 s` で待つ（再試行の回数に数えない）。**PAGING の状態からの全部の出口で `page_end`**: Create Connection の同期の失敗、`hid_connected`（成功・失敗）、PAGING の state の時間切れ（`btd_hid_tick`）、`hid_cancel_page`、controller の喪失（`btd_hid_lost`）。router の hook と二重でも害が無い（address の一致しない end は無視）[m11]。
- pair の変更: BR/EDR の pairing の Create Connection（pair.c:290）の前に `page_begin`、EBUSY なら `btd_pair_start` は EBUSY（今の「HID の接続の途中は busy」と同じ答え `ERROR busy`）。Create Connection の同期の失敗（Command Status の失敗）、pairing の Connection Complete、pairing の時間切れ・取り消しで `page_end` [m11]。LE の pairing（pair.c:275）は変えない。
- 調停しない事（記録）: LE の Create Connection と BR/EDR の page の同時。controller によっては Command Disallowed を返す。今の HID と pair はその失敗を「unreachable」・pairing の失敗として扱い、再試行の規則に乗る（p008 で実機の頻度を見る）[m11]。
- Write Scan Enable を出すのは linkmgr だけになる（今は hid.c:2366-2384 だけ）。

## 7. pairing と handoff（[T3]、N4）

### 7.1 `PAIR` の `phone=1` と uid

- socket の request: `PAIR ADDRESS TYPE [phone=1]`。`phone=1` は BR/EDR の address の型だけ（LE は `ERROR phone-le`）。
- `struct btd_pair` に `phone`（意図）と `uid`（request の client の uid）を足す。`btd_pair_start` の引数に足す（main.c の btd_pair の呼び出しを変える）。
- **phone=1 と保存の鍵** [m9]: phone=1 の pairing で、controller の Link Key Request に保存の鍵が認証済み（§7.4 の 2）でない時は、保存の鍵を使わず Negative Reply を返して SSP をやり直させる（今の `pair_key_request` は保存の鍵があれば使う、pair.c:1000-1008）。スマホが MITM の守りを持つ IO を出さなければ、やり直しても Just Works になり §7.4 の 2 で断る。
- `phone=1` の pairing の間、pair の l2cap の表に accept の hook を付ける: PSM 0x0001（SDP）と 0x0003（RFCOMM）は **Pending**（status 0x0000）、他は今どおり断る。`phone` の無い pairing は今どおり hook 無し（全部 PSM not supported）。pairing の前の SDP が要るかは未確認で、Pending にして handoff の後に phone が答える形なら、要っても要らなくても害が無い。

### 7.2 handoff の鎖

- pair に 2 つ目の hook `btd_pair_set_phone_handoff(pair, fn, context)` を足す。HID の hook の型（pair.h:69、hid.c が実装）は変えない。
- `pair_hand_over`: `pair->phone` で phone の hook があれば先に phone の hook、受けなければ HID の hook（今の順と規則のまま）。
- **phone が受けない時**: HID の hook へ進む前に、pair の表の Pending の channel（§7.1 の hook が Pending にした PSM 1・3）に断りの Connection Response（result No resources 0x0004）を送る（`btd_l2cap_answer_pending(…, BTD_L2CAP_NO_RESOURCES, …)`）。HID が受けると pair の表は今どおり `btd_l2cap_drop` で消えるので、断りを先に送らないと相手の channel が黙って消える [m6]。
- **PAIRED の行** [m10]: 今は handoff の前に作られる（pair.c:1478-1497）。phone=1 の時は handoff の hook の後に ` phone=1` か ` phone=0 why=<理由>` を足してから答える（192 byte の答えに収まる）。
- phone の hook の引数は構造体 `struct btd_pair_handoff`: address・type・handle・bond（読んだ物）・uid・key_size（pair が Read Encryption Key Size で読んだ値）・class_of_device（§7.4）・`struct btd_l2cap *l2cap`（pair の表、移し替えの元）・`struct btd_reassembly *reassembly`（組み立て中の frame）。

### 7.3 L2CAP の移し替え `btd_l2cap_move`（[T3]）

`int btd_l2cap_move(struct btd_l2cap *from, struct btd_l2cap *to, uint16_t handle, unsigned *moved, unsigned *left)` と `int btd_l2cap_refuse_left(struct btd_l2cap *from, uint16_t handle, uint8_t *answer, size_t size, size_t *length)` [review-2 の 3]

- `from` の表の、その handle の FREE でない channel を全部、`to` の**同じ枠**（local CID は枠の番号そのもの: `local_cid − BTD_CID_DYNAMIC`、l2cap.c:218・712、`signal_find` が CID から枠を引く 1113-1121）へ、状態・CID・相手の CID・MTU・identifier・inbound をそのまま写し、`from` の枠を FREE にする [M5]。`to->next_identifier` は `from` の値にする（未答えの request の identifier と重ねない）。`information_pending`・`echo_pending` も写す。
- `to` のその枠が空いていない channel（phone の表は handoff の時は空なので、起きるのは表の誤用の時だけ）は移さず `from` に印を付けて残す。呼び手は `btd_l2cap_refuse_left(from, handle, answer, size, &length)` を繰り返し呼び、1 回に 1 つの signalling の command（OPEN・CONFIGURING は Disconnection Request、PENDING は Connection Response の No resources）を受け取り、**1 つずつ別の C-frame で送る**（BR/EDR の MTUsig の最小は 48 byte、Core Vol 3 Part A 表 4.1。1 つの command は 12 byte 以下）。返す物が無くなったら 0、`from` から消す [m7]。
- 移った channel の accept は `to` の hook の物になる。PENDING の channel は `to` の持ち主が `btd_l2cap_answer_pending` で答える（phone は handoff の直後、link が暗号化済みで鍵 16 byte なら成功で答える）。
- 組み立て: phone は `*reassembly` を自分の組み立てに写す（frame の途中の断片を失わない）。pair は今どおり自分の物を消す。
- 試験: pairing の間にスマホが SDP（PSM 1）を開け Pending、handoff の後に Connection Response（成功）が出て configuration が続き、移した後に local CID で `btd_l2cap_channel` が引ける [M5]。`to` の枠が埋まっている時の `refuse_left` が 1 つずつ返す。phone が受けず HID が受ける時の断り（m6）。

### 7.4 phone が受ける条件の順（N4）

phone の hook は次の順で見て、1 つでも外れたら受けない（0 を返す。bond は普通の bond として残り、pairing は HID の hook へ、HID も受けなければ今どおり接続を切る）:

1. `phone` の意図と BR/EDR。
2. bond に link key があり、その型が認証済み（Core Vol 4 Part E 7.7.24 の Key_Type: 0x05 Authenticated P-192、0x08 Authenticated P-256）。他（0x04・0x07 の Just Works、0x00 の legacy、0x03 debug）は `why=unauthenticated`（p001 §3.4、S19）。
3. 鍵の長さ 16（`key_size`）。他は `why=key-size`。
4. Class of Device: pairing の時の scan の表（`session->devices`）から読む（hid.c:837-848 と同じ）。major class（bit 8-12）が 0x02（Phone）でなければ `why=not-phone`。**表に無い時（scan の外から PAIR した時）は受け、log に `cod=unknown`**（ユーザーが「スマホとして」と選んだ意図を優先。Connection Request から来た接続は p003 の再接続で、その事象の class を使う）。
5. phone の枠（1 台、Q1）が空いていて、HID が同時につないでいる数が 5 以下（§7.5）。他は `why=busy`・`why=busy-links`。
6. （p003）持ち主の記録 `.phone` を書く。書けなければ `why=store` で受けない。

PAIR の答え: 今の `PAIRED …` の行に ` phone=1` か ` phone=0 why=<理由>` を足す（phone=1 を頼んだ時だけ、§7.2）。

受けた時: phone は `btd_router_assign(router, handle, BTD_OWNER_PHONE)`（HID の hid.c:887 と同じ）で route を持ち、L2CAP を移し（§7.3）、Pending の channel に答える [m5]。

### 7.5 接続の数

session の link は 8（`BTD_LINKS_MAX`）＝ pairing 1 ＋ 断るための 1 ＋ HID 6。phone link が 1 つある間（p002）か「スマホとして使う」が有効な間（p003）は、**HID が同時につなぐ数**の上限を 6 から 5 にする（`btd_hid_set_limit`（新）。HID の記録の枠（`BTD_HID_MAX` 6）の数は変えず、つなぐ・受ける時に、つないでいる・つなぎかけの数が上限以上なら新しい接続を始めない・受けない）[m8]。`phone=1` の pairing の始めと handoff の時（§7.4 の 5）に、HID のつないでいる数が 6 なら `ERROR busy-links`・`why=busy-links`（pairing の間に 6 台目が入る競合は handoff の時の確かめで捕まえる）。

## 8. RFCOMM（`rfcomm.c`）

### 8.1 frame（TS 27.010 §5.2、RFCOMM 1.2 §5.1）

- address: EA（bit 1、常に 1）・C/R（bit 2）・DLCI（bit 3-8、RFCOMM では D（bit 3）と server channel（bit 4-8））。control: SABM 0x2F、UA 0x63、DM 0x0F、DISC 0x43、UIH 0xEF、P/F は 0x10。length: EA が 1 なら 7 bit（1 byte）、0 なら 15 bit（2 byte、little-endian の順に EA と L1-L7、L8-L15）。credit: UIH で P/F=1 かつ credit の流れの制御の session の時の 1 byte。FCS: 1 byte。
- FCS（TS 27.010 §5.2.1.6 と Annex B）: 多項式 x⁸+x²+x+1 の反転の表、初期 0xFF、最後に 0xFF から引く。範囲は SABM・UA・DM・DISC は address・control・length、UIH は address・control だけ（RFCOMM 1.2 §5.1.1）。受けの検査は表に通して 0xCF。試験の正解: Annex B の例（`0x07 0x3F` → FCS `0x89`、検査で `0xCF`）と、§5.2.1.6 の多項式の割り算の定義どおり bit ごとに計算した値（表の方法とは別の計算、2026-10-10 P1 の python で Annex B の `0x89` を再現した上で）: `03 3F 01`（DLCI 0 の SABM）→ `1C`、`03 73 01`（UA）→ `D7`。UIH の値も同じ計算で試験の中に書く [m18]。
- C/R（TS 27.010 表 1、§5.4.3.1、RFCOMM §5.1.3）: SABM・DISC（command）は initiator（DLCI 0 の SABM を送った側）が送れば 1、responder が送れば 0。UA・DM（response）は initiator が送れば 0、responder が送れば 1。UIH は中身に関わらず initiator が 1、responder が 0。多重化の message の中の C/R（type の bit 2）は別で、command 1・response 0。
- 受けの検査: address と length の EA、length が その DLCI の N1（credit 付きは N1 − 1、RFCOMM §6.5.2）以下、FCS、control が上の 5 つ。DLCI 0 と、PN を交わさずに SABM で開いた DLC の N1 は既定の 127（RFCOMM 表 5.1）[m12]。外れた frame は数えて捨てる（session は切らない。RFCOMM は L2CAP の上で信頼できる前提なので、続けて 4 つ外れたら session を閉じる）。

### 8.2 session と DLCI（RFCOMM §5.2・§5.4）

- 1 本の ACL に session は 1 つ（PSM 0x0003 の L2CAP の channel 1 本）。session の initiator（L2CAP を開け DLCI 0 に SABM を送った側）の D は 1、responder は 0。**DLC を開ける側は、相手の server channel と自分の D の反転で DLCI を作る**: `dlci = (server_channel << 1) | (D ^ 1)`。つまり initiator の server の DLCI は奇数（3〜61）、responder の server は偶数（2〜60）。同じ session の上で zedBSD の server（MNS 16、HF 17）とスマホの server（MAS・PSE・AG）の両方を使う。
- 衝突（RFCOMM §5.2.1 の注）: 自分の PSM 3 の channel が CONNECTING・CONFIGURING・OPEN の間に相手の PSM 3 の Connection Request が来たら、相手のを断る（result No resources。session は 1 つだけ）[m15]。自分のが断られたら 100〜500 ms の乱数の後に 3 回まで繰り返す。p001 §5.2 の仮の方針（自分のを閉じる）は仕様に合わせて改めた。
- 始め: L2CAP の channel が開いたら initiator は DLCI 0 に SABM（P=1）、UA を待つ（T1、20 s）。responder は SABM に UA。DLCI 0 が開くまで他の frame は DM。
- 終わり: 最後の DLC を閉じた側が L2CAP の channel を閉じる（その前に DLCI 0 の DISC を送ってよい、§5.2.2）。zedBSD が最後の DLC を閉じた時は 2 s 後に DISC（DLCI 0）と L2CAP の Disconnection Request。相手が最後の DLC を閉じた時は相手が閉じるのを 2 s 待ち、来なければ zedBSD が同じ手順で閉じる。両方の DISC・Disconnection Request が重なっても、相手の DISC には UA、閉じかけの channel への Disconnection Request には Response を返すだけで害が無い [m15]。相手の DISC（DLCI 0）には UA。
- link の喪失（L2CAP の channel が閉じた、ACL が切れた）: 全 DLC の持ち主に切断を知らせ、session を消す（§5.2.3）。

### 8.3 DLC

- 開ける（client）: PN（command、§8.4）→ PN の response → SABM（P=1）→ UA で開く（DM なら断られた）→ 自分の MSC の command を送り、相手の MSC の command に response を返す。**自分の MSC の response を受け、相手の MSC の command を受けるまで data を送らない**（RFCOMM §6.3 の注 2、TS 27.010 §5.4.6.3.7）。T1 は DLC の SABM で 60 s（§5.3）、多重化の command の T2 は 20 s。時間切れは session を閉じる（§5.3）。
- 相手が PN を交わさずに SABM で開けた DLC（PN は 2 つ目以降の DLC では任意、RFCOMM §6.5.1）: その session で credit の流れが既に決まっている（前の DLC で PN を交わした）時だけ受け、N1 は 127、相手の初期の credit は 0 とし、UA の後に自分から credit を与える（§8.5）[m12]。session の最初の DLC が PN 無しの SABM で来たら credit の流れが有効でないので DM（m13 と同じ理由）[review-2 の 5]。
- 受ける（server）: 相手の PN に、その DLCI の server channel が登録され有効で持ち主が居れば response（§8.4）、居なければ DM（RFCOMM §5.5）。SABM: 同じ規則で UA か DM。security の始動は SABM の時だけ（§5.3 の最後の段落）: phone link は暗号化と鍵 16 byte を L2CAP の accept で済ませているので、SABM では確かめだけ。
- 閉じる: DISC（P=1）→ UA（相手が既に閉じていれば DM）。相手の DISC には UA。DISC か自分の閉じで、その DLCI の PN の値と credit は既定に戻す（§5.5）。
- 数: 1 session に DLC 6 つまで（MAS・MNS・PSE・AG と予備）。

### 8.4 多重化の command（DLCI 0 の UIH、TS 27.010 §5.4.6）

- 形: type の byte（EA=1、C/R、T1-T6）、length の byte（EA、7 bit）、値。RFCOMM は 1 frame に 1 message（§5.5）。
- PN（type 0x80 の組で 0x83 command・0x81 response、値 8 byte、TS 27.010 表 3）: DLCI（6 bit）、I=0 と CL（command は 0xF、response は 0xE、RFCOMM §5.5.3 表 5.3）、priority（0〜63、0 を送る）、T=0、N1（2 byte、little-endian）、NA=0、K（credit の初期値 0〜7）。受けた response の N1 は自分の申し出以下（違えば DISC）。CL が 0xE でない response は credit の無い相手（v1.0B）: 受けの量を縛れず §8.5 の予算（[T1] の前提）が成り立たないので、log して その DLC に DISC（最近のスマホでは来ない見込み）[m13]。まだ開いていない DLC への相手の PN の command の CL が 0xF でない時も同じく DM。開いた DLC への PN（CL は 0、RFCOMM §5.5.3）には今の値をそのまま返す（responder は変えなくてよい）[review-2 の 4]。
- MSC（0xE0 の組、値 2 byte: DLCI（EA=1、C/R=1 の address の形）と V.24 の byte）。送る V.24: EA=1、FC=0（credit の session では意味を持たない、§6.5.3）、RTC=1、RTR=1、IC=0、DV=1（= 0x8D）。break の byte は送らず、受けたら無視。
- RPN（0x90 の組）: 値 1 byte（問い合わせ）か 8 byte。response は値 8 byte、受けた値をそのまま（問い合わせには 9600 bit/s・8N1・流れの制御なしの既定）。
- RLS（0x50 の組）: response は受けた値をそのまま。
- Test（0x20 の組）: 同じ値で response。
- FCon（0xA0）・FCoff（0x60）: response を返す。credit の session では送らず、受けても流れは変えない（§6.5.3）。
- NSC（0x10 の組）: 知らない type の command に、その type の byte を値に返す（RFCOMM §4.3）。
- 多重化の command の response を待つのは 1 DLC につき 1 つ（PN と MSC は順に）。

### 8.5 credit（RFCOMM §6.5、[T1]、p001 §5.3）

- 自分の送り: DLC ごとの `tx_credits`。0 なら data を持つ UIH を送らない（data の無い UIH は送れる）。受けた credit は足す（255 を越えたら 255 で止め log）。
- 相手への credit: **session 全体の予算** `BTD_RFCOMM_CREDIT_BYTES`（8 KB）: 相手に与えて未使用の credit の合計 × N1 ≤ 8 KB。開いた DLC ごとに少なくとも 1。PN の K は `min(7, 予算の割り当て)`。相手が frame を 1 つ使うと、上の部品（OBEX の組み立て、AT の行）がその frame を処理し終えた時に返す（今の部品は同期で処理するので、受けた loop の中で返る）。返す credit は「返す数」として DLC に数えて持ち、P/F=1 の UIH（data が無ければ data 0 byte）で送る。送りの queue に置けなかった（ENOBUFS）時は数えたまま次の loop で送る（[R5]）。
- 相手が credit を越えて送った data は数えて捨て、session を閉じる（相手の誤り）。

### 8.6 大きさ

- N1 の申し出: `min(自分の L2CAP MTU 672, 相手の MTU) − 5`（address 1、control 1、length 2、FCS 1。credit の byte は N1 − 1 の規則で中に入る、RFCOMM §6.5.2）[m14]。L2CAP の frame は 1024 byte（`BTD_L2CAP_MAX`）まで組み立てるので、672 の MTU で足りる。
- 1 つの UIH の data は N1（credit 付きは N1 − 1）以下に分けて送る。

### 8.7 API（`rfcomm.h`、system call を持たない）

- `struct btd_rfcomm`（1 session: 状態、initiator、L2CAP の local・remote の CID、MTU、N1 の既定、DLC の表、統計、送りの関数の pointer（`int (*send)(void *, const uint8_t *, size_t)`：L2CAP の payload を phone の queue へ、ENOBUFS を返し得る）、時刻）。
- `btd_rfcomm_init`、`btd_rfcomm_open_session`（initiator として、L2CAP の channel が開いた後）、`btd_rfcomm_input(rf, payload, length, now)`（L2CAP の 1 frame）、`btd_rfcomm_listen(rf, server_channel, accept_fn, ctx)`、`btd_rfcomm_connect(rf, server_channel, events, ctx, &dlc)`、`btd_rfcomm_write(rf, dlc, data, length)`（credit と N1 で切って送り、残りは返す数で知らせる）、`btd_rfcomm_close(rf, dlc)`、`btd_rfcomm_tick(rf, now)`、`btd_rfcomm_lost(rf)`。上へは callback（`opened`・`data`・`closed(reason)`・`writable`）。
- 送りは全部 `send` の hook を通す。hook が ENOBUFS を返した frame は rfcomm の中の小さい queue（DLCI 0 の制御 4 frame）に残し、data の UIH は呼び手に「今は書けない」を返す。

## 9. OBEX（`obex.c`、GOEP 1.1 の client と server）

### 9.1 packet と header（p001 §6.1、(IrOBEX、確かめる)）

- packet: opcode か response code（1 byte、最上位 bit が Final）、長さ（2 byte、big-endian、packet 全体）、Connect は版 0x10・flags 0x00・最大 packet 長（2 byte）、SetPath は flags（bit 0 が「上へ」、bit 1 が「作らない」）と constants 0、続いて header の列。
- header の型（id の上位 2 bit）: 00 Unicode（2 byte の長さ（HI と長さの 3 byte を含む）、UTF-16BE、NUL 終わり、空（長さ 3）は正しい）、01 byte の列（同じ長さ）、10 1 byte、11 4 byte。使う header: Name 0x01、Type 0x42、Length 0xC3、Target 0x46、Who 0x4A、Connection ID 0xCB、Body 0x48、End of Body 0x49、Application Parameters 0x4C（TLV: tag 1・長さ 1・値）。
- opcode: Connect 0x80、Disconnect 0x81、Put 0x02（Final 0x82）、Get 0x03（0x83）、SetPath 0x85、Abort 0xFF。response: Continue 0x90、Success 0xA0、Bad Request 0xC0、Unauthorized 0xC1、Forbidden 0xC3、Not Found 0xC4、Not Acceptable 0xC6、Precondition Failed 0xCC、Not Implemented 0xD1、Service Unavailable 0xD3。
- 検査: 長さ ≥ 3、packet の長さが交渉の最大以下で受けた量と合う、header の長さが packet の残りを越えない、Unicode が偶数の長さで NUL 終わり（空を除く）、App Parameters の TLV が header に収まる。知らない header は型で長さが分かるので飛ばす。Connection ID は request の最初の header（p001 R17）。

### 9.2 RFCOMM の上の packet の組み立て

DLC の data は byte の流れ。先頭 3 byte で長さを読み、長さ分そろったら 1 packet。長さが 3 未満か交渉の最大（自分の受けの上限 `BTD_OBEX_PACKET_MAX` 8192）を越えたら DLC を閉じる。1 DLC に組み立ての buffer 1 つ（8192 byte）。

### 9.3 client

- Connect: Target（呼び手が渡す 16 byte）。自分の最大は 8192。答えの最大が 255 未満なら失敗（OBEX の下限、p001 R17）。交渉の値は `min(自分, 相手)`。答えの Who と Connection ID を持つ。**App Parameters は付けない**（§1 の (a)）。
- 1 つの OBEX の接続に操作は 1 つ（MAP の MAS の queue、p001 §7.1.0 は上の層）。
- Get: 答えが Continue なら Final の Get を続け、Success で終わる。Body・End of Body を呼び手の buffer に足し、上限（呼び手が渡す、例 listing 256 KB）を越えたら Abort して失敗。
- Put: Body を「交渉の最大 − header」で分け、最後を End of Body。Body の無い Put（削除）は呼び手が明示した時だけ。filler（0x30 の 1 byte の End of Body）は呼び手（MAP）が Body として渡す。
- SetPath、Abort、Disconnect。各 request の答えの時間 10 s（大きい Get は packet ごと）。時間切れ・DLC の切断は操作の失敗で、自動で繰り返さない。

### 9.4 server（p003 の MNS が使う）

Connect（Target を呼び手が照合、Who と Connection ID を返す）、Put（呼び手へ header と Body、答えは呼び手が決める）、Disconnect、Abort。他の opcode は Not Implemented。認証（challenge の header 0x4D）の付いた Connect は Unauthorized（Q12）。

### 9.5 API（`obex.h`、system call を持たない）

`struct btd_obex`（1 つの OBEX の接続: 役、状態、交渉の最大、Connection ID、組み立て、操作の状態、送りの hook（rfcomm の DLC へ））、`btd_obex_packet_build`・`btd_obex_header_*`・`btd_obex_parse`（純粋な関数、fuzz の対象）、`btd_obex_client_connect`・`_get`・`_put`・`_setpath`・`_abort`・`_disconnect`、`btd_obex_input(ob, data, length, now)`、`btd_obex_tick`。結果は callback（`done(status, response_code)`、`body(data, length)`）。

## 10. SDP

### 10.1 server（`sdps.c`、Core Vol 3 Part B §4）

- 受ける PDU: ServiceSearch（0x02 → 0x03）、ServiceAttribute（0x04 → 0x05）、ServiceSearchAttribute（0x06 → 0x07）。他は ErrorResponse（0x01）。error code: 0x0001 版、0x0002 record handle、0x0003 syntax、0x0004 PDU の大きさ、0x0005 continuation、0x0006 資源。
- 検索の pattern（UUID 1〜12 個）: 16・32・128 bit の UUID を Base UUID で 128 bit に広げ、record の属性の値の中の UUID（入れ子を含む）に**全部**含まれる record が当たる。
- 属性の範囲の list（16 bit の id か 32 bit の範囲）、MaximumAttributeByteCount（7 未満は syntax の error）、MaximumServiceRecordCount。
- record の handle は 0x00010000 から（Core Vol 3 Part B §5.1.1: 0x00000001〜0x0000FFFF は予約、0 は SDP server 自身）。各 record は ServiceRecordHandle（0x0000）と BrowseGroupList（0x0005、PublicBrowseRoot 0x1002）を持つ。
- 部分の答え: 1 つの request の全体の答え（属性の list の列）を channel ごとの buffer（`BTD_SDPS_RESPONSE_MAX` 4096）に組み、MaximumAttributeByteCount と MTU で切る。continuation の情報は 3 byte（版の通し番号 1、次の offset 2）で、版は record の表が変わるたびに増やし、合わない continuation は 0x0005。continuation の付いた request は最初の request と同じ pattern・範囲であること（違えば 0x0005）。
- record の表: profile の部品が `btd_sdps_register(db, record_bytes, length, &handle)`・`unregister` で登録する（data element の列の形のまま）。**p002 の daemon は何も登録しない**（MNS は p003、PCE は p005、HF は p006。版は §1 の (a)）。空の表への検索は 0 件の答え。
- server は phone link の PSM 0x0001 の channel ごとに 1 つ（同時に 2 つまで）。

### 10.2 client の一般化（`sdp.c`、p001 §4.1）

- `btd_sdp_init` の service class を 16 bit のまま（MAS 0x1132、PSE 0x112F、AG 0x111F、HID、PnP。128 bit の UUID の検索は要らない）。
- 足す読み: `btd_sdp_rfcomm_channel(sdp, uuid, &channel)`（その class の record の ProtocolDescriptorList の L2CAP → RFCOMM の channel）、`btd_sdp_profile_version(sdp, uuid, profile_uuid, &version)`、`btd_sdp_uint_attribute(sdp, uuid, attribute_id, &value)`、`btd_sdp_records(sdp, uuid, …)`（同じ class の record が複数ある時の列挙、MAS の instance、p003 の MASInstanceID・SupportedMessageTypes に使う）。
- 8192 byte の上限で足りるかは host の相手か実機の record で測る（p001 §4.1、未確認のまま）。足りなければ `BTD_SDP_MAX` を 16384 にする（stack ではなく phone の構造体の中）。

## 11. phone link の輸送（`phone.c` の p002 の分）

- `struct btd_phone`（1 台）: 状態（none・handed・securing・ready・closing）、address、handle、uid、`struct btd_l2cap`、`struct btd_reassembly`、暗号化と鍵の長さ、SDP の server の channel（2）、SDP の client の query、RFCOMM の session、送りの queue（16 frame、§4.2 の上限の中で session へ移す）。
- router の hook: `wants`（**p002 は常に 0**: スマホから来る接続は受けない。受けると認証の開始（p003）が無いので channel が Pending のまま止まる。再接続と認証は p003）、`claims`（handoff した link の handle の address）、`handle`（event と ACL、0xF0 の知らせ）[m5]。
- handoff（§7.3〜§7.4）: 受けたら L2CAP を移し、session の link の上限（§4.2）を付け、HID の上限を 5 に（§7.5）、Pending の channel に答える。
- 相手の L2CAP: PSM 1 は SDP の server、PSM 3 は RFCOMM の responder。他は PSM not supported。暗号化と鍵 16 byte が無ければ Pending（再接続の時の認証の開始は p003。p002 では handoff の link は暗号化済み）。
- 切断（Disconnection Complete、drop の知らせの回復、`PHONE DROP`）: RFCOMM と OBEX を失敗にし、L2CAP を消し、HID の上限を 6 に戻す。
- 送り: `phone_pump`（daemon の loop の各回）が queue から `btd_session_link_room` の分だけ session へ移す。
- btsnoop（snoop.c と main.c の `btd_trace`）: router の持ち主が phone の handle の ACL と、**持ち主が pair で `pair->phone` が立っている handle の ACL**（handoff の前に読んだ packet、待ちの中で読んだ packet を含む）は [m16]、ACL の header 4 byte と（first 断片なら）L2CAP の header 4 byte を残し、残りを 0 で埋めて書く（長さは保つ）。事象と HCI の command は今どおり（command の中の address・名前は今と同じ扱い）。
- 試しの request（root だけ、p002 の受け入れの道具。p003 で MAP の操作に置き換える）: `PHONE PROBE ADDRESS uuid=0x1132|0x112F` → SDP で channel を読み、RFCOMM の DLC を開け、OBEX の Connect（Target: MAS か PBAP の PSE、p001 §6.2）、Get `x-obex/folder-listing`（Name 無し）、Disconnect。答えは各段の結果と byte 数だけ（中身は書かない、p001 R22）。

## 12. 試験

### 12.1 host（`plan/ws197/tests/bt-phone-host-test.c` と `.sh`、ASan・UBSan、WS143 の bt-daemon-host-test.sh と同じ flags）

| 対象 | 内容（正解の出典） |
| --- | --- |
| RFCOMM の frame | FCS（TS 27.010 Annex B: `07 3F` → `89`、検査 `CF`）、SABM・UA・DM・DISC・UIH の組み立てと分解、2 byte の length、credit 付き UIH、壊れた frame（EA、FCS、length > N1） |
| RFCOMM の DLCI と C/R | zedBSD が initiator の session で相手の server channel 5 → DLCI 10、zedBSD が responder の session で相手の server channel 5 → DLCI 11、同じ session で両方向の DLC（RFCOMM §5.4 の規則と表 1 の C/R を正解に）。両端を zedBSD の code にした試験だけでは対称の誤りを捕まえられないので、相手の側は試験の中に手で書いた byte の列（仕様から） |
| RFCOMM の台本 | SABM（DLCI 0）→ UA → PN → SABM → UA → MSC の交換 → data → DISC → DISC（DLCI 0）。最初の DLC の PN 無しの SABM → DM、2 つ目の DLC の PN 無しの SABM → UA と credit、開いた DLC への PN に今の値。MSC の前に data を送らない。credit 0 で止まる、返す credit を ENOBUFS で失わない、予算 8 KB、相手が credit を越えて送ると閉じる、CL=0 の相手、PN の N1 が申し出より大きい答え、衝突（PSM 3 の Connection Request の交差）、時間切れ、未登録の channel への SABM・PN に DM、NSC |
| OBEX | packet と header（3 byte を含む長さ、空の Unicode、App Parameters の TLV）、Connect の交渉（254 は失敗、255 は可）、Get の Continue の連続と上限の Abort、Put の分割と End of Body、SetPath、時間切れ、server の Connect・Put・認証付き Connect の拒否、RFCOMM の流れの上の組み立て（1 byte ずつの到着） |
| SDP の server | 16・32・128 bit の UUID の検索、複数 UUID の pattern、属性の範囲、continuation（切って続ける、古い版の continuation は 0x0005）、自分の record を今の client（`sdp.c`）で読み戻す、壊れた request の各 error、空の表 |
| SDP の client | MAP 1.4.2 表 7.1 の形に作った MAS の record 2 つから channel・版・SupportedMessageTypes、PBAP の PSE の record |
| session の受け（[T1]） | 偽の controller（WS143 の bt-link-host-test の形）: (a) 同期の command の待ちの間に 2 つの link の ACL を 40 KB 送り、事象（Encryption Change・Disconnection Complete）と LE の advertising report を混ぜる → 数えた事象は全部渡り、捨てた link だけが封じられ、他の link の ACL は渡り、0xF0 は捨てた物より前の物の後に handle ごとに 1 回、flags と cid 付き。(b) **待ちが続く場合** [B1]: ring に物が残る間に handler が次の command を出し（偽の handler）、その待ちの中に封じた link の続きの断片が来る → 知らせの前に渡らない。(c) 知らせの後に続きの断片が来る → first の断片まで捨てられ、組み立てがつながない。(d) 知らせが期日の間 `btd_session_pending` が 1 [M3]。(e) `last_cid` が着いた順（ring の古い packet の dispatch で変わらない）[M2]。(f) Hardware Error が ring の満ちた時も処理される [m1]。(g) 数えた事象が入らない場合の `COUNTED` の知らせと router の照合（handle の使い直しで address が違う場合を含む [m3]） |
| session の送り（[T2]） | §4.2 の試験 |
| router | Connection Request の link type（SCO・eSCO は Reject Synchronous 0x042A、AES-CCM の ACL があれば reason 0x0E [M4]）、Synchronous Connection Complete を捨てる、HID → phone の順、Link Key Request、0xF0 の配り、照合 |
| pair と L2CAP の移し替え（[T3]） | phone=1 の pairing の間の SDP の Pending、handoff の後の Connection Response と CID で引ける事 [M5]、`refuse_left` が 1 command ずつ [m7]、§7.4 の各条件で受けない時の答え（`phone=0 why=…`、PAIRED の行の後ろ [m10]）と断りの Connection Response の後に HID の hook への続き [m6]、phone=1 で保存の Just Works の鍵に Negative Reply [m9] |
| linkmgr | scan の on・off の合成、page の token（HID の page が出ている間の pair は EBUSY、HID は 2 s 後）、router の Connection Complete で終わる、PAGING の各出口の `page_end` [m11]、15 s の守り |
| fuzz | RFCOMM の frame と多重化の message、OBEX の packet、SDP の server の request（固定の seed、各 20 万回、WS143 の hid-report の fuzz と同じ形） |

WS143 の host の試験（`bt-daemon-host-test.sh` と同じ script の中の pair・link・hid・hidhost・hog の試験）を変更の後も流す（WS143 の code を変えるため）。期待値の変わる物（SCO の Connection Request、`PAIRED` の行に足す ` phone=` は phone=1 の時だけ）は WS143 の試験を直す（試験の直しは見つけた担当、AGENTS.md）。

### 12.2 QEMU（T1、[T4]）

保留の branch で image を作り（WS143 の `plan/ws143/tests/config-amd64-bt.mk`、`build-bt-image.sh`）、WS143 の `bt-hid-p005.sh`・`bt-pair-p004.sh`・`bt-daemon-p003.sh`・`bt-loopback-p002.sh` を流す（HID の回帰）。WS143 が先に完了して script を消す時は、それを `plan/tools` か `tests/` のシナリオに移すよう Q1 に頼む。

### 12.3 host の相手（dongle）

firmware の要らない USB の Bluetooth の dongle が 2 本ある時だけ（p001 §9.2、ユーザーへの情報のお願いの答えは未着）: QEMU の zedBSD と host の Linux の BlueZ の obexd（PBAP の dummy の電話帳）で `PHONE PROBE`。無ければ未実施と書き、p008 の実機で確かめる。

## 13. 実装の順（保留の branch の WIP commit の単位）

| i | 内容 | 確かめ |
| --- | --- | --- |
| i01 | session: 受けの予約・封・印・0xF0（§3）、送りの上限と round-robin（§4） | host（§12.1 の session の 2 行）、WS143 の host の試験 |
| i02 | linkmgr の本体（§6.2 の API と試験、HID・pair の使い方はまだ）、router: phone の hook、SCO を断る、0xF0 の配り、照合、linkmgr の hook（§5）[m17] | host |
| i03 | l2cap の移し替え、pair の phone=1・uid・accept の Pending・phone の handoff の鎖（§7）、main の PAIR | host |
| i04 | HID・pair の linkmgr の使い方（§6.2）、HID・pair の 0xF0 の回復（§3.5） | host、WS143 の host の試験 |
| i05 | RFCOMM（§8） | host、fuzz |
| i06 | OBEX（§9） | host、fuzz |
| i07 | SDP の server と client の一般化（§10） | host、fuzz |
| i08 | phone.c の輸送、main の配線、btsnoop を伏せる、`PHONE PROBE`（§11） | host、build（warning 0） |
| i09 | T1 への依頼（§12.2）、style-check、phase.md の記録 | T1 |

各 i の後に WIP commit し、SHA を「ws197 branch」と明記して Q1 に送る（main には merge しない）。WS143 の file の変更は i01〜i04 に集め、WS143 の Phase が同じ file を変えている間は Q1 に順を聞く（p001 §12 の取り込みの規則）。

## 14. 受け入れ

- §12.1 の host の試験と fuzz が全部 PASS、WS143 の host の試験が PASS、build（`bluetoothd` を target の clang で、-Werror）warning 0、style-check の変更箇所 0。
- T1 の HID の回帰（§12.2）が PASS（[T4]）。
- `PHONE PROBE` は host の相手か実機で Connect・Get・Disconnect が往復した時に「確かめた」と書く。相手が無ければ未実施（p008 で）と書き、それだけでは p002 を uncleared にしない（p001 §1 の 4、Q1 の判定）。
- 仕様の確かめ（§1）と Q16 の答えの記録。

## 15. 危険と未確認

| 項目 | 内容 |
| --- | --- |
| MAP 1.1・PBAP 1.1 の相手の振る舞い | Android・iPhone が 1.1 の MCE・PCE に何を見せるか（送信、通知の形）。p003・p005 の host の台本と p008 |
| IrOBEX の値 | 公開されていない。OBEX の値は MAP・PBAP の本文と相手の trace で照合 |
| 受けの封 | link ごと（§3.3）。その link の知らせまで ACL を捨てる。HID の link が捨てられるのは HID 自身の ACL が 4 KB の余白を越えた時だけ（HID の入力は小さく、まず起きない） |
| interrupt の drop で HID を作り直す | 押されたままの key を残さないため。実機で頻度を見る（p008） |
| `BTD_SDP_MAX` | スマホの record の大きさ（未確認） |

## Event

- 2026-10-10: 第 1 版（P1）。§1 の仕様の確かめ（MAP 1.4.2 §9、PBAP 1.2.3 §9: GOEP 2.0 は必須）を Q1 に報告。
- 2026-10-10: Q16 の決定 (a)（ユーザー、Q1 経由）。ERTM・GOEP 2.0 の Future Work の行は Q1 に依頼。
- 2026-10-10: design-reviewer の review（agent a70a3277735e7af2e）→ [review-1.md](review-1.md)（blocker 1・major 6・minor 18、一部 GO: i05〜i07 は可、i01〜i04 は直しと短い再確認の後）。第 2 版で全部に答えた。
- 2026-10-10: 第 2 版の短い再確認 → [review-2.md](review-2.md): **i01〜i04 も GO**。minor 5 つは本文を直した（第 2.1 版）。

## 実装の進み（2026-10-10、P1、ws197 branch）

| i | commit | 状態 |
| --- | --- | --- |
| i05 RFCOMM | c5a174129 | 実装と host 試験 PASS（bt-phone-host-test、302 checks） |
| i06 OBEX | 0f5d78fb8 | 実装と host 試験 PASS（bt-obex-host-test、2073 checks） |
| i07 SDP の server と client の一般化 | e90f6822d | 実装と host 試験 PASS（bt-sdp-host-test、45 checks）、WS143 の host 試験 PASS |
| i01 session の受けの drop と送りの上限 | 3c73e181c（実装）、この commit（試験） | session.c・session.h を実装（§3・§4、build warning 0、style-check 0）、WS143 の host 試験 PASS（bt-link-host-test の queue の期待値を「events の知らせ 1 つ」で 201 に直した）。新しい試験 `plan/ws197/tests/bt-session-host-test.c`（51 checks）PASS、`bt-phone-host-test.sh` に足した: 偽の controller の script を thread で書く形にし（socket の buffer を越える flood を session が読みながら入れる）、試験の期待値の誤り 4 つを直した（ring の計算では 10 の report は余白に入るので 40 に、(d) は ring が空で知らせが期日の時の場面へ移した、数えた事象は 600 で溢れる、送りの frames は待つ frame の数なので 1 つ目は送られて 10 本目が断られる）。実装の直しは無し |

再開点: i02（router と linkmgr）。2026-10-10 Q1 の割り込み（ベータ2 の UAT の BUG-275〜280、BUG-222）で中断したのち、同日 bt-session-host-test を通した。
