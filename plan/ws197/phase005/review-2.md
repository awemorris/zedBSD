<!-- awesome-plan project=zedbsd record=ws197-p005-review-2 -->

# ws197-p005 第 2 版の review（design-reviewer、2026-10-11）

対象: [phase.md](phase.md) 第 2 版（branch `agent/p1-ws197`、9a2020f1b）。今の code（bluetoothd、backend、compositor、libkeiland、Phone の app、Settings）と照らした。PBAP の byte 列（tag、PropertySelector `…00 20 00 87`・`…10 00 00 87`、Format、MaxListCount=0、Target）は第 1 版の review と同じく正しい。review-1 の B1・M2〜M13 は方向として直っているが、下の N1〜N9 は今の code と合わない所・新しく入った欠陥。

## review-1 の指摘の確かめ

| 指摘 | 判定 | 根拠 |
| --- | --- | --- |
| B1 | 直った（ただし N7・N8） | §7.1 の `phonebook/` と名前の索引で store の配列に入れない。名前の引きの関数と 1024 の上限が残る |
| M1 | 直った | phone-zedbsd.c:1699-1750 は引用の外の欄の頭だけを見る |
| M2 | 大筋で直った（N3・N4） | 終わりの判定・SIZE の取り直し・半分・2 回。末尾の page と vCard の切れ目に穴 |
| M3 | 直った（minor 1） | handoff の既定と `asked`。downgrade の記述が code と違う |
| M4・M7 | 直った（N5） | `what`・`contacts_why`。CALL-LOG の `length=` が矛盾 |
| M5 | 直った（minor 6） | zone 0・2・3 |
| M6 | 一部（N6） | size の規則は良い（`offsetof(contacts)` ＝ 旧 `sizeof` ＝ 88）。新しい event が owed の仕組みの外 |
| M8・M9・M11 | 直った（minor 5） | PCE の record、App Parameters 無し（UAT）、cursor の世代 |
| M10 | 一部（N1・N2） | DM の振り分けは正しい（rfcomm.c:1545-1567、`rfcomm_announced` は ours で真）。期限の長さと session 全体の timeout が新しい問題 |
| M12 | 一部（N1） | 「OPENING・CONNECTING の closed は全部 permission」が広すぎる |
| M13 | 一部 | 下の各 N の試験が無い |
| minor 1〜14 | 1・13 は一部（minor 2・3）、他は直った | |

## blocker

無し。

## major

