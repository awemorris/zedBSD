<!-- awesome-plan project=zedbsd record=ws197-p005 -->

# ws197-p005: PBAP の PCE（スマホの電話帳と通話の履歴）、Phone の app の連絡先の組（詳細設計）

Phase ID: `ws197-p005`
Parent: [WS197](../ws.md)
Status: planning（2026-10-11 P1: 詳細設計の第 1 版。design-reviewer の review の前）
Phase disposition: normal
Queue: Q1 の投入（2026-10-11「次は WS197 p005（PBAP、plan/ws197/ws.md の Phase の表どおり）に進んで」、ユーザー 2026-10-09 の順「OBEX, MAP, Integration, PBAP, HFP」）
Branch: `agent/p1-ws197`（区切りごとに main へ merge、ベータ2 に入れる、2026-10-10 ユーザー）
依存: p002（RFCOMM・OBEX・SDP、cleared）、p003（phone link・MAP・socket の PHONE、cleared）、p004a〜c（SMS の層、cleared 候補・UAT 待ち）、BUG-287（usb-bt の data pipe、UAT 待ち。実機の PBAP は ACL の受けが要る）
所有 path: `userland/base/bluetoothd/`、`userland/desktop/libkeiland-backend*/`（phone の分）、`userland/desktop/wayland/phone-shell.c`、`userland/desktop/libkeiland/`（phone の分）、`userland/desktop/include/keiland/keiland.h`（phone の分）、`userland/desktop/phone/`、`userland/desktop/settings/page-bluetooth.c`、`plan/ws197/`

版: 2026-10-11 第 1 版（P1）。

前提のユーザーの決定（p001 §11、2026-10-09「全部推しどおり」）:

- Q2 持ち主だけが見る。bluetoothd と compositor は中継だけで連絡先・履歴の中身を disk に書かない。保存は Phone の app の `~/Documents/Phone/`。
- **Q3 スマホの連絡先は別の組**: 差分で更新、手では直さない、一覧では番号で手元の連絡先と重ねる。
- Q12 OBEX の認証は使わない。Q13 (a) Android が先。Q14 logout で切る。
- **Q16 (a) MAP 1.1・PBAP 1.1 を名乗り GOEP 1.1（RFCOMM）だけ**。Folder Version Counters・Database Identifier（1.2）は使わず、電話帳は毎回全部を読み、差分は app の側で取る。

## 0. 出典

- PBAP 1.2.3 の PDF（2026-10-09 に P1 が手元に取った。複写は repository に入れない）。節番号はこの版の物。1.1 の相手との形は §2.7・§9（表 9.1: PCE が RFCOMM だけを出せば、L2CAP と RFCOMM の両方を出す PSE とも GOEP 1.1 で話す）。
- 確かめた事: application parameter の tag（§6.2.1 の表: Order 0x01 … MaxListCount 0x04（2 byte）、ListStartOffset 0x05（2 byte）、PropertySelector 0x06（8 byte）、Format 0x07（0 = 2.1、1 = 3.0）、PhonebookSize 0x08（2 byte）、NewMissedCalls 0x09、PbapSupportedFeatures 0x10（4 byte））、PropertySelector の bit（表 5.1: 0 VERSION、1 FN、2 N、7 TEL、21 UID、28 X-IRMC-CALL-DATETIME、31 X-BT-UID）、MaxListCount = 0 は PhonebookSize だけで Body 無し（§5.1.4.3）、Get は 0x83 だけ（0x03 は使わない、§6.1）、Target の UUID `796135f0-f0c5-11d8-0966-0800200c9a66`（§6.4）、Connect の PbapSupportedFeatures は PSE の record にその属性がある時に必須（§6.4 の C3）、pb の 0.vcf は持ち主の card（§3.1.5.2）、履歴の 1.vcf が一番新しい（§3.1.5.3 の終わり）、履歴の時刻はスマホの local time（§3.1.4.1）、文字は UTF-8 だけ（§3.1.4）、PSE の record の SupportedRepositories と PbapSupportedFeatures（§7.1.2、無ければ 0x00000003 と見なす）。
- Assigned Numbers の値（PSE 0x112F、PCE 0x112E、PBAP の profile 0x1130、SupportedRepositories 0x0314、PbapSupportedFeatures 0x0317）は手元に文書が無いので「(Assigned Numbers、確かめる)」。p008 の実機の SDP の dump で照合する（p003 §0 と同じ扱い）。
- vCard 2.1（versit）・3.0（RFC 2425・2426）の行の折り返し・quoted-printable・parameter の形は記憶による（「(vCard、確かめる)」）。試験の正解は手で書いた例にする。
- 他の OS の実装（BlueZ・Android・Apple）の code は読まない。

