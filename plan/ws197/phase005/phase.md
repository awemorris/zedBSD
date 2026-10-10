<!-- awesome-plan project=zedbsd record=ws197-p005 -->

# ws197-p005: PBAP の PCE（スマホの電話帳と通話の履歴）、Phone の app の連絡先の名前（詳細設計）

Phase ID: `ws197-p005`
Parent: [WS197](../ws.md)
Status: planning（2026-10-11 P1: 第 1 版の review-1 は blocker 1・major 13・minor 14、i01 vcard.c だけ GO → **i01 は実装済み（da5dbf9ac）**。第 2 版（この版）を review-2 に出す。ユーザーの判断 Pc1〜Pc6 は答え待ちで、推しを**仮**として書いた（§12）。再開点は下の「再開の手順」）
Phase disposition: normal
Queue: Q1 の投入（2026-10-11「次は WS197 p005（PBAP、plan/ws197/ws.md の Phase の表どおり）に進んで」、ユーザー 2026-10-09 の順「OBEX, MAP, Integration, PBAP, HFP」）
Branch: `agent/p1-ws197`（区切りごとに main へ merge、ベータ2 に入れる、2026-10-10 ユーザー）
依存: p002（RFCOMM・OBEX・SDP、cleared）、p003（phone link・MAP・socket の PHONE、cleared）、p004a〜c（SMS の層、cleared 候補・UAT 待ち）、BUG-287（usb-bt の data pipe、UAT 待ち。実機の PBAP は ACL の受けが要る）
所有 path: `userland/base/bluetoothd/`、`userland/desktop/libkeiland-backend*/`（phone の分）、`userland/desktop/wayland/phone-shell.c`・`kl-system-protocol.h`（phone の分）、`userland/desktop/libkeiland/`（phone の分）、`userland/desktop/include/keiland/keiland.h`（phone の分）、`userland/desktop/phone/`、`userland/desktop/settings/page-bluetooth.c`、`plan/ws197/`

版: 2026-10-11 第 1 版（P1）。2026-10-11 **第 2 版**（P1、review-1 の B1・M2〜M13・minor 1〜14 を直した。直した所は `[B1]`・`[M2]`・`[m3]`（minor 3）の印。ユーザーの判断の仮の所は「**仮（ユーザーの答え待ち）**」の印）。

前提のユーザーの決定（p001 §11、2026-10-09「全部推しどおり」）:

- Q2 持ち主だけが見る。bluetoothd と compositor は中継だけで連絡先・履歴の中身を disk に書かない。保存は Phone の app の `~/Documents/Phone/`。
- **Q3 スマホの連絡先は別の組**: 差分で更新、手では直さない、一覧では番号で手元の連絡先と重ねる。
- Q12 OBEX の認証は使わない。Q13 (a) Android が先。Q14 logout で切る。
- **Q16 (a) MAP 1.1・PBAP 1.1 を名乗り GOEP 1.1（RFCOMM）だけ**。Folder Version Counters・Database Identifier（1.2）は使わず、電話帳は毎回全部を読み、差分は app の側で取る。

## 0. 出典

- PBAP 1.2.3 の PDF（2026-10-09 に P1 が手元に取った。複写は repository に入れない）。節番号はこの版の物。1.1 の相手との形は §2.7・§9（表 9.1: PCE が RFCOMM だけを出せば、L2CAP と RFCOMM の両方を出す PSE とも GOEP 1.1 で話す）。
- 確かめた事: application parameter の tag（§6.2.1 の表: Order 0x01 … MaxListCount 0x04（2 byte）、ListStartOffset 0x05（2 byte）、PropertySelector 0x06（8 byte）、Format 0x07（0 = 2.1、1 = 3.0）、PhonebookSize 0x08（2 byte）、NewMissedCalls 0x09、PbapSupportedFeatures 0x10（4 byte））、PropertySelector の bit（表 5.1: 0 VERSION、1 FN、2 N、7 TEL、21 UID、28 X-IRMC-CALL-DATETIME、31 X-BT-UID）、MaxListCount = 0 は PhonebookSize だけで Body 無し（§5.1.4.3）、**Get は 0x83 だけ（0x03 は使わない、§6.2.2）[m3]**、Target の UUID `796135f0-f0c5-11d8-0966-0800200c9a66`（§6.4）、Connect の PbapSupportedFeatures は PSE の record にその属性がある時に必須（§6.4 の C3、§5.1 の M9 の扱い）、pb の 0.vcf は持ち主の card（§3.1.5.2）、履歴の 1.vcf が一番新しい（§3.1.5.3 の終わり、**should**であって must でない [m4]）、履歴の時刻はスマホの local time（§3.1.4.1）、文字は UTF-8 だけ（§3.1.4）、PSE の record の SupportedRepositories と PbapSupportedFeatures（§7.1.2、無ければ 0x00000003 と見なす）、PCE の record（§7.1.1: ServiceClassIDList に PCE、BluetoothProfileDescriptorList に PBAP と版、M）[M8]。
- Assigned Numbers の値（PSE 0x112F、PCE 0x112E、PBAP の profile 0x1130、SupportedRepositories 0x0314、PbapSupportedFeatures 0x0317）は手元に文書が無いので「(Assigned Numbers、確かめる)」。p008 の実機の SDP の dump で照合する（p003 §0 と同じ扱い）。
- vCard 2.1（versit）・3.0（RFC 2425・2426）の行の折り返し・quoted-printable・parameter の形は記憶による（「(vCard、確かめる)」）。試験の正解は手で書いた例にする。2.1 の折り返しは RFC 822 の形（CRLF の後の空白は値に残る）、3.0 は空白 1 字を除く、と読んだ（i01 で実装済み）[m8]。
- 他の OS の実装（BlueZ・Android・Apple）の code は読まない。Android・iPhone の振る舞いの記述は全部「推測」で、§13 の UAT で確かめる。

## 1. 範囲

作る物:

1. bluetoothd の PCE（`pbap.c`）: SDP で PSE を探し、RFCOMM の DLC、OBEX Connect（Target PBAP）、PullPhoneBook（`x-bt/phonebook`）で `telecom/pb.vcf`（電話帳）と `telecom/ich.vcf`・`och.vcf`・`mch.vcf`（受けた・掛けた・出なかった通話）を page で読む。
2. `vcard.c`（i01、**実装済み** da5dbf9ac）: vCard 2.1・3.0 の読み（§4）。
3. phone link の profile の多重（`phonemux.c`、§3）: 今の phone link は profile を 1 つ（MAP）しか持たない。MAP と PBAP（後で HFP）を束ねる。DM で断られた DLC の振り分け（MAP の既存の穴も）[M10]。
4. bluetoothd の PCE の SDP record（phone.c、§3.3）[M8]、handoff の profiles の既定を messages だけに（§8.1）[M3]。
5. socket の `PHONE PAGE contacts|calls`（§5）と item の行。
6. 中継: libkeiland-backend（zedBSD）・compositor・libkeiland の `what` に `KL_PHONE_CONTACTS`・`KL_PHONE_CALLS`、link の `contacts`・`contacts_why`（§6）。
7. Phone の app: スマホの電話帳の写し `phonebook/bt-<address>/`（store の連絡先の配列に入れない）、番号から名前を引く表、通話の履歴の item（§7）[B1]。
8. Settings の「Use as phone」で contacts の profile も入れる（§8）。

