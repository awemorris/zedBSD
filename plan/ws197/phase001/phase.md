<!-- awesome-plan project=zedbsd record=ws197-p001 -->

# ws197-p001: Bluetooth のスマホ連携の設計（MAP・PBAP・HFP、RFCOMM・OBEX）

Phase ID: `ws197-p001`
Parent: [WS197](../ws.md)
Status: in-progress（2026-10-09 深夜 P1 が第 1 版を書いた。design-reviewer の review の前。code は書かない）
Phase disposition: normal
Queue: Q1 の投入（2026-10-09「beta2.md の P1 の必須は T1・UAT の待ちだけになったので、WS197 の p001 設計を始める」、ユーザー 2026-10-09「OBEX, MAP, Integration, PBAP, HFPの順で実装しますか。beta2.mdの必須が終わってからです。」）
依存: [WS143](../../ws143/ws.md) の bluetoothd（p004 cleared: L2CAP・SSP・鍵・特権の分離。p005 の SDP client と router、p006 の desktop の口）、[WS170](../../ws170/ws.md) の Phone の app と `kl_system_phone_v1`（p001〜p004 cleared）。
成果: この文書（設計）。§11 のユーザーの判断と §12 の Phase の分け方。

版: 2026-10-09 第 1 版（P1）。

## 0. 読み方と前提

- 「スマホ」は電話の側（Bluetooth の役は AG・MSE・PSE）、「zedBSD」は Keiland の機械の側（HF・MCE・PCE）。
- 事実の出典は zedBSD の今の code（path と行）と Bluetooth SIG の公開の仕様（Core 5.4、RFCOMM 1.2（TS 07.10 の部分集合）、GOEP 1.1/2.x、IrOBEX 1.5、MAP 1.4、PBAP 1.2、HFP 1.8/1.9）。他の OS の実装（BlueZ・Android・Apple）の code は読まない。BlueZ の obexd・PipeWire は**試験の相手**として動かすだけ（§9）。
- 「未確認」と書いた事実は、その Phase の最初に確かめる。

## 1. 目標と受け入れ（WS197 の ws.md から）

1. 1 台のスマホ（Android・iPhone）を Settings から「スマホとして」つなぎ、その人の Phone の app に次が出る:
   - MAP: SMS の受信（届いた時の通知と、タイムラインへの取り込み）、過去の SMS（§11 Q4 の期間）、既読の反映。Android では送信（スマホが MAP の送信に対応する時だけ）。
   - PBAP: スマホの連絡先と通話の履歴（§11 Q3 の形で保存）。
   - HFP: 着信（番号・名前）、応答・拒否・終話、発信、通話の音を zedBSD の speaker と mic で（§11 Q7）。
2. 受信と着信は Phone の app が閉じていても通知（compositor の banner）。
3. スマホの側の制約を画面に出す: iPhone の MAP は送信できない（「送信はスマホで」）、スマホで許可が要る（Android の「メッセージへのアクセス」、iPhone の「通知を表示」「連絡先を同期」）。
4. 電波の相手が要る確かめは実機（p008）。それ以外は host の試験と、BlueZ・PipeWire を相手にした QEMU の試験（§9）。
5. ANCS（iPhone の通知）、A2DP（音楽）、PAN、LE Audio、IP の経路（WS170 の bridge）は範囲の外。

## 2. 各 profile の役と、zedBSD が演じる側

| profile | zedBSD の役 | スマホの役 | 下の層 | 何に使う |
| --- | --- | --- | --- | --- |
| SDP | client（今ある、`sdp.c`）と **server（新規）** | 両方 | L2CAP PSM 0x0001 | スマホの RFCOMM の channel・features を読む。スマホが zedBSD の MNS・HF・PCE の record を読む（iPhone は record を見て「連絡先を同期」などの項目を出すと言われる。**未確認**、p008） |
| RFCOMM | **多重化の両側（新規）** | 両側 | L2CAP PSM 0x0003 | MAP・PBAP（GOEP 1.1 の RFCOMM の経路）と HFP の下 |
| OBEX（GOEP 1.1） | client と server（新規） | 両方 | RFCOMM の 1 本の DLC | MAP の MAS・MNS、PBAP |
| MAP 1.4 | MCE: MAS の client と **MNS の server** | MSE: MAS の server と MNS の client | OBEX | SMS の一覧・取得・既読・送信、届いた時の event |
| PBAP 1.2 | PCE（client） | PSE（server） | OBEX | 連絡先（`telecom/pb.vcf`）と通話の履歴（`ich`・`och`・`mch`、`cch`） |
| HFP 1.8 | HF（Hands-Free） | AG（Audio Gateway） | RFCOMM（AT の command）と SCO/eSCO（音） | 着信・発信・応答・終話、発信者、音量、通話の音 |

L2CAP の ERTM と GOEP 2.0（OBEX over L2CAP、SRM）は作らない（ws.md の「後回し +10 LW」）。MAP・PBAP の MSE・PSE は RFCOMM の経路も持つ（GOEP 2.0 の機器も RFCOMM の channel を SDP に出す、MAP 1.4 §7・PBAP 1.2 §7 の「後方互換」）。**機種ごとの実の record は未確認**（p002 の最初に BlueZ の obexd の record、p008 で実機の record を `bt` の SDP の dump で読む）。

## 3. bluetoothd の今の構造と、足す物

### 3.1 今（WS143）