## 1. 範囲

作る物:

1. bluetoothd の PCE（`pbap.c`）: SDP で PSE を探し、RFCOMM の DLC、OBEX Connect（Target PBAP）、PullPhoneBook（`x-bt/phonebook`）で `telecom/pb.vcf`（電話帳）と `telecom/ich.vcf`・`och.vcf`・`mch.vcf`（受けた・掛けた・出なかった通話）を page で読む。
2. `vcard.c`: vCard 2.1・3.0 の読み（§4）。電話帳の 1 件を「縮めた vCard 3.0」（FN・N・TEL・UID だけ）にし、履歴の 1 件から時刻・種類・番号・名前を取る。
3. phone link の profile の多重（`phonemux.c`、§3）: 今の phone link は profile を 1 つ（MAP）しか持たない。MAP と PBAP（後で HFP）を束ねる。
4. socket の `PHONE PAGE contacts|calls`（§5）と item の行。
5. 中継: libkeiland-backend（zedBSD）・compositor・libkeiland の `what` に `KL_PHONE_CONTACTS`・`KL_PHONE_CALLS`（§6）。
6. Phone の app: スマホの連絡先の組 `contacts/bt-<address>/`、差分の書き込みと消し、番号での重ね、通話の履歴の item、同期の目印（§7）。
7. Settings の「Use as phone」で contacts の profile も入れる（§8）。

作らない物: SIM の電話帳（`SIM1/telecom`）、speed dial・favorites（1.2）、PHOTO、vCard の検索・並べ替え（`x-bt/vcard-listing`）、`cch.vcf`（ich・och・mch で足りる）、Folder Version Counters・Database Identifier・X-BT-UID・Enhanced Missed Calls（1.2、Q16 (a)）、手元からスマホへの書き込み（PBAP に無い）、出なかった着信の通知（HFP の p006）。

### 1.1 p001 からの変更

| p001 | この設計 | 理由 |
| --- | --- | --- |
| §8.3 `PHONE CONTACT source=… length=` の event | live の event は作らない（PBAP に通知は無い）。`PHONE PAGE contacts` の答えの item の行（§5.2） | PBAP 1.1 は pull だけ |
| §7.2 「Folder Version Counters・Database Identifier が変わっていなければ電話帳を読まない」 | 毎回全部を読む（Q16 (a)） | 1.2 の機能 |
| §8.5 store の連絡先の上限 6000 | 手元 1024 と別に、スマホの組は 5000 まで（§7.1） | p001 §7.2 の 5000 に合わせる |
| p004 §0 の profile の bit（1 messages、2 contacts、4 calls） | contacts の bit で電話帳と通話の履歴の両方を読む（§8） | Android の許可は「連絡先と通話履歴」で 1 つ（推測、p008 で確かめる）。calls の bit は HFP（p006）の通話の制御 |

## 2. 部品と file

| 部品 | file | 変更 |
| --- | --- | --- |
| vCard | `userland/base/bluetoothd/vcard.c`・`.h`（新） | §4。純粋な関数 |
| PCE | `userland/base/bluetoothd/pbap.c`・`.h`（新） | §5。map.c と同じ形の hook と操作の queue |
| profile の多重 | `userland/base/bluetoothd/phonemux.c`・`.h`（新） | §3。純粋な振り分け |
| main | `main.c` | mux・pbap の配線、`PHONE PAGE contacts|calls` の request、SHOW と STATE に `contacts=` |
| socket の行 | `phoneio.c`・`protocol.h` | §5.2 の item の行の組み立て |
| backend | `libkeiland-backend-zedbsd/phone-zedbsd.c`、`libkeiland-backend/keiland-backend.h` | `KL_BACKEND_PHONE_WHAT_CONTACTS`・`_CALLS`、item の行の読み |
| compositor | `wayland/phone-shell.c` | `what` の検査を広げる、STATE の contacts |
| libkeiland | `libkeiland/system/*`、`include/keiland/keiland.h` | `KL_PHONE_CONTACTS`・`KL_PHONE_CALLS`、`struct kl_phone_link` の `contacts`（最後に足す）、能力の bit `KL_SYSTEM_HAS_PHONE_CONTACTS`。版の番号は merge の時に Q1 が割り当てる |
| Phone の app | `phone/store.c`・`phone.h`・`main.c`・`view.c` | §7 |
| Settings | `settings/page-bluetooth.c` | §8 |