作らない物: SIM の電話帳（`SIM1/telecom`）、speed dial・favorites（1.2）、PHOTO、vCard の検索・並べ替え（`x-bt/vcard-listing`）、`cch.vcf`（ich・och・mch で足りる）、Folder Version Counters・Database Identifier・X-BT-UID・Enhanced Missed Calls（1.2、Q16 (a)）、手元からスマホへの書き込み（PBAP に無い）、出なかった着信の通知（HFP の p006）、新しい message の宛先の名前の候補（view に候補の UI が無い、Future Work）、スマホの連絡先だけの一覧（Pc1 (a)、§7.4）。

### 1.1 p001 からの変更

| p001 | この設計 | 理由 |
| --- | --- | --- |
| §8.3 `PHONE CONTACT source=… length=` の event | live の event は作らない（PBAP に通知は無い）。`PHONE PAGE contacts` の答えの item の行（§5.2） | PBAP 1.1 は pull だけ |
| §7.2 「Folder Version Counters・Database Identifier が変わっていなければ電話帳を読まない」 | 毎回全部を読む（Q16 (a)）、1 日 1 回まで | 1.2 の機能 |
| §8.5 store の連絡先の上限 6000 | 手元の連絡先（1024、今のまま）と別の `phonebook/` に 5000 件まで（§7.1）。store の連絡先の配列には入れない [B1] | p001 §7.2 の 5000、review-1 B1 |
| p004 §0 の profile の bit（1 messages、2 contacts、4 calls） | contacts の bit で電話帳と通話の履歴の両方を読む（§8、Pc2） | Android の許可は「連絡先と通話履歴」で 1 つ（推測）。calls の bit は HFP（p006）の通話の制御 |
| p001 §7.2 の vCard の文字 | CHARSET は UTF-8 か無しだけ（他の値の property は捨てる）[m8] | PBAP §3.1.4 は UTF-8 だけを許す |

## 2. 部品と file

| 部品 | file | 変更 |
| --- | --- | --- |
| vCard | `userland/base/bluetoothd/vcard.c`・`.h` | §4（i01 済み） |
| PCE | `userland/base/bluetoothd/pbap.c`・`.h`（新） | §5。map.c と同じ形の hook と操作の queue |
| profile の多重 | `userland/base/bluetoothd/phonemux.c`・`.h`（新） | §3。純粋な振り分け |
| phone link | `phone.c`・`phone.h` | PCE の SDP record（§3.3）[M8]、`BTD_PHONE_PENDING_DLCS` 2 → 4 [m10]、handoff の profiles の既定（§8.1）[M3] |
| record | `phonerec.c`・`.h` | `asked` の行（§8.1、Pc4 の仮） |
| MAP | `map.c`・`.h` | DM で断られた MAS の DLC を失敗に、OPENING の期限 [M10] |
| main | `main.c` | mux・pbap の配線（§5.5 [m1]）、`PHONE PAGE contacts|calls`、SHOW と STATE に `contacts=` |
| socket の行 | `phoneio.c`・`protocol.h` | §5.2 の item の行の組み立て |
| backend | `libkeiland-backend-zedbsd/phone-zedbsd.c`、`libkeiland-backend/keiland-backend.h` | §6.2 [M4]: `KL_BACKEND_PHONE_WHAT_CONTACTS`・`_CALLS`、`kl_backend_phone_item` に `what`、`kl_backend_phone_state` に `contacts`・`contacts_why`、item の行の読み |
| compositor | `wayland/phone-shell.c`、`kl-system-protocol.h` | §6.3 [M4・M6]: `what` の検査を 0〜2 に、item の送りの `what` を backend の item から、新しい event `link_contacts`（manager の新しい版） |
| libkeiland | `libkeiland/system/*`、`include/keiland/keiland.h` | §6.1 [M5・M6・M7]: `KL_PHONE_CONTACTS`・`KL_PHONE_CALLS`、`struct kl_phone_link` の最後に `contacts`・`contacts_why`、短い size の受け入れ、page の終わりの印の bit、`KL_SYSTEM_HAS_PHONE_CONTACTS`。版の番号は merge の時に Q1 |
| Phone の app | `phone/store.c`・`phone.h`・`main.c`・`view.c` | §7 [B1] |
| Settings | `settings/page-bluetooth.c` | §8 |

## 3. profile の多重（`phonemux.c`）と PCE の record

### 3.1 振り分け

今の `struct btd_phone_profile`（phone.h）は 1 つで、main が MAP を入れている。phone.c の profile の口は変えず、main と phone.c の間に振り分けの profile（mux）を挟む:

- mux は子の profile を 3 つまで持つ（MAP、PBAP、後の HFP）。phone.c には mux の hook を 1 つの profile として `btd_phone_set_profile` で渡す。
- `ready`・`ended`: 全部の子へ順に（登録の順）。
- **SDP**: phone.c の問い合わせは 1 度に 1 つ（`btd_phone_sdp_query` は 2 つ目に EBUSY）。子は `btd_phonemux_sdp_query(mux, child, uuid)` を呼ぶ。mux は持ち主の子を覚えて phone.c に渡し、`sdp_done` をその子だけに返す。EBUSY はそのまま子に返す（MAP は今も EBUSY で `BTD_MAP_BUSY_MS` 後にやり直す。PBAP も同じ）。
- **DLC を開く**: 子は `btd_phonemux_dlc_open(mux, child, server_channel, now)` を呼ぶ。mux は「server channel → 子」の表（8 個、満ちれば ENOSPC を子に）に書いてから phone.c を呼ぶ。
- **opened(dlci, server_channel, ours)**: ours=1 は「server channel → 子」の表の子へ、表の行を「dlci → 子」の表（8 個）へ移す。ours=0（スマホが開いた bluetoothd の server channel、今は MNS の 16 だけ）は `accept` が 1 を返した子へ（accept の時に覚えた「server channel → 子」）。
- 以後 dlci → 子の表で `data`・`writable`・`closed` を振り分け、`closed` で表から消す。
- **[M10] 表に無い dlci の closed**: 自分で開いた DLC が PN か SABM の答えの DM で断られると、rfcomm.c は `opened` 無しで `closed(dlci, BTD_RFCOMM_CLOSED_REFUSED)` だけを出す（rfcomm.c の `rfcomm_frame_dm` → `rfcomm_free`、`rfcomm_announced` が ours を真にするため）。mux は dlci の表に無い dlci の closed を `dlci >> 1`（server channel）で「server channel → 子」の表から引き、その子へ渡して行を消す。どちらの表にも無ければ捨てる（log に 1 行）。
- `open_failed(server_channel)`（RFCOMM の session が立たなかった）は「server channel → 子」の表の子へ、行を消す。
- `accept(server_channel)`: 子に順に聞き、最初に 1 を返した子を覚える。
- 子の `dlc_write`・`dlc_close` は mux を通らない（phone.c の関数を直に。dlci は一意）。
- `ended`: 全部の表を空にしてから子へ配る。
- 純粋な表の操作にして host で試す（偽の phone の hook と 2 つの偽の子）。