| # | 節・i | 内容（根拠） | 直し |
| --- | --- | --- | --- |
| N1 | §5.1・§3.2、i02・i03 | rfcomm.c:489-507 は自分の DLC の PN（T2 20 s）か SABM（T1_DLC 60 s、rfcomm.c:1859・1913）の答えが来ないと **session 全体**を終える（`rf->failed = TIMEOUT` → `rfcomm_end`、全部の DLC に closed TIMEOUT）。profile が MAP だけの時は害が無かったが、§5.1 M12 の見込み（Android は RFCOMM の受けで許可を聞く）どおりなら、利用者が 60 s 以上答えない PBAP の SABM が **MAP の DLC も落とす**。さらに §5.1 は「OPENING・CONNECTING の間の DLC の closed」を全部 `permission`（600 s）にするので、他の DLC の timeout・`ERROR`・link の切れで閉じた PBAP も 10 分止まる | rfcomm.c で DLCI≠0 の T1・T2 の期限切れはその DLC だけを閉じる（closed TIMEOUT、session は残す）か、残すなら理由を §3 に書く。permission にするのは reason が `REFUSED`（DM）と `REMOTE`（DISC）の時だけ、TIMEOUT・ERROR・LOST は `timeout`・`closed` の 30 s から。bt-phone-host-test（rfcomm）に「DLC の SABM の期限切れで他の DLC が残る」、bt-pbap に reason ごとの why |
| N2 | §3.2、i02 | MAP の OPENING の期限 30 s は rfcomm 自身の上限（PN 20 s + SABM 60 s ＝ 80 s）より短く、MAP の Connect は利用者の答えに 60 s 待つ（map.c:407）のに DLC の段では 30 s で諦める。30 s で `map_fail` → 後から来た opened は FAILED の MAP が `map_close_later`（map.c:435）で閉じる（許可された DLC を捨てる）。30 s 後のやり直しの `dlc_open` は前の DLC が rfcomm に残っていて EEXIST（rfcomm.c:252-255）→ また失敗。前の試みの遅い closed は `dlci >> 1 == mas_channel` で**新しい試み**の refused として扱われる | 期限は rfcomm の上限より長く（90 s 以上）するか、N1 の後は持たない（rfcomm が必ず opened か closed を出す）。前の DLC が rfcomm に残る間は次の `dlc_open` をしない（mux の行が残っていれば待つ）。bt-map に「35 s で opened → READY に進む」「やり直しで EEXIST にならない」 |
| N3 | §5.3、i04 | 終わりは「Body の件数 < N」だけなので、電話帳の件数（0.vcf を含む）が N の倍数の時は `ListStartOffset ＝ PhonebookSize` の Get が 1 回要る。その時の PSE の答えは仕様で確かめていない（**推測**: 空の Body か 0xC4・0xC0・0xD0 の機種がある）。error なら contacts の同期は毎回失敗し目印も消しも進まない（約 1/32 の電話帳で常に）。calls も同じ（SIZE を取らない） | contacts は o ≥ 始めの PhonebookSize で Get をせずに終わる（終わりの SIZE の取り直しは今のまま）。calls も object ごとに SIZE（MaxListCount 0）を取り同じに。または o ≥ size の Get の error を終わりとして扱う。bt-pbap に「32 の倍数の電話帳」「offset＝size に 0xC4 を返す PSE」 |
| N4 | §5.3・§7.2、i04 | `btd_vcard_next`（vcard.c:239-248）は END:VCARD の無い card で Body の残りを全部飲み、`*at = length` の EINVAL を返す。§5.3 は「次の o ＝ o + Body の件数」「件数 < N で終わり」なので、壊れた card 1 枚で page の件数が減り、**電話帳の終わりと取り違える**。その後ろの連絡先は missing になり、同じ壊れ方が続けば 2 回目で消える（M2 の条件 1〜4 は error と見ないので通る）。next の EINVAL が skipped か error かも書かれていない | page の件数は Body の頭の段の `BEGIN:VCARD` の行の数で数え、next の EINVAL があった page は「信用できない」（`capped` の bit 4 か error）として終わりの判定にも消しにも使わない。bt-pbap に「END の無い card の後の page」、phone-store の prune に bit 4 |
| N5 | §5.2・§6.2・§9.1、i04・i05 | §5.2 の `PHONE CALL-LOG` の行に `length=` が無いが、§6.2 は「今の `PHONE MESSAGE` の読みと同じ口」で、その口は `length=` が無いと EPROTO で接続を壊す（phone-zedbsd.c:1399-1404）。§9.1 は「CONTACT・CALL-LOG の読み（本文付き）」で、どちらとも矛盾 | CALL-LOG も `length=0` を付けるか、CALL-LOG は別の読み（本文無し）と決めて §5.2・§6.2・§9.1 を合わせる。名前は行の最後の欄で phone の文字なので、`length=` を付けるなら名前の前に置く |
| N6 | §6.1・§6.3、i05 | `link` は compositor の「失わない」event で、送れない時は owed に積み、古い LINK は新しい物に置き換える（phone-shell.c:1530-1580 `phone_owe`、1640-1655 `phone_emit_owed`）。§6.3 の `link_contacts` を「`link` の前に」直に `kwl_emit` すると ENOBUFS で失われ、owed の古い `link` の後に新しい contacts が先に届く。libkeiland は「次の `link` で入れる」ので古い link に新しい contacts が付く | `link_contacts` は owed の LINK の中で送る: `phone_link_event` に contacts・contacts_why を足し、`phone_emit_owed` の LINK で版が新しい object には `link_contacts` → `link` の順に出す（後ろが ENOBUFS なら LINK ごと owed に残り、次に両方を出し直す）。phone-shell-host-test に ENOBUFS の場面 |
| N7 | §7.3、i06 | 「手元の連絡先（今の `ph_store_find_number`）にあれば NULL」とあるが、`ph_store_find_number`（store.c:299-311）は `ph_store_conversation` を通して**番号の会話の行（`conversation` 1）も返す**（store.c:597-602）。会話のある番号はどれも「手元にある」になり、電話帳の名前はこの Phase の主目的の番号の会話に一度も付かない | 「手元の連絡先」は `conversation == 0` の行だけ（新しい関数か引数）。phone-store の試験は番号の会話の行がある fixture で「名前が電話帳の物になる」を確かめる |
| N8 | §7.1、i06 | 番号の会話も `STORE_CONTACTS_MAX` 1024（store.c:57、1275-1276）の中。§7.1 で通話の履歴は番号ごとに会話を作るので、月を重ねた着信（迷惑電話を含む）で 1024 に届き得る。届くと `ph_store_conversation` は folder を mkdir した後（store.c:622）に -1 を返し、次の起動で `store_load_conversations` が空の folder で ENOSPC になり break（store.c:1021-1025）→ `ph_store_open` が失敗（store.c:160-162）し、app は store を開けない。§7 は上限の時の振る舞いを書いていない | 上限の時: 新しい番号の通話は会話を作らず数える（同期は失敗にしない、目印は進める）、mkdir は追加の成功の後、開く時は上限を越えた folder を読み飛ばす（失敗にしない）。または上限を上げる。どれかを §7.1 に書き、phone-store に「1024 の時の通話」「開き直し」 |
| N9 | §8.2（Pc3 (b)）、i06・i07 | 「link の変化で Stop・記録が消えた・別のスマホ」を見たら `phonebook/bt-<address>/` を消すが、`kl_phone_link` に「記録が無い」の欄は無く、backend は bluetoothd が落ちている時（`unreachable`）・持ち主でない時（`not-owner`、phone-zedbsd.c:1559-1569）・SHOW の no-record（1307-1314）に address を空・enabled を古い値や 0 にし、compositor はそのまま写す（phone-shell.c:1713-1728、backend の設定が none・loopback の時も）。bluetoothd の再起動・Bluetooth の off・設定の切り替えで 5000 件を消して 157 回の Get で取り直す恐れ | 消すのは明示の場合だけに絞り、判定を純粋な関数にする: Stop は `owner` 1・`why` が空か `link` の言葉・同じ address で `enabled` 0 を見た時、別のスマホは `owner` 1 で別の address を見た時、FORGET は no-record を bluetoothd が届く（reachable）状態で見た時。`unreachable`・`not-owner`・`no-backend`・backend≠bluetooth では消さない。phone-store か app の試験に一時的な状態の列 |