## 3. profile の多重（`phonemux.c`）

今の `struct btd_phone_profile`（phone.h）は 1 つで、main が MAP を入れている（main.c の `phone_profile`）。phone.c は変えず、main と phone.c の間に振り分けの profile を挟む:

- mux は子の profile を 3 つまで持つ（MAP、PBAP、後の HFP）。phone.c には mux の hook を 1 つの profile として渡す。
- `ready`・`ended`: 全部の子へ順に。
- **SDP**: phone.c の問い合わせは 1 度に 1 つ（`btd_phone_sdp_query` は 2 つ目に EBUSY）。子は mux の `sdp_query(child, uuid)` を呼ぶ。mux は持ち主の子を覚えて phone.c に渡し、`sdp_done` をその子だけに返す。EBUSY はそのまま子に返す（MAP は今も EBUSY で `BTD_MAP_BUSY_MS` 後にやり直す。PBAP も同じ）。
- **DLC**: 子は mux の `dlc_open(child, server_channel)` を呼ぶ。mux は「server channel → 子」の表（8 個）を持ち、`opened(dlci, server_channel, ours)` の ours=1 は表の子、ours=0（スマホが開いた bluetoothd の server channel、今は MNS の 16 だけ）は `accept` が 1 を返した子へ。以後 dlci → 子の表（8 個）で `data`・`writable`・`closed` を振り分け、`closed` で表から消す。`open_failed(server_channel)` は表の子へ。
- `accept(server_channel)`: 子に順に聞き、最初に 1 を返した子を覚える。
- 子の dlc_write・dlc_close は mux を通らない（phone.c の関数を直に。dlci は一意）。
- 純粋な表の操作にして host で試す（偽の phone の hook と 2 つの偽の子）。

## 4. vCard（`vcard.c`）

### 4.1 読む形（vCard、確かめる）

- 入力は 1 件（`BEGIN:VCARD` 〜 `END:VCARD`）か、それの並び（PullPhoneBook の Body）。`btd_vcard_next(text, length, &card_start, &card_length)` で 1 件ずつ切り出す（`BEGIN:VCARD` の行から、対の `END:VCARD` の行まで。入れ子の `AGENT` の vCard は数えて飛ばす）。
- 行: CRLF と LF を受ける。**3.0 の折り返し**（次の行が空白か TAB で始まれば、その 1 字を除いて前の行に続ける）。**2.1 の quoted-printable の soft line break**（`ENCODING=QUOTED-PRINTABLE` の値で行の最後が `=` なら次の行に続く）。
- 名前と parameter: `[group.]NAME[;param]*:value`。名前と parameter の名前は大文字・小文字を問わない。2.1 の裸の parameter（`TEL;CELL:`）は `TYPE=CELL` と同じ。`CHARSET`: UTF-8 か無しだけを受け、他の値（ISO-8859-1 も、PBAP §3.1.4 は UTF-8 だけを許す）の値は捨てる（その property だけ）。`ENCODING`: QUOTED-PRINTABLE（2.1）は解く、`BASE64`・`B` は値を飛ばす（PHOTO を頼まないが送る機種のため）。
- 値の escape（3.0）: `\n`・`\N` は改行、`\,`・`\;`・`\\` は字。
- 読む property: `VERSION`、`FN`、`N`（`;` で 5 つ、FN が空の時に「名 姓」の順で作る。順はスマホの言語によらず given + family、日本語の名では違和感があるので **FN を先に使う**）、`TEL`（値と TYPE: CELL・HOME・WORK・VOICE・FAX・PAGER・PREF。値の中の `[0-9+*#]` 以外を除いた物を番号にする。`tel:` の URI の形も受ける）、`UID`、`X-IRMC-CALL-DATETIME`（parameter の MISSED・RECEIVED・DIALED（2.1 は裸、3.0 は `TYPE=`）と値 `YYYYMMDDTHHMMSS[Z]`）。他の property は捨てる。
- 上限: 1 件 16 KB（越えれば読まない、数える）、1 件の TEL は 8 個まで（越えた分は捨てる）、FN・N は 256 byte で UTF-8 の文字の途中で切らない（`btd_mapxml_utf8_cut`）、不正な UTF-8 と制御文字は U+FFFD。Body 全体の上限は §5.3。

