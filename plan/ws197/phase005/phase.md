<!-- awesome-plan project=zedbsd record=ws197-p005 -->

# ws197-p005: PBAP の PCE（スマホの電話帳と通話の履歴）、Phone の app の連絡先の名前（詳細設計）

Phase ID: `ws197-p005`
Parent: [WS197](../ws.md)
Status: planning（2026-10-11 P1: 第 1 版の review-1 は blocker 1・major 13・minor 14、i01 vcard.c だけ GO → **i01 は実装済み（da5dbf9ac）**。第 2 版の review-2（[review-2.md](review-2.md)）は blocker 0・major 9・minor 15 で i02〜i07 は直してから。第 3 版の review-3（[review-3.md](review-3.md)）は blocker 0・major 3・minor 8 で、i02 は条件付き GO、i03・i04・i07 は GO、i05・i06 は R1・R2 を書いてから（再 review 不要）。第 3.1 版（この版）で R1〜R3 と minor 1〜8 を書いた。ユーザーの判断 Pc1〜Pc6 は答え待ちで、推しを**仮**として書いた（§12）。再開点は下の「再開の手順」）
Phase disposition: normal
Queue: Q1 の投入（2026-10-11「次は WS197 p005（PBAP、plan/ws197/ws.md の Phase の表どおり）に進んで」、ユーザー 2026-10-09 の順「OBEX, MAP, Integration, PBAP, HFP」）
Branch: `agent/p1-ws197`（区切りごとに main へ merge、ベータ2 に入れる、2026-10-10 ユーザー）
依存: p002（RFCOMM・OBEX・SDP、cleared）、p003（phone link・MAP・socket の PHONE、cleared）、p004a〜c（SMS の層、cleared 候補・UAT 待ち）、BUG-287（usb-bt の data pipe、UAT 待ち。実機の PBAP は ACL の受けが要る）
所有 path: `userland/base/bluetoothd/`、`userland/desktop/libkeiland-backend*/`（phone の分）、`userland/desktop/wayland/phone-shell.c`・`kl-system-protocol.h`（phone の分）、`userland/desktop/libkeiland/`（phone の分）、`userland/desktop/include/keiland/keiland.h`（phone の分）、`userland/desktop/phone/`、`userland/desktop/settings/page-bluetooth.c`、`plan/ws197/`

版: 2026-10-11 第 1 版（P1）。2026-10-11 第 2 版（P1、review-1 の B1・M2〜M13・minor 1〜14 を直した。直した所は `[B1]`・`[M2]`・`[m3]`（minor 3）の印）。2026-10-11 **第 3 版**（P1、review-2 の N1〜N9 と minor 1〜15 を直した。印は `[N1]`・`[r2m4]`（review-2 の minor 4））。2026-10-11 **第 3.1 版**（P1、review-3 の R1〜R3 と minor 1〜8、印は `[R1]`・`[r3m1]`）。ユーザーの判断の仮の所は「**仮（ユーザーの答え待ち）**」の印（Pc1〜Pc6 の選択肢はユーザーに出した物と一致、Q1 2026-10-11）。

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
3. phone link の profile の多重（`phonemux.c`、§3）: 今の phone link は profile を 1 つ（MAP）しか持たない。MAP と PBAP（後で HFP）を束ねる。DM で断られた DLC の振り分け（MAP の既存の穴も）[M10]。RFCOMM の DLC の期限切れで session 全体を終えない（rfcomm.c）[N1]。
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
| RFCOMM | `rfcomm.c` | 自分の DLC の PN・SABM の期限切れはその DLC だけを閉じる（§3.2）[N1] |
| phone link | `phone.c`・`phone.h` | PCE の SDP record（§3.3）[M8]、`BTD_PHONE_PENDING_DLCS` 2 → 4 [m10]、handoff の profiles の既定（§8.1）[M3] |
| record | `phonerec.c`・`.h` | `asked` の行（§8.1、Pc4 の仮） |
| MAP | `map.c`・`.h` | DM で断られた MAS の DLC を失敗に [M10]、やり直しは前の DLC の終わりの後 [N2] |
| main | `main.c` | mux・pbap の配線（§5.5 [m1]）、`PHONE PAGE contacts|calls`、SHOW と STATE に `contacts=` |
| socket の行 | `phoneio.c`・`protocol.h` | §5.2 の item の行の組み立て |
| backend | `libkeiland-backend-zedbsd/phone-zedbsd.c`、`libkeiland-backend/keiland-backend.h` | §6.2 [M4]: `KL_BACKEND_PHONE_WHAT_CONTACTS`・`_CALLS`、`kl_backend_phone_item` に `what`、`kl_backend_phone_state` に `contacts`・`contacts_why`、item の行の読み |
| compositor | `wayland/phone-shell.c`、`kl-system-protocol.h` | §6.3 [M4・M6・N6]: `what` の検査を 0〜2 に、item の送りの `what` を backend の item から、新しい event `link_contacts`（manager の新しい版、owed の LINK の中で送る） |
| libkeiland | `libkeiland/system/*`、`include/keiland/keiland.h` | §6.1 [M5・M6・M7・N9]: `KL_PHONE_CONTACTS`・`KL_PHONE_CALLS`、`struct kl_phone_link` の最後に `contacts`・`contacts_why`・`record`、短い size の受け入れ、page の終わりの印の bit、`KL_SYSTEM_HAS_PHONE_CONTACTS`。版の番号は merge の時に Q1 |
| Phone の app | `phone/store.c`・`phone.h`・`main.c`・`view.c` | §7 [B1] |
| Settings | `settings/page-bluetooth.c` | §8 |

## 3. profile の多重（`phonemux.c`）と PCE の record

### 3.1 振り分け

今の `struct btd_phone_profile`（phone.h）は 1 つで、main が MAP を入れている。phone.c の profile の口は変えず、main と phone.c の間に振り分けの profile（mux）を挟む:

- mux は子の profile を 3 つまで持つ（MAP、PBAP、後の HFP）。phone.c には mux の hook を 1 つの profile として `btd_phone_set_profile` で渡す。
- `ready`・`ended`: 全部の子へ順に（登録の順）。
- **SDP**: phone.c の問い合わせは 1 度に 1 つ（`btd_phone_sdp_query` は 2 つ目に EBUSY）。子は `btd_phonemux_sdp_query(mux, child, uuid)` を呼ぶ。mux は持ち主の子を覚えて phone.c に渡し、`sdp_done` をその子だけに返す。EBUSY はそのまま子に返す（MAP は今も EBUSY で `BTD_MAP_BUSY_MS` 後にやり直す。PBAP も同じ）。
- **表の key [r2m4]**: 「開いている途中の DLC」の表の key は（server channel、ours）。自分が開く DLC（ours 1、スマホの server channel）と、スマホが開く bluetoothd の server channel（ours 0、MNS の 16）は同じ番号でも別の行（スマホの PSE・MAS の channel が 16 でも衝突しない）。
- **DLC を開く**: 子は `btd_phonemux_dlc_open(mux, child, server_channel, now)` を呼ぶ。mux は（server channel、1）の行（8 個、満ちれば ENOSPC を子に）に書いてから phone.c を呼ぶ。**同じ（server channel、1）の行がまだある時、または dlci の表に `dlci >> 1` がその channel で ours の行がある時（子が `dlc_close` した DLC が rfcomm で CLOSING の間）は EBUSY を返す**（前の試みの DLC が rfcomm に残っている間に開き直すと rfcomm が EEXIST を返し、前の試みの遅い closed を新しい試みの物と取り違えるため）[N2・R3]。**`btd_phone_dlc_open` が同期の error を返したら（ENOTCONN・EBUSY・EEXIST・ENOSPC、この時は opened も closed も来ない）行を消し**、EEXIST は EBUSY にして子に返す [R3]。子は EBUSY を SDP の EBUSY と同じく `BTD_MAP_BUSY_MS` の後にやり直す。
- **opened(dlci, server_channel, ours)**: ours=1 は（server channel、1）の行の子へ、行を「dlci → 子」の表（8 個）へ移す。ours=0 は `accept` が 1 を返した子へ（accept の時に書いた（server channel、0）の行。rfcomm は 1 つの DLC の PN と SABM で accept を 2 回呼ぶので、同じ行を書き直すだけ）。
- 以後 dlci → 子の表で `data`・`writable`・`closed` を振り分け、`closed` で表から消す。
- **[M10] 表に無い dlci の closed**: 自分で開いた DLC が PN か SABM の答えの DM で断られると、rfcomm.c は `opened` 無しで `closed(dlci, BTD_RFCOMM_CLOSED_REFUSED)` だけを出す（rfcomm.c の `rfcomm_frame_dm` → `rfcomm_free`、`rfcomm_announced` が ours を真にするため、review-2 で確かめた）。期限切れ（§3.2 の N1 の直しの後）も同じく `closed(dlci, TIMEOUT)` だけ。mux は dlci の表に無い dlci の closed を（`dlci >> 1`、1）の行から引き、その子へ渡して行を消す。どちらの表にも無ければ捨てる（log に 1 行）。
- `open_failed(server_channel)`（RFCOMM の session が立たなかった）は（server channel、1）の行の子へ、行を消す。
- `accept(server_channel)`: 子に順に聞き、最初に 1 を返した子を（server channel、0）の行に書く。その DLC の opened で行を dlci の表へ移す。opened が来ないまま次の accept が同じ channel に来たら書き直す。
- **行の寿命 [r2m4]**: 行が消えるのは opened（dlci の表へ移る）・closed・open_failed・`ended` だけ。rfcomm は WAITING・NEGOTIATING で `btd_rfcomm_close` された自分の DLC に closed を出さない（rfcomm.c:371-377、reason 0）ので、**子は開いている途中の自分の DLC を閉じない**: profile が off になった・失敗した時も opened か closed を待ち（N1 の後は rfcomm が必ず 100 s 以内にどちらかを出す）、opened が来たらすぐ `dlc_close` する。
- 子の `dlc_write`・`dlc_close` は mux を通らない（phone.c の関数を直に。dlci は一意）。
- `ended`: 全部の表を空にしてから子へ配る。
- 純粋な表の操作にして host で試す（偽の phone の hook と 2 つの偽の子）。