### 3.2 子の側の DM と期限 [M10]

- MAP（既存の穴を直す）: `btd_map_closed` は `dlci == mas_dlci` だけを見ていて、OPENING の間（`mas_dlci` は opened まで 0）の DM の closed を捨て、OPENING には期限が無い。直し: (1) state が OPENING で `dlci >> 1 == mas_channel` の closed は `map_fail(map, "refused", MAP_RETRY_STEP)`。(2) OPENING に期限 `BTD_MAP_OPEN_MS` 30 s（`btd_map_deadline` に入れる、過ぎれば `map_fail("open-timeout")`）。
- PBAP も同じ形（OPENING の期限 30 s、DM は §5.1 の M12 の扱い）。

### 3.3 PCE の SDP record [M8]

- p002 は「PCE の record は p005 が登録」と決めた（PBAP §7.1.1 は M）。置き場所: phone.c の `phone_mns_update` を `phone_records_update` に広げ、MNS と並べて PCE の record を出し入れする（phone.c の SDP の db と record の handle を持つのは phone.c なので、mux や pbap に db を渡さない）。
- 出す時: link が READY、有効な記録が enabled で profiles に contacts。外す時: その逆と link の終わり（今の MNS と同じ時期）。
- record の属性（`phone_pce_record`、MNS の `phone_mns_record` と同じ書き方）: ServiceClassIDList（0x0001）= UUID16 0x112E（PCE）、BluetoothProfileDescriptorList（0x0009）= ((0x1130、0x0101))（PBAP 1.1）、LanguageBaseAttributeIDList（0x0006、MNS と同じ）、ServiceName（0x0100）= "zedBSD Phonebook"。ProtocolDescriptorList は無し（PCE は client）。
- 試験: `bt-phone-link-host-test` の profile の場面に、contacts の記録で PCE の record が db に入り、link の終わりで出ること、record の byte（手の正解）。

## 4. vCard（`vcard.c`、i01 済み）

第 1 版 §4 のとおり実装した（da5dbf9ac、「実装の進み」の i01 の行）。実装で決めた事:

- API: `btd_vcard_next`（Body から 1 件、`ENOENT` で終わり、切れた件は `EINVAL`）、`btd_vcard_contact_read`（名前も番号も無い件は `ENOENT`、16 KB 越えは `E2BIG`、壊れた件は `EINVAL`）、`btd_vcard_reduce`（縮めた vCard 3.0、`BTD_VCARD_REDUCED_MAX` 6144）、`btd_vcard_call_read(card, length, folder_kind, call)`、`btd_vcard_call_time(call, local_offset, &seconds, &zone)`（`BTD_VCARD_ZONE_PHONE`・`_LOCAL`・`_NONE`）。
- 連絡先の key: UID があれば FNV-1a 64（`u|` + UID）、無ければ（`n|` + 表示の名前（FN、無ければ N の「名 姓」、無ければ最初の番号）+ `|` + 並べた番号を `,` で）。縮めた vCard を読み直しても同じ key（fuzz で確かめた）。
- 通話の key: FNV-1a（kind の 1 字 `r`・`d`・`m`・`u` + `|` + datetime + `|` + 番号）。**datetime の無い通話は kind と番号が同じなら同じ key で、1 件に重なる**（区別する物が無い。毎回の同期で増え続けるより良い）[m5]。
- 4 KB を越える行（折り返しを繋いだ後）は値を使わない（TEL なら dropped に数える）。入れ子は 8 段まで。

## 5. PCE（`pbap.c`）と socket

### 5.1 状態と接続

- 状態: OFF → SDP → OPENING（DLC）→ CONNECTING（OBEX Connect）→ READY ⇄ IDLE、FAILED（やり直しの待ち）。map.c と同じ hook の組（clock・wall・local_offset・wanted・sdp_query・dlc_open・dlc_write・dlc_close・answer・room・changed・up・log）。`wanted` は有効な記録が enabled で profiles に contacts があること。
- link が ready になったら（mux の ready）: SDP で PSE（0x112F）。record の RFCOMM の channel、SupportedRepositories（0x0314、bit 0 が無ければ `why=no-pb`、やり直さない）、PbapSupportedFeatures（0x0317）の有無。PSE が無ければ `why=no-pse`（やり直さない、次の link で）。
- DLC → OBEX Connect: Target（PBAP の UUID、16 byte）。**[M9] App Parameters は付けない**（推し (a)）: 我々は PCE の record で PBAP 1.1 を名乗り、1.1 の Connect に PbapSupportedFeatures は無い。§6.4 の C3 は 1.2 の PCE の義務と読む。今の `btd_obex_connect`（target だけ）のまま、obex.c を変えない。Android が付けない Connect を断る（0xC0 など）と分かった時は (b) `btd_obex_connect` に追加の header の引数を足す（UAT の結果で、§13）。
- **Connect の答えの時間は 60 s**（スマホが許可の画面を出す間、MAP と同じ `btd_obex_set_timeout`）。
- **[M12] 許可の扱い**: 次のどれも `why=permission`、600 s ごとにやり直す（`PHONE LINK on` か Settings の「Use as phone」ですぐ）: Connect の 0xC1・0xC3、READY に一度も届いていない間の DLC の DM（断り）、OPENING・CONNECTING の間の DLC の closed（Android は RFCOMM の受けで許可を聞き、拒否で DLC を閉じる見込み、推測）。他の失敗（Connect の他の code、timeout）は `why=refused`・`timeout` で 30 s から倍で 600 s まで。一度 READY に届いた後の DLC の DM・closed は §5.1 の下の IDLE か失敗。UAT に「拒否の後に許可の画面が繰り返し出ない（10 分に 1 回より多くない）」を入れる。
- READY: STATE に `contacts=ready`。`up` の hook で main が `btd_phone_profile_ok`（ページの待ちを戻す）を呼ぶ [m11]（MAP の up と同じ、contacts だけの記録でも呼ばれる）。MAP と PBAP は別の DLC・別の OBEX session（同じ RFCOMM の session の上）。
- **[M11] 相手の切断**: 実行中・待ちの操作が無い時に DLC が閉じた・OBEX が切れた時は失敗にせず IDLE（PSE の channel は覚えたまま、STATE は `contacts=ready`、「接続できる」の意味）。次の PAGE で OPENING から接続し直す（許可は Android が覚えている見込み、推測、UAT）。操作の途中の切断は実行中・待ちの request に `ERROR lost`、FAILED（`why=closed`、30 s から）。
- **自分から切る時期**: 持たない（同期は 1 日 1 回なので、idle の DLC を持ち続けても費用は小さく、接続のたびの許可の画面の危険を避ける）。link の終わり・profile の off・持ち主の変化で切る（MAP と同じ）。

### 5.2 socket の request と item の行