### 4.2 出す物

- 電話帳の 1 件 → `struct btd_vcard_contact`（name（FN、無ければ N から、無ければ最初の番号）、tel[8]（番号と TYPE の語）、uid、`key`）。**縮めた vCard 3.0** を `btd_vcard_reduce` で書く: `BEGIN:VCARD`、`VERSION:3.0`、`FN:`、`N:`（あれば）、`TEL;TYPE=…:` を順に、`UID:`（あれば）、`END:VCARD`、CRLF、値は 3.0 の escape、75 byte の折り返しはしない（app の store が読む）。
- **key**（連絡先の同一性、p001 R11）: UID があれば FNV-1a 64 bit（`u|` + UID）、無ければ FNV-1a（`n|` + FN + `|` + 番号を並べ替えて `,` で繋いだ物）の 16 桁の 16 進。限界: UID の無いスマホで名前か番号を変えると別の連絡先になる（古い物は全体の同期の後に消える、§7.2）。
- 履歴の 1 件 → `struct btd_vcard_call`（kind: received・dialed・missed（parameter、無ければ folder から）、datetime の文字列、番号（最初の TEL、空もある）、名前（FN か N）、`key` ＝ FNV-1a（kind の 1 字 + `|` + datetime + `|` + 番号））。datetime の無い・読めない件は `key` を出すが時刻は 0（§5.3 で扱う）。
- 時刻: `Z` があれば UTC、無ければスマホの local time で、zedBSD の timezone で UNIX 秒にする（`zone=local`、MAP の `local_offset` の hook と同じ物を使う）。限界: スマホと zedBSD の timezone が違えば時刻がずれる（記録、MAP の MSETime に当たる物が PBAP 1.1 に無い）。

## 5. PCE（`pbap.c`）と socket

### 5.1 始めと状態

- 状態: OFF → SDP → CONNECTING（DLC と OBEX Connect）→ READY → FAILED（やり直しの待ち）。map.c と同じ hook の組（clock・wall・local_offset・wanted・sdp_query・dlc_open・dlc_write・dlc_close・answer・room・changed・log）。`wanted` は持ち主の記録の profiles に contacts の bit があること。
- link が READY になったら（mux の ready）: SDP で PSE（0x112F）。record の RFCOMM の channel、SupportedRepositories（0x0314、bit 0 が無ければ `why=no-pb`、やり直さない）、PbapSupportedFeatures（0x0317 の有無を覚える）。PSE が無ければ `why=no-pse`（やり直さない、次の link で）。
- DLC → OBEX Connect: Target（PBAP の UUID、16 byte）、PSE の record に 0x0317 があれば App Parameters の PbapSupportedFeatures（tag 0x10、4 byte、値 0x00000001 = Download だけ）（§6.4 の C3。1.1 の PSE には付けない）。**Connect の答えの時間は 60 s**（スマホが許可の画面を出す間、MAP と同じ）。0xC1・0xC3 は `why=permission`（600 s ごとにやり直す、`PHONE LINK on` ですぐ）、他は `why=refused`（30 s から倍で 600 s まで）。
- READY: STATE に `contacts=ready`。MAP と PBAP は別の DLC・別の OBEX session（同じ RFCOMM の session の上）。
- 失敗（DLC の切断、timeout、link の喪失）: 実行中・待ちの request は `ERROR lost`、`contacts=failed why=…`、上の間隔でやり直す。
- スマホが PBAP の OBEX を一定時間で切る機種（推測）: 切れたら FAILED にせず OFF に戻し、次の PAGE で接続し直す（`idle`。STATE は `contacts=ready` のまま、`ready` は「接続できる」の意味）。