- 1 process の poll の loop（`userland/base/bluetoothd/main.c`）。特権の分離（`privsep.c`）: root の親は node を開けるだけ、子は `_bluetooth` で電波の相手を全部解析する。
- session（`session.c`）: HCI の command の流れ、ACL の buffer、接続 8 本まで（`session.h` の `BTD_LINKS_MAX`）。
- router（`router.c`）: 接続ごとの持ち主（`BTD_OWNER_NONE`・`PAIR`・`HID`）を Connection Complete で決め、持ち主の無い接続は切る。
- L2CAP（`l2cap.c`）: basic mode、持ち主ごとの channel の表 16 本（`BTD_CHANNELS_MAX`）、相手から来る channel は持ち主の accept hook で受ける・Pending・断る。MTU 672、frame の payload は 1024 byte まで（`acl.h` の `BTD_L2CAP_MAX`）。
- SDP は client だけ（HID 0x1124 と PnP 0x1200 の record、`sdp.h`）。
- socket `/run/bluetoothd.sock` は text の行（512 byte まで、`protocol.h`）。誰でも読め、変更は root・seat の人・wheel（D8）。
- 鍵と device の記録は `/var/db/bluetooth/<controller>/`（system 共有、D9）。

### 3.2 足す部品（新しい file は全て `userland/base/bluetoothd/`、system call を持たない部品は host の試験で build する）

| 部品 | file（案） | 内容 | Phase |
| --- | --- | --- | --- |
| SDP の server | `sdps.c` | zedBSD の record（§4.4）を持ち、相手の ServiceSearch・ServiceAttribute・ServiceSearchAttribute の request に答える（continuation 付き）。相手から来る PSM 0x0001 を受ける | p002 |
| SDP の client の一般化 | `sdp.c` | UUID ごとの record から ProtocolDescriptorList の RFCOMM の channel、GoepL2capPsm（読むだけ）、profile の版、SupportedFeatures（MAP 0x0317、PBAP 0x0317、HFP 0x0311）を読む | p002 |
| RFCOMM | `rfcomm.c` | 1 つの ACL の上の多重化（§5） | p002 |
| OBEX | `obex.c` | packet の組み立てと分解、client・server の状態（§6） | p002 |
| MAP | `map.c`・`mapxml.c`・`bmsg.c` | MAS の client と MNS の server（§7.1）、listing と event の XML の小さな parser、bMessage の parser と builder | p003 |
| phone link | `phone.c` | 1 台のスマホの接続の持ち主（router の新しい `BTD_OWNER_PHONE`）、profile ごとの状態、持ち主の人への中継（§8） | p003（骨格）・p004 |
| PBAP | `pbap.c`・`vcard.c` | PCE の client、vCard 2.1・3.0 の parser（§7.2） | p005 |
| HFP | `hfp.c`・`at.c` | HF の SLC と AT の command の parser・builder、indicator、通話の状態（§7.3） | p006 |
| SCO と音 | `sco.c`・`msbc.c` | SCO/eSCO の接続、audiod との PCM の受け渡し、mSBC の encoder・decoder（§7.4） | p007 |

### 3.3 router と持ち主

- `BTD_OWNER_PHONE` を足す。スマホの device の記録（`<address>-bredr.phone`、§8.2）がある device の接続は phone link の物。1 本の ACL に SDP・RFCOMM（その中に MAS・MNS・PBAP・HFP の DLC）・SCO が乗る。
- 相手から来る L2CAP の channel（SDP 0x0001・RFCOMM 0x0003）は、HID と同じ規則で受ける: bond 済み、暗号化済み、鍵の長さ 16（KNOB の検査、WS143 §6.2）。暗号化の前の request は Pending（HID の §9.8 と同じ）。
- **SDP の server は bond していない相手にも答える**（pairing の前後にスマホが zedBSD の record を読む。中身は公開の情報だけ）。これは今の router の「持ち主の無い接続は切る」と衝突するので、pairing の間（pair が持ち主）の SDP の channel は pair が sdps に渡す。pairing の外で bond していない相手の接続は今どおり切る（攻撃面を増やさない）。
- `BTD_HID_MAX` は 6（8 本の接続から pairing と断る 1 本を引いた数）。スマホは 1 台（§11 Q1）なので、HID の上限を 5 にして 1 本をスマホに取っておく。**今の HID の受け入れ（6 台）を 5 台に減らす変更**なので p003 で WS143 の記録にも書く。

### 3.4 帯域と大きさ

- RFCOMM の frame の大きさ: L2CAP の MTU 672 から RFCOMM の header（最大 5 byte）と FCS 1 byte を引いた 666 以下を PN で申し出る（N1）。OBEX の packet はそれより大きくてよい（RFCOMM の上の byte の流れなので、OBEX の 1 packet は複数の RFCOMM の frame に分かれる）。
- OBEX の最大 packet 長は zedBSD から 8192 byte を申し出る（MAP の listing・vCard の 1 応答が大きい時に往復を減らす。受けの buffer の上限も同じ）。相手の申し出がそれより小さければ小さい方。
- 1 通の SMS は bMessage で数 KB、MAP の listing は 1 件 300〜500 byte の XML。最初の同期で 1 folder 200 件なら 100 KB 程度で、8 KB の packet で約 13 往復（§11 Q4）。PBAP の電話帳は 1000 件で数百 KB（写真を除く、§7.2 の filter）。

## 4. 層の設計: SDP

### 4.1 SDP の client の一般化

今の `btd_sdp`（`sdp.h` 66〜75 行）は 1 つの UUID を ServiceSearchAttributeRequest で全属性まで読み、HID と PnP の record を取り出す。これを「UUID と、読む属性の範囲」で一般化し、取り出し（`btd_sdp_hid`）と並べて `btd_sdp_rfcomm_channel`（ProtocolDescriptorList の L2CAP→RFCOMM の channel 番号）、`btd_sdp_profile_version`（BluetoothProfileDescriptorList）、`btd_sdp_features`（属性 id を引数）を足す。属性の list の上限 8192 byte（`BTD_SDP_MAX`）は MAP の MSE が複数の MAS instance（SMS と email など）を出す時に足りるかが**未確認**（p002 で BlueZ と実機の record の大きさを測る）。

### 4.2 MAP の MAS の instance