| request（持ち主と root） | 答え |
| --- | --- |
| `PHONE PAGE contacts [cursor=C] count=N` | `PHONE CONTACT …` の行（N 個まで）、`PHONE PAGE-END cursor=… more=0|1 count=… skipped=… capped=…`、`DONE` |
| `PHONE PAGE calls since=S [cursor=C] count=N` | `PHONE CALL-LOG …` の行、同じ終わり |

```
PHONE CONTACT key=<16 hex> tels=<n> name="<表示の名前>" peer="<最初の番号>" length=<m>
<m byte: 縮めた vCard 3.0（§4）>
PHONE CALL-LOG key=<16 hex> kind=received|dialed|missed time=<UNIX 秒> zone=phone|local|none datetime="<スマホの文字列>" peer="<番号>" name="<名前>"
```

- 行の escape と 2047 byte は p003 §9.1（`phoneio.c`）。名前・番号の 128 byte の切りも同じ。`length=` は引用の外の欄としてだけ読まれる（M1 で直した backend の `phone_field`、eee2a5d12）。
- `capped` は bit の組: 1 = 5000 件（calls は object ごとの 500 件）で止めた、2 = 同期の間に電話帳の件数が変わった（§5.3、M2）。
- contacts が FAILED か OFF の時は `ERROR not-ready`。IDLE の時は接続してから読む。1 つの client が待つ request は 1 つ（今の `waits_phone`）。

### 5.3 page の進め方

- **[M11] cursor** ＝ `<record の世代 8 桁の 16 進>.<object>.<offset>.<始めの PhonebookSize>`（object は 0 pb、1 ich、2 och、3 mch）。record の世代は bluetoothd の起動からの数で、記録のスマホ（address）が変わるたびに 1 増える。OBEX の session と link には結ばない（PBAP の offset は電話帳の中の番号で、接続し直しても同じ物を指す）。世代が違えば `ERROR stale-cursor`（app は最初から）。
- **SIZE** の答え: PhonebookSize（tag 0x08）は Get の最初の packet の App Parameters に来るので、obex の `response` の hook で読む [m2]。
- **contacts**: object 0 だけ。cursor が無い時（始め）に SIZE（MaxListCount 0x04 = 0、Name `telecom/pb.vcf`）で始めの PhonebookSize を得て cursor に入れる。PULL（Name `telecom/pb.vcf`、Type `x-bt/phonebook`、MaxListCount＝N、ListStartOffset＝o、Format 0x07 = 1（3.0）、PropertySelector 0x06 = `00 00 00 00 00 20 00 87`（bit 0 VERSION・1 FN・2 N・7 TEL・21 UID））。Body を `btd_vcard_next` で切り、1 件ずつ item の行。**index 0（持ち主の card、§3.1.5.2）は出さず、skipped にも数えない** [m9]。skipped は読めなかった件（`EINVAL`・`E2BIG`・`ENOENT`）だけ。次の o ＝ o + Body の件数。
- **[M2] 終わり**: Body の件数 < MaxListCount（PhonebookSize との比較で決めない）。または o が 5001（持ち主の card + 5000 件）に届いた時（`capped` bit 1）。終わりの page では SIZE をもう 1 度取り、始めの PhonebookSize と違えば `capped` bit 2。
- **calls**: object 1・2・3 の順。object ごとに PULL（Name `telecom/ich.vcf` など、PropertySelector = bit 0・1・2・7・28 = `00 00 00 00 10 00 00 87`）を offset 0 から、Body の件数 < N で次の object。**[m4] since で途中で打ち切らない**: 各 object を 500 件まで全部読み、datetime が since より前の件は出さない（数えない）。datetime の無い件と読めない件は出す（zone none）。object が 500 件で止まれば `capped` bit 1。SIZE は取らない（件数の変化は消しに使わない）。
- **Body の上限** 256 KB（N 件の vCard）。越えたら Abort し、同じ offset で N を半分にしてやり直す（N＝1 でも越えれば、その 1 件を飛ばして o + 1、skipped に数える）。
- 操作の queue: map.c §8.4 と同じ形（SIZE・PULL の 2 種、24 個まで、token の cancel、client の room の待ち 30 s で `ERROR slow`）。PBAP は live の event が無いので割り込みは無い。
- 限界（記録）: page の間にスマホの電話帳が変わると offset がずれ、1 件の重複（key で除く）か飛び（次の全体の同期で拾う）がある。消しは §7.2 の条件で守る。
- **[m12] 大きく取らない**: 5000 件は 32 件ずつ約 157 回の Get。bluetoothd が大きく取って memory から渡す形は取らない（bluetoothd が連絡先の中身を長く持たない、Q2。1 回の Get は小さく、待ちの多くは Phone の app の書き込み）。

### 5.4 SHOW・STATE・log

- `PHONE SHOW` と STATE に `contacts=off|connecting|ready|failed`（IDLE は ready）と `contacts_why=`（messages の `why=` と別）を足す。
- log は件数・response code・why だけ（名前・番号・vCard は書かない、p001 R22）。

### 5.5 main の配線 [m1]

MAP と同じ所に PBAP を足す:

| 所 | MAP の今 | PBAP |
| --- | --- | --- |
| 起動 | `btd_map_init` と hook、`btd_phone_set_profile(phone_profile)` | `btd_phonemux_init`、子に MAP と PBAP、`btd_phone_set_profile(mux の profile)`、`btd_pbap_init` と hook（wanted は contacts の bit） |
| loop の毎回 | `btd_map_tick` | `btd_pbap_tick` |
| `btd_timeout` | `btd_map_deadline` | `btd_pbap_deadline` |
| round の終わり（`btd_phone_watch`） | `btd_map_check`、`btd_map_pump` | `btd_pbap_check`、`btd_pbap_pump` |
| PHONE LINK の後 | `btd_map_check` | `btd_pbap_check` |
| client の close | `btd_map_cancel(token)` | `waits_phone` を「待つ profile」（0 無し、1 MAP、2 PBAP）にして、その profile の cancel |
| SHOW・STATE の行 | `btd_phone_line` の MAP の部分 | `contacts=`・`contacts_why=` |

main.c は host で build しないので、配線は i04 の target の build と UAT で確かめる（p003 の限界と同じ）。

## 6. 中継（libkeiland・backend・compositor）

### 6.1 libkeiland [M5・M6・M7]

- `KL_PHONE_CONTACTS 1U`・`KL_PHONE_CALLS 2U`（`KL_PHONE_MESSAGES` は 0U）。`kl_system_phone_sync(what, since, limit, cursor, count)`: contacts は since・limit を使わない（0）、calls は since を使う（limit は 0）。能力の bit `KL_SYSTEM_HAS_PHONE_CONTACTS`。KL_VERSION の番号は merge の時に Q1。
- `struct kl_phone_item` は変えない（大きさも）。使い方:
  - contacts: `what` 1、`key`、`name` ＝ 表示の名前、`peer` ＝ 最初の番号、`text`・`length` ＝ 縮めた vCard 3.0、`folder` ＝ TEL の数、他は 0。
  - calls: `what` 2、`key`、`folder` ＝ 0 received・1 dialed・2 missed、`direction` ＝ dialed なら 1、`time`、`zone`（**今の定義のまま** [M5]: 0 phone（datetime が Z・offset を持った）、2 local（zedBSD の zone で読んだ）、3 received（datetime が無い・読めない: time は bluetoothd がその行を作った時、`partial` 1））、`datetime`、`peer`、`name`、`text` は空。zone 1（MSE）は calls に出ない。keiland.h の item の説明に calls の zone の意味を足す。