### 5.2 socket の request と item の行

| request（持ち主と root） | 答え |
| --- | --- |
| `PHONE PAGE contacts cursor=C count=N` | `PHONE CONTACT …` の行（N 個まで）、`PHONE PAGE-END cursor=… more=0|1 count=… skipped=…`、`DONE` |
| `PHONE PAGE calls since=N cursor=C count=N` | `PHONE CALL-LOG …` の行、同じ終わり |

```
PHONE CONTACT key=<16 hex> name="<FN>" peer="<最初の番号>" tels=<n> length=<m>
<m byte: 縮めた vCard 3.0（§4.2）>
PHONE CALL-LOG key=<16 hex> kind=received|dialed|missed time=<UNIX 秒> zone=local|utc|none datetime="<スマホの文字列>" peer="<番号>" name="<名前>"
```

- 行の escape と 2047 byte は p003 §9.1（`phoneio.c`）。名前・番号の 128 byte の切りも同じ。
- contacts が ready でない時は `ERROR not-ready`。1 つの client が待つ request は 1 つ（今の `waits_phone`）。

### 5.3 page の進め方

- **cursor** ＝ `<pbap session の 8 桁の 16 進>.<object>.<offset>`（object は 0 pb、1 ich、2 och、3 mch）。session（OBEX の Connect ごとに 1 ずつ増やす）が違えば `ERROR stale-cursor`（app は最初から）。
- **contacts**: object 0 だけ。offset 0 の最初に SIZE（MaxListCount 0x04 = 0、Name `telecom/pb.vcf`）で PhonebookSize を得る。PULL（Name `telecom/pb.vcf`、Type `x-bt/phonebook`、MaxListCount＝N、ListStartOffset＝o、Format 0x07 = 1（3.0）、PropertySelector 0x06 = `00 00 00 00 00 20 00 87`（bit 0 VERSION・1 FN・2 N・7 TEL・21 UID））。Body を `btd_vcard_next` で切り、1 件ずつ item の行を出す。**index 0（持ち主の card、§3.1.5.2）は飛ばす**（skipped に数える）。次の o ＝ o + 受けた件数。`o >= PhonebookSize` か受けた件数が 0 なら `more=0`。PhonebookSize が 5000 を越えても 5000 件で `more=0`（`capped=1` を PAGE-END に足す）。
- **calls**: object 1・2・3 の順。各 object の offset 0 で SIZE、PULL（Name `telecom/ich.vcf` など、PropertySelector = bit 0・1・2・7・28 = `00 00 00 00 10 00 00 87`）。履歴は新しい順（§3.1.5.3）なので、datetime が `since` より前の件が来たらその object を終えて次へ（その件は出さない）。datetime の無い件は出す（time 0、`zone=none`）。1 object 500 件まで。
- **Body の上限** 256 KB（N 件の vCard）。越えたら Abort し、同じ offset で N を半分にしてやり直す（N＝1 でも越えれば、その 1 件を飛ばして o + 1、skipped に数える）。
- 操作の queue: map.c §8.4 と同じ形（SIZE・PULL の 2 種、24 個まで、token の cancel、client の room の待ち 30 s で `ERROR slow`）。PBAP は live の event が無いので割り込みは無い。
- 限界（記録）: page の間にスマホの電話帳が変わると offset がずれ、1 件の重複（key で除く）か飛び（次の全体の同期で拾う）がある。

### 5.4 SHOW・STATE・log

- `PHONE SHOW` と STATE に `contacts=off|connecting|ready|failed` を足す（`why` は messages の why と別の `contacts_why=`）。
- log は件数・response code・why だけ（名前・番号・vCard は書かない、p001 R22）。

## 6. 中継（backend・compositor・libkeiland）