### 3.2 DLC の期限と子の側の DM [M10・N1・N2]

- **[N1] rfcomm.c の直し**: 今は自分の DLC の PN（T2 20 s）か SABM（T1_DLC 60 s）の答えが来ないと、`btd_rfcomm_tick` が `rf->failed = TIMEOUT` で **session 全体**を終え、他の DLC（MAP）も落ちる（rfcomm.c:489-507）。profile が MAP だけの時は害が無かったが、PBAP の SABM に利用者が許可の画面で 60 s 答えないだけで MAP が落ちる。直し: DLCI が 0 でない自分の DLC の期限切れは、その DLC だけを終える:
  - NEGOTIATING（PN の答え待ち）: frame を送らずに free、`closed(dlci, TIMEOUT)`（スマホの PN の状態は次の PN で戻る、今の `btd_rfcomm_close` の WAITING・NEGOTIATING と同じ考え）。
  - CONNECTING（SABM の答え待ち）: その DLCI に DISC を送って CLOSING（期限は今の DISC と同じ T1 20 s）、UA・DM か期限切れで free、`closed(dlci, TIMEOUT)`（遅い UA で開いたと取り違えない）。
  - CLOSING（自分の DISC の答え待ち）の期限切れ: frame を送らずに free（closed の reason は今のまま）。
  - **[r3m1] DLC ごとの閉じの理由**: CONNECTING の期限で CLOSING にした DLC は、UA・DM の答え（今は `CLOSED_LOCAL` で free、rfcomm.c:1539・1561）でも期限切れでも `TIMEOUT` で free する。`struct btd_rfcomm_dlc` に `close_reason`（0 は今の扱い）を足す。
  - **[r3m2] OPEN（MSC の答え待ち、T2、rfcomm.c:2029-2032）の期限切れ**も同じくその DLC だけ（DISC → CLOSING → `TIMEOUT`）。tick の DLC の loop の中で closed の hook を呼ぶ（hook が `btd_rfcomm_open` で取る空きの slot は期限が未来なので、同じ pass で期限切れにならない）。rfcomm.c:466-470 の注記を直す。
  - **[r3m3] REFUSED の意味**: スマホの PN の答えを rfcomm 自身が拒む時（credit が無い・大きすぎる frame、rfcomm.c:1888-1895）は `REFUSED` でなく `ERROR` で閉じる（`REFUSED` はスマホの DM だけ、§5.1 の permission の元）。
  - DLCI 0（session の SABM・DISC）の期限切れは今のまま session 全体を終える。
  - これで自分が開いた DLC は、PN 20 s ＋ SABM 60 s ＋ DISC 20 s ＝ 最長 100 s で必ず opened か closed のどちらかになる。
- **[N2] 子の側の OPENING の期限は持たない**（第 2 版の 30 s をやめた: rfcomm の上限より短いと、許可された遅い DLC を捨て、やり直しが EEXIST になり、前の試みの closed を取り違える）。N1 の後は rfcomm が必ず終わりを出す。やり直しは前の DLC の行が mux から消えた後だけ（§3.1 の EBUSY）。
- MAP（既存の穴を直す）: `btd_map_closed` は `dlci == mas_dlci` だけを見ていて、OPENING の間（`mas_dlci` は opened まで 0）の closed を捨てる。直し: state が OPENING で `dlci >> 1 == mas_channel` の closed は、reason が REFUSED なら `map_fail(map, "refused", MAP_RETRY_STEP)`、TIMEOUT なら `map_fail(map, "timeout", MAP_RETRY_STEP)`、他は `map_fail(map, "closed", MAP_RETRY_STEP)`。
- PBAP も同じ形で、reason ごとの why は §5.1。

### 3.3 PCE の SDP record [M8]

- p002 は「PCE の record は p005 が登録」と決めた（PBAP §7.1.1 は M）。置き場所: phone.c の `phone_mns_update` を `phone_records_update` に広げ、MNS と並べて PCE の record を出し入れする（phone.c の SDP の db と record の handle を持つのは phone.c なので、mux や pbap に db を渡さない）。
- 出す時: link が READY、有効な記録が enabled で profiles に contacts。外す時: その逆と link の終わり（今の MNS と同じ時期）。
- record の属性（`phone_pce_record`、MNS の `phone_mns_record` と同じ書き方）: ServiceClassIDList（0x0001）= UUID16 0x112E（PCE）、BluetoothProfileDescriptorList（0x0009）= ((0x1130、0x0101))（PBAP 1.1）、LanguageBaseAttributeIDList（0x0006、MNS と同じ）、ServiceName（0x0100）= "Keiland Phonebook"（MNS の "Keiland MNS" に合わせた、i02）。ProtocolDescriptorList は無し（PCE は client）。
- 試験: `bt-phone-link-host-test` の profile の場面に、contacts の記録で PCE の record が db に入り、link の終わりで出ること、record の byte（手の正解）。
- 限界（推測、§13）: handoff の既定が messages だけ（§8.1）なので、ペアの時の SDP には PCE の record が無い。Android がペアの時に PCE の有無で連絡先の共有を出し分けるなら、後の「Use as phone」で許可を聞くかを UAT で見る [r2m15]。

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
- **[M12・N1] 許可の扱い**: 次の時だけ `why=permission`、600 s ごとにやり直す（`PHONE LINK on` か Settings の「Use as phone」ですぐ）: Connect の 0xC1・0xC3、READY に一度も届いていない間（OPENING・CONNECTING）の DLC の closed で reason が `REFUSED`（DM）か `REMOTE`（スマホの DISC）の時（Android は RFCOMM の受けで許可を聞き、拒否で DM か DISC を返す見込み、推測）。同じ間の closed で reason が `TIMEOUT`（利用者が許可の画面に答えなかった見込み、§3.2 の N1 の直しの後）と Connect の timeout は `why=timeout` で **permission と同じ 600 s**（30 s でやり直すと許可の画面が約 90 s ごとに出直すため [r3m3]）、`ERROR`・`LOST`・link の切れは `why=closed`、Connect の他の code は `why=refused`: この 2 つは 30 s から倍で 600 s まで。一度 READY に届いた後の切断は下の [M11] の IDLE か失敗。UAT に「拒否の後に許可の画面が繰り返し出ない（10 分に 1 回より多くない）」を入れる。
- READY: STATE に `contacts=ready`。`up` の hook で main が `btd_phone_profile_ok`（ページの待ちを戻す）を呼ぶ [m11]（MAP の up と同じ、contacts だけの記録でも呼ばれる）。MAP と PBAP は別の DLC・別の OBEX session（同じ RFCOMM の session の上）。
- **[M11] 相手の切断**: 実行中・待ちの操作が無い時に DLC が閉じた・OBEX が切れた時は失敗にせず IDLE（PSE の channel は覚えたまま、STATE は `contacts=ready`、「接続できる」の意味）。次の PAGE で OPENING から接続し直す（許可は Android が覚えている見込み、推測、UAT）。操作の途中の切断は実行中・待ちの request に `ERROR lost`、FAILED（`why=closed`、30 s から）。
- **[r2m11] 接続の全体の上限**: PAGE が IDLE からの接続を待つ時、PAGE を受けてから 100 s で READY に届かなければ、その PAGE に `ERROR timeout`（接続の試みは続け、次の PAGE で使う）。backend の答えの待ち 120 s（phone-zedbsd.c の 120 s）より短くし、backend が socket を作り直して取り消すのを避ける。app は次の同期の時期にやり直す。
- **自分から切る時期**: 持たない（同期は 1 日 1 回なので、idle の DLC を持ち続けても費用は小さく、接続のたびの許可の画面の危険を避ける）。link の終わり・profile の off・持ち主の変化で切る（MAP と同じ）。

### 5.2 socket の request と item の行

| request（持ち主と root） | 答え |
| --- | --- |
| `PHONE PAGE contacts [cursor=C] count=N` | `PHONE CONTACT …` の行（N 個まで）、`PHONE PAGE-END cursor=… more=0|1 count=… skipped=… capped=…`、`DONE` |
| `PHONE PAGE calls since=S [cursor=C] count=N` | `PHONE CALL-LOG …` の行、同じ終わり |

```
PHONE CONTACT key=<16 hex> tels=<n> length=<m> peer="<最初の番号>" name="<表示の名前>"
<m byte: 縮めた vCard 3.0（§4）>
PHONE CALL-LOG key=<16 hex> kind=received|dialed|missed time=<UNIX 秒> zone=phone|local|none partial=0|1 length=0 datetime="<スマホの文字列>" peer="<番号>" name="<名前>"
```