- `struct kl_phone_link` の最後に `unsigned contacts;`（0 off、1 connecting、2 ready、3 failed）と `char contacts_why[KL_PHONE_WHY_MAX];` を足す [M7]。
- **[M6] 短い size**: 今の `system_view_phone_link_get`・`system_view_take_phone_item` は `size < sizeof(*link)` を拒むので、欄を足すと古い app が動かなくなる。直し: `KL_PHONE_LINK_SIZE_79`（＝ `offsetof(struct kl_phone_link, contacts)`）以上の size を受け、`min(size, sizeof)` だけ写し、残りを 0 にする（p004 §3 の規則を実装する）。item は大きさを変えないが同じ形にしておく。
- **[M6] link の event**: wayland の `link` event の signature は固定（system.c の listener）。新しい event `link_contacts(contacts, contacts_why)` を manager の新しい版で足し、compositor は版がそれ以上の object に `link` の直前に送る。libkeiland は受けた値を覚え、次の `link` で `kl_phone_link` に入れる（古い compositor では 0 と空のまま）。
- page の終わり: `kl_system_phone_page_end` の `capped` を bit の組と説明し直す（1 limit で止めた、2 同期の間に件数が変わった、§5.2）。今の app は `capped != 0` だけを見るので壊れない。

### 6.2 backend（zedBSD）[M4]

- `KL_BACKEND_PHONE_WHAT_CONTACTS 1U`・`_CALLS 2U`。`kl_backend_phone_page` の what 1・2 の行（§5.2）。
- `struct kl_backend_phone_item` に `unsigned what;` を足す（compositor と backend は一緒に build されるので順は自由、最初に置く）。`PHONE CONTACT`・`PHONE CALL-LOG` の行を読む（今の `PHONE MESSAGE` の読みと同じ口、`kind` は folder に、`zone=phone|local|none` は 0・2・3 に、none は partial）。
- `struct kl_backend_phone_state` に `contacts`・`contacts_why[KL_BACKEND_BT_REASON_MAX]`（STATE・SHOW の `contacts=`・`contacts_why=`）。
- Linux・FreeBSD の backend は「無い」のまま（Q10）。

### 6.3 compositor（phone-shell.c）[M4]

- `sync` の `page.what` の検査（今は MESSAGES だけ）を 0〜2 に。
- item の送り（今は `KL_SYSTEM_PHONE_MESSAGES` に固定）を backend の item の `what` に。
- loopback の backend の link に contacts 0。`link_contacts` event（§6.1）を `link` の前に。
- 同期は全体で 1 本のまま（p004 §4）。contacts・calls・messages の page は app が順に出す（§7.2）。

## 7. Phone の app（WS170 の store の変更）[B1]

### 7.1 置き方

- **スマホの電話帳の写しは store の連絡先の配列（`ph_contacts`）に入れない** [B1]。`phonebook/bt-<address の : を除いた 12 字>/<key>.vcf`（縮めた vCard 3.0 に `X-KEILAND-SOURCE:bt:<address>` の行を足した物）。`contacts/` の下に置かない（`store_load_contacts` と他の program が手元の連絡先と取り違えない）。手では直さない。上限 5000。
- memory: `struct ph_phonebook_entry`（key、表示の名前、番号の鍵（`ph_number_key`、8 個まで、それぞれ malloc））の配列と、（番号の鍵 → entry）の並べた索引 [m7]。名前の引きは索引の二分探索。開く時に全部の .vcf を読む（§13 の大きさの危険）。
- **通話の履歴**: item は番号の会話か手元の連絡先の folder に書く（`ph_store_conversation(number, name, create=1)` が返す行、今の MAP の item と同じ）。file は `c<key>.txt`（`Kind: call`、`Channel: line`、`Direction: in|out`、`Date`、`State: answered`（received）・`missed`・なし（dialed）、`Source: bt:<address>:pbap:<key>`、zone 3 は `Partial: 1`）。同じ key の file があれば書かない。通知は出さない。
- **[m5] 番号の無い通話**（非通知）: 会話 `w`（folder `messages/w/`、名前「No caller ID」）にまとめる。`store_load_conversations` は `n`・`a` に加えて `w` を読む。
- **[m6] key の引き**: `store_find_key` は `:map:` と `s<key>.txt` に固定なので、source の文字列（`bt:<address>:map:<key>`・`bt:<address>:pbap:<key>`）と file の頭（`s`・`c`）を引数にする。
- **[m6] 同期の目印**: `sync/bt-<address>.state` の読み書きを `struct ph_sync_marks`（`messages_since`、`deep_at`、`contacts_at`、`calls_since`）に。`ph_store_sync_save` は全部の行を書き直す（今は 2 行だけを書くので、足した行を消してしまう）。知らない行は残す。

### 7.2 同期

- 順: messages（今のまま）→ calls → contacts（1 日 1 回まで）。compositor の同期は全体で 1 本なので、contacts の 160 page の間は他が待つ。messages を先にする（新しい SMS が早く見える）。
- 時期: link の contacts が ready になった時と Phone の app の起動・「今すぐ同期」（今の messages の同期の口）。contacts は `contacts_at` から 24 時間たっていなければ飛ばす（「今すぐ同期」は飛ばさない）。calls は `calls_since` から（初回は 30 日、重なり 24 時間）。
- contacts の全体の同期: page を `more=0` まで（count 32）。受けた key の集合を覚え、1 件ごとに file が無いか中身が違えば書く（同じなら触らない、cloud の上の書き換えを減らす）。
- **[M2] 消しの条件**（全部を満たす時だけ、組の folder の中の集合に無い file を消す候補にする）:
  1. 全体の同期が `more=0` で終わり、途中に error・stale・`KL_PHONE_DROPPED` が無い。
  2. 最後の page の `capped` が 0（5000 件で止まっていない、件数が変わっていない）。
  3. 受けた件数が 1 以上で、組の今の件数の半分以上（一時的な空の電話帳・大きな減りで全部を消さない）。
  4. **2 回続けて**無い: 1 回目は `phonebook/bt-<address>/missing.txt`（key の行）に書くだけ。次の全体の同期でもまだ無い key だけを消す。また現れた key は missing から外す。
- 失敗・stale・DROPPED: 目印を進めず、次の時期にやり直す（messages と同じ）。
- 消しの判断（受けた集合・今の組・missing・件数から、消す key の組を出す）は純粋な関数 `ph_phonebook_prune_plan` にして host で試す [M13]。

### 7.3 表示と重ね