- libkeiland: `KL_PHONE_CONTACTS 1U`・`KL_PHONE_CALLS 2U`（keiland.h の `KL_PHONE_MESSAGES` の注記で予約済み）。`kl_system_phone_sync(what, since, limit, cursor, count)`: contacts は since・limit を使わない（0）、calls は since を使う。能力の bit `KL_SYSTEM_HAS_PHONE_CONTACTS`（compositor が contacts・calls を中継できる）。版の番号は Q1 が merge の時に割り当てる。
- `struct kl_phone_item` は変えない（大きさも）。使い方:
  - contacts: `what` 1、`key` ＝ key、`name` ＝ FN、`peer` ＝ 最初の番号、`text`・`length` ＝ 縮めた vCard 3.0、`folder` ＝ TEL の数、他は 0。
  - calls: `what` 2、`key`、`folder` ＝ 0 received・1 dialed・2 missed、`direction` ＝ dialed なら 1、`time`・`zone`（0 phone は使わない、1 utc、2 local、3 none）、`datetime`、`peer`、`name`、`text` は空。
- `struct kl_phone_link` の最後に `unsigned contacts;`（0 off、1 connecting、2 ready、3 failed）を足す（p004 §3 の size の規則で、古い app は短い size を渡す）。
- backend（zedBSD）: `KL_BACKEND_PHONE_WHAT_CONTACTS`・`_CALLS`、`PHONE PAGE contacts|calls` の行、`PHONE CONTACT`・`PHONE CALL-LOG` の行と本文の読み（今の `PHONE MESSAGE` の読みと同じ口）、STATE・SHOW の `contacts=`。Linux・FreeBSD の backend は「無い」のまま（Q10）。
- compositor（phone-shell.c）: `page.what` の検査（今は MESSAGES だけ、734 行）を 0〜2 に。page の item の中継は what を運ぶだけ（今の形のまま）。同期は全体で 1 本（p004 §4）のまま: contacts の page と messages の page は順に。

## 7. Phone の app（WS170 の store の変更）

### 7.1 置き方

- スマホの連絡先: `contacts/bt-<address の : を除いた 12 字>/<key>.vcf`（縮めた vCard 3.0 に `X-KEILAND-SOURCE:bt:<address>` の行を足した物）。手では直さない（app は編集の UI を出さない）。上限 5000（手元の `STORE_CONTACTS_MAX` 1024 と別）。
- 通話の履歴: 会話の folder（`messages/<連絡先の id か n<数字>>/`）に `c<key>.txt`（`Kind: call`、`Channel: line`、`Direction: in|out`、`Date`、`State: answered`（received）・`missed`・なし（dialed）、`Source: bt:<address>:pbap:<key>`）。同じ key の file があれば書かない。通知は出さない。
- 同期の目印 `sync/bt-<address>.state` に `contacts_at <UNIX 秒>`（最後に全体の同期を終えた時）と `calls_since <UNIX 秒>` の行を足す。

### 7.2 同期

- 時期: link の contacts が ready になった時（1 日 1 回まで、`contacts_at` から 24 時間）、と Phone の app の「今すぐ同期」（今の messages の同期の口に足す）。calls は messages の同期と一緒に（`calls_since` から、初回は 30 日、重なり 24 時間）。
- contacts の全体の同期: page を `more=0` まで（count 32）。受けた key の集合を覚え、1 件ごとに file が無いか中身が違えば書く（同じなら触らない、cloud の上の書き換えを減らす）。**`more=0` で途中の失敗・DROPPED が無かった時だけ**、組の folder の中で集合に無い file を消す（スマホで消した連絡先）。`capped=1` の時は消さない。
- 失敗・stale・DROPPED: 目印を進めず、次の時期にやり直す（messages と同じ）。

### 7.3 表示と重ね

- 名前の引き: 番号の鍵（p004 §0 の `ph_number_key`）→ 名前の表を、手元の連絡先とスマホの連絡先（全部の TEL）から作る。**手元の連絡先が先**。番号の会話（`n<数字>`）とスマホの item の `name` が空の時は、この表の名前を出す。
- 一覧: 今の一覧（会話のある連絡先と手元の連絡先）に、**会話か通話のあるスマホの連絡先だけ**を足す（5000 件を一覧に並べない）。スマホの連絡先のどの番号かが手元の連絡先の番号と同じ鍵なら、スマホの連絡先は一覧に出さない（手元が勝つ）。スマホの連絡先の行は名前の横に小さく「Phone」の印（編集できない）。
- 新しい message の宛先: 今の「番号を書く」に加え、名前を書くとスマホの連絡先の名前で候補を出す（**p005 では入れない、Future Work**。理由: view の候補の UI が今は無い）。
- `struct ph_contact` に `numbers`（TEL の配列、スマホの連絡先だけ）と `phone`（スマホの組の印）を足す。`ph_store_conversation` の番号の照合は numbers も見る。