MSE は MAS の record を複数持てる（MASInstanceID、SupportedMessageTypes の bit: EMAIL・SMS_GSM・SMS_CDMA・MMS・IM）。zedBSD は SupportedMessageTypes に SMS_GSM か SMS_CDMA を持つ最初の instance に接続する。MMS・IM の instance は使わない（MMS は §11 Q5）。

### 4.3 相手の features で決める事

| 値 | 決める事 |
| --- | --- |
| MAP の MapSupportedFeatures（MSE） | Notification Registration・Notification・Browsing・Uploading（送信）・Extended Event Report 1.1・Message Listing Format 1.1 の有無。Uploading が無ければ送信の button を出さない |
| PBAP の PbapSupportedRepositories・Features | 電話帳・通話の履歴の有無、vCard の Selecting、Folder Version Counters（差分の同期の目安） |
| HFP の SupportedFeatures（AG の record）と `+BRSF` の答え | 3-way calling、EC/NR、音声認識、in-band ringtone、拒否、enhanced call status（`+CLCC`）、codec negotiation（mSBC）、HF indicators |

### 4.4 SDP の server（新規）と zedBSD の record

| record | ServiceClassIDList | 中身 |
| --- | --- | --- |
| MNS | 0x1133（Message Notification Server） | ProtocolDescriptorList: L2CAP・RFCOMM（MNS の channel、§5.4）・OBEX、BluetoothProfileDescriptorList: MAP 0x1134 の 1.4、ServiceName「Keiland MNS」、MapSupportedFeatures（0x0317）: Notification・Extended Event Report 1.1。GoepL2capPsm は出さない（L2CAP の経路を持たない） |
| HF | 0x111E（Handsfree）と 0x1203（Generic Audio） | L2CAP・RFCOMM（HF の channel）、HFP 0x111E の 1.8、SupportedFeatures（0x0311）: EC/NR 無し・3-way・CLI・音声認識・音量・wide band speech（mSBC を作った時、p007） |
| PCE | 0x112E（Phonebook Client Equipment） | BluetoothProfileDescriptorList: PBAP 0x1130 の 1.2、ServiceName。protocol は持たない（PBAP 1.2 §7.1.1） |
| Device ID | 0x1200（PnP Information） | VendorIDSource 0xFFFF（Bluetooth SIG の vendor id を持たない、仕様の「未割当」）、Product・Version。iPhone が見るかは**未確認**（要らなければ出さない） |

server は record を compile の時の固定の表で持ち（data element の列）、相手の request を解析して答えを組む。request の解析は client と同じ長さの検査（data element の長さ、continuation の 16 byte まで、属性の範囲の list の数の上限）。host の fuzz の対象（§9.1）。

## 5. 層の設計: RFCOMM（新規、TS 07.10 の部分集合と RFCOMM 1.2）

### 5.1 frame

- address（EA・C/R・DLCI 6 bit）、control（SABM 0x2F・UA 0x63・DM 0x0F・DISC 0x43・UIH 0xEF、P/F bit）、length（EA で 1 か 2 byte、最大 32767）、credit（UIH で P/F が 1 の時の 1 byte）、information、FCS（CRC-8、多項式 x^8+x^2+x+1。UIH は address と control だけ、他は length まで。TS 07.10 の付録の表）。
- DLCI は server channel（上位 5 bit）と方向の bit（D、最下位）からなり、D は「多重化の session を始めた側（L2CAP の channel を開けた側）か」と「どちらの機械の server channel か」で決まる（RFCOMM 1.2 §5.4）。正確な規則は p002 の詳細設計で仕様の節を引いて書き（この版では値を決めない）、同じ session の上で zedBSD の server channel（MNS・HF）とスマホの server channel（MAS・PSE・AG）の両方を使う場合を host の試験に入れる。DLCI の取り違えは相互の接続の典型の誤り。

### 5.2 多重化の session

- 1 本の ACL に 1 つの session（L2CAP PSM 0x0003 の 1 channel）。zedBSD が先に開けても（MAS・PBAP・HF の client）、スマホが先に開けても（MNS・AG からの HFP）同じ session を両方の向きで使う。両側が同時に L2CAP の PSM 0x0003 を開けた時（衝突）の扱いは仕様の規則に従う（**規則と節は p002 の詳細設計で確かめる**。確かめるまでは「zedBSD が開けかけた channel を閉じ、相手の session を使う」を仮の方針とする）。
- DLCI 0 の制御 channel: SABM/UA で始め、DISC で終える。最後の DLC が閉じたら session を閉じる（idle の timer 数秒）。
- 多重化の command（DLCI 0 の UIH）: PN（DLC の parameter: frame の最大、credit の初期値、CL の bit で credit-based flow control を申し出る 0xF0 → 答え 0xE0）、MSC（modem の状態。DLC を開けた後に両側が送る。送らないと相手が data を送らない実装がある）、RPN（port の設定、答えるだけ）、RLS（line の状態、答えるだけ）、Test（echo）、NSC（知らない command への答え）。FCon/FCoff は credit を使う間は使わない。
- 1 つの session の DLC の数は 8 まで（MAS・MNS・PBAP・HFP で 4 本、余裕）。

### 5.3 credit による流れの制御

- 各 DLC は「送ってよい frame の数」（相手から受けた credit）と「相手に与えた credit の残り」を持つ。受けた frame を上（OBEX・AT）が消費したら、与えた残りが半分を切った時に credit を足す（空の UIH、P/F=1 と credit の byte）。
- 上が詰まった時（Phone の app が受けを読まない、§8.3）は credit を足さない（相手を止める、捨てない）。HID の bt-usb の backpressure と同じ考え。
- 送り: credit が 0 なら DLC の送りの queue（上限、例 16 frame）に置く。queue が満ちたら上の層に EAGAIN（OBEX は次の packet の組み立てを止める）。

### 5.4 server channel の割り当て