- 行の escape と 2047 byte は p003 §9.1（`phoneio.c`）。名前・番号の 128 byte の切りも同じ。`length=` は引用の外の欄としてだけ読まれる（M1 で直した backend の `phone_field`、eee2a5d12）。それでも `length=` は引用の欄（スマホの文字）より前に置く。
- **[N5] CALL-LOG も `length=0` を持つ**（本文は無い）。backend は `PHONE MESSAGE` と同じ口（`length=` が要る）で CONTACT・CALL-LOG を読む。
- **[r2m6] zone は言葉で運ぶ**: vcard.h の値（`BTD_VCARD_ZONE_NONE 0`・`PHONE 1`・`LOCAL 2`）と keiland の値（0 phone、2 local、3 received）は違うので、phoneio は `phone|local|none` の言葉を書き、backend が言葉から keiland の値（0・2・3）に写す。`none` の時は bluetoothd が `time=` に行を作った時の wall の秒と `partial=1` を書く（backend は写すだけ）。
- `capped` は bit の組: 1 = 5000 件（calls は object ごとの 500 件）で止めた、2 = 同期の間に電話帳の件数が変わった（§5.3、M2）、**4 = この page の Body に終わらない card があり、件数を信用できない（§5.3、N4）**。
- contacts が FAILED か OFF の時は `ERROR not-ready`。IDLE の時は接続してから読む。1 つの client が待つ request は 1 つ（今の `waits_phone`）。

### 5.3 page の進め方

- **[M11] cursor** ＝ `<record の世代 8 桁の 16 進>.<object>.<offset>.<その object の始めの PhonebookSize>`（object は 0 pb、1 ich、2 och、3 mch）。record の世代は bluetoothd の起動の時に乱数から始め（MAP の `first_session` と同じ、main.c の起動の乱数、bluetoothd の再起動を跨いだ cursor を通さない）[r2m5]、記録のスマホ（address）が変わるたびに 1 増える。OBEX の session と link には結ばない（PBAP の offset は電話帳の中の番号で、接続し直しても同じ物を指す）。世代が違えば `ERROR stale-cursor`（app は最初から）。
- **SIZE** の答え: PhonebookSize（tag 0x08）は Get の最初の packet の App Parameters に来るので、obex の `response` の hook で読む [m2]。
- **contacts**: object 0 だけ。cursor が無い時（始め）に SIZE（MaxListCount 0x04 = 0、Name `telecom/pb.vcf`）で始めの PhonebookSize を得て cursor に入れる。PULL（Name `telecom/pb.vcf`、Type `x-bt/phonebook`、MaxListCount＝N、ListStartOffset＝o、Format 0x07 = 1（3.0）、PropertySelector 0x06 = `00 00 00 00 00 20 00 87`（bit 0 VERSION・1 FN・2 N・7 TEL・21 UID））。Body を `btd_vcard_next` で切り、1 件ずつ item の行。**index 0（持ち主の card、§3.1.5.2）は出さず、skipped にも数えない** [m9]。skipped は card として読めなかった件（`btd_vcard_contact_read` の `EINVAL`・`E2BIG`・`ENOENT`）だけ。
- **[N4] Body の件数**: Body の件数は一番外の段の card の数で数える（新しい `btd_vcard_count`: `btd_vcard_next` と同じ行の比べ方と同じ段の数え方で、`BEGIN:VCARD` が段 0 で現れた数。終わらない card はその BEGIN で 1 と数える）[r3m5]。`btd_vcard_next` が `EINVAL`（END:VCARD の無い card が Body の残りを飲んだ）を返した page は、それより前の card の item を出し、`capped` に bit 4 を立てる（その pass の消しを止める、§7.2）。次の o ＝ o + Body の件数（BEGIN の数）。
- **[M2・N3] 終わり**: 次のどれか。(1) Body の件数 < MaxListCount。(2) 直前の page が満ちていて（件数 ＝ N）o ≥ 始めの PhonebookSize の時は、o で 1 度だけ確かめの Get をし、0xC4・0xC0・0xD0 か空の Body なら終わり（error にしない）、件数があれば続ける（PhonebookSize が 0.vcf を数えない PSE で最後の 1 件を落とさないため、推測 [r3m4]。N3 の「error を返す機種」もここで終わりとして扱う）。(3) o が 5001（持ち主の card + 5000 件）に届いた（`capped` bit 1）。終わりの page では SIZE をもう 1 度取り、始めの PhonebookSize と違えば `capped` bit 2（page の間に電話帳が変わった時もここで分かる）。
- **calls**: object 1・2・3 の順。object ごとに始めに SIZE（MaxListCount 0）を取り [N3]、PULL（Name `telecom/ich.vcf` など、PropertySelector = bit 0・1・2・7・28 = `00 00 00 00 10 00 00 87`）を offset 0 から。次の object へ移るのは contacts と同じ (1)〜(3)（(3) は 500 件、`capped` bit 1）。**[m4] since で途中で打ち切らない**: 各 object を 500 件まで全部読み、datetime が since より前の件は出さない（数えない）。**[r2m10]** card として読めない件は skipped（出さない）、card は読めて datetime が無い・読めない件は出す（`zone=none`・`partial=1`）。calls は終わりの SIZE を取らない（件数の変化は消しに使わない）。
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
| client の枠の初期化（main.c:1062 の辺り） | `waits_phone` 0 | 同じ（待つ profile 0） [r2m3] |
| 持ち主の変化で待つ client を閉じる所（main.c:3405-3418 の辺り） | `btd_map_cancel` と閉じ | 待つ profile の cancel と閉じ [r2m3] |
| answer の hook の DONE（main.c:3810-3813 の辺り） | `waits_phone` を 0 に | PBAP の answer の hook も DONE・ERROR で 0 に [r2m3] |
| SHOW・STATE の行 | `btd_phone_line` の MAP の部分 | `contacts=`・`contacts_why=` |

main.c は host で build しないので、配線は i04 の target の build と UAT で確かめる（p003 の限界と同じ）。

## 6. 中継（libkeiland・backend・compositor）

### 6.1 libkeiland [M5・M6・M7・N6・N9]

- `KL_PHONE_CONTACTS 1U`・`KL_PHONE_CALLS 2U`（`KL_PHONE_MESSAGES` は 0U）。`kl_system_phone_sync(what, since, limit, cursor, count)`: contacts は since・limit を使わない（0）、calls は since を使う（limit は 0）。能力の bit `KL_SYSTEM_HAS_PHONE_CONTACTS`。KL_VERSION の番号は merge の時に Q1。
- `struct kl_phone_item` は変えない（大きさも）。使い方:
  - contacts: `what` 1、`key`、`name` ＝ 表示の名前、`peer` ＝ 最初の番号、`text`・`length` ＝ 縮めた vCard 3.0、`folder` ＝ TEL の数、他は 0。
  - calls: `what` 2、`key`、`folder` ＝ 0 received・1 dialed・2 missed、`direction` ＝ dialed なら 1、`time`、`zone`（**今の定義のまま** [M5]: 0 phone（datetime が Z・offset を持った）、2 local（zedBSD の zone で読んだ）、3 received（datetime が無い・読めない: time は bluetoothd がその行を作った時、`partial` 1））、`datetime`、`peer`、`name`、`text` は空。zone 1（MSE）は calls に出ない。keiland.h の item の説明に calls の zone の意味を足す。
- `struct kl_phone_link` の最後に `unsigned contacts;`（0 off、1 connecting、2 ready、3 failed）、`char contacts_why[KL_PHONE_WHY_MAX];`（[M7]）、`unsigned record;`（[N9・R1・R2]: 0 分からない（loopback、bluetoothd が答えない、答えるようになってから記録の有無をまだ読んでいない）、1 記録が無い（bluetoothd が答え、SHOW に記録の行が無かった）、2 記録がある）を足す。**phone.backend の設定（none・bluetooth）によらず** zedBSD の backend の状態から決める（Settings の Stop は設定を 0 にするので、設定で決めると Stop の後に写しを消せない、R1）。今の `why` の言葉（`unreachable` は bluetoothd が答えない時と、phone.c の「スマホに届かない」の両方に使われる）から記録の有無を推さない。
- **[M6] 短い size**: 今の `system_view_phone_link_get`・`system_view_take_phone_item` は `size < sizeof(*link)` を拒むので、欄を足すと古い app が動かなくなる。直し: `KL_PHONE_LINK_SIZE_79`（＝ `offsetof(struct kl_phone_link, contacts)`）以上の size を受け、`min(size, sizeof)` だけ写し、残りを 0 にする（p004 §3 の規則を実装する）。item は大きさを変えないが同じ形にしておく。
- **[M6・N6] link の event**: wayland の `link` event の signature は固定（system.c の listener）。新しい event `link_contacts(contacts, record, contacts_why)` を manager の新しい版で足す。compositor は §6.3 の owed の LINK の中で `link_contacts` → `link` の順に送る。libkeiland は `link_contacts` の値を覚え、**次の `link` で** `kl_phone_link` に入れて覚えた値を消す（古い compositor では 0 と空のまま。`link_contacts` 無しに来た `link` は contacts 0・record 0）。
- page の終わり: `kl_system_phone_page_end` の `capped` を bit の組と説明し直す（1 limit で止めた、2 同期の間に件数が変わった、4 件数を信用できない page、§5.2）。今の app は `capped != 0` だけを見るので壊れない。

### 6.2 backend（zedBSD）[M4]

