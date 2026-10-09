<!-- awesome-plan project=zedbsd record=ws197-p001 -->

# ws197-p001: Bluetooth のスマホ連携の設計（MAP・PBAP・HFP、RFCOMM・OBEX）

Phase ID: `ws197-p001`
Parent: [WS197](../ws.md)
Status: in-progress（第 2 版。第 1 版の指摘 R1〜R24（[review-1.md](review-1.md)）を反映済み。第 2 版の再 review の指摘 S1〜S25（[review-2.md](review-2.md)）は未反映: 第 3 版で p002 の前に S1〜S5・S14・S18 を直す（2026-10-09 深夜、Q1 の割り込み（WS193 の menuconfig）で中断）。code は書かない）
Phase disposition: normal
Queue: Q1 の投入（2026-10-09「beta2.md の P1 の必須は T1・UAT の待ちだけになったので、WS197 の p001 設計を始める」、ユーザー 2026-10-09「OBEX, MAP, Integration, PBAP, HFPの順で実装しますか。beta2.mdの必須が終わってからです。」）
依存（設計）: [WS143](../../ws143/ws.md) の bluetoothd の今の code（p004 cleared。p005 は in-progress、p006 は test-wait、p003 i02 は未着手。§12 で実装の Phase の依存に直した [R6]）、[WS170](../../ws170/ws.md) の Phone の app と `kl_system_phone_v1`（p001〜p004 cleared）。
成果: この文書。§11 のユーザーの判断、§12 の Phase の分け方。

版: 2026-10-09 第 1 版（P1）→ 同日深夜 第 2 版（P1、R1〜R24）。各節の `[Rn]` はその節が答える指摘。

## 0. 読み方と前提

- 「スマホ」は電話の側（Bluetooth の役は AG・MSE・PSE）、「zedBSD」は Keiland の機械の側（HF・MCE・PCE）。
- 事実の出典は zedBSD の今の code（path と行）と Bluetooth SIG の公開の仕様（Core 5.4、RFCOMM 1.2（TS 07.10 の部分集合）、GOEP 1.1、IrOBEX 1.5、MAP 1.4、PBAP 1.2、HFP 1.8、Device ID 1.3）。他の OS の実装（BlueZ・Android・Apple）の code は読まない。BlueZ・oFono・PipeWire は**試験の相手**として動かすだけ（§9）。
- 「(仕様、確かめる)」は仕様の節を引いて確かめていない値で、その Phase の詳細設計で節を引く。「未確認」は実の機器・相手の振る舞い。

## 1. 目標と受け入れ（WS197 の ws.md から）

1. 1 台のスマホ（Android・iPhone）を Settings から「スマホとして」つなぎ、その持ち主の Phone の app に次が出る:
   - MAP: SMS の受信（届いた時の通知と、タイムラインへの取り込み）、過去の SMS（§11 Q4 の期間）、既読の反映。Android では送信（スマホが MAP の Uploading を持つ時だけ）。
   - PBAP: スマホの連絡先と通話の履歴（§11 Q3 の形）。
   - HFP: 着信（番号・名前）、応答・拒否・終話、発信、通話の音を zedBSD の音の出入りで（§11 Q7）。
2. 受信と着信は Phone の app が閉じていても、持ち主の session に通知（compositor の banner、§8.5）[R12]。app が閉じている間に届いた SMS は、次に app が開いた時の同期でタイムラインに入る（スマホが元の data を持つ。§8.3）[R1]。
3. スマホの側の制約を画面に出す: iPhone の MAP は送信できない（「送信はスマホで」）、スマホで許可が要る（Android の「メッセージへのアクセス」、iPhone の「通知を表示」「連絡先を同期」）。
4. 電波の相手が要る確かめは実機（p008）と、host の Linux の相手（§9.2、dongle の入手次第）。それ以外は host の試験。
5. 範囲の外: ANCS（iPhone の通知）、A2DP（音楽）、PAN、LE Audio、MMS（§11 Q5）、IP の経路（WS170 の bridge）、Linux・FreeBSD の Keiland の backend（§11 Q10）。

## 2. 各 profile の役

| profile | zedBSD の役 | スマホの役 | 下の層 | 何に使う |
| --- | --- | --- | --- | --- |
| SDP | client（今ある、`sdp.c`）と **server（新規）** | 両方 | L2CAP PSM 0x0001 | スマホの RFCOMM の channel・features を読む。スマホが zedBSD の MNS・HF・PCE の record を読む |
| RFCOMM | **多重化の両側（新規）** | 両側 | L2CAP PSM 0x0003 | MAP・PBAP（GOEP 1.1 の RFCOMM の経路）と HFP の下 |
| OBEX（GOEP 1.1） | client と server（新規） | 両方 | RFCOMM の DLC | MAP の MAS・MNS、PBAP |
| MAP 1.4 | MCE: MAS の client と **MNS の server** | MSE | OBEX | SMS の一覧・取得・既読・送信、届いた時の event |
| PBAP 1.2 | PCE（client） | PSE | OBEX | 連絡先（`telecom/pb.vcf`）と通話の履歴（`ich`・`och`・`mch`） |
| HFP 1.8 | HF | AG | RFCOMM（AT）と SCO/eSCO（音） | 着信・発信・応答・終話、発信者、音量、通話の音 |

L2CAP の ERTM と GOEP 2.0（OBEX over L2CAP、SRM）は作らない（ws.md の「後回し +10 LW」）。GOEP 2.0 の MSE・PSE も RFCOMM の channel を SDP に出す（後方互換）(仕様、確かめる)。**機種ごとの実の record は未確認**（p002 の最初に host の BlueZ、p008 で実機の record を dump する）。

## 3. bluetoothd の今と、足す・変える物

### 3.1 今（WS143）

- 1 process の poll の loop（`userland/base/bluetoothd/main.c`）。特権の分離（`privsep.c`）: root の親は node を開けるだけ、子は `_bluetooth` で電波の相手を全部解析する。
- session（`session.c`）: HCI の command の流れ、ACL の buffer、ACL の接続 8 本まで（`session.h` の `BTD_LINKS_MAX`）。送りの queue は全 link で 16 frame（`BTD_SEND_FRAMES`、満ちると ENOBUFS、`session.c:453-456`）。H4 の packet は ACL と event だけを扱う（他の型は event として解析されて壊れ物と数えられる、`session.c:655-666`）。
- router（`router.c`）: 接続ごとの持ち主（`BTD_OWNER_NONE`・`PAIR`・`HID`）。相手からの Connection Request は HID の `wants` だけが受け（513-523 行）、Link Key Request は pair と HID だけ（544-560 行）、Synchronous Connection Complete は扱わない（312-313 行の既定で pairing へ）。pairing の後の handoff は HID だけ（`main.c:331`、`hid.c` は周辺機器でない class を断る）。
- L2CAP（`l2cap.c`）: basic mode、持ち主ごとの channel の表 16 本、相手から来る channel は持ち主の accept hook で受ける・Pending・断る。MTU 672、frame の payload 1024 byte まで（`acl.h`）。pair の l2cap の表には accept の hook が無い。
- SDP は client だけ（HID 0x1124・PnP 0x1200、`sdp.h:56-65` の `struct btd_sdp`、属性の list 8192 byte まで）。
- socket `/run/bluetoothd.sock` は text の行（512 byte、`protocol.h`）。出力は `btd_write`（`main.c:1650-1703`）で、長い物は切り、EAGAIN では 1 秒まで poll して client を落とす。SUBSCRIBE は無い（[F-086](../../future-work.md)）。
- 鍵と device の記録は `/var/db/bluetooth/<controller>/`（system 共有、WS143 D9）。btsnoop の trace（`-s`）は ACL を 1100 byte まで書く（`snoop.h:33`）。