### 7.4 判断の要る点（ユーザー、推し付き）

| # | 問い | 選択肢 | 推し |
| --- | --- | --- | --- |
| Pc1 | スマホの連絡先を一覧にどう出すか | (a) 会話・通話のある物だけ（他は名前の引きにだけ使う）/ (b) 全部（5000 件まで、手元の後ろに）/ (c) 出さない（名前の引きだけ） | **(a)**（一覧が会話の画面として使える。全部を見るのはスマホで） |
| Pc2 | 通話の履歴をどの switch で読むか | (a) contacts の switch（Android の許可が連絡先と履歴で 1 つ）/ (b) calls の switch（HFP と一緒） | **(a)** |

## 8. Settings

- 「Use as phone」の `kl_system_phone_link_set` の profiles を `KL_PHONE_PROFILE_MESSAGES | KL_PHONE_PROFILE_CONTACTS` にする。行の文に「Contacts …」の状態（connecting・ready・許可の案内「Allow access to contacts on the phone」）を足す。profile ごとの switch は作らない（1 つの「Use as phone」、p004c のまま）。
- 既に使っているスマホ（profiles=m の記録）は、次に「Use as phone」を押すか、Settings を開いた時に profiles を足さない（利用者の操作なしに許可の画面をスマホに出さない）。行に「Also use contacts」の button を出す（押すと LINK on profiles=m,c）。

## 9. 試験

### 9.1 host（`plan/ws197/tests/`、ASan・UBSan）

試験の期待値（tag・PropertySelector の byte・Target の UUID・opcode）は §0 の表から手で書いた定数にし、実装の macro を使わない。

| 試験 | 内容 |
| --- | --- |
| `bt-vcard-host-test`（新） | 2.1 と 3.0 の手の例（折り返し、QP と soft break、裸の parameter、CHARSET の UTF-8・他、BASE64 の PHOTO を飛ばす、escape、group 付きの名前、`tel:`、TEL 9 個、16 KB、AGENT の入れ子、並びの切り出し、持ち主の card）、縮めた vCard の手の正解、key の手の値（UID あり・無し、番号の並べ替え）、履歴（MISSED・RECEIVED・DIALED の 2.1 と 3.0、`Z`、空の datetime）、fuzz 20 万回 |
| `bt-phonemux-host-test`（新） | SDP の持ち主、EBUSY、DLC の ours・accept の振り分け、dlci の表と closed、open_failed、表の満ち、ready・ended の全部への配り |
| `bt-pbap-host-test`（新） | 台本の PSE（map の試験と同じ形）: SDP の record（RFCOMM の channel、0x0314 の bit 0 無し → no-pb、0x0317 の有無で Connect の App Parameters の有無）、Connect の byte（Target 16 byte、tag 0x10 の 4 byte）と 60 s、0xC3 → permission と 600 s、SIZE の App Parameters の byte（`04 02 00 00`）、PULL の byte（MaxListCount・ListStartOffset・Format `07 01 01`・PropertySelector の 8 byte）、0.vcf を飛ばす、cursor（session・object・offset）と stale、5000 の capped、calls の since で object を終える・3 つの順、256 KB で N の半分と 1 件の飛ばし、Get の Continue（0x90）の続き、room の待ちと slow、cancel、DLC の切断で lost と idle、STATE の contacts |
| `bt-phoneio-host-test`（足す） | CONTACT・CALL-LOG の行、escape、2047 byte |
| `phone-backend-host-test`（足す） | `PHONE PAGE contacts|calls` の行、CONTACT・CALL-LOG の読み（本文付き）、STATE の contacts |
| `phone-shell-host-test`（足す） | what 1・2 の中継、what 3 は EINVAL、link の contacts |
| `phone-store-host-test`（足す） | 組の folder の読み・書き、同じ中身は書かない（mtime）、全体の同期の後の消し、capped・失敗では消さない、5000 の上限、名前の引き（手元が先）、一覧の (a)、numbers の照合、通話の item の書きと重複無し |
| WS143 `bt-daemon-host-test.sh` | main.c を変えるため |