## minor

1. §8.1・§13 の downgrade: phonerec.c:579-581 は知らない key を読み飛ばすので、古い bluetoothd は記録を拒まない。実際は「古い bluetoothd は `asked` を捨てて書き直す（btd_phonerec_format、phonerec.c:146）→ 戻すと移行がまた c を外し、利用者が入れた contacts が消える」。記述を直す。`asked` は `PHONEREC_KEYS_ALL` に入れない、移行は有効な記録だけ、profiles に m が無い記録の `asked` の値も決める。
2. §8.2「Stop using as phone は今のまま `link_set(address, 0, <今の profiles>)`」: 今の code は on・off とも `KL_PHONE_PROFILE_MESSAGES` を渡す（page-bluetooth.c:844）。off で contacts の bit が消える。off は `phone_link.profiles` を渡すと書く。
3. §5.5 の表: 持ち主の変化で待つ client を閉じる所（main.c:3405-3418 の `btd_map_cancel`）と client の枠の初期化（main.c:1062）が無い。PBAP の answer の hook も DONE で `waits_phone` を 0 に（main.c:3810-3813 と同じ）。
4. mux の「server channel → 子」の表: MNS の accept の記憶と自分の DLC の待ちが同じ key（server channel）で、スマホの PSE・MAS の channel が 16 だと衝突する。key は (channel, ours)。accept は 1 つの DLC に PN と SABM で 2 回呼ばれ（rfcomm.c の `rfcomm_server_offered`）、WAITING・NEGOTIATING で `btd_rfcomm_close` された自分の DLC は closed を出さない（rfcomm.c:371-377、reason 0）ので、行の寿命（同じ channel の新しい `dlc_open` で置き換え、`ended` で空）を書く。
5. cursor の世代が起動ごとに同じ値から始まると、bluetoothd の再起動を跨いだ cursor が通る。MAP の `first_session` と同じく乱数で始める（main.c:527）。
6. zone: vcard.h の `BTD_VCARD_ZONE_NONE 0・PHONE 1・LOCAL 2` と keiland の 0 phone・2 local・3 received は値が違う。phoneio は言葉（`phone|local|none`）で写すと書き、bt-phoneio で確かめる。`partial` は bluetoothd が `partial=1` を出すか backend が none から立てるかを決める。
7. backend の `phone_state_line` の not-owner の分（phone-zedbsd.c:1559-1569）と SHOW の no-record（1307-1314）で `contacts`・`contacts_why` も 0 と空に。
8. 名前の索引は `ph_number_key`（`store_country` を使う、store.c:552-560）で作るので、`ph_store_set_country` の後に作り直す。
9. 非通知の `w`: `ph_store_conversation` は空の番号で作れない（`ph_number_key` が EINVAL）ので別の関数が要る。`store_folder_key` に `w` を足す所も書く。
10. §5.3 calls の「読めない件は出す（zone none）」と「skipped は読めなかった件」が混ざる。card が読めない（EINVAL・E2BIG・ENOENT）は skipped、datetime だけ読めないのは zone none、と分ける。
11. IDLE からの PAGE は SDP（EBUSY のやり直し）＋ PN 20 s ＋ SABM 60 s ＋ Connect 60 s で backend の 120 s（phone-zedbsd.c:61）を越え得る → ETIMEDOUT で接続を作り直し、bluetoothd は client の close で取り消す。PBAP の接続の全体に上限（例 100 s）を置くか、app のやり直しで足りると記録する。
12. M2 の条件 3 で、利用者が半分より多くを消した時は写しが残り続ける。消えた連絡先の名前も番号の会話に残る（`store_apply_phone_names` は「無ければ今のまま」）。§13 と UAT に書く。
13. M10・N1 の経路は偽の hook の bt-phonemux だけでなく、本物の rfcomm.c・phone.c を通す bt-phone-link-host-test にも 1 場面（PN の DM、SABM の DM、SABM の期限）を入れる。
14. ws.md の p005 の行は 8 LW・planned のまま（§14 は 10.5 LW）。Q1 に直してもらう。
15. （推測）Android はペアの時の SDP で PCE の有無を見て連絡先の共有を出し分けるかもしれない。§8.1 で handoff は messages だけなので、ペアの時に PCE の record は無い。§13 の UAT の「PCE の record」の行に「ペアの時に無くても後で許可を聞くか」を足す。

## 判定

| i | 判定 |
| --- | --- |
| i01 vcard.c | 実装済み、GO のまま（N4 は pbap 側の扱い。next の EINVAL の時に Body の頭の段の BEGIN を数える補助を vcard.c に置いてもよい） |
| i02 phonemux・map の穴・PCE の record・記録 | 直してから（N1 の rfcomm、N2、minor 1・4・13） |
| i03 pbap の始め | 直してから（N1 の permission の reason の絞り。N1 の rfcomm の直しが i02 に入れば条件付き GO） |
| i04 page と socket | 直してから（N3・N4・N5、minor 3・5・10・11） |
| i05 中継 | 直してから（N5・N6、minor 6・7） |
| i06 Phone の app | 直してから（N7・N8・N9、minor 8・9・12、Pc1・Pc3・Pc6 の答え） |
| i07 Settings | 直してから（N9、minor 2、Pc2・Pc4 の答え） |