### 3.2 足す部品（新しい file は `userland/base/bluetoothd/`。system call を持たない部品は host の試験で build する）

| 部品 | file（案） | 内容 | Phase |
| --- | --- | --- | --- |
| SDP の server | `sdps.c` | zedBSD の record（§4.4）、ServiceSearch・ServiceAttribute・ServiceSearchAttribute の答え（continuation、UUID の幅の照合） | p002 |
| SDP の client の一般化 | `sdp.c` | UUID ごとの record から RFCOMM の channel、profile の版、SupportedFeatures、MAP の MASInstanceID・SupportedMessageTypes | p002 |
| RFCOMM | `rfcomm.c` | 1 つの ACL の上の多重化（§5） | p002 |
| OBEX | `obex.c` | packet の組み立てと分解、client・server（§6） | p002 |
| phone link | `phone.c`・`phonerec.c` | 1 台のスマホの持ち主・profile の状態・再接続（§8.2）、持ち主の記録（§8.2） | p003（骨格）・p004 |
| 出力の queue | `outq.c` | socket の client ごとの non-blocking の出力の queue（§8.3） | p003 |
| MAP | `map.c`・`mapxml.c`・`bmsg.c` | MAS の client と MNS の server（§7.1）、listing と event の XML、bMessage | p003 |
| PBAP | `pbap.c`・`vcard.c` | PCE と vCard 2.1・3.0（§7.2） | p005 |
| HFP | `hfp.c`・`at.c` | HF の SLC、AT、通話の状態（§7.3） | p006 |
| SCO と音 | `sco.c`・`msbc.c`・`pcmlink.c` | SCO/eSCO、audiod との PCM、clock のずれの調整、mSBC（§7.4） | p007b |

### 3.3 変える今の file（WS143 の code。§12 の取り込みの規則の対象）[R2]

| file | 変更 |
| --- | --- |
| `router.c`・`router.h` | `BTD_OWNER_PHONE`。phone の hook（`struct btd_router_hid` と同じ形の `wants`・`claims`・`handle`）。相手からの Connection Request: HID の次に phone の `wants`（持ち主が居る時だけ、§8.2）。Link Key Request も同じ順。SCO/eSCO の Connection Request（link type 0x00・0x02）と Synchronous Connection Complete・Changed（event 0x2C・0x2D）は、相手の address の ACL の持ち主が phone なら phone へ、他は断る |
| `session.c`・`session.h` | H4 の SCO（型 0x03）の読み書き。SCO の buffer（Read Buffer Size の SCO の欄）と、Synchronous Flow Control を使うなら Number Of Completed Packets の SCO の handle の数え。SCO の handle は ACL の link の表とは別の表（最大 1 本）。送りの queue の持ち主ごとの割り当て（§5.3） |
| `pair.c`・`main.c` | pairing の後の handoff を鎖に: phone の handoff（相手の Class of Device の major が phone 0x02、または SDP で AG・MSE・PSE の record がある）→ HID の handoff。pair の l2cap に accept の hook（pairing の間にスマホが開ける SDP の channel を sdps へ、§3.4） |
| `l2cap.c` | 持ち主の表は今のまま（phone link は自分の表を 1 つ持つ: SDP・RFCOMM の 2〜3 本） |
| `snoop.c` | phone link の RFCOMM・SDP の payload は header だけ書き、中身を伏せる [R22] |

接続の数 [R2]: ACL 8 本 ＝ pairing 1 ＋ 断るための 1 ＋ HID 5 ＋ phone 1。今の `BTD_HID_MAX`（6）を 5 にする（HID の受け入れが 6 台から 5 台に減る。WS143 の記録にも書く）。SCO は ACL の表の外の 1 本。

### 3.4 相手から来る channel と security [R19]

- phone link の ACL の上で相手が開ける L2CAP の channel（SDP 0x0001・RFCOMM 0x0003）は、HID と同じ規則で受ける: bond 済み、暗号化済み、鍵の長さ 16。暗号化の前の request は Pending にし、**zedBSD から Authentication_Requested と Set_Connection_Encryption を始める**（HID host の §9.8 と同じ）。
- pairing の間（pair が持ち主）にスマホが開ける SDP の channel は、pair の accept の hook で受けて sdps に渡す（pairing の手続きの間の SDP は、スマホが相手の service を知るために使う。**pairing の前の SDP が要るかは未確認**で、要らないと分かれば pair の hook は作らない）。bond していない相手の pairing の外の接続は今どおり切る。
- 相手の DLC の SABM は、その channel の profile が有効で持ち主が居る時だけ UA、他は DM [R16]。

### 3.5 大きさ

- RFCOMM の frame の最大（N1）: 自分と相手の L2CAP の MTU（configuration の値）の小さい方から 6（RFCOMM の header の最大 5 と FCS 1）を引いた値以下を PN で申し出る [R16]。
- OBEX の最大 packet 長: zedBSD は 8192 byte を申し出る。相手がそれより小さければ相手の値。最小 255（OBEX の下限）より小さい相手は断る [R17]。
- 1 通の SMS は bMessage で数 KB、MAP の listing は 1 件 300〜500 byte。最初の同期（§11 Q4: 30 日、folder ごとに最大 500 通）は 1 folder 250 KB、8 KB の packet で約 30 往復。PBAP の電話帳は 1000 件で数百 KB（写真を除く）。

## 4. SDP

### 4.1 client の一般化

`btd_sdp`（`sdp.h:56-65`）を「UUID と読む属性の範囲」で一般化し、`btd_sdp_hid` と並べて `btd_sdp_rfcomm_channel`（ProtocolDescriptorList の L2CAP→RFCOMM の channel）、`btd_sdp_profile_version`、`btd_sdp_features`（属性 id を引数）、MAP の `btd_sdp_mas`（MASInstanceID・SupportedMessageTypes）を足す。8192 byte の上限が複数の MAS の record に足りるかは**未確認**（p002 で host の相手と実機の record の大きさを測る）。

### 4.2 MAS の instance

SupportedMessageTypes に SMS_GSM か SMS_CDMA を持つ最初の MAS に接続する。MMS・EMAIL・IM の instance は使わない。

### 4.3 相手の features で決める事 [R15]