- `KL_BACKEND_PHONE_WHAT_CONTACTS 1U`・`_CALLS 2U`。`kl_backend_phone_page` の what 1・2 の行（§5.2）。
- `struct kl_backend_phone_item` に `unsigned what;` を足す（compositor と backend は一緒に build されるので順は自由、最初に置く）。`PHONE CONTACT`・`PHONE CALL-LOG` の行を読む（今の `PHONE MESSAGE` の読みと同じ口、`kind` は folder に、`zone=phone|local|none` は 0・2・3 に、none は partial）。
- `struct kl_backend_phone_state` に `contacts`・`contacts_why[KL_BACKEND_BT_REASON_MAX]`（STATE・SHOW の `contacts=`・`contacts_why=`）。今ある `reachable`・`have_record` と新しい `record_known`（[R2]: SHOW の答えか記録の有無を言う STATE の行を読んだら 1、届かなくなった時の状態の全消し（phone-zedbsd.c:1632-1635）で 0。届くようになった直後（1643-1646）は 0 のまま）が §6.3 の `record` の元。
- **[r2m7]** `phone_state_line` の not-owner の分（phone-zedbsd.c:1559-1569）、SHOW の no-record（1307-1314）、届かない時（253・1634）は `contacts` 0・`contacts_why` 空にもする（messages と同じ所）。
- Linux・FreeBSD の backend は「無い」のまま（Q10）。

### 6.3 compositor（phone-shell.c）[M4]

- `sync` の `page.what` の検査（今は MESSAGES だけ）を 0〜2 に。
- item の送り（今は `KL_SYSTEM_PHONE_MESSAGES` に固定）を backend の item の `what` に。
- loopback の backend の link に contacts 0・record 0。
- **[N6] `link_contacts` は owed の LINK の中で送る**: `link` は「失わない」event で、送れない時は owed に積み、古い LINK は新しい物に置き換える（phone-shell.c の `phone_owe`、`phone_emit_owed`）。`phone_link_event`（owed の LINK が持つ値）に contacts・record・contacts_why を足し、LINK を出す所（直の送りと `phone_emit_owed`）で object の版が新しければ `link_contacts` を先に出し、続けて `link`。`link_contacts` か `link` のどちらかが ENOBUFS なら LINK ごと owed に残し、次に両方を出し直す（libkeiland は `link` で受けた組を使うので、2 度来た `link_contacts` は後の物が勝つ）。
- **[N9・R1・R2] record の写し**（`phone_link_fill`）: 設定が loopback → 0。他（none・bluetooth）は zedBSD の backend の状態から: `reachable` 0 か `record_known` 0 → 0、`have_record` 0 → 1、他は 2。
- 同期は全体で 1 本のまま（p004 §4）。contacts・calls・messages の page は app が順に出す（§7.2）。

## 7. Phone の app（WS170 の store の変更）[B1]

### 7.1 置き方

- **スマホの電話帳の写しは store の連絡先の配列（`ph_contacts`）に入れない** [B1]。`phonebook/bt-<address の : を除いた 12 字>/<key>.vcf`（縮めた vCard 3.0 に `X-KEILAND-SOURCE:bt:<address>` の行を足した物）。`contacts/` の下に置かない（`store_load_contacts` と他の program が手元の連絡先と取り違えない）。手では直さない。上限 5000。
- memory: `struct ph_phonebook_entry`（key、表示の名前、番号の鍵（`ph_number_key`、8 個まで、それぞれ malloc））の配列と、（番号の鍵 → entry）の並べた索引 [m7]。名前の引きは索引の二分探索。開く時に全部の .vcf を読む（§13 の大きさの危険）。
- **通話の履歴**: item は番号の会話か手元の連絡先の folder に書く（`ph_store_conversation(number, name, create=1)` が返す行、今の MAP の item と同じ）。
- **[N8] 行の上限**: 番号の会話も `STORE_CONTACTS_MAX` 1024 の中にあり、通話は番号ごとに会話を作るので届き得る。今の code は上限で `ph_store_conversation` が mkdir の後に -1 を返し、次の起動で `store_load_conversations` が ENOSPC で `ph_store_open` を失敗させる（MAP の SMS でも起き得る既存の欠陥）。直し（i06）: (1) `ph_store_conversation` は行を足せた後に mkdir する。(2) 開く時に上限を越えた会話の folder は読み飛ばして数え（log に 1 行）、`ph_store_open` を失敗にしない。(3) **[r3m6] 溢れの会話**: 行の 1 つを溢れの会話 `o`（folder `messages/o/`、名前「Other numbers」）に取っておき（普通の会話は 1023 まで）、上限の時の新しい番号の通話・SMS は `o` に入れる（item の `Source` と相手の番号は残るので失われない）。同期は失敗にせず目印を進める。開く時の読み飛ばしは上限を既に越えた store だけで、どれを飛ばすかは readdir の順（記録）。`store_folder_key` と `store_load_conversations` は `o` を知る。file は `c<key>.txt`（`Kind: call`、`Channel: line`、`Direction: in|out`、`Date`、`State: answered`（received）・`missed`・なし（dialed）、`Source: bt:<address>:pbap:<key>`、zone 3 は `Partial: 1`）。同じ key の file があれば書かない。通知は出さない。
- **[m5・r2m9] 番号の無い通話**（非通知）: 会話 `w`（folder `messages/w/`、名前「No caller ID」）にまとめる。`ph_store_conversation` は空の番号で作れない（`ph_number_key` が EINVAL）ので、新しい `ph_store_withheld_conversation(create)` が `w` の行を引くか作る。`store_load_conversations` と `store_folder_key` は `n`・`a` に加えて `w` を知る（`w` は番号の鍵を持たず、名前の引きの対象にならない）。
- **[m6] key の引き**: `store_find_key` は `:map:` と `s<key>.txt` に固定なので、source の文字列（`bt:<address>:map:<key>`・`bt:<address>:pbap:<key>`）と file の頭（`s`・`c`）を引数にする。
- **[m6] 同期の目印**: `sync/bt-<address>.state` の読み書きを `struct ph_sync_marks`（`messages_since`、`deep_at`、`contacts_at`、`calls_since`）に。`ph_store_sync_save` は全部の行を書き直す（今は 2 行だけを書くので、足した行を消してしまう）。知らない行は残す。

### 7.2 同期

- 順: messages（今のまま）→ calls → contacts（1 日 1 回まで）。compositor の同期は全体で 1 本なので、contacts の 160 page の間は他が待つ。messages を先にする（新しい SMS が早く見える）。
- 時期: link の contacts が ready になった時と Phone の app の起動・「今すぐ同期」（今の messages の同期の口）。contacts は `contacts_at` から 24 時間たっていなければ飛ばす（「今すぐ同期」は飛ばさない）。calls は `calls_since` から（初回は 30 日、重なり 24 時間）。
- contacts の全体の同期: page を `more=0` まで（count 32）。受けた key の集合を覚え、1 件ごとに file が無いか中身が違えば書く（同じなら触らない、cloud の上の書き換えを減らす）。
- **[M2] 消しの条件**（全部を満たす時だけ、組の folder の中の集合に無い file を消す候補にする）:
  1. 全体の同期が `more=0` で終わり、途中に error・stale・`KL_PHONE_DROPPED` が無い。
  2. その pass のどの page の `capped` も 0（5000 件で止まっていない、件数が変わっていない、件数を信用できない page が無い [N4]）。
  3. 受けた件数が 1 以上で、組の今の件数の半分以上（一時的な空の電話帳・大きな減りで全部を消さない）。
  4. **2 回続けて**無い: 1 回目は `phonebook/bt-<address>/missing.txt`（key の行）に書くだけ。次の全体の同期でもまだ無い key だけを消す。また現れた key は missing から外す。
- 失敗・stale・DROPPED: 目印を進めず、次の時期にやり直す（messages と同じ）。
- 消しの判断（受けた集合・今の組・missing・件数・pass の capped の和から、消す key の組を出す）は純粋な関数 `ph_phonebook_prune_plan` にして host で試す [M13]。
- 限界（記録）[r2m12]: 利用者がスマホで半分より多くの連絡先を一度に消した時は、条件 3 で写しが残り続ける（次の pass でも同じ）。消えた連絡先の名前は番号の会話に残る（`store_apply_phone_names` は「電話帳に無ければ今のまま」）。UAT に書く。

### 7.3 表示と重ね

- **名前の引き**（`ph_store_phone_name(number)`）: 番号の鍵で、**手元の連絡先（`conversation == 0` の行だけ）** にあれば NULL（手元の名前が出る）、無ければ電話帳の索引の名前。[N7] 今の `ph_store_find_number` は `ph_store_conversation` を通して番号の会話の行（`conversation` 1）も返すので使わない（使うと会話のある番号はどれも「手元にある」になり、電話帳の名前が番号の会話に付かない）。新しい static の `store_find_local_number` を足す。
- **[r2m8] 索引の作り直し**: 番号の鍵は `ph_number_key`（`store_country` を使う）で作るので、`ph_store_set_country` の後に索引を作り直し、`store_apply_phone_names` を呼ぶ。**番号ごとに引く**（Pc6 (a) **仮（ユーザーの答え待ち）**: 手元とスマホの連絡先で番号が一部だけ重なる時、重なった番号は手元の名前、残りの番号の会話はスマホの連絡先の名前）。
- 番号の会話（`conversation` 1、id `n<数字>`）の `name` は、store を開いた時と電話帳の同期の後に `store_apply_phone_names` で引き直す（MAP の item の名前より電話帳の名前が先、電話帳に無ければ今のまま）。`struct ph_contact` に `phone_named`（名前が電話帳の物、view が名前の横に小さく「Phone」）を足す。
- **一覧**（Pc1 (a) **仮（ユーザーの答え待ち）**）: スマホの連絡先そのものの行は作らない。会話か通話のある番号は番号の会話の行として今の一覧に出て、名前が電話帳の物になる。同じ人の 2 つの番号に会話があれば、同じ名前の行が 2 つ（記録の限界）。
- **[B1] 行の index**: 電話帳の同期は `ph_contacts` の行を足さない・消さない・並べ替えない（名前を変えるだけ）ので、view の request（行の index）は同期で別の人を指さない。通話の item を足す時の並べ替えは今の MAP の item と同じ（新しい危険を足さない）。
- 送り先: 番号の会話の送り先はその番号（今のまま）。スマホの連絡先の複数の番号から選ぶ UI は作らない。