- **名前の引き**（`ph_store_phone_name(number)`）: 番号の鍵で、手元の連絡先（今の `ph_store_find_number`）にあれば NULL（手元の名前が出る）、無ければ電話帳の索引の名前。**番号ごとに引く**（Pc6 (a) **仮（ユーザーの答え待ち）**: 手元とスマホの連絡先で番号が一部だけ重なる時、重なった番号は手元の名前、残りの番号の会話はスマホの連絡先の名前）。
- 番号の会話（`conversation` 1、id `n<数字>`）の `name` は、store を開いた時と電話帳の同期の後に `store_apply_phone_names` で引き直す（MAP の item の名前より電話帳の名前が先、電話帳に無ければ今のまま）。`struct ph_contact` に `phone_named`（名前が電話帳の物、view が名前の横に小さく「Phone」）を足す。
- **一覧**（Pc1 (a) **仮（ユーザーの答え待ち）**）: スマホの連絡先そのものの行は作らない。会話か通話のある番号は番号の会話の行として今の一覧に出て、名前が電話帳の物になる。同じ人の 2 つの番号に会話があれば、同じ名前の行が 2 つ（記録の限界）。
- **[B1] 行の index**: 電話帳の同期は `ph_contacts` の行を足さない・消さない・並べ替えない（名前を変えるだけ）ので、view の request（行の index）は同期で別の人を指さない。通話の item を足す時の並べ替えは今の MAP の item と同じ（新しい危険を足さない）。
- 送り先: 番号の会話の送り先はその番号（今のまま）。スマホの連絡先の複数の番号から選ぶ UI は作らない。

## 8. Settings と記録の profiles

### 8.1 bluetoothd の記録 [M3]

- handoff（`PAIR … phone=1` の後、phone.c の `phone_take_record`）の新しい記録の profiles の既定を `BTD_PHONEREC_PROFILES`（m,c,h）から `BTD_PHONEREC_MESSAGES` に。同じ持ち主の pairing のやり直しは今の profiles を残す（今のまま）。PAIR から LINK の間や CLI の pairing で PBAP が勝手に始まり、許可の画面が出ることは無くなる。
- 記録に `asked <m,c,h の組>` の行を足す（LINK の profiles か handoff で明示に頼まれた profile。無い行は読みで許す）。**既存の記録の扱い**（Pc4 (a) **仮（ユーザーの答え待ち）**）: p005 の bluetoothd が `asked` の行の無い（p005 より前の）記録を読んだら、profiles から c と h を外して `asked m` で書き直す（1 回だけ）。p003 の handoff が書いた 0x07 も、CLI の `PHONE LINK … profiles=m,c,h` で作った物も同じに扱う（区別できない）。CLI の人は p005 の後にもう 1 度 LINK する。
- 古い bluetoothd は `asked` の行を知らない行として記録全体を拒む（downgrade で記録が無効になる、記録の限界。ベータの間は戻さない）。

### 8.2 Settings（page-bluetooth.c）[M7・m13]

- 「Use as phone」の `kl_system_phone_link_set(address, 1, KL_PHONE_PROFILE_MESSAGES | KL_PHONE_PROFILE_CONTACTS)`（Pc2 (a) **仮（ユーザーの答え待ち）**: contacts の 1 つの switch で電話帳と通話の履歴）。profile ごとの switch は作らない（1 つの「Use as phone」、p004c のまま）。
- 行の文に contacts の状態を足す: connecting は「Contacts connecting...」、ready は「Contacts connected」、`contacts_why` が `permission` は「Allow access to contacts on the phone」、他の failed は「Contacts not available」。
- 既に使っているスマホで profiles に contacts が無い時（p004c で「Use as phone」を押した人、§8.1 の移行の後の人）: 行に「**Also use contacts**」の button。押すと `link_set(address, 1, MESSAGES | CONTACTS)`（スマホが許可を聞く）。Settings を開いただけでは profiles を変えない（利用者の操作なしに許可の画面をスマホに出さない）。
- 「Stop using as phone」は今のまま `link_set(address, 0, <今の profiles>)`: off の時も profiles の bit は記録に残り、次の「Use as phone」で `MESSAGES | CONTACTS` に書き直す。
- **電話帳の写しの扱い**（Pc3 (b) **仮（ユーザーの答え待ち）**）: Phone の app は「Stop using as phone」（link の enabled 0）・記録が消えた（ペアの解除・FORGET）・別のスマホの記録になった、のどれかを link の変化で見たら、そのスマホの `phonebook/bt-<address>/` を消す。会話の中の通話の item（`c<key>.txt`）は SMS と同じく残す（履歴）。

## 9. 試験

### 9.1 host（`plan/ws197/tests/`、ASan・UBSan）[M13]

試験の期待値（tag・PropertySelector の byte・Target の UUID・opcode・record の byte）は §0 の表から手で書いた定数にし、実装の macro を使わない。

| 試験 | 内容 |
| --- | --- |
| `bt-vcard-host-test`（i01 済み、170 checks） | 第 1 版 §9.1 の内容と、縮めた vCard の読み直し、fuzz 20 万回 |
| `bt-phonemux-host-test`（新） | SDP の持ち主、EBUSY、DLC の ours・accept の振り分け、dlci の表と closed、**[M10] opened の無い closed（DM）を server channel から子へ**、open_failed、表の満ち（ENOSPC）、ended で表が空、ready・ended の全部への配り |
| `bt-map-host-test`（足す） | [M10] OPENING の DM の closed で `refused`、OPENING の 30 s の期限 |
| `bt-phone-link-host-test`（足す） | [M8] contacts の記録で PCE の record（手の byte）が db に入り link の終わりで出る、messages だけでは入らない。[M3] handoff の新しい記録は profiles=m。PENDING_DLCS 4 |
| `bt-phonerec-host-test`（足す） | `asked` の行の読み書き、無い行の記録の移行（c・h を外す）、壊れた `asked` |
| `bt-pbap-host-test`（新） | 台本の PSE（map の試験と同じ形）: SDP の record（RFCOMM の channel、0x0314 の bit 0 無し → no-pb、PSE 無し → no-pse）、Connect の byte（Target 16 byte、**App Parameters 無し** [M9]）と 60 s、0xC3 → permission と 600 s、**[M12] READY 前の DM・closed → permission**、SIZE の App Parameters の byte（`04 02 00 00`）と **response の hook の最初の packet からの PhonebookSize** [m2]、PULL の byte（MaxListCount・ListStartOffset・Format `07 01 01`・PropertySelector の 8 byte）、0.vcf を出さず skipped に数えない [m9]、**[M2] 終わりは件数 < N・終わりの SIZE の取り直しと capped bit 2**、cursor（世代・object・offset・size）と stale、**[M11] 世代が同じなら接続し直しても cursor が通る**、5000 の capped bit 1、calls の 3 つの順・**since で打ち切らず全部読んで filter** [m4]・500 で capped、256 KB で N の半分と 1 件の飛ばし、Get の Continue（0x90）の続き、room の待ちと slow、cancel、**[M11] 操作の無い時の切断は IDLE、次の PAGE で接続し直す、操作中の切断は lost と failed**、STATE の contacts と contacts_why、up の hook |
| `bt-phoneio-host-test`（足す） | CONTACT・CALL-LOG の行、escape、2047 byte、名前の中の ` length=`（M1 の形） |
| `phone-backend-host-test`（足す） | `PHONE PAGE contacts|calls` の行、CONTACT・CALL-LOG の読み（本文付き）、item の `what`、zone の写し、STATE の contacts・contacts_why、capped の bit |
| `phone-shell-host-test`（足す） | what 1・2 の中継（item の what が backend の物）、what 3 は EINVAL、`link_contacts` の event と版、libkeiland の短い size の link（79 の size で写せる、それより短いと EINVAL）、link の contacts・contacts_why |
| `phone-store-host-test`（足す） | [B1] `phonebook/` の読み・書き（`contacts/` に混ざらない、`ph_contacts` の数が変わらない）、同じ中身は書かない（mtime）、**[M2] `ph_phonebook_prune_plan`: 1 回目は missing だけ、2 回目で消す、また現れた key、capped・失敗・空・半分より少ない時は消さない**、5000 の上限、名前の引き（手元が先、番号ごと）、`store_apply_phone_names` で行の数と順が変わらない、通話の item の書きと重複無し、非通知の `w` の会話を開き直して見える、key の引きの `:pbap:`、同期の目印の 4 行と知らない行が残る |
| WS143 `bt-daemon-host-test.sh` | main.c を変えるため |