| 値 | 決める事 |
| --- | --- |
| MAP の MapSupportedFeatures（MSE の record）と、zedBSD が OBEX の Connect で送る MapSupportedFeatures（App Parameters の tag 0x29、(仕様、確かめる)） | Notification Registration・Notification・Browsing・Uploading（送信）・Extended Event Report 1.1（送り主の名前と時刻が event に入る）・Message Listing Format 1.1・Database Identifier・Persistent Message Handles。Uploading が無ければ送信の button を出さない |
| PBAP の PbapSupportedRepositories・Features と、Connect で送る PbapSupportedFeatures（tag 0x10、(仕様、確かめる)） | 電話帳・履歴の有無、vCard の Selecting、Folder Version Counters、Database Identifier |
| HFP の SupportedFeatures（AG の record）と `+BRSF` | 3-way、EC/NR、音声認識、in-band ringtone、拒否、enhanced call status、codec negotiation（mSBC）、HF indicators |

### 4.4 SDP の server と zedBSD の record [R10]

- 検索の UUID は 16・32・128 bit のどれでも来る。server は Base UUID（`00000000-0000-1000-8000-00805F9B34FB`）で 16・32 bit を 128 bit に広げて照合する。
- 各 record は ServiceRecordHandle（0x0000）と BrowseGroupList（0x0005、PublicBrowseRoot 0x1002）を持つ。
- record は有効な profile だけを出す（Calls が無効なら HF の record を出さない。相手が繰り返し HFP を試すのを防ぐ）[R24]。

| record | ServiceClassIDList | 中身 |
| --- | --- | --- |
| MNS | 0x1133 | ProtocolDescriptorList: L2CAP・RFCOMM（MNS の channel）・OBEX、BluetoothProfileDescriptorList: MAP 0x1134 の 1.4、ServiceName「Keiland MNS」、MapSupportedFeatures（0x0317）。GoepL2capPsm は出さない |
| HF | 0x111E と 0x1203 | L2CAP・RFCOMM（HF の channel）、HFP 0x111E の 1.8、SupportedFeatures（0x0311）。wide band speech の bit は mSBC を作った後（p007b） |
| PCE | 0x112E | BluetoothProfileDescriptorList: PBAP 0x1130 の 1.2、ServiceName（protocol は持たない、(仕様、確かめる)） |

Device ID の record は出さない（VendorIDSource の値の範囲の扱い（0xFFFF は予約）、(仕様、確かめる)）。

- Class of Device の service class の bit（Telephony・Object Transfer・Audio）と EIR の UUID の list に MNS・HF・PCE を出すか: スマホがそれを見て「通話」「連絡先」の項目を出すかは**未確認**。p002 で出す形を決め、p008 で効き目を見る [R24]。

## 5. RFCOMM（新規）

### 5.1 frame

- address（EA・C/R・DLCI 6 bit）、control（SABM 0x2F・UA 0x63・DM 0x0F・DISC 0x43・UIH 0xEF、P/F bit）、length（EA で 1 か 2 byte）、credit（UIH で P/F が 1 の時の 1 byte）、information、FCS（CRC-8、TS 07.10 の多項式と表。UIH は address と control、他は length まで）。
- DLCI は server channel（上位 5 bit）と方向の bit（D）。D の正確な規則（session を始めた側か、どちらの機械の server channel か）は RFCOMM 1.2 §5.4 を引いて p002 の詳細設計で書く。同じ session の上で zedBSD の server channel（MNS・HF）とスマホの server channel（MAS・PSE・AG）の両方を使う場合を、**仕様の例と実機・相手の trace を正解にした** host の試験に入れる（両端を zedBSD の code にした試験では対称の誤りを捕まえられない）[R13]。

### 5.2 多重化の session

- 1 本の ACL に 1 つの session（PSM 0x0003 の 1 channel）。zedBSD が開けても（MAS・PSE・AG の client）スマホが開けても（MNS・HF の server）同じ session を両方の向きで使う。両側が同時に開けた時（衝突）の規則は p002 の詳細設計で仕様から引く（それまでの仮の方針: zedBSD が開けかけた channel を閉じ、相手の session を使う）。
- DLCI 0 の制御 channel を SABM/UA で始め DISC で終える。最後の DLC が閉じたら数秒の後に session を閉じる。
- 多重化の command: PN（frame の最大 N1、credit の初期値 K（0〜7）[R16]、CL の bit で credit による流れの制御を申し出る 0xF0 → 答え 0xE0）、MSC（DLC を開けた後に両側が送る。**仕様の必須**で、相手の MSC を受けるまで data を送らない）[R16]、RPN・RLS（答えるだけ）、Test、NSC。
- PN の答えが CL=0（相手が credit を持たない）なら、FCon/FCoff の流れの制御で続ける（RFCOMM 1.0 の相手。最近のスマホでは来ない見込み。来たら log）[R16]。

### 5.3 credit と送りの queue [R4, R5]

- 各 DLC は「送ってよい frame の数」と「相手に与えた残り」を持つ。受けた frame を上（OBEX・AT）が消費し、**上の出口（socket の client の出力の queue、§8.3）に空きがある時だけ** credit を足す（空の UIH、P/F=1 と credit の byte）。出口が詰まれば相手は止まる（捨てない）。
- credit を足す UIH は失ってはならない: 「足すべき credit の数」を数えて持ち、session の送りの queue に置けなかった（ENOBUFS）時は次の loop で送り直す。
- session の 16 frame の送りの queue は持ち主ごとに割り当てる: phone は 8 まで、HID の出力（LED など）と pairing の signalling に残りを保証する。phone の OBEX の大きな送り（PushMessage）は phone の中の queue（16 frame まで）で待ち、session の割り当ての空きで流す。

### 5.4 server channel

MNS 16、HF 17（値は任意で、SDP の record が教える）。

## 6. OBEX（GOEP 1.1）

### 6.1 packet と header [R17]

- packet: opcode（Final の bit 0x80）、長さ（2、big-endian、packet 全体）、header の列。Connect は版 0x10・flags・最大 packet 長、SetPath は flags と constants を header の前に持つ。
- header の型（id の上位 2 bit）: Unicode（長さ 2 byte、**長さは HI と長さの 3 byte を含む**、UTF-16BE、NUL 終わり。**空の Unicode（長さ 3、中身なし）は正しい**: SetPath の root、一覧の空の Name）、byte の列（長さに 3 byte を含む）、1 byte、4 byte。使う header: Name 0x01、Type 0x42、Length 0xC3、Target 0x46、Who 0x4A、Connection ID 0xCB（**request の最初の header**）、Body 0x48、End of Body 0x49、Application Parameters 0x4C（tag・長さ・値）。
- 検査: header の長さが packet の残りを越えない、Unicode の中身が偶数の長さで NUL で終わる（空を除く）、App Parameters の TLV が header に収まる、packet が交渉の最大以下。知らない header は長さの分かる型だけ飛ばす。

### 6.2 client（MAS・PBAP）

- Connect: Target（MAS `bb582b40-420c-11db-b0de-0800200c9a66`、PBAP の PSE `796135f0-f0c5-11d8-0966-0800200c9a66`）と App Parameters の SupportedFeatures（§4.3）→ 答えの Who と Connection ID。
- Get: 答えが Continue（0x90）なら Final 付きの Get を続け、Success（0xA0）で終わる。Body の上限（listing 1 page 256 KB、vCard の全体 2 MB、bMessage 64 KB。越えたら Abort して失敗）。
- Put: Body を分けて最後を End of Body。**Body の無い Put は削除の意味なので、MAP の SetNotificationRegistration・SetMessageStatus・UpdateInbox は End of Body に filler の 1 byte 0x30 を付ける**（MAP の該当の節、(仕様、確かめる)）[R8]。
- SetPath、Abort、Disconnect。各 request に timeout（10 秒、大きい Get は packet ごと）。timeout・切断は上の操作の失敗（自動で繰り返さない）。