## 8. Settings と記録の profiles

### 8.1 bluetoothd の記録 [M3]

- handoff（`PAIR … phone=1` の後、phone.c の `phone_take_record`）の新しい記録の profiles の既定を `BTD_PHONEREC_PROFILES`（m,c,h）から `BTD_PHONEREC_MESSAGES` に。同じ持ち主の pairing のやり直しは今の profiles を残す（今のまま）。PAIR から LINK の間や CLI の pairing で PBAP が勝手に始まり、許可の画面が出ることは無くなる。
- 記録に `asked <m,c,h の組>` の行を足す（LINK の profiles か handoff で明示に頼まれた profile。書く時はいつも今の profiles と同じ値）。`asked` は無くてもよい行（`PHONEREC_KEYS_ALL` の要る行の組に入れない）。
- **既存の記録の扱い**（Pc4 (a) **仮（ユーザーの答え待ち）**）: p005 の bluetoothd が `asked` の行の無い（p005 より前の）**有効な**記録を読んだら（`btd_phone_load`）、profiles を `profiles & m` にし、`asked` をその値で書き直す（1 回だけ。profiles に m が無い記録は profiles 0・`asked -`）。p003 の handoff が書いた 0x07 も、CLI の `PHONE LINK … profiles=m,c,h` で作った物も同じに扱う（区別できない）。CLI の人は p005 の後にもう 1 度 LINK する。無効な記録は書き直さない（持ち主が無い）。
- **[r2m1] downgrade**: 古い bluetoothd は知らない行を読み飛ばす（phonerec.c:579-581）ので記録を拒まないが、書き直す時（`btd_phonerec_format`）に `asked` を落とす。その後に p005 に戻すと移行がもう 1 度走り、利用者が入れた contacts が外れる（「Also use contacts」をもう 1 度押す）。記録の限界。

### 8.2 Settings（page-bluetooth.c）[M7・m13]

- 「Use as phone」の `kl_system_phone_link_set(address, 1, KL_PHONE_PROFILE_MESSAGES | KL_PHONE_PROFILE_CONTACTS)`（Pc2 (a) **仮（ユーザーの答え待ち）**: contacts の 1 つの switch で電話帳と通話の履歴）。profile ごとの switch は作らない（1 つの「Use as phone」、p004c のまま）。
- 行の文に contacts の状態を足す: connecting は「Contacts connecting...」、ready は「Contacts connected」、`contacts_why` が `permission` は「Allow access to contacts on the phone」、他の failed は「Contacts not available」。
- 既に使っているスマホで profiles に contacts が無い時（p004c で「Use as phone」を押した人、§8.1 の移行の後の人）: 行に「**Also use contacts**」の button。押すと `link_set(address, 1, MESSAGES | CONTACTS)`（スマホが許可を聞く）。Settings を開いただけでは profiles を変えない（利用者の操作なしに許可の画面をスマホに出さない）。
- **[r2m2・r3m7]「Stop using as phone」**: 今の code（page-bluetooth.c:844）は on・off とも `KL_PHONE_PROFILE_MESSAGES` を渡し、off で contacts の bit を消す。直し: off は profiles を変えない。backend は off の `PHONE LINK` に `profiles=` を書かない（bluetoothd は `profiles=` の無い LINK で記録の bit を残す、main.c:2278。空の `profiles=` は bit を消すので書かない）。on（「Use as phone」）は `MESSAGES | CONTACTS`、「Also use contacts」は `MESSAGES | CONTACTS`。off の時も profiles の bit は記録に残る。
- **[N9] 電話帳の写しを消す時**（Pc3 (b) **仮（ユーザーの答え待ち）**）: Phone の app は link の状態を見るたびに、純粋な関数 `ph_phonebook_forget(copy_address, link)` で、持っている写しを消すかを決める。消すのは次の明示の場合だけ:
  1. Stop: `record` 2、`owner` 1、`address` が写しのスマホ、`enabled` 0。
  2. 別のスマホ: `record` 2、`owner` 1、`address` が写しのスマホと違う（空でない）。
  3. 記録が消えた（ペアの解除・FORGET）: `record` 1（bluetoothd が答えて、記録の有無を読んで、無い）。
  `backend`（設定）は見ない [R1]（Stop の答えで Settings が設定を 0 にする、page-bluetooth.c:893-899）。消さない: `record` 0（loopback・bluetoothd が答えない・答えるようになって記録の有無をまだ読んでいない [R2]）、`owner` 0（not-owner、他の人の記録）、link を一度も受けていない時。状態から決める（変化の瞬間を見ない）ので、app が閉じている間の Stop も次に開いた時に効く。会話の中の通話の item（`c<key>.txt`）は SMS と同じく残す（履歴）。写しの folder の消しは app が自分の store の中で行う。

## 9. 試験

### 9.1 host（`plan/ws197/tests/`、ASan・UBSan）[M13]

試験の期待値（tag・PropertySelector の byte・Target の UUID・opcode・record の byte）は §0 の表から手で書いた定数にし、実装の macro を使わない。

| 試験 | 内容 |
| --- | --- |
| `bt-vcard-host-test`（i01 済み、170 checks） | 第 1 版 §9.1 の内容と、縮めた vCard の読み直し、fuzz 20 万回 |
| `bt-phone-host-test`（rfcomm、足す） | **[N1]** 自分の DLC の PN の期限切れはその DLC だけ（closed TIMEOUT、frame 無し）、SABM の期限切れは DISC と CLOSING の後に UA でも DM でも closed TIMEOUT [r3m1]、遅い UA で開かない、MSC の期限切れも同じ [r3m2]、**他の DLC は残る**、DLCI 0 の期限切れは今のまま session 全体、スマホの PN の答えを拒む時は ERROR [r3m3] |
| `bt-phonemux-host-test`（新） | SDP の持ち主、EBUSY、**[R3] dlc_open の同期の error で行が消える、EEXIST は EBUSY に、dlci の表の ours の行（CLOSING の間）があれば EBUSY**、DLC の ours・accept の振り分け、dlci の表と closed、**[M10] opened の無い closed（DM・TIMEOUT）を（channel、1）から子へ**、**[r2m4] 同じ番号の（16、0）と（16、1）が別の行**、accept の 2 回、**[N2] 行がある間の dlc_open は EBUSY、closed の後は通る**、open_failed、表の満ち（ENOSPC）、ended で表が空、ready・ended の全部への配り |
| `bt-map-host-test`（足す） | [M10] OPENING の DM の closed で `refused`、TIMEOUT で `timeout`、**[N2] 35 s 後の opened で READY に進む（期限で捨てない）**、やり直しが前の DLC の closed の後 |
| `bt-phone-link-host-test`（足す） | [M8] contacts の記録で PCE の record（手の byte）が db に入り link の終わりで出る、messages だけでは入らない。[M3] handoff の新しい記録は profiles=m。PENDING_DLCS 4。**[r2m13] 本物の rfcomm.c・phone.c・mux を通す場面**: 2 つの子（MAP と偽の PBAP）、PBAP の DLC の PN に DM・SABM に DM・SABM の期限切れで、PBAP の子だけに closed が届き MAP の DLC は残る |
| `bt-phonerec-host-test`（足す） | `asked` の行の読み書き、`asked` の無い記録は読める、壊れた `asked`、**移行の関数**（有効な記録の c・h を外す、m の無い記録は 0 と `-`） |
| `bt-pbap-host-test`（新） | 台本の PSE（map の試験と同じ形）: SDP の record（RFCOMM の channel、0x0314 の bit 0 無し → no-pb、PSE 無し → no-pse）、Connect の byte（Target 16 byte、**App Parameters 無し** [M9]）と 60 s、0xC3 → permission と 600 s、**[M12・N1] READY 前の closed の reason ごとの why（REFUSED・REMOTE → permission 600 s、TIMEOUT → timeout 30 s、ERROR・LOST → closed 30 s）**、**[r2m11] IDLE からの PAGE が 100 s で timeout、接続はその後も続く**、**[N3・r3m4] 電話帳が 32 の倍数（32・64）の時の確かめの Get: 0xC4・0xC0・0xD0・空の Body で終わる、PhonebookSize が 0.vcf を数えない PSE で最後の 1 件も出る、calls の object ごとの SIZE**、**[N4] END の無い card の後の page: 前の item は出る、capped bit 4、o は BEGIN の数だけ進む**、**[r2m10] 読めない card は skipped、datetime だけ読めない件は zone none・partial 1**、**[r2m5] 世代が乱数から**、SIZE の App Parameters の byte（`04 02 00 00`）と **response の hook の最初の packet からの PhonebookSize** [m2]、PULL の byte（MaxListCount・ListStartOffset・Format `07 01 01`・PropertySelector の 8 byte）、0.vcf を出さず skipped に数えない [m9]、**[M2] 終わりは件数 < N・終わりの SIZE の取り直しと capped bit 2**、cursor（世代・object・offset・size）と stale、**[M11] 世代が同じなら接続し直しても cursor が通る**、5000 の capped bit 1、calls の 3 つの順・**since で打ち切らず全部読んで filter** [m4]・500 で capped、256 KB で N の半分と 1 件の飛ばし、Get の Continue（0x90）の続き、room の待ちと slow、cancel、**[M11] 操作の無い時の切断は IDLE、次の PAGE で接続し直す、操作中の切断は lost と failed**、STATE の contacts と contacts_why、up の hook |
| `bt-phoneio-host-test`（足す） | CONTACT・CALL-LOG の行（`length=` が引用の欄より前、CALL-LOG の `length=0`）、zone の言葉、escape、2047 byte、名前の中の ` length=`（M1 の形） |
| `phone-backend-host-test`（足す） | `record_known` の列（届かない → 届く → SHOW の答え）[R2]、off の LINK に `profiles=` が無い [r3m7]、`PHONE PAGE contacts|calls` の行、CONTACT（本文付き）と CALL-LOG（`length=0`、本文無し）の読み [N5]、item の `what`、zone の言葉から 0・2・3 と partial、STATE の contacts・contacts_why、not-owner・no-record・届かない時に contacts 0 [r2m7]、capped の bit |
| `phone-shell-host-test`（足す） | what 1・2 の中継（item の what が backend の物）、what 3 は EINVAL、`link_contacts` の event と版（古い版の object には出ない）、**[N6] ENOBUFS の時に LINK ごと owed に残り、次に `link_contacts` → `link` の順で出し直す、古い LINK が新しい LINK に置き換わる**、**[N9・R1・R2] record の写し（loopback は 0、設定 none でも bluetooth の状態から、unreachable・届いた直後で記録を読む前は 0、no-record は 1、記録は 2）、bluetoothd の再起動の列（届かない → 届く → SHOW）で record が 1 を通らない**、libkeiland の短い size の link（KL_VERSION 79 の大きさ ＝ `offsetof(contacts)` ＝ 88 byte で写せる、それより短いと EINVAL）[r3m8]、link の contacts・contacts_why・record、`link_contacts` 無しの `link` は 0 |
| `phone-store-host-test`（足す） | [B1] `phonebook/` の読み・書き（`contacts/` に混ざらない、`ph_contacts` の数が変わらない）、同じ中身は書かない（mtime）、**[M2] `ph_phonebook_prune_plan`: 1 回目は missing だけ、2 回目で消す、また現れた key、capped（bit 1・2・4 のどれでも）・失敗・空・半分より少ない時は消さない**、5000 の上限、**[N7] 番号の会話の行がある fixture で名前が電話帳の物になる**、名前の引き（手元の連絡先が先、番号ごと）、**[r2m8] country を変えた後の引き**、`store_apply_phone_names` で行の数と順が変わらない、通話の item の書きと重複無し、**[N8・r3m6] 1023 の後の新しい番号の通話と SMS は `o` の会話に入り開き直して見える、mkdir の残りが無い、上限を越えた folder がある store を開ける**、非通知の `w` の会話を開き直して見える [r2m9]、key の引きの `:pbap:`、同期の目印の 4 行と知らない行が残る、**[N9・R1・R2] `ph_phonebook_forget` の列（Stop（設定 0 の link）・別のスマホ・no-record で消す、record 0・not-owner・link 未受信では消さない）** |
| WS143 `bt-daemon-host-test.sh` | main.c を変えるため |