### 9.2 QEMU

PBAP の相手が QEMU に無い（p003 §10.2 と同じ）。T1 は頼まない（HID の回帰も、2026-10-10 ユーザー「流しすぎです。もう不要」）。

### 9.3 実機（5330、ユーザーの UAT。BUG-287 の直しの image の後）

Android で: 「Use as phone」→ スマホの「連絡先と通話履歴へのアクセス」の許可 → Phone の app の番号の会話に名前が出る、通話の履歴が会話に入る（受けた・掛けた・出なかった）、スマホで連絡先を消して「今すぐ同期」を 2 回 → app の `phonebook/` から消える。許可を拒否 → Settings に「Allow access to contacts on the phone」、10 分の間に許可の画面が繰り返し出ない。bluetoothd の log に `pbap: ready`・`pbap: page contacts …`。§13 の未確認の各行を見る。

## 10. 実装の順（WIP commit の単位、区切りごとに SHA を Q1 へ）

| i | 内容 | 確かめ |
| --- | --- | --- |
| i01 | `vcard.c`（§4） | **済み**（da5dbf9ac） |
| i02 | `phonemux.c`（§3.1）と main の配線（MAP を mux の子に）、map.c の DM と OPENING の期限（§3.2）、phone.c の PCE の record（§3.3）・PENDING_DLCS 4・handoff の既定と `asked`・移行（§8.1、phonerec） | bt-phonemux、bt-map、bt-phone-link、bt-phonerec、bt-phone-host-test.sh 全部、WS143、build |
| i03 | `pbap.c` の始め（SDP・DLC・Connect・許可・IDLE・やり直し・STATE）（§5.1） | bt-pbap（始めの部分）、build |
| i04 | `pbap.c` の page（§5.3）、`phoneio.c` の行、main の `PHONE PAGE contacts|calls`・SHOW・配線（§5.2・§5.4・§5.5） | bt-pbap、bt-phoneio、WS143、build |
| i05 | backend・compositor・libkeiland（§6） | phone-backend、phone-shell、zedBSD と keiland-linux の build |
| i06 | Phone の app の store と同期と表示（§7） | phone-store、build |
| i07 | Settings（§8.2）、style-check、phase.md の記録 | bt-desktop、build |

## 11. 受け入れ

- §9.1 の host の試験と fuzz が全部 PASS、WS143 の host の試験が PASS、zedBSD（bluetoothd・wayland・libkeiland・phone・settings）と keiland-linux の build が warning 0、style-check の変更箇所 0。
- 実機（§9.3）は p008 とユーザーの UAT。この Phase では未実施と書く（p003 §12 と同じ）。

## 12. 判断の要る点（ユーザー、推し付き）

Pc1・Pc2 は第 1 版 §7.4、Pc3〜Pc6 は review-1 の問い。選択肢はこの版で書いた（Q1 がユーザーに出した形と食い違う時は Q1 の物を正にして直す）。**答えが来るまで推しを仮として設計に入れた**。

| # | 問い | 選択肢 | 推し（仮に入れた所） |
| --- | --- | --- | --- |
| Pc1 | スマホの連絡先を一覧にどう出すか | (a) 会話・通話のある番号だけ（番号の会話の行の名前になる。他は名前の引きにだけ）/ (b) 全部（5000 件まで、手元の後ろに）/ (c) 出さない（名前の引きだけ、行に「Phone」の印も無し） | **(a)**（§7.3） |
| Pc2 | 通話の履歴をどの switch で読むか | (a) contacts の switch（Android の許可が連絡先と履歴で 1 つ）/ (b) calls の switch（HFP と一緒） | **(a)**（§8.2） |
| Pc3 | 「Stop using as phone」・ペアの解除・別のスマホの時、電話帳の写しと通話の履歴を | (a) 両方残す / (b) 電話帳の写し（`phonebook/bt-<address>/`）は消し、会話の中の通話の履歴は残す / (c) 両方消す | **(b)**（§8.2） |
| Pc4 | p005 より前の記録（handoff の m,c,h、CLI の m,c,h）で、bluetoothd の更新の後に contacts を自動で始めてよいか | (a) 始めない（1 回だけ c・h を外し、Settings の「Also use contacts」か LINK で明示に入れる）/ (b) 始める（次の link でスマホが許可を聞く） | **(a)**（§8.1） |
| Pc5 | mch（出なかった通話）を読むとスマホの不在着信の印が消える機種（推測）でも読むか | (a) 読む（UAT で印が消えるか確かめ、消えるなら記録）/ (b) 読まない（出なかった通話は app に出ない）/ (c) 「今すぐ同期」の時だけ読む | **(a)**（§5.3） |
| Pc6 | 手元とスマホの連絡先で番号が一部だけ重なる時、残りの番号の会話の名前 | (a) 番号ごとに引く（重なった番号は手元の名前、残りはスマホの連絡先の名前）/ (b) 1 つでも重なればスマホの連絡先を使わない（残りは番号のまま） | **(a)**（§7.3） |

## 13. 危険と未確認