### 6.3 server（MNS）

- スマホの MSE が MNS の channel に接続し、Connect（Target MNS `bb582b41-420c-11db-b0de-0800200c9a66`）→ Who と Connection ID を返す。
- Put（Type `x-bt/MAP-event-report`、App Parameters の MASInstanceID、Body が event の XML）に Success、event を MAP の部品へ。他の opcode は Bad Request か Not Implemented。
- OBEX の認証（digest challenge）は使わない。challenge の付いた Connect は断る（§11 Q12）。

## 7. profile

### 7.1 MAP の MCE（p003）

| 操作 | OBEX | 中身 |
| --- | --- | --- |
| 接続 | RFCOMM（MAS の channel）→ Connect | SMS の MAS（§4.2） |
| 通知の登録 | Put `x-bt/MAP-NotificationRegistration`、NotificationStatus=1、End of Body 0x30 [R8] | スマホが MNS に接続してくる |
| folder | SetPath `telecom`→`msg`→`inbox`（`sent` も） | |
| 一覧 | Get `x-bt/MAP-msg-listing`、MaxListCount・ListStartOffset（page）、FilterPeriodBegin（§7.1.2）、ParameterMask | XML の `<MAP-msg-listing>` の `<msg …/>` |
| 1 通 | Get `x-bt/message`、Name＝handle、Charset=UTF-8、Attachment=0 | bMessage |
| 既読 | Put `x-bt/messageStatus`、Name＝handle、StatusIndicator=read、StatusValue=1、End of Body 0x30 [R8] | |
| 送信 | SetPath `outbox` → Put `x-bt/message`、Charset=UTF-8、Body＝bMessage | 答えの Name が新しい handle。結果は MNS の event（SendingSuccess・SendingFailure・DeliverySuccess） |
| event | MNS の Put: `<MAP-event-report version="1.0|1.1"><event type=… handle=… folder=… msg_type=… …/>` | NewMessage → Get して持ち主へ。MessageDeleted・MessageShift・ReadStatusChanged も |

#### 7.1.1 XML と bMessage [R9, R18]

- `mapxml.c` は listing と event の 2 つの形だけを読む: XML 宣言、内部 subset の無い DOCTYPE（飛ばす）[R18]、1 つの root、空要素、属性の値の 5 つの実体と数値の文字参照。CDATA・namespace・内部 subset は失敗。上限（属性 32、値 1024 byte、件数 1024）。fuzz の対象。
- bMessage: 行の形（CRLF、入れ子は 4 段まで）。**`LENGTH` は仕様では `BEGIN:MSG<CRLF>` から `END:MSG<CRLF>` までの全体を数える**（(仕様、確かめる)）。読み: LENGTH で `END:MSG` の位置を探し、合わなければ「本文だけを数えた」場合と「BEGIN・END の行の数え違い」の場合を順に試す（機種差を受ける）。どれも合わなければ最後の `END:MSG` の行で切り、log に記録。組み立て: 仕様どおりに数える。両方の形を試験に [R9]。

#### 7.1.2 時刻と handle [R18]

- `datetime` は `YYYYMMDDTHHMMSS` でスマホの local time、UTC の offset（`+hhmm`）は任意。offset があれば UNIX 秒へ、無ければ zedBSD の local の timezone で読む（スマホと同じ地域を仮定し、記録に「offset 無し」を残す）。FilterPeriodBegin も同じ形で zedBSD の local time で書く。
- message の重複の判定は、スマホが Database Identifier と Persistent Message Handle（MAP 1.3 以降）を出す時はそれ、出さない時は handle・時刻・相手の番号の組（スマホの初期化で handle が変わり得る）。

#### 7.1.3 iPhone

iPhone の MAP は Uploading を持たない見込み（ws.md）。iPhone が MAP を見せる条件（HFP の接続が先に要るか）は**未確認**で、ユーザーの順（MAP が HFP の前）と衝突し得る（§11 Q13）[R7]。

### 7.2 PBAP の PCE（p005）

- Connect（Target PSE、SupportedFeatures）→ Get `x-bt/phonebook`、`telecom/pb.vcf`・`ich.vcf`・`och.vcf`・`mch.vcf`、Format（3.0 を申し出、無ければ 2.1）、PropertySelector（VERSION・FN・N・TEL・UID・X-IRMC-CALL-DATETIME。PHOTO は取らない）、MaxListCount（まず 0 で件数、多ければ ListStartOffset で分ける）。
- `vcard.c`: 折り返し、2.1 の quoted-printable と soft line break、CHARSET（UTF-8 と ISO-8859-1 だけ、他の値は捨てる）、BASE64 は飛ばす、TEL の TYPE、FN・N。1 件 16 KB、件数 5000 の上限。
- 連絡先の同一性: UID があれば UID、無ければ FN と番号の組（PBAP の `n.vcf` の番号は安定しない）[R11]。
- 同期の時期: スマホの接続の時と「今すぐ同期」。Folder Version Counters・Database Identifier が変わっていなければ電話帳を読まない。

### 7.3 HFP の HF（p006）

- SLC: `AT+BRSF=<HF の features>`、（両側が codec negotiation を持てば）`AT+BAC=1[,2]`、`AT+CIND=?`・`AT+CIND?`・`AT+CMER=3,0,0,1`、（3-way）`AT+CHLD=?`、（HF indicators）`AT+BIND`…。続けて `AT+CLIP=1`、`AT+CCWA=1`、`AT+CMEE=1`、`AT+NREC=0`（AG が EC/NR を持つ時、§7.4 の echo の扱いと合わせる）。
- 状態: `+CIEV` の indicator、`AT+CLCC` の通話の一覧、`RING` と `+CLIP` の番号、名前は PBAP の連絡先から。
- 操作: `ATA`、`AT+CHUP`、`ATD<番号>;`、`AT+BLDN`、`AT+CHLD=1|2`、`AT+VTS=`、`AT+VGS=`・`AT+VGM=`、`AT+BVRA=1`（§11 Q11）。
- `at.c`: CR で区切り、1 行 256 byte、応答ごとの parser、知らない応答は捨てる、一度に 1 つの command の答えを待つ。fuzz の対象。

### 7.4 HFP の音（p007a・p007b に分けた [R14]）