zedBSD の server channel（1〜30）は固定にする: MNS 16、HF 17（他の機器の慣習と重ならない数を選ぶ。値そのものは任意で、SDP の record が教える）。相手が知らない channel の SABM には DM で答える。

## 6. 層の設計: OBEX（GOEP 1.1、IrOBEX 1.5 の部分集合）

### 6.1 packet と header

- packet: opcode（1、Final の bit 0x80）、長さ（2、big-endian、packet 全体）、header の列。Connect の request と response は版（0x10）・flags・最大 packet 長（2）を header の前に持ち、SetPath は flags（backup・don't create）と constants を持つ。
- header の id の上位 2 bit で型: 0x00 Unicode（長さ 2 byte、UTF-16BE、NUL 終わり）、0x40 byte の列（長さ 2 byte）、0x80 1 byte、0xC0 4 byte。使う header: Name 0x01、Type 0x42、Length 0xC3、Target 0x46、Who 0x4A、Connection ID 0xCB、Body 0x48、End of Body 0x49、Application Parameters 0x4C（中は tag・長さ・値の 1 byte・1 byte・n の列）。
- 解析の検査: header の長さが packet の残りを越えない、Unicode の長さが偶数で NUL で終わる、Application Parameters の TLV が header の中に収まる、1 packet は Connect で決めた最大以下。知らない header は飛ばす（長さが分かる型だけ）。

### 6.2 client（MAS・PBAP）

- Connect（Target に MAS の UUID `bb582b40-420c-11db-b0de-0800200c9a66`、PBAP の PSE の UUID `796135f0-f0c5-11d8-0966-0800200c9a66`）→ 答えの Who と Connection ID を以後の request に付ける。
- Get: request（Type・Name・App Parameters）を送り、答えが Continue（0x90）なら Final 付きの Get を続け、Success（0xA0）で終わる。Body を集める上限（MAP の listing 256 KB、vCard の全体 2 MB、bMessage 64 KB。越えたら Abort して失敗）。
- Put（MAP の PushMessage・SetNotificationRegistration・SetMessageStatus）: Body を最大 packet に合わせて分け、最後を End of Body で。
- SetPath（MAP の folder 移動 `telecom`→`msg`→`inbox`）、Abort、Disconnect。
- 各 request に timeout（例 10 秒、大きい Get は packet ごと）。timeout・切断は上の操作の失敗として返す（自動で繰り返さない）。

### 6.3 server（MNS）

- スマホの MSE が zedBSD の MNS の channel に RFCOMM で接続し、OBEX の Connect（Target に MNS の UUID `bb582b41-420c-11db-b0de-0800200c9a66`）→ zedBSD は Who と Connection ID を返す。
- Put（Type `x-bt/MAP-event-report`、App Parameters の MASInstanceID、Body が event の XML）を受けて Success を返し、event を MAP の部品へ渡す。他の opcode は Bad Request か Not Implemented。
- OBEX の認証（digest challenge）は使わない。相手が challenge を付けた Connect は断る（§11 Q12）。

## 7. profile の設計

### 7.1 MAP の MCE（p003）

| 操作 | OBEX | 中身 |
| --- | --- | --- |
| 接続 | RFCOMM（MAS の channel）→ Connect | 最初の MAS（SMS の instance、§4.2） |
| 通知の登録 | Put `x-bt/MAP-NotificationRegistration`、App Parameters NotificationStatus=1 | これでスマホが zedBSD の MNS に接続してくる |
| folder | SetPath `telecom`→`msg`→`inbox`（`sent`・`outbox`・`deleted` は名前を変えて） | |
| 一覧 | Get `x-bt/MAP-msg-listing`、App Parameters: MaxListCount、ListStartOffset、FilterPeriodBegin（§11 Q4）、ParameterMask（subject・datetime・sender・recipient・type・size・read・sent） | 答えは XML の `<MAP-msg-listing>` の `<msg handle=… subject=… datetime=… sender_addressing=… type=… read=…/>` |
| 1 通 | Get `x-bt/message`、Name = handle、App Parameters Charset=UTF-8、Attachment=0 | 答えは bMessage（`BEGIN:BMSG`・VERSION・STATUS・TYPE・FOLDER・送り主の vCard・`BEGIN:BENV`・`BEGIN:BBODY`・CHARSET・LENGTH・`BEGIN:MSG`…） |
| 既読 | Put `x-bt/messageStatus`、Name = handle、StatusIndicator=readStatus、StatusValue=1 | |
| 送信 | SetPath `outbox` → Put `x-bt/message`、App Parameters Charset=UTF-8、Body = bMessage（宛先の vCard の TEL と本文） | 答えの Name が新しい handle。送信の結果は MNS の event（SendingSuccess・SendingFailure・DeliverySuccess） |
| event | MNS の Put（§6.3）: `<MAP-event-report version="1.0|1.1"><event type="NewMessage" handle=… folder=… msg_type="SMS_GSM" …/>` | NewMessage → その handle を Get して Phone へ。MessageDeleted・MessageShift・ReadStatusChanged も反映（§8.3） |

- XML: MAP の listing と event は 1 段の要素と属性だけの決まった形。`mapxml.c` は汎用の XML parser ではなく、この 2 つの形（XML 宣言、1 つの root、`msg`・`event` の空要素、属性の値の `&amp;` などの 5 つの実体と数値の文字参照）だけを読む。DTD・CDATA・namespace は失敗とする。入力は全て長さを持ち、1 要素の属性 32 個、属性の値 1024 byte、件数 1024 の上限（fuzz の対象）。
- bMessage: 行の形（CRLF、`BEGIN`・`END` の入れ子は BMSG → VCARD・BENV → VCARD・BBODY → MSG の 4 段まで）。本文は `LENGTH` の byte 数で切る（本文の中の `END:MSG` を本文の終わりと読み違えない）。送信は UTF-8 の本文だけ（SMS の分割はスマホが行う）。
- iPhone: Uploading（送信）を持たない（ws.md の制約。**実の features は p008 で確かめる**）。