### 9.2 QEMU

PBAP の相手が QEMU に無い（p003 §10.2 と同じ）。T1 は頼まない（HID の回帰も、2026-10-10 ユーザー「流しすぎです。もう不要」）。

### 9.3 実機（5330、ユーザーの UAT。BUG-287 の直しの image の後）

Android で: 「Use as phone」→ スマホの「連絡先と通話履歴へのアクセス」の許可 → Phone の app の会話に名前が出る（番号の会話がスマホの連絡先の名前に）、通話の履歴が会話に入る、スマホで連絡先を消して「今すぐ同期」→ app の組から消える。bluetoothd の log に `pbap: ready`・`pbap: page contacts …`。

## 10. 実装の順（WIP commit の単位、区切りごとに SHA を Q1 へ）

| i | 内容 | 確かめ |
| --- | --- | --- |
| i01 | `vcard.c`（§4） | bt-vcard-host-test、fuzz |
| i02 | `phonemux.c`（§3）と main の配線（MAP を mux の子に） | bt-phonemux、bt-phone-host-test.sh 全部、WS143、build |
| i03 | `pbap.c` の始め（SDP・DLC・Connect・やり直し・STATE）（§5.1） | bt-pbap（始めの部分）、build |
| i04 | `pbap.c` の page（§5.3）、`phoneio.c` の行、main の `PHONE PAGE contacts|calls`・SHOW（§5.2・§5.4） | bt-pbap、bt-phoneio、WS143、build |
| i05 | backend・compositor・libkeiland（§6） | phone-backend、phone-shell、zedBSD と keiland-linux の build |
| i06 | Phone の app の store と同期と表示（§7） | phone-store、build |
| i07 | Settings（§8）、style-check、phase.md の記録 | bt-desktop、build |

## 11. 受け入れ

- §9.1 の host の試験と fuzz が全部 PASS、WS143 の host の試験が PASS、zedBSD（bluetoothd・wayland・libkeiland・phone・settings）と keiland-linux の build が warning 0、style-check の変更箇所 0。
- 実機（§9.3）は p008 とユーザーの UAT。この Phase では未実施と書く（p003 §12 と同じ）。

## 12. 危険と未確認

| 項目 | 内容 | いつ |
| --- | --- | --- |
| GOEP 1.1 の PCE への相手の振る舞い | Android の PSE が RFCOMM の 1.1 の PCE に電話帳を出すか（表 9.1 では出すはず） | UAT |
| 許可の画面 | Android は Connect の間に「連絡先と通話履歴へのアクセス」を聞くか、MAP の許可と別か | UAT |
| UID | Android・iPhone の vCard に UID が入るか（無ければ名前と番号の key、改名で別の連絡先） | UAT |
| 時刻 | 履歴の datetime はスマホの local time。zedBSD と timezone が違うとずれる | UAT |
| SDP の属性 ID | Assigned Numbers の値を手元で確かめていない | 実機の SDP の dump |
| page の間の変化 | offset のずれ（§5.3）。全体の同期の消しは `more=0` で失敗が無い時だけなので、飛んだ 1 件は消されない（次の同期で書かれる） | — |
| store の大きさ | 5000 件の .vcf を開く時に全部読む（今の store は全部を読む）。1 件 200 byte で 1 MB、開く時間は計る | i06 |
| 1 つの Phone の app の同期 | compositor の同期は全体で 1 本。contacts の 5000 件（160 page）の間 messages の同期が待つ | i06 で順を決める（messages を先） |

## 13. 見積もり

i01 1.5、i02 1、i03 1.5、i04 1.5、i05 1、i06 2、i07 0.5、計 **9 LW**（ws.md の 8 LW に mux の +1）。

## Event

- 2026-10-11: 第 1 版（P1）。PBAP 1.2.3 の §2.7・§3.1・§5.1・§6.2〜§6.4・§7.1・§9 を読んで書いた。