| 段 | 内容 | Phase |
| --- | --- | --- |
| xHCI の isochronous | USB の core の isochronous の URB の API は**既にある**（`include/drivers/usb/usb.h:223` `struct drv_usb_iso_packet`、958 行 `drv_usb_urb_setup_isochronous`、`src/drivers/usb/usb.c:2308`）。無いのは host controller の側: xHCI の Isoch TRB と frame の時刻合わせ（`src/drivers/pci/pci-xhci.c:2034` は endpoint の型の設定だけ。EHCI も ENOTSUP、`pci-ehci.c:1823`）。kernel の内部の仕事で UAPI ではない。USB の音・camera にも効き、全ての USB の device の回帰の危険があるので、**独立の Phase p007a として p001 の後に並行で始められる**（kernel の詳細設計と design-reviewer が先） | p007a |
| bt-usb の interface 1 | 今の bt-usb は interface 1（isochronous）を取らない（`src/drivers/usb/usb-bt.c:12-14`）。`drv_usb_interface_claim`（usb.h:743）で取り、SCO の本数と air mode で `drv_usb_interface_set_alternate`（usb.h:738）。**5330 の interface 1 の alternate の値は未確認**（WS143 p002 の descriptor の dump を見直す） | p007a |
| UAPI | `include/uapi/bluetooth.h` の `BT_PACKET_SCO`（今は予約で EINVAL）を使えるようにし、alternate を選ぶ ioctl を足す（§11 Q9） | p007a |
| HAL | 変更は無い見込み。DMA・cache の API が要ると分かったら実装せずに止め、差分を Q1 に出す（HAL の API は承認が要る） | p007a |
| HCI | AG が SCO/eSCO を開ける（または HF が `AT+BCC` で頼む）。Connection Request に Accept Synchronous Connection Request（CVSD: Voice Setting 0x0060、mSBC: 0x0063）。eSCO の組（CVSD の S4・S3・S1・D1、mSBC の T2・T1）は HFP 1.8 の表 | p007b |
| bluetoothd と audiod | SCO の packet を `/dev/bluetoothN` で読み書きし、audiod の client として再生の stream（スマホの声）と録音の stream（mic）を 8000 Hz（CVSD）・16000 Hz（mSBC）・mono・16 bit で開ける（audiod は 8〜192 kHz を受ける、`userland/base/audiod/main.c:483`） | p007b |
| clock のずれ [R20] | SCO の clock（controller）と audiod の device の clock はずれる。jitter の buffer（60 ms）の水位を見て、適応で 1 sample を間引く・足す（0.1% 程度まで）。受けが途切れたら無音 | p007b |
| 録音の質 [R20] | audiod の録音の変換は線形補間で anti-alias の filter が無い（`userland/base/audiod/mix.c:81-93,245`）。48 kHz から 8・16 kHz に落とすと折り返しの雑音が出る。**bluetoothd の側で 48 kHz の stream を開けて自分で filter と間引きをする**（audiod を変えない。WS をまたがない） | p007b |
| echo [R20] | laptop の speaker と mic では相手に自分の声が返る（echo の打ち消しは作らない）。AG が EC/NR を持てば `AT+NREC` を送らずに AG に任せる。Settings と Phone の画面で「ヘッドセットを勧める」（§11 Q7） | p007b |
| mSBC [R20] | SBC の mSBC の固定の設定（16 kHz、mono、8 subband、15 block、loudness、bitpool 26）。空中の 1 frame は 60 byte（H2 の header 2 byte、SBC の frame 57 byte（sync word 0xAD）、pad 1 byte）(仕様、確かめる)。encoder と decoder は自前（base の再実装の方針、WS143 §6.6）。PLC は最初は無音。**独立の decoder（host の試験の道具としてだけ使う libavcodec の msbc の decoder、製品には入れない）と照合**する [R13] | p007b |
| 1 つの loop の予算 [R4] | SCO の frame は 3.75〜7.5 ms ごと。bluetoothd の poll の loop は 1 回の処理を限る（OBEX の 1 packet・vCard の 1 件・XML の 1 page 分の解析で区切る）。p007b の最初に loop の 1 回の最大の時間を測り、SCO の 1 周期を越える時は SCO と PCM を別の thread に分ける（判断は p007b の詳細設計で、Q1 に報告） | p007b |

CVSD を先に通し、mSBC は p007b の後半（§11 Q6）。LC3-SWB と、Intel の音の offload（I2S で DSP へ）は使わない。

## 8. phone link と WS170 との境界

### 8.1 層

```
Phone の app（WS170、保存の持ち主: ~/Documents/Phone/）
   │ kl_system_phone_v1（版を上げて広げる、§8.4）
compositor の phone-shell.c（backend の表に "bluetooth"）と banner（§8.5）
   │ libkeiland-backend の kl_backend_phone（新規）
   │   zedBSD: libkeiland-backend-zedbsd/phone-zedbsd.c（bluetoothd の socket）
   │   Linux・FreeBSD: libkeiland-backend/unsupported/phone-unsupported.c（「無い」）[R21]
bluetoothd の phone.c（MAP・PBAP・HFP を束ね、持ち主だけに中継）
   │ RFCOMM・OBEX・SDP・L2CAP・SCO
スマホ
```

- Guardrail の配置（guardrail.md 33〜34 行）: bluetoothd の socket の code は `libkeiland-backend-zedbsd/` だけ。compositor は `kl_backend_phone` だけを知る。`plan/tools/keiland-os-boundary`（check.sh）の表に新しい file を足す [R21]。
- 保存の持ち主は Phone の app（WS170 p002 の 1 item 1 file）。bluetoothd と compositor は message・連絡先の中身を disk に書かない（log は件数と長さだけ）。btsnoop の trace は phone link の payload を伏せる（§3.3）[R22]。

### 8.2 持ち主と許可 [R3]

- 持ち主: スマホを「スマホとして使う」にした seat の人の uid。記録 `<controller>/<address>-bredr.phone`（uid と、uid の名前（再利用の検出）、有効な profile: messages・contacts・calls、0600、`_bluetooth`）。
- **持ち主の変更は持ち主か root だけ**。別の人が使うには持ち主（か root）が「スマホとして使う」を外すか、スマホを忘れて（FORGET）その人が pairing し直す（スマホがもう一度許可を求める）。D8 の「seat の人・wheel は変更できる」の例外として phone link に限る。
- **持ち主が居る**＝持ち主が seat の人（`/dev/gpu0` の持ち主、`_greeter` を除く。今の `btd_permitted` の seat の判定、`main.c:1503-1510`）。SSH だけの login は居ると数えない。
- 持ち主が居ない間: phone の profile を切り、スマホからの Connection Request（phone の `wants`）と RFCOMM の SABM を断る（§3.4）。持ち主が seat の人になったら zedBSD から page する（§8.2.1）。
- 記録の uid が無い（account の削除）か名前が違う（uid の再利用）時は、記録を無効にして「スマホとして使う」をやり直させる。
- スマホの側の許可（Android の確認、iPhone の toggle）が無い時は OBEX の Connect が失敗する（Forbidden 等）。Settings に「スマホで許可してください」。
- 残る危険: 乗っ取られた bluetoothd（`_bluetooth`）はスマホの SMS を読める（HID の「key を打てる」と同じ種類）。§11 Q2 で受け入れを尋ねる。

#### 8.2.1 再接続と suspend [R2, R24]