### 7.2 PBAP の PCE（p005）

- Connect（Target に PSE の UUID）→ Get `x-bt/phonebook`、Name `telecom/pb.vcf`（電話帳）・`telecom/ich.vcf`・`och.vcf`・`mch.vcf`（着信・発信・不在着信）、App Parameters: Format（vCard 3.0 を申し出、相手が 2.1 しか無ければ 2.1）、PropertySelector（VERSION・FN・N・TEL・UID・X-IRMC-CALL-DATETIME。PHOTO は取らない（大きさ）、§11 Q3）、MaxListCount（まず 0 で件数だけ読み、多ければ ListStartOffset で分ける）。
- vCard（`vcard.c`）: 行の折り返し（CRLF の後の空白）、2.1 の quoted-printable と soft line break、CHARSET（UTF-8 と ISO-8859-1 だけ。他は Latin-1 として扱わず、その値を捨てる）、ENCODING=BASE64 は飛ばす、TEL の TYPE、FN・N。1 件 16 KB、件数 5000 の上限（越えたら途中まで）。WS170 の連絡先（`~/Documents/Phone/contacts/<id>.vcf`、FN と TEL だけ）へ写す形は §8.4。
- 通話の履歴は WS170 のタイムラインの `Kind: call` の item（`Direction`・`State: answered|missed`・`Date`）に写す（§8.4、重複は X-IRMC-CALL-DATETIME と番号で判定）。
- 同期の時期: スマホの接続の時と、Settings・Phone の「今すぐ同期」。PBAP の Folder Version Counters があれば、変わっていない時は電話帳を読まない。

### 7.3 HFP の HF（p006）

- SLC（Service Level Connection）: RFCOMM（AG の channel、または AG から zedBSD の HF の channel へ）の上で、`AT+BRSF=<HF の features>` → `+BRSF`、（両側が codec negotiation を持てば）`AT+BAC=1[,2]`、`AT+CIND=?`（indicator の名前と順）→ `AT+CIND?`（今の値）→ `AT+CMER=3,0,0,1`（indicator の event を有効に）、（3-way を両側が持てば）`AT+CHLD=?`、（HF indicators を持てば）`AT+BIND`・`AT+BIND=?`・`AT+BIND?`。続けて `AT+CLIP=1`（発信者）、`AT+CCWA=1`（割り込み）、`AT+CMEE=1`（拡張の error）、`AT+NREC=0`（zedBSD に EC/NR が無ければ送らない）。
- 状態: indicator（`service`・`call`・`callsetup`・`callheld`・`signal`・`roam`・`battchg`）を `+CIEV` で更新し、通話の一覧は `AT+CLCC`（enhanced call status を持つ AG）で読み直す。`RING` と `+CLIP` で着信の番号、名前は PBAP の連絡先から（§8.4）。
- 操作: 応答 `ATA`、拒否・終話 `AT+CHUP`、発信 `ATD<番号>;`、再発信 `AT+BLDN`、保留と切り替え `AT+CHLD=1|2`、DTMF `AT+VTS=<字>`、音量 `AT+VGS=`・`AT+VGM=`（0〜15）と `+VGS`・`+VGM`、音声認識 `AT+BVRA=1`（Siri・Google、§11 Q11）。
- AT の parser（`at.c`）: 行は CR（AG からは CR LF で囲む）で区切り、1 行 256 byte まで。応答の名前ごとの parser（数の list・引用符の文字列）で、知らない応答は捨てる。command の答え（`OK`・`ERROR`・`+CME ERROR:`）は一度に 1 つの command を待つ（HFP の規則）。fuzz の対象。

### 7.4 HFP の音（p007、WS197 で一番大きく不確かな部分）

| 段 | 内容 | 不確かさ |
| --- | --- | --- |
| HCI | AG が SCO/eSCO を開ける（または `AT+BCC` で HF が頼む）。Connection Request（link type SCO/eSCO）に Accept Synchronous Connection Request（CVSD: Voice Setting 0x0060（linear PCM 16 bit、air coding CVSD）、mSBC: Transparent Data 0x0063）で答える。eSCO の parameter の組（S4・S3・S1・D1、mSBC の T2・T1）は HFP 1.8 §5.7 の表 | 小 |
| kernel の USB | **xHCI の isochronous の経路が今は無い**（`src/drivers/pci/pci-xhci.c:2034` は endpoint の型の設定だけ）。Isoch TRB、frame の時刻合わせ（SIA か Frame ID）、URB の isochronous の packet の列（usb core の API の追加） | 大。USB の音・camera にも効く仕事 |
| bt-usb | interface 1 の alternate の選択（SET_INTERFACE）: SCO の本数と air mode（CVSD 16 bit・mSBC）で決まる（USB の Bluetooth の class の慣習: CVSD 1 本は alt 2 など、mSBC は alt 1 か 6）。**5330 の interface 1 の alternate の実の値は未確認**（WS143 p002 の descriptor の dump を見直す） | 中 |
| UAPI | `include/uapi/bluetooth.h` の `BT_PACKET_SCO`（今は予約で write は EINVAL）を使えるようにし、alternate を選ぶ ioctl を足す。**UAPI の追加は §11 Q9 で承認** | 中 |
| bluetoothd | SCO の packet（handle・長さ・PCM か mSBC の frame）を `/dev/bluetoothN` で読み書きし、audiod の client として再生の stream（スマホからの声）と録音の stream（mic）を 8000 Hz（CVSD）・16000 Hz（mSBC）・mono・16 bit で開ける（audiod は 8〜192 kHz の stream を受けて変換する、`userland/base/audiod/main.c:483`）。jitter の buffer（例 60 ms）と、受けが途切れた時の無音 | 中 |
| mSBC | SBC の mSBC の固定の設定（16 kHz、mono、8 subband、15 block、loudness、bitpool 26、57 byte の frame と H2 の sync header）の encoder と decoder を自前で（base の再実装の方針、master-design-policy §2.1。libavcodec は encoder を持たない構成、WS143 §7）。PLC（欠けた frame の補い）は最初は無音 | 中 |