### 9.2 QEMU

PBAP の相手が QEMU に無い（p003 §10.2 と同じ）。T1 は頼まない（HID の回帰も、2026-10-10 ユーザー「流しすぎです。もう不要」）。

### 9.3 実機（5330、ユーザーの UAT。BUG-287 の直しの image の後）

Android で: 「Use as phone」→ スマホの「連絡先と通話履歴へのアクセス」の許可 → Phone の app の番号の会話に名前が出る、通話の履歴が会話に入る（受けた・掛けた・出なかった）、スマホで連絡先を消して「今すぐ同期」を 2 回 → app の `phonebook/` から消える。許可を拒否 → Settings に「Allow access to contacts on the phone」、10 分の間に許可の画面が繰り返し出ない。bluetoothd の log に `pbap: ready`・`pbap: page contacts …`。§13 の未確認の各行を見る。

## 10. 実装の順（WIP commit の単位、区切りごとに SHA を Q1 へ）

| i | 内容 | 確かめ |
| --- | --- | --- |
| i01 | `vcard.c`（§4） | **済み**（da5dbf9ac） |
| i02 | rfcomm.c の DLC の期限（§3.2 の N1）、`phonemux.c`（§3.1）と main の配線（MAP を mux の子に）、map.c の DM と EBUSY のやり直し（§3.2）、phone.c の PCE の record（§3.3）・PENDING_DLCS 4・handoff の既定と `asked`・移行（§8.1、phonerec） | bt-phone（rfcomm）、bt-phonemux、bt-map、bt-phone-link（本物の rfcomm を通す場面）、bt-phonerec、bt-phone-host-test.sh 全部、WS143、build |
| i03 | `pbap.c` の始め（SDP・DLC・Connect・許可・IDLE・やり直し・STATE）（§5.1） | bt-pbap（始めの部分）、build |
| i04 | `pbap.c` の page（§5.3）、`btd_vcard_count`、`phoneio.c` の行、main の `PHONE PAGE contacts|calls`・SHOW・配線（§5.2・§5.4・§5.5） | bt-pbap、bt-vcard、bt-phoneio、WS143、build |
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
| 許可の画面 [M12・N1] | Android は Connect の間か RFCOMM の受けで「連絡先と通話履歴へのアクセス」を聞くか、MAP の許可と別か、拒否の形（DM・DISC・0xC3）、利用者が 60 s 答えない時（SABM の期限で DISC、30 s 後にやり直す） | UAT |
| ペアの時の PCE の record [r2m15] | handoff は messages だけなので、ペアの時の SDP に PCE の record が無い。Android がペアの時に PCE の有無で連絡先の共有を出し分けるなら、後の「Use as phone」で許可を聞くか（推測） | UAT |
| 末尾の offset [N3] | o ＝ PhonebookSize の Get はしない設計にしたので答えは要らない。電話帳が page の間に増えた時は capped bit 2 で消しを止める | — |
| 相手の切断 [M11] | Android が idle の PBAP の DLC を切るか、接続し直しで許可を聞き直すか | UAT |
| UID | Android・iPhone の vCard に UID が入るか（無ければ名前と番号の key、改名で別の連絡先 → 古い物は §7.2 の 2 回の後に消える） | UAT |
| 時刻 | 履歴の datetime はスマホの local time。zedBSD と timezone が違うとずれる | UAT |
| 履歴の順 [m4] | 新しい順は should。since で打ち切らないので順は結果に効かない | — |
| mch の印 [m14] | mch を読むとスマホの不在着信の印が消える機種（Pc5） | UAT |
| SDP の属性 ID | Assigned Numbers の値を手元で確かめていない | 実機の SDP の dump |
| page の間の変化 | offset のずれ（§5.3）。消しは §7.2 の 4 つの条件 | — |
| 大きな削除 [r2m12] | スマホで半分より多くを一度に消すと写しが残り続け、消えた名前が番号の会話に残る（§7.2 の限界） | UAT |
| 行の上限 [N8・r3m6] | 番号の会話が 1023 に届いた後の新しい番号の通話・SMS は溢れの会話 `o` にまとまる | UAT・記録 |
| store の大きさ | 5000 件の .vcf を開く時に全部読む。1 件 200 byte で 1 MB、開く時間は i06 で計る（遅ければ 1 つの索引の file に寄せる） | i06 |
| 同期の待ち | contacts の 157 page の間 messages の同期が待つ（messages を先、contacts は 1 日 1 回） | i06 |
| downgrade [r2m1] | 古い bluetoothd は `asked` を落として書き直し、p005 に戻すと移行がもう 1 度走って contacts が外れる（§8.1） | 記録 |

## 14. 見積もり

i01 1.5（済み）、i02 2（mux に rfcomm の DLC の期限・map の穴・PCE の record・記録の移行が加わった）、i03 1.5、i04 1.5、i05 1.5（M6 の size と owed の中の新しい event）、i06 3（B1 の電話帳の表・消しの計画・会話の名前・行の上限・写しの消し）、i07 0.5、計 **11.5 LW**（ws.md の 8 LW から +3.5、ws.md の行は第 3 版で直した）。

## Event

- 2026-10-11: 第 1 版（P1）。PBAP 1.2.3 の §2.7・§3.1・§5.1・§6.2〜§6.4・§7.1・§9 を読んで書いた。
- 2026-10-11: design-reviewer（agent ad782c2d4ddaad97c）→ [review-1.md](review-1.md)。blocker B1（スマホの連絡先を store の配列に混ぜる形）、major 13、minor 14、追加の判断 Pc3〜Pc6。i01 は GO。
- 2026-10-11: M1（backend の `phone_field` が引用の中の ` length=` を欄と読む、今の `PHONE MESSAGE` にもある既存の欠陥）を直した（eee2a5d12、phone-backend-host-test に偽の欄の例、PASS、zedBSD の libkeiland.so の build rc 0）。