- 持ち主が居て、スマホが bond 済みで「スマホとして使う」なら、zedBSD が page する（30 秒・1 分・2 分…最大 10 分の間隔、成功で戻す）。スマホからの接続も受ける（page scan は HID と同じく有効）。
- suspend: 入る時に phone の profile を閉じ（OBEX の Disconnect、RFCOMM の DISC）、ACL を切る。resume（`/dev/system` の POWER の `sleep.end`、WS143 §5.3）で持ち主が居れば page をやり直す。通話中の suspend は作らない（蓋を閉じても通話中は suspend しない、は範囲の外として記録）。

### 8.3 bluetoothd の socket [R1, R4]

- **出力の queue**（`outq.c`）: socket の client ごとに non-blocking の出力の queue（上限 256 KB）。`btd_write` の「1 秒まで待って落とす」は phone の client には使わない。queue が上限に近づいたら RFCOMM の credit を止める（§5.3）。上限を越えたら client を落とす（読まない client）。
- **SUBSCRIBE**: F-086（状態の変化の通知）の一部をこの WS で作る（Q1 に Future Work の行の更新を頼む）。phone の event は SUBSCRIBE した持ち主の client だけ。
- 長い値は「行 + 長さ付きの byte」（`… length=<n>` の行の後に n byte）。行の中の値は引用符と `\` の escape。1 つの本文は 64 KB まで。

| request（持ち主だけ） | 意味 |
| --- | --- |
| `PHONE LINK address on|off profiles=m,c,h` | スマホとして使う・外す（§8.2 の規則） |
| `PHONE SUBSCRIBE` | live の event（新しい受信・通話・状態）を流す |
| `PHONE PAGE messages|contacts|calls since=<UNIX 秒> after=<cursor> count=<n>` | 過去の分を 1 page（n は 32 まで）。答えは item の列と `PHONE PAGE-END cursor=… more=0|1` |
| `PHONE GET handle=<h>` | 1 通の全文（live の event が本文を省いた時） |
| `PHONE SEND to=<番号> length=<n>` ＋ n byte | 送信 |
| `PHONE READ handle=<h>` | 既読 |
| `PHONE DIAL to=<番号>`・`ANSWER`・`HANGUP`・`DTMF <字>`・`VOLUME <0..15>` | HFP |

| event（持ち主の SUBSCRIBE した client へ） | 意味 |
| --- | --- |
| `PHONE STATE address=… connected=… messages=… contacts=… calls=… send=0|1 why=…` | 状態と理由 |
| `PHONE MESSAGE source=… folder=… dir=in|out time=… peer=… read=0|1 length=<n>` ＋ n byte | 1 通 |
| `PHONE CONTACT source=… length=<n>` ＋ n byte（FN・TEL だけの vCard） | 1 件 |
| `PHONE CALL id=… state=incoming|dialing|alerting|active|held|ended number=… dir=… time=…` | 通話 |
| `PHONE SENT request=… state=sent|delivered|failed` | 送信の結果 |

### 8.4 compositor の API と Phone の app の同期 [R1, R21]

- `kl_system_phone_v1` の版を上げる（manager の版と `KL_VERSION` を次の番号に）。v1 の client（今の Phone の app）には今の `received` を、新しい版の client には下の event を送る（版で分ける）。
- **同期は pull で、目印は app が持つ**: Phone の app は `~/Documents/Phone/` に「スマホごとの同期の目印」（`sync/bt-<address>.state`: messages・contacts・calls の最後の時刻と cursor）を持つ。app は起動の時・`link` の connected の時・「今すぐ同期」の時に `sync_page(what, since, cursor)` を出し、compositor が backend 経由で bluetoothd の `PHONE PAGE` を 1 回出し、答えの item（32 個まで）を `page_item` の event で返し、最後に `page_end(cursor, more)`。app は page を保存してから目印を書き、次の page を頼む。ring を経ないので落ちない。
- live の event（新しい受信・通話・状態）は今の 16 個の ring（`system-private.h:36`）を使い、ring が満ちて古い物を捨てた時は「落ちた」の印を立てる。app は印を見たら目印から `sync_page` で取り直す（live で落ちても同期で戻る）。
- 本文の上限: `KL_PHONE_TEXT_MAX`（1024）は v1 のまま。新しい版の `message`・`page_item` は本文を 16 KB まで運ぶ（compositor の wire の上限は 65532 byte、`kwl.h:64`）。16 KB を越える本文（長い連結 SMS でも数 KB）は切り、item に `Truncated: yes` を書く。libkeiland の ring の 1 個の大きさは本文の分だけ増える（16 個 × 16 KB = 256 KB、app ごと）。大きければ live の event は本文の先頭 1 KB と `length` だけにし、app が `fetch(source)` で全文を取る形に変える（p004 の詳細設計で決め、Q1 に報告）。

| 向き | 名前 | 引数 |
| --- | --- | --- |
| request | answer・hangup・dtmf | request (,digit) |
| request | sync_page | request, what, since, cursor |
| request | fetch | request, source |
| request | mark_read | request, source |
| event | link | device_name, connected, features（send・calls・contacts）, why |
| event | message | source, channel, direction, peer, text, time, read, truncated |
| event | page_item | request, kind（message・contact・call）, 中身 |
| event | page_end | request, cursor, more |
| event | call_state | call_id, state, number, direction, time |
| event | dropped | （live の ring が古い物を捨てた） |

- `phone.backend` の設定に値 2「bluetooth」を足す。Settings で「スマホとして使う」を on にした時、compositor は `phone.backend` を 2 にする（持ち主の desktop.conf）[R21]。
- Settings の「スマホとして使う」と profile の switch: `kl_system_bluetooth_*` の拡張（`kl_system_bluetooth_phone_link(address, on, profiles)`）→ compositor の bluetooth の拡張 → `kl_backend_bluetooth`（`PHONE LINK`）。KL_VERSION は上と同じ版で [R21]。

### 8.5 通知の banner [R12]

- 着信: compositor が `call_state` の incoming を受けた時、Phone の app が無くても banner（相手の名前か番号、応答・拒否）。応答の後は Phone の app を開く。
- SMS: compositor が live の `message`（dir=in）を受けた時、Phone の app が前面に無ければ banner（相手と本文の 1 行目、押すと Phone の app のその相手のタイムライン）。
- lock の画面では「新しいメッセージ」「着信」と相手の名前だけ（本文は出さない）。着信の応答は lock の画面からもできる（§11 Q8）。
- 持ち主の session だけ（他の人の session、greeter には出ない。§8.2 で持ち主が居ない間は接続していない）。

### 8.6 WS170 の保存への写し方（Phone の app、p004・p005。WS170 の code を変える WS を跨ぐ仕事で、割り当ては Q1）[R11]

| スマホの data | WS170 の保存と、要る変更 |
| --- | --- |
| MAP の 1 通 | `messages/<key>/<日時>-<通し番号>.txt`、`Kind: text`・`Channel: sms`・`Direction`・`Date`・`State`、新しい header `Source: bt:<address>:map:<id>`（§7.1.2 の重複の鍵）と `Truncated: yes`（切った時）。**`<key>` の変更**: 今は連絡先の id で、知らない番号は連絡先を作る。同期で大量の連絡先を作らないよう、知らない番号の会話は番号の key（`n<正規化した番号>`）の folder にし、連絡先は作らない（一覧には番号で出す）。手元かスマホの連絡先の番号と一致すれば、その連絡先のタイムラインに見せる |
| PBAP の連絡先 | §11 Q3 の推し: スマホごとの別の組 `contacts/bt-<address>/<UID か FN と番号の hash>.vcf`（`X-KEILAND-SOURCE`）。**store の変更**: `contacts/` の下の 1 段の subfolder を読む、件数の上限 1024（`STORE_CONTACTS_MAX`、`store.c:42`）を 6000 に。**差分の更新**: 同期では変わった・増えた・消えた連絡先の file だけを書く（cloud の上の書き換えを減らす）。手では直さない（直すのはスマホで） |
| PBAP の通話の履歴 | `Kind: call`・`Direction`・`State: answered|missed|no-answer`・`Source: bt:<address>:pbap:<時刻>:<番号>` |
| HFP の通話 | 終わった時に `Kind: call`（PBAP の履歴と同じ時刻・番号なら 1 つに） |
| 同期の目印 | `sync/bt-<address>.state`（§8.4） |

## 9. 試験の方法

### 9.1 host の試験

| 対象 | 内容（正解の出典） |
| --- | --- |
| RFCOMM | FCS（TS 07.10 の例の値）、frame の組み立て・分解、DLCI の向き（仕様の例と実の trace を正解に）[R13]、PN・MSC・credit の台本（SABM から DISC）、credit 0 で止まる、足す credit を ENOBUFS で失わない [R5]、CL=0 の相手 |
| OBEX | packet、header の長さ（3 byte を含む、空の Unicode）[R17]、Connect の交渉（最小 255）、Get の Continue、Put の分割と filler [R8]、Abort |
| SDP の server | 16・32・128 bit の UUID の検索 [R10]、自分の record を今の client で読み戻す、continuation、壊れた request |
| MAP | XML（仕様の例、DOCTYPE、実体参照、上限）、bMessage の LENGTH の 3 つの形 [R9]、時刻の変換（offset の有無）[R18]、送信の bMessage |
| PBAP | vCard 2.1・3.0、壊れた入力、上限、UID の無い連絡先の同一性 |
| HFP | AT の parser、SLC・着信・応答・終話・割り込みの台本、codec の交渉 |
| mSBC | 独立の decoder（host の道具の libavcodec の msbc）との照合、正弦波の SNR、60 byte の frame の形 [R13, R20] |
| clock のずれ | 0.1% 速い・遅い受けで buffer の水位が保たれる [R20] |
| fuzz | 全 parser（WS143 の hid-report の fuzz と同じ道具） |
| 持ち主 | 持ち主でない uid に phone の event が出ない・request が断られる、持ち主の変更の規則、uid の再利用 [R3] |
| 出力の queue | 読まない client で daemon が止まらない、credit が止まる [R4] |
| router | 相手からの Connection Request・Link Key Request・SCO の振り分け、handoff の鎖、ACL 8 本の割り当て（偽の controller の台本、WS143 の道具）[R2] |
| Phone の app | page の同期（中断・再開・ring の dropped）、番号の key、連絡先の差分の更新（WS170 の host の試験に足す） |

### 9.2 相手の実物（host の Linux と QEMU）[R13]

- **前提: firmware の要らない USB の Bluetooth の dongle が 2 本**（1 本を QEMU の zedBSD に、1 本を host の Linux に）。WS143 で尋ねたが**答えは未確認**（`plan/ws143/phase005/phase.md:458`）。§11 の情報のお願いで尋ね、無ければ host の相手の試験は無しで、実機（p008）だけになる。
- MAP・PBAP: host の BlueZ の obexd の PSE（dummy の電話帳）で PBAP。MAP の MNS の event は obexd の MSE では送れない見込みなので、**project で書く小さな台本の MSE**（host の Python の AF_BLUETOOTH の RFCOMM の socket の上で、決まった listing・bMessage・event を返す。BlueZ の code は使わない）。
- HFP: **台本の AG**（同じく Python の RFCOMM の socket の上で RING・+CLIP・+CIEV・CLCC を返す）。oFono と phonesim は代わりの候補（入るかは未確認）。
- SCO の音: qemu-xhci の isochronous の emulation と usb-host の isochronous の passthrough、dongle（clone の CSR8510 は SCO が壊れている物がある）の三つが揃うかは**未確認**（§10）。揃わなければ SCO は 5330 の実機だけ。
- 試験の image は WS143 の `plan/ws143/tests/config-amd64-bt-desktop.mk` に WS197 の config を重ねる（T1）。

### 9.3 実機（p008）

- 5330 とユーザーの Android・iPhone。pairing → 「スマホとして使う」→ スマホの許可 → SMS の受信・既読・送信（Android）、連絡先・履歴、着信・応答・終話・発信、通話の音（CVSD・mSBC）、Wi-Fi との共存、suspend、持ち主の logout と別の人の login。
- 実機の trace（btsnoop）は payload を伏せた形だけを残す。実の SMS・連絡先の中身は証拠に残さない（件数・状態・PNG は本文を見せない画面で）[R22]。
- 実機の trace の header の部分（RFCOMM の DLCI・PN・MSC、OBEX の header の並び）は host の試験の正解に足す [R13]。
- QEMU の証拠と実機の証拠は分けて書く。

## 10. 危険と未確認

| 項目 | 内容 | いつ確かめる |
| --- | --- | --- |
| xHCI の isochronous | 新しい kernel の経路。USB の他の device の回帰 | p007a の詳細設計（design-reviewer） |
| 5330 の interface 1 | alternate の値。AX211 で USB の SCO が使えるか | p007a の前に descriptor と Read Local Supported Codecs |
| iPhone の MAP | HFP の接続が先に要るか | §11 Q13 の選択による |
| Android の MAP の送信 | 機種ごとに Uploading の有無 | p008 |
| QEMU の SCO | qemu-xhci・usb-host の isochronous、dongle の SCO | p007a の後（dongle がある時） |
| 相手の実装の癖 | MSC を待つ、PN の大きさ、OBEX の最大 packet、bMessage の LENGTH | 台本の相手と実機の trace を試験に |
| 帯域 | 最初の同期の時間と、その間の HID の遅れ（§5.3 の割り当て） | p003（host）、p008 |
| loop の時間 | SCO の周期と OBEX・vCard の解析 | p007b の最初に測る（§7.4） |
| 個人の data | bluetoothd が message の中身を扱う。保存先は cloud の `~/Documents` | §11 Q2 |
| WS143 の変更との衝突 | router.c・pair.c・session.c・hid.c は WS143 でまだ変わる | §12 の取り込みの規則 |

## 11. ユーザーの判断（推しは太字）

| # | 判断 | 選択肢 | 推し |
| --- | --- | --- | --- |
| Q1 | 同時につなぐスマホの数 | **1 台**（Bluetooth のキーボード・マウスの上限が 6 台から 5 台になる）/ 2 台以上（接続の数の見直し） | **1 台** |
| Q2 | スマホの data の扱い | **「スマホとして使う」にした人（持ち主）だけが見る。持ち主の変更は持ち主か管理者だけで、別の人はスマホの許可からやり直す。bluetoothd は中継だけで disk に書かない（乗っ取られた bluetoothd は SMS を読める危険は残る）。保存先は Phone の app の `~/Documents/Phone/`（cloud に back される前提の場所）** / その機械の全員が見る / 保存しない（接続の間だけ見せる） | **持ち主だけ・中継だけ・Documents に保存** |
| Q3 | スマホの連絡先の置き方 | **スマホごとの別の組（差分で更新、手では直さない、一覧では番号で手元の連絡先と重ねる。Phone の store の変更が要る）** / 手元の連絡先に混ぜる（重複と食い違いの解決が要る）/ 保存しない | **別の組** |
| Q4 | 最初の同期で読む SMS | **過去 30 日（folder ごとに最大 500 通）** / 全部 / 新しく届く物だけ | **過去 30 日** |
| Q5 | MMS | **範囲の外（後の WS）** / 入れる | **範囲の外** |
| Q6 | 通話の音の質 | **CVSD（狭帯域）を先に、mSBC（広帯域）を同じ Phase の後半で（SBC は自前）** / CVSD だけ | **CVSD の後に mSBC** |
| Q7 | 通話の音の出入り | **audiod の既定の出力と mic（ヘッドホン・ヘッドセットを挿せばそちら）。laptop の speaker では相手に echo が返り得るので、画面でヘッドセットを勧める（echo の打ち消しは作らない）** / echo の打ち消しを作る（大きい） | **既定＋ヘッドセットを勧める** |
| Q8 | 着信と SMS の banner | **Phone の app が閉じていても banner。lock の画面では相手の名前だけ（本文は出さない）、着信は lock の画面から応答できる** / app が開いている時だけ | **banner** |
| Q9 | UAPI の追加 | **`include/uapi/bluetooth.h` の `BT_PACKET_SCO` を使えるようにすることと、bt-usb の alternate を選ぶ ioctl の形を承認し、struct は p007a の詳細設計で review**（USB の isochronous の URB の API は kernel の内部に既にあり UAPI ではない）/ 別の形 | **形を承認** |
| Q10 | Linux・FreeBSD の Keiland | **WS197 では作らない（「無い」の backend だけ。Linux の BlueZ の obexd・oFono・PipeWire を包む backend は Future Work）** / WS197 で Linux も | **作らない** |
| Q11 | 音声認識（Siri・Google） | **入れる（小さい）** / 範囲の外 | **入れる** |
| Q12 | OBEX の認証 | **使わない（link の暗号と bond で守る）** / 使う | **使わない** |
| Q13 | iPhone の MAP の確かめの時期（iPhone は HFP の接続が無いと MAP を見せないかもしれない、未確認） | (a) **p003（MAP）の後に Android で受信を確かめ、iPhone は p006（HFP の制御）の後に確かめる**（順はユーザーの指示のまま、iPhone の MAP の受け入れは遅くなる）/ (b) HFP の SLC（音なし）の最小を p003 の前に入れる（順の例外、約 +3 LW）/ (c) p008 でまとめて | **(a)** |
| Q14 | 持ち主の logout の時 | **スマホの profile を切り、持ち主が seat に戻ったら再接続** / つないだまま（data は捨てる） | **切る** |
| Q15 | 通話中の蓋 | **通話中も蓋を閉じれば suspend（通話は切れる）。通話中は suspend しない、は後の Phase** / 今作る | **今は suspend** |

情報のお願い（判断ではない）: 試験に使う Android と iPhone の機種と OS の版。**firmware の要らない USB の Bluetooth の dongle が 2 本あるか**（無ければ host の相手の試験は無く、実機だけになる。WS143 で尋ねた時の答えが記録に無い）。

## 12. Phase の分け方と依存 [R6, R14]

| Phase | 内容 | 依存 |
| --- | --- | --- |
| p002 | SDP の server と client の一般化、RFCOMM、OBEX、router・session・pair の変更（§3.3 のうち phone の hook と handoff の鎖、送りの割り当て）。host の試験と fuzz。host の相手（dongle がある時）で Connect・Get の往復 | p001、**WS143 p005 cleared と main への merge**（router.c・pair.c・hid.c が落ち着いてから） |
| p003 | MAP（MAS・MNS、XML、bMessage）、phone link の骨格（持ち主、記録、再接続）、出力の queue と SUBSCRIBE（F-086 の一部）、socket の PHONE の request・event | p002 |
| p004 | Integration: `kl_backend_phone`（zedBSD と unsupported）、compositor の backend「bluetooth」と API の版、banner、Settings の「スマホとして使う」、**Phone の app と store の変更（page の同期、Source、番号の key、目印）＝ WS170 の code（Q1 が割り当てを決める）** | p003、WS170 |
| p005 | PBAP（vCard、連絡先の組と差分、履歴）。store の subfolder と上限 | p004 |
| p006 | HFP の制御（SLC、AT、着信・発信・応答・終話・DTMF・音量・音声認識）。Q13 (a) なら iPhone の MAP の確かめ | p004 |
| p007a | **xHCI の isochronous（kernel の詳細設計と design-reviewer が先）、bt-usb の interface 1、UAPI（Q9）**。USB の回帰（T1） | p001 と Q9。p002〜p006 と並行してよい（Q1 の割り当て次第） |
| p007b | SCO・eSCO、audiod、clock のずれ、録音の filter、CVSD の後に mSBC | p006、p007a |
| p008 | 実機の試験と debug（Android・iPhone） | p007b、**WS143 p003 i02（5330 の firmware）と p005 i04（5330 の実機の門）** |
| p009 | 規約の全文の見直し | p002〜p008 |

**保留の branch の取り込みの規則**（ws.md 12 行: WS197 の code は 10/17 まで main に入れない保留の branch）: WS197 の code は Q1 が作る branch（例 `agent/p1-ws197`）に置き、各 Phase の始めと merge 依頼の前に main を取り込む。§3.3 の WS143 の file の変更は小さく、関数の追加と hook の表に限り、WS143 の Phase が同じ file を変えている間は Q1 に順を聞く。

**見積もりの見直し**（ws.md の約 121 LW に足す）: router・session・pair の変更 +4（p002）、出力の queue と SUBSCRIBE +3（p003）、page の同期と store の変更・banner・Settings の経路 +6（p004）、clock のずれと録音の filter +3（p007b）、台本の MSE・AG +3（p002・p003・p006）。計 **約 140 LW**。p007（25）は p007a（xHCI・bt-usb・UAPI 15）と p007b（SCO・音・mSBC 13）に分ける。

## Event

- 2026-10-09 深夜: 第 1 版（P1、711caae2f）。
- 2026-10-09 深夜: design-reviewer（agent abb385aae6187503d）の review → [review-1.md](review-1.md)（blocker 2・major 12・minor 10）。第 2 版で全てに答えた（各節の `[Rn]`）。
- 2026-10-09 深夜: 第 2 版の再 review（agent a6b845b338bd97e31）→ [review-2.md](review-2.md)（S1〜S25。p002 の前に S1〜S5・S14・S18）。第 3 版は未着手（Q1 の割り込みで中断、再開点はここ）。