CVSD（狭帯域）を先に通し、mSBC は同じ Phase の後半（§11 Q6）。LC3-SWB（HFP 1.9 の超広帯域）は範囲の外。Intel の controller の「音の offload」（I2S で audio の DSP へ）は使わない（zedBSD に DSP の経路が無い）。

## 8. phone link と WS170 との境界

### 8.1 層の分け方

```
Phone の app（WS170、保存の持ち主: ~/Documents/Phone/）
   │ kl_system_phone_v1（compositor の protocol、版を上げて広げる §8.3）
compositor の phone-shell.c（backend の表に "bluetooth" を足す）
   │ libkeiland-backend の kl_backend_phone（新規、zedBSD の実装は bluetoothd の socket）
bluetoothd の phone.c（MAP・PBAP・HFP を束ね、持ち主の人だけに中継する）
   │ RFCOMM・OBEX・SDP・L2CAP・SCO
スマホ
```

- Guardrail の配置（guardrail.md 34 行）: OS に固有の code（bluetoothd の socket）は `libkeiland-backend-zedbsd/phone-zedbsd.c` にだけ置く。compositor は backend の口（`kl_backend_phone`）だけを知る。Linux・FreeBSD の backend は最初は「無い」（§11 Q10）。
- 保存の持ち主は今と同じ Phone の app（WS170 p002 の規則: 1 item 1 file）。bluetoothd も compositor も message・連絡先の中身を disk に書かない（bluetoothd の log は件数と長さだけ、WS170 の phone-shell と同じ）。

### 8.2 誰のスマホか（持ち主）と許可

- bond は system 共有（WS143 D9）だが、message・連絡先・通話は人の物。スマホを「スマホとして」つないだ人（Settings で操作した seat の人の uid）を持ち主として device の記録 `<controller>/<address>-bredr.phone`（uid、有効な profile の bit: messages・contacts・calls、最後の同期の時刻・MAP の最後の handle）に保存する（0600、`_bluetooth`）。
- bluetoothd は phone の data（message の中身・番号・連絡先・着信）を、socket の SO_PEERCRED の uid が持ち主と同じ client にだけ送る。phone の操作（送信・発信・応答・同期）も持ち主だけ。他の人には「スマホ（名前）がつながっている」だけが見える。
- 持ち主の session が無い間（logout、別の人の login）は、スマホの profile の接続を切る（ACL は他の用が無ければ切る）。持ち主が login したら再接続する（§11 Q9）。
- スマホの側の許可: Android は MAP・PBAP の接続の時に「メッセージ・連絡先へのアクセス」の確認を出し、断られると OBEX の Connect が失敗する（または Forbidden）。iPhone は device の設定の「通知を表示」「連絡先を同期」を人が on にする。zedBSD は失敗の理由を Settings に出す（「スマホで許可してください」）。

### 8.3 socket と compositor の API の広げ方

**bluetoothd の socket**（`protocol.h` の text の行に足す。持ち主だけ）:

| request | 意味 |
| --- | --- |
| `PHONE LINK address on|off profiles=m,c,h` | スマホとしてつなぐ・外す（持ち主になる） |
| `PHONE SUBSCRIBE` | この client に phone の event を流す |
| `PHONE SYNC messages|contacts|calls since=<UNIX 秒>` | 過去の分を読む（答えは event の列と `PHONE SYNC-END`） |
| `PHONE SEND to=<番号> length=<n>` の後に n byte の本文 | 送信（MAP の Uploading がある時だけ） |
| `PHONE READ handle=<h>` | 既読 |
| `PHONE DIAL to=<番号>`・`PHONE ANSWER`・`PHONE HANGUP`・`PHONE DTMF <字>`・`PHONE VOLUME <0..15>` | HFP |

| event（持ち主の SUBSCRIBE した client へ） | 意味 |
| --- | --- |
| `PHONE STATE address=… connected=0|1 messages=… contacts=… calls=… send=0|1 why=…` | profile ごとの状態と理由（スマホの許可待ちなど） |
| `PHONE MESSAGE handle=… folder=inbox|sent dir=in|out time=… from=… read=0|1 length=<n>` の後に n byte の本文 | 1 通（受信の時と SYNC の時） |
| `PHONE CONTACT uid=… length=<n>` の後に n byte の vCard（FN・TEL だけに整えた物） | 1 件 |
| `PHONE CALL id=… state=incoming|dialing|alerting|active|held|ended number=… dir=in|out time=…` | 通話の状態 |
| `PHONE SENT request=… state=sent|delivered|failed` | 送信の結果 |