- 2026-10-11: i01（vcard.c）を実装（da5dbf9ac、P1 の新しい世代）。
- 2026-10-11: 第 2 版（P1、9a2020f1b）。review-1 の B1（電話帳の写しを `phonebook/` と名前の表に、item は番号の会話）、M2〜M13、minor 1〜14 を直した。Pc1〜Pc6 は推しを仮に入れた（§12）。
- 2026-10-11: design-reviewer（agent abafef44a63431227）→ [review-2.md](review-2.md)。blocker 0、major 9（N1 rfcomm の DLC の期限切れが session 全体を終える、N2 OPENING の 30 s、N3 末尾の offset の Get、N4 終わらない card、N5 CALL-LOG の length、N6 link_contacts と owed、N7 名前の引きが番号の会話を手元と見る、N8 1024 の行の上限、N9 写しの消しの条件）、minor 15。i02〜i07 は直してから。
- 2026-10-11: 第 3 版（P1、9c71a95b5、Q1 が main に merge）。N1〜N9 と minor 1〜15 を直した（§3.2 の rfcomm の直し、§3.1 の表の key と EBUSY、§5.1 の reason ごとの why と 100 s、§5.2 の行の形と capped bit 4、§5.3 の終わりと件数、§6 の record と owed の中の event、§7 の手元だけの引き・行の上限・`w`、§8 の移行・Stop の profiles・写しの消しの関数）。

- 2026-10-11: design-reviewer（agent a35a020e382fe05ac）→ [review-3.md](review-3.md)。blocker 0、major 3（R1 Stop の後に設定が 0 で写しが消えない、R2 届いた直後の「記録無し」で写しが消える、R3 mux の行の漏れと EBUSY）、minor 8。i02 条件付き GO、i03・i04・i07 GO、i05・i06 は R1・R2 を書いてから（再 review 不要）。
- 2026-10-11: 第 3.1 版（P1）。R1〜R3 と minor 1〜8 を書いた（§3.1・§3.2・§5.1・§5.3・§6・§7.1・§8.2・§9.1）。これで i02〜i07 は GO（Pc1〜Pc6 の答えで直す所は残る）。

## 実装の進み

| i | commit | 内容 | 確かめ |
| --- | --- | --- | --- |
| i01 vcard.c（§4、review-1 で GO） | da5dbf9ac | 新 `vcard.c`・`.h`: `btd_vcard_next`（Body から 1 件ずつ、前の行・間の行を飛ばす、入れ子の AGENT を数える、途中で切れた件は EINVAL）、`btd_vcard_contact_read`（§4.1 の読み: CRLF・LF、3.0 の折り返しは 1 字を除き 2.1 は空白を残す（VERSION の行を先に探す、不明は 3.0 の扱い）、QP の soft line break（行の head が QP の時だけ）、group、名前・parameter の大文字・小文字、2.1 の裸の parameter、引用の TYPE、CHARSET は UTF-8 か無しだけ、BASE64・B・知らない ENCODING は捨てる、3.0 の escape、FN が先・無ければ N を「名 姓」・無ければ最初の番号、TEL 8 個（空・数字無し・33 桁以上も dropped に数える）、`tel:` の URI、UID、不正な UTF-8 と制御文字は U+FFFD・改行と TAB は空白・前後の空白を除く・256 byte で文字の途中で切らない）、`btd_vcard_reduce`（§4.2 の縮めた vCard 3.0、NUL 付き、`BTD_VCARD_REDUCED_MAX` 6144）、`btd_vcard_call_read`（kind は datetime の parameter、無ければ folder、datetime は `[0-9TZ+-]` の 23 byte まで）、`btd_vcard_call_time`（`btd_mapxml_time_unix` で Z・offset は `ZONE_PHONE`、無ければ zedBSD の offset で `ZONE_LOCAL`、無い・読めない時は `ZONE_NONE` と 0）。**設計の補い**: (1) 連絡先の key の無 UID の形は「`n|` + 表示の名前（番号で代えた時はその番号）+ `|` + 並べた番号」（縮めた vCard を読み直しても同じ key になる）。(2) datetime の無い通話は kind と番号が同じなら key が同じ（区別する物が無い、review-1 minor 5 の記録）。(3) 名前も番号も無い件は ENOENT（pbap は skipped に数える）。(4) 4 KB を越える行は head だけ読み、値を使わない（TEL なら dropped）。(5) 入れ子は 8 段まで（9 段は EINVAL）。Makefile に vcard.c。試験: 新 `bt-vcard-host-test`（170 checks: Body の切り出し、2.1（QP の UTF-8、文字の途中の soft break、裸の parameter、BASE64 の PHOTO、2.1 の折り返し）、3.0（escape、group、引用の TYPE、`tel:`、折り返し、TAB）、charset、U+FFFD、256 byte の切り、TEL 9 個・33 桁、番号だけ、名前も番号も無い、16 KB・4 KB・9 段・開かない・閉じない、縮めた vCard の手の byte 列と全部の短い room と読み直し、試験の側の FNV-1a（既知の値で確かめた）での key、通話（2.1 MISSED、3.0 DIALED の UTC 1704110400、+9 時間の local 1704078000、folder の kind、datetime 無し・読めない・変な byte、範囲外の folder）、fuzz 20 万回で「読めた件は約束を守り、縮めた vCard から同じ名前・key・番号の数に読み直せる」） | `bt-phone-host-test.sh` PASS（16 本）、WS143 `bt-daemon-host-test.sh` PASS（FAIL 0）、target の bluetoothd（`ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/p1-uat`）rc 0・warning 0、style-check（vcard.c・.h・試験）0、`git diff --check` |
| i02 rfcomm・mux・MAP の穴・PCE の record・記録（§3・§8.1） | 8b0de28ad・4eefcce2c・この commit | **rfcomm.c**（§3.2 N1・r3m1〜r3m3）: 自分の DLC の期限切れは `rfcomm_dlc_expired` でその DLC だけ（NEGOTIATING は frame 無しで TIMEOUT、CONNECTING・OPEN（MSC 待ち）は DISC を送って CLOSING、UA・DM・DISC の期限で TIMEOUT、スマホが開いて owner に告げていない DLC は黙って消す、CLOSING の期限は frame 無し）、`struct btd_rfcomm_dlc` に `close_reason`、スマホの PN の答えを拒む時は ERROR（前は REFUSED）。DLCI 0 の期限切れは今のまま session 全体。**phonemux.c・.h**（新、§3.1・R3）: 子 3 つ、行 8 つ（channel・ours・dlci・子）、SDP は 1 本で答えは頼んだ子だけ、dlc_open は（channel、自分）の待ちの行か `dlci >> 1` が同じ自分の開いた行があれば EBUSY、phone の同期の error で行を消し EEXIST は EBUSY、opened は（channel、ours）の行から dlci の表へ、opened の無い closed は（`dlci >> 1`、自分）の行から子へ、accept は最初に 1 の子が（channel、スマホ）の行（2 回目は書き直し）、持ち主の無い opened は `dlc_close` の hook で閉じる、ended で全部の行を空に。**map.c**（§3.2・N2）: OPENING で MAS の channel の closed は reason で `refused`・`timeout`・`closed` の失敗、dlc_open の EBUSY は失敗にせず SDP から 2 s 後にやり直す、OPENING の期限は持たない。**phone.c・.h**（§3.3・§8.1）: `phone_records_update`（MNS と PCE）、PCE の record（0x112E、言語 base、PBAP 1.1、"Keiland Phonebook"、protocol 無し）は READY・有効・enabled・contacts の時だけ、`BTD_PHONE_PENDING_DLCS` 4、handoff の新しい記録は messages だけ（同じ持ち主の記録は移行してから profiles を残す）、`btd_phone_load` は有効な古い記録を移行して書き戻す。**phonerec**: 無くてもよい `asked m,c,h|-` の行（format はいつも今の profiles で書く、parse は 1 回だけ・字は m・c・h・`,` の間）、`btd_phonerec_migrate`（`asked` の無い記録は messages だけ残す）。**main.c**: `btd_profiles`（mux）を phone link の profile に、MAP をその最初の子に（MAP の SDP と DLC は mux を通す）、mux の hook（phone の SDP・DLC・close、log）。Makefile に phonemux.c。試験: bt-phone（rfcomm）に `test_dlc_timeouts`（PN・SABM・SABM と DISC の両方・MSC の期限、遅い UA、PN の答えの拒み、スマホの DLC と session が残る）と既存の拒みの期待値を ERROR に（325 checks）、新 bt-phonemux（70）、bt-map に `test_opening`（DM・TIMEOUT・他の channel・35 s 後の opened・EBUSY でのやり直し、167）、bt-phonerec に `asked` と移行（49）、bt-phone-link に `test_pbap`（handoff は messages だけ、PCE の record の手の byte と出し入れ、古い記録の load での移行と書き戻し、本物の phone・rfcomm の上の mux: 1 つ目の子の DLC が開き、2 つ目の子の DLC はスマホの DM で 2 つ目だけに closed REFUSED、1 つ目の DLC は残る、同じ channel をまた頼める、214）。既存の phone-link の試験の style の 4 か所も直した。**限界**: SABM の期限切れを本物の phone の上で通す場面は作らなかった（偽のスマホの rfcomm は答えを止められない。rfcomm の単体の試験が見る） | `bt-phone-host-test.sh` PASS（17 本）、WS143 `bt-daemon-host-test.sh` PASS（FAIL 0）、target の bluetoothd（`ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/p1-uat`）rc 0・warning 0、style-check（bluetoothd の全部と変えた試験）0、`git diff --check` |
| i03・i04 pbap.c・phoneio の行・main（§5） | この commit | 新 **pbap.c・.h**: 状態 OFF・SDP・OPENING・CONNECTING・READY・IDLE・FAILED（§5.1）。SDP で PSE（0x112F）の最初の RFCOMM channel、0x0314 が無ければ local の bit を仮定、bit 0 が無ければ `no-pb`、PSE が無ければ `no-pse`（どちらもこの link ではやり直さない）、0x0317 の有無は覚えるだけ（M9 (a)）。DLC（mux の EBUSY は 2 s 後にやり直し）→ Connect（Target だけ、60 s）。READY 前の DLC の closed は REFUSED・REMOTE が `permission`、TIMEOUT が `timeout`（どちらも 600 s）、他は `closed`（30 s から倍）。Connect の 0xC1・0xC3 は `permission`、答えが無い（ETIMEDOUT・BROKEN）は `timeout` で 600 s、他の失敗は `refused`。READY で `up` の hook。操作の無い時の相手の切断は IDLE（STATE は ready）、次の PAGE が開き直す、操作中の切断は `closed` の失敗で PAGE は `ERROR lost`。page（§5.3）: 2 つまで、古い方から 1 つずつ（1 つの操作だけが走る）。cursor `<世代 8 桁>.<object>.<offset>.<始めの size の 16 進、ffffffff は不明>`、世代は起動の乱数から、hook の address が前と違うスマホなら +1、違う世代は `stale-cursor`。SIZE（04 02 00 00）の PhonebookSize は response の hook から。PULL（Name・Type `x-bt/phonebook`・06 の 8 byte・07 01 01・04・05）。body は `btd_vcard_next` で切り、件数は新しい `btd_vcard_count`（段 0 の BEGIN の数）。pb の index 0 は出さず数えない。読めない card は skipped。終わり: 件数 < 頼んだ数（終わらない card の page は bit 4 を立て、件数を信用せず終わりにしない）、または上限（pb は 5001、履歴は 500、bit 1）。size 以上の offset の PULL の失敗・空は終わり（N3・r3m4）、size より前の失敗は `ERROR refused`。pb の終わりは SIZE を取り直し、違う・取れない・始めが不明なら bit 2。履歴は ich・och・mch の順で、SIZE が取れない object は空として次へ、object の終わりで次の object（cursor の size は ffffffff）、mch の後は more=0。since より前の通話は出さない、datetime の無い・読めない通話は `zone=none partial=1` で今の wall の時刻。256 KB を越えた Get は Abort（obex の body_limit）で頼む数を半分、1 件で越えればその 1 件を skipped にして次へ。client の room が 32 KB 未満なら待ち、30 s で `ERROR slow`。IDLE・接続中の page は 100 s で `ERROR timeout`（試みは続ける）。cancel は走っている操作の page を終わりで黙って消す。**phoneio**: `btd_phoneio_contact_line`・`btd_phoneio_call_line`（daemon の欄の後に `length=`、それから引用の文字列、CALL-LOG は `length=0`）。**vcard**: `btd_vcard_count`。**main.c**: `btd_contacts`（PBAP）を mux の 2 つ目の子に（hook は MAP の物を使い回し、wanted は contacts の bit、address は記録の address、SDP・DLC は mux を通す）、loop の tick・deadline、PHONE LINK と持ち主の変化の後の check、round の終わりの pump、client の close と持ち主の変化の cancel は MAP と PBAP の両方に（待つ profile の欄を足さず、両方に cancel を呼ぶ: 知らない token は何もしない）。`PHONE PAGE contacts [cursor=] count=` と `PHONE PAGE calls since= [cursor=] count=`（limit は messages だけ、contacts は since を使わない）。SHOW・STATE の行に `contacts=`（と `contacts_why=`）。protocol.h の説明。Makefile に pbap.c。**設計の補い**: (1) 操作の queue（24）は作らず、page を 2 つまで古い順に 1 つずつ（PBAP は 1 page ＝ 1 Get で、割り込みの event が無い）。(2) `waits_phone` を「待つ profile」にせず cancel を両方に呼ぶ（同じ結果で簡単）。(3) Connect の BROKEN も 600 s。試験: 新 `bt-pbap-host-test`（240 checks: setup の byte、拒否の各形と待ち、no-pse・no-pb、busy の channel、contacts の SIZE・PULL の byte、持ち主の card、行と縮めた vCard、PAGE-END、size の取り直しと bit 2、Continue の 2 packet、終わらない card と bit 4、読めない card、32 の倍数の probe（0xC4・空）、size が持ち主の card を数えない PSE、size 前の失敗、256 KB の Abort と半分と 1 件の skipped、calls の 3 つの object・UTC・local・since・datetime 無し・size の無い object、cursor の不正・stale・同じスマホ、IDLE と開き直し、操作中の切断、100 s、cancel・slow・pump・busy・not-ready・link の終わり。期待値をわざと変えると FAIL になることを確かめた）、bt-phoneio に `test_items`（64）、bt-vcard に count（172） | `bt-phone-host-test.sh` PASS（18 本）、WS143 `bt-daemon-host-test.sh` PASS（FAIL 0）、target の bluetoothd rc 0・warning 0、style-check（bluetoothd の全部と ws197 の試験、ただし i05 で触る phone-backend-host-test.c の既存の 35 か所を除く）0、`git diff --check`。**限界**: main.c の配線（PAGE の文法、SHOW の行、cancel）は host 試験が無い（p003 と同じ、UAT で見る） |

