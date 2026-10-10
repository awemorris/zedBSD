<!-- awesome-plan project=zedbsd record=ws197-p005-review-3 -->

# ws197-p005 第 3 版の review（design-reviewer、2026-10-11）

対象: [phase.md](phase.md) 第 3 版（branch `agent/p1-ws197`、9c71a95b5）。今の code（bluetoothd の rfcomm.c・phone.c・map.c・main.c・phonerec.c、backend、compositor、libkeiland、Phone の app の store、Settings）と照らした。review-2 の N1〜N8 と minor は方向として直っている。N9 の直しは今の Settings・backend の状態の作り方と合わず、2 つの向きで壊れる（R1・R2）。mux の行の寿命に新しい穴（R3）。他は実装の中で直せる。

## review-2 の指摘の確かめ

| 指摘 | 判定 | 根拠 |
| --- | --- | --- |
| N1 | 大筋で直った（minor 1・2・3） | §3.2 は NEGOTIATING・CONNECTING・CLOSING を DLC だけで終える。permission は REFUSED・REMOTE だけに絞った（§5.1）。CLOSING の答えの reason、OPEN（MSC 待ち）の期限、tick の中の closed の呼び出しが抜け |
| N2 | 直った（R3） | 子の 30 s をやめ、（channel、1）の行で EBUSY。同期の失敗の時の行の扱いと、dlci の表に残る前の DLC が抜け |
| N3 | 直った（minor 4、推測） | o ≥ 始めの PhonebookSize で Get しない。calls も object ごとに SIZE |
| N4 | 直った（minor 5） | BEGIN の数で o を進め、EINVAL の page は capped bit 4 |
| N5 | 直った | CALL-LOG に `length=0`。backend の `phone_message_line` は length 0 を即座に item にする（phone-zedbsd.c:1428-1432） |
| N6 | 直った | `phone_emit_owed` の LINK の中で `link_contacts` → `link`、ENOBUFS で LINK ごと owed（phone-shell.c:1535-1580・1622-1636 と合う） |
| N7 | 直った | `conversation == 0` の行だけを引く `store_find_local_number`（store.c:299-311・597-602 の問題を避ける） |
| N8 | 大筋で直った（minor 6） | mkdir の順、開く時の読み飛ばし、上限の時は数えるだけ |
| N9 | **直っていない（R1・R2）** | `record` の欄と純粋な関数は良いが、Stop で `phone.backend` が 0 になる・再接続の時に一時的に have_record 0 になる、を見ていない |
| minor 1 | 直った | §8.1 の downgrade の記述は phonerec.c:579-581・146 と合う。`asked` は KEYS_ALL に入れない |
| minor 2 | 直った（minor 7） | off は今の profiles。profiles を付けない形の方が安全 |
| minor 3・5・6・7・8・9・10・11・12・13・14・15 | 直った | §5.5 の表、乱数の世代（main.c:527 と同じ）、zone の言葉、not-owner・no-record で contacts 0、索引の作り直し、`w`、skipped と zone none、100 s、§7.2 の限界、本物の rfcomm を通す試験、ws.md の 11.5、§3.3 と §13 |
| minor 4 | 直った（R3） | key は（channel、ours）、accept の 2 回 |

## blocker

無し。

## major