本文などの長い値は「行 + 長さ付きの byte」で運ぶ（今の 512 byte の行に収まらないため。行の中の値は引用符と `\` の escape、改行は入れない）。1 つの本文は 64 KB まで。

**compositor の API**（`kl_system_phone_v1` の版を上げる。WS170 p001 §3 の send・call・received・status・result に足す）:

| 向き | 名前 | 引数 |
| --- | --- | --- |
| request | answer・hangup | request |
| request | sync | request, what（messages・contacts・calls）, since |
| request | mark_read | request, source_id |
| event | call_state | call_id, state, number, direction, time |
| event | message | source_id, channel, direction, peer, text, time, read（received の代わりに、送り元の id 付き） |
| event | contact | source_id, name, numbers |
| event | link | connected, features（send できるか、通話できるか）, why |

- source_id は「スマホの address と MAP の handle（または PBAP の UID）」から作る文字列で、Phone の app が item の header（新しい `Source:` の行）に書き、重複を判定する（WS170 の保存の形の追加。p004 で WS170 の記録にも書く）。MAP の handle は MSE の間だけ一意で、スマホの初期化で変わり得るので、重複の判定は handle と時刻と相手の番号の組（§11 Q3 の注）。
- 着信の banner: compositor が `call_state` の incoming を受けた時、Phone の app が無くても banner（応答・拒否）を出す（§11 Q8）。応答の後の画面（通話中の時間・終話・mute・DTMF）は Phone の app を開く。
- Settings: Bluetooth の頁の device の行に、種類が phone（Class of Device の major 0x02、`KL_BACKEND_BT_KIND_PHONE` は既にある）なら「スマホとして使う」と profile の switch（Messages・Contacts・Calls）と状態（§8.2 の許可待ち）。

### 8.4 WS170 の保存への写し方（Phone の app の仕事、p004・p005）

| スマホの data | WS170 の保存 |
| --- | --- |
| MAP の 1 通 | `messages/<連絡先の id>/<日時>-<通し番号>.txt`、`Kind: text`、`Channel: sms`、`Direction`、`Date`、`State: read|unread`（受信）・`sent`（送信済み）、新しい `Source: bt:<address>:map:<handle>` |
| PBAP の連絡先 | §11 Q3 の推し: スマホごとの別の組 `contacts/bt-<address>/<UID か連番>.vcf`（`X-KEILAND-SOURCE`）。同期のたびに組ごと作り直し、手で直さない（直すのはスマホで）。一覧では手元の連絡先と番号で重ねて 1 人に見せる |
| PBAP の通話の履歴 | `Kind: call`、`Direction`、`State: answered|missed|no-answer`、`Source: bt:<address>:pbap:<日時>:<番号>` |
| HFP の通話 | 終わった時に `Kind: call` の item（PBAP の履歴と重なれば Source で 1 つに） |

## 9. 試験の方法

### 9.1 host の試験（全部の parser と状態機械、system call 無し。bluetoothd の今の host の試験と同じ形）

| 対象 | 内容 |
| --- | --- |
| RFCOMM | FCS の表（TS 07.10 の例）、frame の組み立て・分解（短い・長い length、壊れた FCS・EA）、DLCI の向き（両方が始める側）、PN・MSC・credit の台本（相手の役の台本で SABM から DISC まで）、credit 0 で止まる・足す、衝突 |
| OBEX | packet の組み立て・分解、header の型ごとの長さの検査、Connect の最大 packet の交渉、Get の Continue の連鎖、Put の分割、Abort、壊れた長さ・Unicode |
| SDP の server | 自分の record を client（今の `sdp.c` と新しい取り出し）で読み戻す往復、continuation、壊れた request |
| MAP | listing と event の XML（仕様の例、属性の順の違い、実体参照、上限）、bMessage（本文の中の `END:MSG`、LENGTH の不一致）、送信の bMessage の組み立て |
| PBAP | vCard 2.1（quoted-printable、soft line break、CHARSET）・3.0（折り返し）、壊れた入力、上限 |
| HFP | AT の parser、SLC の台本（AG の役）、着信・応答・終話・割り込みの台本、codec の交渉 |
| mSBC | encoder・decoder の往復（正弦波の SNR）、frame の形（H2 の header、57 byte） |
| fuzz | 上の全 parser にランダムと変異の入力（WS143 の hid-report の fuzz と同じ道具） |
| 持ち主 | 持ち主でない uid の client に phone の event が出ない、request が断られる |

### 9.2 QEMU（zedBSD）と相手の実物

- zedBSD の QEMU には Bluetooth の controller の emulation が無い（WS143 §10.2）。WS143 と同じく firmware の要らない USB の dongle（CSR8510 など、ユーザーの手元の有無は WS143 で尋ね済み）を QEMU に渡し、host の Linux の 2 つ目の controller（または同じ型の 2 本目の dongle）で BlueZ を「スマホの役」にする:
  - MAP・PBAP: BlueZ の obexd の MAS・PSE の server（obexd の server の plugin の dummy の backend。**obexd の MAS の server の出来と、dummy の data の入れ方は未確認**、p002 で試す）。
  - HFP: PipeWire の bluez5 の native backend の AG（HFP AG の役）か oFono。**SCO の音の往復は host の controller の SCO の経路次第で未確認**。
- 試験の image は WS143 の `plan/ws143/tests/config-amd64-bt-desktop.mk` に WS197 の config を重ねる（T1、2026-10-04 の image の作り方の規則）。
- 依頼の単位: p002（RFCOMM・OBEX の相互の接続を obexd と）、p003（MAP の一覧・event）、p005（PBAP）、p006（HFP の SLC と着信の台本）、p007（SCO の音）。

### 9.3 実機（p008）

- 5330 とユーザーの Android・iPhone（機種は §11 の情報のお願い）。pairing → 「スマホとして使う」→ スマホの許可 → SMS の受信・既読・送信（Android）、連絡先・履歴、着信・応答・終話・発信、通話の音（CVSD と mSBC）、Wi-Fi との共存（通話中の SCO と Wi-Fi の通信、WS143 の F23 の続き）、suspend からの復帰、持ち主の logout で切れる。
- QEMU の証拠と実機の証拠は分けて書く。

## 10. 危険と未確認

| 項目 | 内容 | いつ確かめる |
| --- | --- | --- |
| xHCI の isochronous | 新しい kernel の経路。USB の他の device の回帰の危険 | p007 の最初に詳細設計（design-reviewer） |
| 5330 の interface 1 | alternate の値と、AX211 の CNVi で USB の SCO が使えるか（Intel は SCO を offload の経路に回す機種がある） | p007 の前に descriptor と HCI の Read Local Supported Codecs |
| iPhone の MAP | iPhone が zedBSD に MAP を見せる条件（HFP の接続が先に要るか、SDP の record を見るか） | p003 の後に実機で 1 回（p008 の前倒し、§11 Q13） |
| Android の MAP の送信 | 機種ごとに Uploading の有無が違う | p008 |
| 相手の実装の癖 | RFCOMM の MSC を待つ・PN の frame の大きさ・OBEX の最大 packet | obexd・実機の台本を host の試験に足す |
| 帯域 | MAP の最初の同期の時間（§3.4）と、その間の HID の遅れ（1 本の radio を共有） | p003 の QEMU、p008 |
| 個人の data | bluetoothd が `_bluetooth` で message の中身を扱う（disk には書かない、log は長さだけ）。乗っ取られた bluetoothd はスマホの SMS を読める（HID の「key を打てる」と同じ種類の残る危険） | 記録として受け入れるか §11 Q2 で |

## 11. ユーザーの判断（推しは太字）

| # | 判断 | 選択肢 | 推し |
| --- | --- | --- | --- |
| Q1 | 同時につなぐスマホの数 | **1 台**（HID の上限を 6 → 5 に）/ 2 台以上（接続の数の上限を見直す） | **1 台** |
| Q2 | スマホの data を誰に見せるか | **「スマホとして使う」を押した人（持ち主）だけ。bluetoothd は中継だけで disk に書かない**（§8.2）/ その機械の全員 | **持ち主だけ・中継だけ** |
| Q3 | スマホの連絡先の置き方 | **スマホごとの別の組（同期で作り直し、手では直さない。一覧では番号で手元の連絡先と重ねる）**/ 手元の連絡先に混ぜる（重複と食い違いの解決が要る）/ 保存せず接続の間だけ見せる | **別の組** |
| Q4 | 最初の同期で読む SMS | **過去 30 日**（folder ごとに最大 500 通）/ 全部 / 新しく届く物だけ | **過去 30 日** |
| Q5 | MMS（画像つき） | **範囲の外**（MAP の MMS の instance は機種差が大きい。後の Phase）/ 入れる | **範囲の外** |
| Q6 | 通話の音の質 | **CVSD（狭帯域）を先に、mSBC（広帯域）を同じ Phase で**（SBC は自前）/ CVSD だけ | **CVSD の後に mSBC** |
| Q7 | 通話の音の出入り | **audiod の既定の出力と mic（ヘッドホンを挿せばそちら）**/ 通話の専用の設定を Settings に | **audiod の既定** |
| Q8 | 着信の画面 | **Phone の app が閉じていても compositor の banner（応答・拒否）**/ Phone の app が開いている時だけ | **banner** |
| Q9 | UAPI の追加（`BT_PACKET_SCO` を使えるように、bt-usb の alternate を選ぶ ioctl、usb core の isochronous の URB の API） | **形を承認し、struct は p007 の詳細設計で review**/ 別の形 | **形を承認** |
| Q10 | Linux・FreeBSD の Keiland | **WS197 では作らない（Future Work: Linux は BlueZ の obexd と PipeWire・oFono を backend で包む）**/ WS197 で Linux も | **作らない** |
| Q11 | 音声認識（Siri・Google の `AT+BVRA`） | **入れる（小さい）**/ 範囲の外 | **入れる** |
| Q12 | OBEX の認証 | **使わない（Bluetooth の link の暗号と bond で守る）**/ 使う | **使わない** |
| Q13 | iPhone の早い確かめ | **p003（MAP）の後に実機で 1 回（受信だけ）、p008 を待たない**/ p008 でまとめて | **p003 の後に 1 回** |
| Q14 | 持ち主の logout の時 | **スマホの profile を切り、持ち主の login で再接続**/ つないだまま（data は捨てる） | **切る** |

情報のお願い（判断ではない）: 試験に使う Android と iPhone の機種と OS の版。firmware の要らない USB の dongle（WS143 で尋ね済み）の有無。

## 12. Phase の分け方（ws.md の表の確認と修正）

| Phase | 内容 | 依存 | ws.md からの変更 |
| --- | --- | --- | --- |
| p002 | SDP の server と client の一般化、RFCOMM（両側、credit）、OBEX（client・server）。host の試験と fuzz、obexd の相手で Connect・Get の往復（T1） | p001、WS143 p004・p005 の L2CAP と router | SDP の server を明記（ws.md は「SDP の検索」だけ） |
| p003 | MAP（MAS の client、MNS の server、XML、bMessage）、phone link の骨格（router の `BTD_OWNER_PHONE`、持ち主の記録、socket の PHONE の request・event の一部） | p002 | phone link の骨格を p003 へ前倒し（MAP の試験に持ち主の経路が要る） |
| p004 | Integration: `kl_backend_phone`・`phone-zedbsd.c`、compositor の backend「bluetooth」と `kl_system_phone_v1` の版、Phone の app の Source と同期、着信の banner の骨格、Settings の「スマホとして使う」 | p003、WS170 | 着信の banner を p004 に（HFP の前に API の形だけ） |
| p005 | PBAP（vCard、連絡先の組、通話の履歴） | p004 | — |
| p006 | HFP の制御（SLC、AT、着信・発信・応答・終話・DTMF・音量・音声認識） | p004 | — |
| p007 | HFP の音: xHCI の isochronous（kernel の詳細設計と design-reviewer が先）、bt-usb の interface 1、UAPI（Q9）、SCO・eSCO、audiod、CVSD の後に mSBC | p006、Q9 | — |
| p008 | 実機の試験と debug（Android・iPhone） | p007（Q13 の早い確かめは p003 の後） | — |
| p009 | 規約の全文の見直し | p002〜p008 | — |

見積もりは ws.md の約 121 LW のまま（SDP の server の分は p002 の 22 LW の中に含める。足りなければ p002 の詳細設計で見直す）。

## Event

- 2026-10-09 深夜: 第 1 版（P1）。design-reviewer の review の前。