## 再開の手順（2026-10-11 P1 のラップアップ、context の上限）

状態: 設計は第 3.1 版（review-3 で i02〜i07 GO）。**i01〜i04 は実装済み**（da5dbf9ac、8b0de28ad・4eefcce2c・ea6361393、7b2e6af5c。branch `agent/p1-ws197`）。bluetoothd の側（rfcomm・mux・MAP の穴・PCE の record・記録の移行・pbap.c・socket の `PHONE PAGE contacts|calls`・SHOW/STATE の `contacts=`）は終わった。ユーザーの判断 Pc1〜Pc6 はまだ答えが無い（選択肢はユーザーに出した物と一致、Q1 2026-10-11）。仮に入れた推しは i06・i07 で使う。

次は **i05（中継、§6）**。触る file と要点:

1. `userland/desktop/libkeiland-backend/keiland-backend.h`: `KL_BACKEND_PHONE_WHAT_CONTACTS 1U`・`_CALLS 2U`、`struct kl_backend_phone_item` の先頭に `unsigned what;`、`struct kl_backend_phone_state` に `contacts`・`contacts_why[KL_BACKEND_BT_REASON_MAX]`・`record_known`（R2）。
2. `userland/desktop/libkeiland-backend-zedbsd/phone-zedbsd.c`: `kl_backend_phone_page` の what 1・2 の行（`PHONE PAGE contacts [cursor=] count=`、`PHONE PAGE calls since= [cursor=] count=`、limit は書かない）。`PHONE CONTACT`（本文付き）と `PHONE CALL-LOG`（`length=0`、N5）の読み（今の `PHONE MESSAGE` の口、kind は folder 0・1・2、`zone=phone|local|none` を 0・2・3 に、none は partial）。STATE・SHOW の `contacts=`・`contacts_why=`。not-owner・no-record・届かない時（phone-zedbsd.c の 253・1307-1314・1559-1569・1632-1635）は contacts 0・why 空（r2m7）。`record_known`: SHOW の答えか記録の有無を言う STATE の行で 1、届かない時の全消しで 0（R2）。off の `PHONE LINK` に `profiles=` を書かない（r3m7）。
3. `userland/desktop/wayland/phone-shell.c`・`kl-system-protocol.h`: `sync` の what の検査を 0〜2 に、item の送りの what を backend の item から。`phone_link_event` に contacts・record・contacts_why、`phone_link_fill` で record（loopback 0、他は設定によらず backend の状態から: reachable 0 か record_known 0 → 0、have_record 0 → 1、他 2、R1・R2）。新しい event `link_contacts(contacts, record, contacts_why)` を manager の新しい版で、owed の LINK の中で `link` の前に（ENOBUFS なら LINK ごと owed に残す、N6）。
4. libkeiland（`include/keiland/keiland.h`、`libkeiland/system/system.c`・`system-view.c`・protocol の listener・exports）: `KL_PHONE_CONTACTS 1U`・`KL_PHONE_CALLS 2U`、`KL_SYSTEM_HAS_PHONE_CONTACTS`、`struct kl_phone_link` の最後に `contacts`・`contacts_why[KL_PHONE_WHY_MAX]`・`record`、`system_view_phone_link_get`・`take_phone_item` は `offsetof(struct kl_phone_link, contacts)`（88 byte）以上の size を受けて `min(size, sizeof)` を写す（M6）、`link_contacts` の値を覚えて次の `link` に入れる、`capped` の説明を bit の組に。calls の item の zone の説明。KL_VERSION と manager の版の番号は Q1 に聞く（merge の時に割り当て）。
5. 試験: `plan/ws197/tests/phone-backend-host-test.c`（**既存の style の 35 か所も直す**）・`phone-shell-host-test.c` に §9.1 の行（N5・N6・N9・R1・R2・r2m7・r3m7・r3m8）。zedBSD の wayland・libkeiland.so と keiland-linux の build（p004a の記録の `keiland-linux.mk` の all）warning 0。
6. その後 i06（Phone の app、§7、Pc1・Pc3・Pc6 の答えで直す）、i07（Settings、§8.2、Pc2・Pc4）。
7. 各 i の後: `plan/ws197/tests/bt-phone-host-test.sh`、WS143 の `plan/ws143/tests/bt-daemon-host-test.sh`、target の bluetoothd（`make ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/p1-uat build/p1-uat/bin/bluetoothd`）warning 0、style-check、WIP commit、SHA を Q1 へ。desktop の試験は `phone-backend-host-test.sh`・`phone-shell-host-test.sh`・`phone-store-host-test.sh`。