| # | 節・i | 内容（根拠） | 直し |
| --- | --- | --- | --- |
| R1 | §6.3・§8.2（N9）、i05・i06 | **Stop・ペアの解除で写しが消えない**。「Stop using as phone」の答えで Settings は `phone.backend` を 0（none）にする（page-bluetooth.c:893-899、冒頭 28-29 行）。compositor の link の `backend` はこの設定の値（phone-shell.c:1692 `link->backend = phone_state.setting`）。§8.2 の規則 1（Stop）と 3（記録が消えた）は `backend` 2 を要り、§6.3 は「backend が bluetooth でない → record 0」なので、Stop の後は backend 0・record 0 で**規則 1 と 3 は決して成り立たない**（成り立つのは別のスマホの規則 2 だけ）。Pc3 (b) の推しが実装されない。backend の object は設定にかかわらず desktop の間ずっと開いていて状態を読む（phone-shell.c:30-33・1713-1722） | `record` は設定ではなく zedBSD の backend の状態から作る（設定が none でも bluetooth の backend の状態を写す。loopback の時だけ 0）。規則 1・3 から「`backend` 2」を外し、bluetoothd の記録の `enabled 0`（Stop）・記録無し（FORGET）だけで決める。設定を none にしただけ（Stop でない）は `enabled` 1 のままなので消えない。phone-store の `ph_phonebook_forget` の列に「Stop の後の backend 0・enabled 0・record 2 で消す」「設定だけ none・enabled 1 で消さない」 |
| R2 | §6.3（N9）、i05・i06 | **一時的な「記録が無い」で写しを消す**。backend は届かなくなると state を全部 0 にし（phone-zedbsd.c:1632-1635、`have_record` 0）、次に繋がった update で `reachable` 1 と `CHANGED_STATE` を立てる（1643-1646）が、`have_record` は SHOW の答え（1300-1314）か STATE の行（1555）が来る**後の** update まで 0。compositor は STATE の変化ごとに `phone_links` で link を送る（phone-shell.c:446-455）ので、その間は §6.3 の写しで `record` 1 →規則 3（`owner` を要らない）で 5000 件の写しを消す。bluetoothd の再起動、backend の作り始め（calloc、`have_record` 0）のどれでも起きる。N9 が避けたかった場面そのもの | backend に「記録の有無が分かった」（`record_known`: 作り始めと unreachable で 0、SHOW の答えか STATE の行で 1）を足し、compositor は `record_known` 0 の間は `record` 0。phone-backend-host-test に「unreachable → reachable（SHOW の答えの前）→ SHOW の no-record」の列で record 0 → 0 → 1、phone-shell-host-test に写しの値 |
| R3 | §3.1（N2 の行の寿命と EBUSY）、i02 | (a) 行が消えるのは opened・closed・open_failed・ended の 4 つだけ、と書いたが、mux は行を書いてから phone.c を呼ぶ。`btd_phone_dlc_open` が同期で返す誤り（ENOTCONN phone.c:333、EBUSY 343、rfcomm の EEXIST・ENOSPC・送りの誤り rfcomm.c:252-258・275-279）では opened も closed も来ないので行が残り、以後その channel の `dlc_open` は link の終わりまで**ずっと EBUSY**（MAP も PBAP も止まる）。(b) EBUSY は（channel、1）の行しか見ない。子が `dlc_close` した前の DLC（mux を通らない、CLOSING で UA 待ち最長 20 s）は dlci の表に残り、DLCI は channel から決まる（rfcomm.c:251）ので、その間の新しい `dlc_open` は rfcomm の EEXIST。§3.1・§5.1 は EEXIST の扱いを書いていない（今の map.c:339-341 は `map_fail("rfcomm")` で間隔が伸びる）。`PHONE LINK on` のすぐのやり直しで当たる | (a) phone.c が誤りを返したら mux は書いた行を消してから子に返す、と §3.1 に書く。(b) EBUSY は「dlci の表に同じ channel の ours の行がある」時も返す（closed で消えるまで）。EEXIST も EBUSY と同じく `BTD_MAP_BUSY_MS` 後のやり直し。bt-phonemux に「ENOTCONN の後に行が残らない」「dlc_close の後 closed の前は EBUSY」 |

## minor