| 項目 | 内容 | いつ |
| --- | --- | --- |
| GOEP 1.1 の PCE への相手の振る舞い | Android の PSE が RFCOMM の 1.1 の PCE に電話帳を出すか（表 9.1 では出すはず） | UAT |
| Connect の App Parameters [M9] | 付けない Connect を Android が受けるか。断られたら (b)（obex.c に header の引数） | UAT |
| PCE の record [M8] | Android が PCE の record を見て許可を決めるか | UAT の SDP の dump |
| 許可の画面 [M12] | Android は Connect の間か RFCOMM の受けで「連絡先と通話履歴へのアクセス」を聞くか、MAP の許可と別か、拒否の形（DM・DLC の close・0xC3） | UAT |
| 相手の切断 [M11] | Android が idle の PBAP の DLC を切るか、接続し直しで許可を聞き直すか | UAT |
| UID | Android・iPhone の vCard に UID が入るか（無ければ名前と番号の key、改名で別の連絡先 → 古い物は §7.2 の 2 回の後に消える） | UAT |
| 時刻 | 履歴の datetime はスマホの local time。zedBSD と timezone が違うとずれる | UAT |
| 履歴の順 [m4] | 新しい順は should。since で打ち切らないので順は結果に効かない | — |
| mch の印 [m14] | mch を読むとスマホの不在着信の印が消える機種（Pc5） | UAT |
| SDP の属性 ID | Assigned Numbers の値を手元で確かめていない | 実機の SDP の dump |
| page の間の変化 | offset のずれ（§5.3）。消しは §7.2 の 4 つの条件 | — |
| store の大きさ | 5000 件の .vcf を開く時に全部読む。1 件 200 byte で 1 MB、開く時間は i06 で計る（遅ければ 1 つの索引の file に寄せる） | i06 |
| 同期の待ち | contacts の 157 page の間 messages の同期が待つ（messages を先、contacts は 1 日 1 回） | i06 |
| downgrade | `asked` の行の記録を古い bluetoothd が拒む（§8.1） | 記録 |

## 14. 見積もり

i01 1.5（済み）、i02 1.5（mux に map の穴・PCE の record・記録の移行が加わった）、i03 1.5、i04 1.5、i05 1.5（M6 の size と新しい event）、i06 2.5（B1 の電話帳の表・消しの計画・会話の名前）、i07 0.5、計 **10.5 LW**（ws.md の 8 LW から +2.5）。

## Event

- 2026-10-11: 第 1 版（P1）。PBAP 1.2.3 の §2.7・§3.1・§5.1・§6.2〜§6.4・§7.1・§9 を読んで書いた。
- 2026-10-11: design-reviewer（agent ad782c2d4ddaad97c）→ [review-1.md](review-1.md)。blocker B1（スマホの連絡先を store の配列に混ぜる形）、major 13、minor 14、追加の判断 Pc3〜Pc6。i01 は GO。
- 2026-10-11: M1（backend の `phone_field` が引用の中の ` length=` を欄と読む、今の `PHONE MESSAGE` にもある既存の欠陥）を直した（eee2a5d12、phone-backend-host-test に偽の欄の例、PASS、zedBSD の libkeiland.so の build rc 0）。

- 2026-10-11: i01（vcard.c）を実装（da5dbf9ac、P1 の新しい世代）。
- 2026-10-11: 第 2 版（P1）。review-1 の B1（電話帳の写しを `phonebook/` と名前の表に、item は番号の会話）、M2〜M13、minor 1〜14 を直した。Pc1〜Pc6 は推しを仮に入れた（§12）。

## 実装の進み

| i | commit | 内容 | 確かめ |
| --- | --- | --- | --- |
| i01 vcard.c（§4、review-1 で GO） | da5dbf9ac | 新 `vcard.c`・`.h`: `btd_vcard_next`（Body から 1 件ずつ、前の行・間の行を飛ばす、入れ子の AGENT を数える、途中で切れた件は EINVAL）、`btd_vcard_contact_read`（§4.1 の読み: CRLF・LF、3.0 の折り返しは 1 字を除き 2.1 は空白を残す（VERSION の行を先に探す、不明は 3.0 の扱い）、QP の soft line break（行の head が QP の時だけ）、group、名前・parameter の大文字・小文字、2.1 の裸の parameter、引用の TYPE、CHARSET は UTF-8 か無しだけ、BASE64・B・知らない ENCODING は捨てる、3.0 の escape、FN が先・無ければ N を「名 姓」・無ければ最初の番号、TEL 8 個（空・数字無し・33 桁以上も dropped に数える）、`tel:` の URI、UID、不正な UTF-8 と制御文字は U+FFFD・改行と TAB は空白・前後の空白を除く・256 byte で文字の途中で切らない）、`btd_vcard_reduce`（§4.2 の縮めた vCard 3.0、NUL 付き、`BTD_VCARD_REDUCED_MAX` 6144）、`btd_vcard_call_read`（kind は datetime の parameter、無ければ folder、datetime は `[0-9TZ+-]` の 23 byte まで）、`btd_vcard_call_time`（`btd_mapxml_time_unix` で Z・offset は `ZONE_PHONE`、無ければ zedBSD の offset で `ZONE_LOCAL`、無い・読めない時は `ZONE_NONE` と 0）。**設計の補い**: (1) 連絡先の key の無 UID の形は「`n|` + 表示の名前（番号で代えた時はその番号）+ `|` + 並べた番号」（縮めた vCard を読み直しても同じ key になる）。(2) datetime の無い通話は kind と番号が同じなら key が同じ（区別する物が無い、review-1 minor 5 の記録）。(3) 名前も番号も無い件は ENOENT（pbap は skipped に数える）。(4) 4 KB を越える行は head だけ読み、値を使わない（TEL なら dropped）。(5) 入れ子は 8 段まで（9 段は EINVAL）。Makefile に vcard.c。試験: 新 `bt-vcard-host-test`（170 checks: Body の切り出し、2.1（QP の UTF-8、文字の途中の soft break、裸の parameter、BASE64 の PHOTO、2.1 の折り返し）、3.0（escape、group、引用の TYPE、`tel:`、折り返し、TAB）、charset、U+FFFD、256 byte の切り、TEL 9 個・33 桁、番号だけ、名前も番号も無い、16 KB・4 KB・9 段・開かない・閉じない、縮めた vCard の手の byte 列と全部の短い room と読み直し、試験の側の FNV-1a（既知の値で確かめた）での key、通話（2.1 MISSED、3.0 DIALED の UTC 1704110400、+9 時間の local 1704078000、folder の kind、datetime 無し・読めない・変な byte、範囲外の folder）、fuzz 20 万回で「読めた件は約束を守り、縮めた vCard から同じ名前・key・番号の数に読み直せる」） | `bt-phone-host-test.sh` PASS（16 本）、WS143 `bt-daemon-host-test.sh` PASS（FAIL 0）、target の bluetoothd（`ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/p1-uat`）rc 0・warning 0、style-check（vcard.c・.h・試験）0、`git diff --check` |

## 再開の手順（2026-10-11 P1）

1. 第 2 版を design-reviewer に出す（review-2.md）。GO の i から進める（i02 から）。
2. ユーザーの判断 Pc1〜Pc6 の答えが Q1 から来たら §12 と仮の印の所（§7.3・§8.1・§8.2・§5.3）を直す。推しと違えば、その i を始める前に設計を直す。
3. 各 i の後: `plan/ws197/tests/bt-phone-host-test.sh`、WS143 の `plan/ws143/tests/bt-daemon-host-test.sh`、target の bluetoothd（`make ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/p1-uat build/p1-uat/bin/bluetoothd`）warning 0、style-check、WIP commit、SHA を Q1 へ。