1. N1 の CLOSING の reason: CONNECTING の期限で DISC → CLOSING にした DLC の UA・DM は、今の code では `CLOSED_LOCAL` で free される（rfcomm.c:1539-1540・1561-1563）。§3.2 の「closed(dlci, TIMEOUT)」には DLC に閉じる理由の欄（例 `close_reason`）が要る。CLOSING の期限切れの「reason は今のまま」は今は DLC の reason が無い（session の TIMEOUT）ので、何を出すかを書く（自分の close なら LOCAL、期限の DISC なら TIMEOUT）。
2. N1 の OPEN（UA の後の MSC 待ち、T2 20 s、rfcomm.c:2029-2032）が §3.2 の一覧に無く、今のまま session 全体を終える（MAP も落ちる）。「最長 100 s」も PN 20 + SABM 60 + MSC 20 の道を含めて書く。OPEN も自分の DLC は DISC → CLOSING で DLC だけにする。tick の DLC の loop の中で closed を呼ぶ（今は NEGOTIATED の reason 0 だけで呼ばない）ので、子は closed の中で `dlc_open` しない（map の `map_close_later` と同じく後で）と書く。rfcomm.c:466-470 の注記（RFCOMM 1.2 §5.3 で session を終える）を直し、DLC だけで終える理由を §3.2 に残す。
3. §5.1 の permission: `REFUSED` は rfcomm が自分で PN の答えを断った時（`rfcomm_pn_response`、rfcomm.c:1888-1895、CL や N1 が合わない）にも出る。これを 600 s と「Allow access to contacts on the phone」にしない（rfcomm は自分の断りを ERROR で出す、または PBAP は NEGOTIATING の間の REFUSED を `refused` に）。また SABM の期限（利用者が 60 s 答えない）は `timeout` の 30 s からで、許可の画面が 90 s ごとに出得る。§9.3 の「10 分に 1 回より多くない」は拒否だけの条件なので、SABM の期限も permission と同じ 600 s にするか、UAT の条件に「答えない時」を足す。
4. N3（推測）: 終わり (1) は PhonebookSize が 0.vcf を数える前提。数えない PSE で連絡先の数が 32 の倍数だと、最後の 1 件を一度も読まず、§7.2 の 2 回で生きている連絡先を消す（capped も立たない）。(1) で終わった最後の page が満ちていた時だけ o ＝ size の Get を 1 度し、0xC4・0xC0・0xD0・空の Body をどれも「終わり」として扱う（error にしない）と §5.3 に。bt-pbap に「0.vcf を数えない台本の PSE で 32 件」。
5. N4 の `btd_vcard_count`: 「行頭の空白を除いて」比べると折り返しの続きの行も数える。行頭（CRLF・LF の直後、空白無し）の `BEGIN:VCARD` だけを、`btd_vcard_next` と同じ深さ（vcard.c:241-259）で一番外の物だけ数える。PropertySelector を守らず AGENT を入れる PSE で o が進みすぎ、間の連絡先を飛ばして消す（2 回）のを避ける。
6. N8: (3) を MAP の SMS にも広げると、上限の後の新しい番号の SMS は目印が進んで app に二度と来ない（今は同期の失敗で止まる）。p004 の振る舞いの変更なので §1.1 に書く。黙って捨てるより、`w` と同じ「上限を越えた番号」の会話 1 つ（例 `o`、名前「Other numbers」）にまとめる方が失わない（推し）。(2) の読み飛ばしは readdir の順で決まるので、どの会話が見えなくなるかが定まらない（最近の会話が消え得る）ことも §13 に。
7. r2m2: off で `phone_link.profiles` を渡すと、Settings の link が古い・0 の時に `profiles=`（空）で記録の bit を消す（main.c:2278-2295 は空を 0 と読む）。main.c は `profiles=` 無しを「今のまま」と読むので、off は profiles を付けない（backend の `link_set` に「profiles を付けない」の形を足す）方が確か。
8. §9.1 の phone-shell の行「79 の size で写せる」は版 79 の大きさ（`offsetof(contacts)` ＝ 88 byte）の意味。「88 byte（版 79 の `sizeof`）」と書く（79 byte と読まれる）。

## 判定

| i | 判定 |
| --- | --- |
| i01 vcard.c | 実装済み、GO のまま（minor 5 の数えの関数を足す時に深さを合わせる） |
| i02 rfcomm・phonemux・map・PCE の record・記録 | **条件付き GO**: R3（行の消しと EBUSY の範囲、EEXIST）と minor 1・2 を実装の中で直し、phase.md に記録する |
| i03 pbap の始め | GO（minor 3 は実装の中で） |
| i04 page と socket | GO（minor 4・5 は実装の中で） |
| i05 中継 | 直してから: R1・R2 の `record` の作り方（設定でなく backend の状態、`record_known`）を §6.1〜§6.3 に書いてから。書けば再 review は要らない |
| i06 Phone の app | 直してから: R1 の §8.2 の規則（`backend` 2 を外す）と R2 を書いてから（minor 6 は実装の中で）。Pc1・Pc3・Pc6 の答え待ちは変わらない |
| i07 Settings | GO（minor 7 は実装の中で。Pc2・Pc4 の答え待ちは変わらない） |
