# ws197-p003 詳細設計 第 1 版の敵対的レビュー（review-1）

対象: [phase.md](phase.md)（branch `agent/p1-ws197`、commit d21a55cae）
日付: 2026-10-10（design-reviewer）
照合した物: p001 §3〜§8・§11・§12、p002 phase.md（cleared）、`userland/base/bluetoothd/` の phone.c・phone.h・obex.c・obex.h・rfcomm.h・sdps.c/h・sdp.h・router.c/h・linkmgr.h・hid.c・pair.c・main.c・keys.c・hidcache.c・protocol.h、`userland/desktop/libkeiland-backend-zedbsd/bluetooth-zedbsd.c`、MAP 1.4.2 の本文（§3.1.1・§3.1.3・§3.1.6・§3.1.7・§3.1.10・§4.1〜§4.5・§5.1〜§5.8・§6.3・§6.4・§7.1）、Core 5.4（Vol 4 Part E 7.1.8・7.1.9・7.7.6、Vol 3 Part B 5.1.8）。
code と設計文書は変えていない。行番号は上の commit の物。

## 0. 結論

- **blocker 3・major 12・minor 22**。
- 仕様の値（App Parameters の tag、ParameterMask、FilterMessageType、SupportedMessageTypes、opcode、LENGTH の規則、datetime の試験の正解）は確かめた範囲で全部正しい（§5 に一覧）。問題は値ではなく、**持ち主の規則の抜け道、client の枠の使い直しでの漏洩、page と受けの交差の誤った前提**と、MAP の失敗・回復の穴に集中している。
- i ごとの判定は §4。i04（mapxml）と i05（bmsg）は GO。i02 は M11 を直してから。i01・i03・i06・i07 は blocker と major を直して短い再確認の後。

## 1. blocker

### B1. 別の人が `PAIR … phone=1` で持ち主を乗っ取れる（§3.2、§1 の 1、i01）

- 誤り: §3.2 の表は「**別の address** の `.phone` があれば受けない（`why=other-phone`）」だけで、**同じ address に別の uid の `.phone` がある時**を決めていない。書き込みの規則（「書く（uid は PAIR の client …）」）のままなら上書きになる。
- 根拠:
  - `PAIR` は D8 の人（root・seat の人・wheel）なら誰でも出せる（`main.c:1368` の `btd_permitted`）。
  - BR/EDR の pairing は保存の bond があっても進み（`pair.c:283-285`）、phone=1 でも**保存の鍵が認証済み（0x05・0x08）なら使う**（`pair.c:1099-1108`、Negative Reply は Just Works の鍵の時だけ）。スマホは保存の鍵で認証するので、スマホの画面には何も出ない。
  - 持ち主 A が seat に居ない間は phone link は NONE（§3.3）なので、p002 の handoff の「1 台だけ」の検査（`phone.c:209-213`）も通る。
- 起こる状況: A がスマホを「スマホとして使う」にした後、B（seat の人か wheel）が `PAIR <A のスマホ> bredr phone=1` を出す → スマホは近くに居れば保存の鍵で無言で認証 → handoff → `.phone` が B の uid で書かれる → B が A の SMS を読み・送れる。p001 S4 の前提「スマホがもう一度許可を求める」（p001 §8.2）は、保存の鍵を使う今の pair.c では成り立たない。Q2 の決定（持ち主だけが見る、持ち主の変更は持ち主か root だけ）に反する。
- 直し方:
  1. handoff の検査（`.phone` を書く前）に「**同じ address の有効な `.phone` があり、その uid が PAIR の client と違い、client が root でない**」→ 受けない（`why=owned`）を足す。
  2. あわせて、phone=1 の pairing で同じ address に他人の `.phone` がある時は、pair の Link Key Request で保存の鍵を使わない（Negative Reply で SSP をやり直させ、スマホの画面で確認させる）か、PAIR を `ERROR owned` で始めない。どちらにするかを決めて書く。
  3. 試験: 保存の認証済みの鍵がある address に、別の uid の PAIR phone=1 → `why=owned`（記録の uid が変わらない）、root なら変わる。

### B2. 要求の client の token が枠の使い直しで別の uid に化ける（§5.7 の `answer(token…)`・`room(token)`、§8.4・§8.5、i06・i07）

- 誤り: map の queue は 16 個まで、複数の client の PAGE・GET・READ・SEND を持つ（§8.4）。token の寿命（client が閉じた時の扱い）が書かれていない。token が client の枠の番号なら、A の PAGE の途中で A の接続が閉じ、同じ枠に B の接続が入ると、残りの item（A の SMS の本文・番号・名前）が B に書かれる。
- 根拠: 今の main は枠の番号で待ちを持ち、`btd_client_close` が `btd_probe_client` などを個別に消している（`main.c:2024-2060`）。`btd_accept` は空いた枠をすぐ使い直す（`main.c:815-824`）。§4.2 で枠は 16 に増え、使い直しは起きやすくなる。
- 起こる状況: compositor の再起動や app の終了で PAGE の client が消える → 次に接続した別の uid（SSH の利用者でも枠が空けば入れる）が持ち主の SMS を受ける。
- 直し方:
  1. token ＝（枠の番号, 世代）。世代は `btd_accept` ごとに増やす。`answer`・`room` は世代が合わなければ何もしない。
  2. `btd_client_close` から `btd_map_cancel(map, token)`（新）: queue のその token の操作を消し、実行中の操作は OBEX の上では続けて答えを捨てる（PAGE は次の sub-op に進まない）。
  3. 試験（bt-map-host-test と main の配線）: PAGE の途中で client を閉じ同じ枠に別の uid → 何も書かれない、queue から消える。

### B3. page と受けの交差で、Reject の後の自分の Connection Complete を page の失敗と誤る（§5.3 の 142 行、§5.2、i03）

- 誤り: 「state が NONE でない（自分の page と交差）なら Reject Connection Request（0x040A、reason 0x0D）で断る（相手は自分の接続を続け、**こちらの page の Connection Complete が来る**）」。Core は Reject の後に**ローカルの controller も Host に HCI_Connection_Complete を送る**と定める。
- 根拠:
  - Core 5.4 Vol 4 Part E 7.1.9: "When the Controller receives the HCI_Reject_Connection_Request command … the local BR/EDR Controller will send an HCI_Connection_Complete event to its Host"（status は失敗）。
  - router はその事象の address で `btd_linkmgr_connected(…, status)` を呼んで page の token を終え（`router.c:462-464`）、失敗を address の持ち主（PAGING の phone は claims＝1）に渡す（`router.c:475-478`）。
  - §5.2 は「Connection Complete の失敗は NONE と次の段」。
- 起こる状況（suspend の後や圏内に戻った時など、両側が同時に再接続する場面）:
  1. Reject の Connection Complete（失敗）で phone は NONE・30 s の backoff になり、linkmgr の token も外れる。しかし controller の Create Connection はまだ出ている。
  2. token が空いたので HID が Create Connection を出す → Command Disallowed（p002 §6.2 の「調停しない事」の外の、調停の破れ）。
  3. 自分の page がその後に成功すると、phone は NONE で claims＝0 → router は route の無い接続として切る（`router.c:1030-1033`）。
- 同じ所の抜け: PAGING の 15 s の守りの切れで NONE にする時に Create Connection Cancel（0x0408）を出さない。出さないと、後で来る成功の Connection Complete が同じく切られる。今の `phone_disconnect` は READY でしか動かない（`phone.c:1053-1055`）ので、SECURING・ACCEPTING で切る時の経路も無い。
- 直し方（どちらかを選んで書く）:
  - (a) PAGING 中の同じ address の Connection Request は **Accept**（role 0x01）し、Create Connection Cancel（0x0408）を出す。Cancel の Command Complete・Connection Complete（0x02 Unknown Connection / 0x0B Already Exists など）を「交差の後始末」として捨てる。
  - (b) Reject を続けるなら、Reject の直後の、その address の失敗の Connection Complete（status が自分の出した reason 0x0D）を phone も linkmgr も「page の終わり」と見ない（router で、phone が Reject した address と reason を覚えて 1 回だけ吸う）。
  - 15 s の守りの切れには 0x0408。`phone_disconnect` を SECURING・ACCEPTING でも動かす。
  - 試験: 偽の controller は Reject の後に**ローカルの Connection Complete（status 0x0D）を必ず出す**（Core 7.1.9 を正解に）。その後に page の成功が来ても link が残る。HID の page が Command Disallowed にならない。
- 注: HID の `hid_request` も同じ形（`hid.c:1259-1266`）で同じ潜在の誤りを持つ（WS143 の範囲。Bug にするかは Q1 に）。

## 2. major

### M1. Link Key Request を READY の間は断る（§5.3 の 141 行、i03）

- 誤り: `btd_phone_wants` は「NONE・PAGING・ACCEPTING・SECURING」の時だけ 1。router は Connection Request と Link Key Request の両方でこれを使う（`router.c:696-704`）ので、READY・CLOSING の link でスマホが認証をやり直すと Negative Reply になる。
- 根拠: HID の wants は IDLE 以外の全部で 1（`hid.c:974-976`、注釈「its key is asked during its own connection」）。認証済みの link でも、相手の LMP の認証の開始でローカルの controller は Link Key Request を出す。
- 起こる状況: スマホがサービスの接続のたびに認証を求める機種で、READY の後の認証が失敗 → スマホが link を切る → 再接続 → 繰り返し（M6 と重なる）。
- 直し方: wants は「在、address が記録の物、state が NONE 以外」で 1。Connection Request の扱いは state ごと（NONE 以外は断る）なので、READY を足しても受けは増えない。試験に READY の Link Key Request を足す。

### M2. handoff で記録を書く口と記録の読み込みの API が無い。§3.2 と §5.4 が矛盾（i01・i03）

- 誤り:
  - §3.2「書けなければ受けない（`why=store`）」に対し、§5.4 の 151 行は「handoff の後に記録を書き」。受けた後（route と L2CAP を移した後、`phone.c:223-252`）では断れない。
  - phone.c は system call を持たない（§2）ので、handoff の hook の中で `.phone` を書く hook（main へ）が要るが、定義が無い。
  - phone.c が記録（address・enabled・profile・有効か）を知る口は `btd_phone_set_present` だけで、記録の読み込み・入れ替え（起動、controller の open（folder は controller の address ごと、`btd_open`）、`PHONE LINK`、FORGET、handoff）が無い。
- 直し方:
  - `struct btd_phone_hooks` に `int (*store)(void *, const uint8_t *address, uid_t uid, const char **why)` を足し、`btd_phone_handoff` の検査（B1 の `owned` を含む）の後、`btd_router_assign` の前に呼ぶ。失敗なら `why=store` で 0。
  - `btd_phone_set_record(phone, const struct btd_phonerec *record_or_NULL)`（main が controller の open・LINK・FORGET・handoff の後に呼ぶ）を足す。§5.2〜§5.4 の条件はこの記録だけで決める。
  - Link Key Request で phone.c が `btd_keys_read`（`keys.c`、system call を持つ）を呼ぶなら（§5.3 の 143 行「hid_key_request と同じ」）、§2 の「system call を持つのは phonerec.c と main.c だけ」を直し、phone に keys の folder を渡すと書く。

### M3. 消せない `.phone` が他のスマホを永久に塞ぐ（§3.1・§3.2、i01）

- 誤り:
  - 無効な記録は「file ごと無効（log、消さない）」・「file は残す」・「直すのは FORGET と再 pairing」。
  - しかし今の FORGET は bond の file が無いと `ERROR not-bonded` で終わり（`main.c:1521-1525`）、`.phone` に触れない。
  - 持ち主の account が消えた記録の FORGET は「持ち主と root だけ」で、持ち主は居ない。
  - §3.2 の「別の address の `.phone` があれば受けない」は、無効な記録でも新しいスマホを断る。
- 起こる状況: bond だけが消えた（手での削除、keys の読みの失敗）・持ち主の account が消えた記録が 1 つ残ると、root が手で file を消すまで誰もスマホを登録できない。
- 直し方:
  - FORGET は `.phone` を bond より先に消し、bond が無くても `.phone` があれば消して DONE。
  - 無効な記録（bond 無し、account 無し、名前違い）の FORGET は D8 の人に許す。
  - `other-phone` の判定は有効な記録だけで行う。
  - hidcache の孤児の掃除（`hidcache.c:508-555`）と同じ掃除を `.phone` にも入れるかを決める。

### M4. 持ち主は「seat の人」という前提を PAIR が守らない（§3.2 の 81 行、p001 §8.2）

- 誤り: 「uid は PAIR の client」。PAIR は wheel・root なら SSH からも出せる（`main.c:1368`）。
- 起こる状況:
  - root が pairing すると持ち主は uid 0 になり、seat に root が座らないので永久に「不在」になる（`PHONE SHOW present=0`、page しない）。
  - wheel の別人が SSH から pairing すると、その人が持ち主になる。
- 直し方: `phone=1` は client の uid が seat の人の時だけ受ける（他は `ERROR phone-seat`）。root が代わりに登録する経路が要るなら `owner=UID` を足し、その uid が seat の人であることを確かめる、と書く。

### M5. 相手から来た接続の認証の順（SECURING）が衝突に弱い（§5.4 の 147 行、i03）

- 誤り: 受けた接続でも Connection Complete の直後に zedBSD から Authentication Requested を出す。やり直すのは status 0x23・0x2A の時だけ。他の失敗（Command Status の 0x0C、Authentication Complete の 0x0C・0x1A・0x05 など）の扱いが無く、§5.4 の 150 行の「失敗 → 切る」に落ちる。
- 根拠:
  - スマホは自分から接続した時、すぐ自分で認証と暗号化を始めるのが普通（推測、実機は p008）。
  - Authentication Complete は開始した側だけに出る（Core 5.4 Vol 4 Part E 7.7.6 の Note）。スマホが先に暗号化まで済ませれば、こちらの要求は衝突か Command Disallowed で失敗しうる。
  - HID は受けた接続では自分から認証を始めない（`hid.c:1319-1321`）。
  - SECURING の中の段（認証待ち・暗号化待ち・鍵の長さ）と、Encryption Change が Authentication Complete より先に来た時の順が決まっていない。
- 起こる状況: 正しく暗号化された受けの link を、こちらの要求の失敗で切る → 再接続 → 同じ（M6 と重なる）。
- 直し方:
  - 受けた接続は、Encryption Change（on）を 2〜3 s 待ってから自分で認証を始める。page した接続は今の案のまま。
  - Encryption Change（成功・on）は、どの段でも鍵の長さの確かめへ進める（暗号化には Link Key Request への自分の Reply、つまり認証済みの bond の鍵が使われている）。
  - 暗号化が on になった後の Authentication Complete の失敗は捨てる。
  - SECURING の中の段を表にする。試験: 受けの接続でスマホが先に暗号化 → 自分の Authentication Requested が 0x0C → READY になる。

### M6. backoff の戻しと即時の再 page で、再接続の嵐とスマホ側の「切断」の無効化が起きる（§5.2 の 135 行、§5.5 の 155 行、i03）

- 誤り: 「link が READY になったら戻す」。「切断 … NONE に戻して次の page を予定」の次は「最初はすぐ」。
- 起こる状況:
  1. スマホの利用者がスマホの Bluetooth の設定で「切断」を押す（reason 0x13 remote user terminated）→ zedBSD がすぐ page し直して戻ってくる。スマホからは切れない。
  2. MAP が許可なしで FAILED（§8.1 で同じ link では繰り返さない）→ profile の無い ACL をスマホが数秒〜数十秒で切る（Android の idle の切断、推測）→ すぐ page → READY で backoff が戻る → 繰り返す。その間 linkmgr の token も取り続けて HID の page を遅らせる。
- 直し方:
  - backoff を戻すのは、link が一定の時間（例 2 分）続いたか、MAP が ready になった時だけにする。
  - 相手が切った時（reason 0x13・0x15）は自動の page を止め、次の合図（不在 → 在、sleep.end、`PHONE LINK on`）まで待つ。スマホからの接続はいつでも受ける。
  - supervision timeout（0x08）だけを「離れた」とみて backoff の page にする。
  - 試験: 0x13 の切断の後に page しない。READY の直後に切れる link では間隔が伸びる。

### M7. MAP の失敗からの回復が無く、MNS だけの喪失で通知が黙って止まる（§8.1 の 244 行、§8.6 の 295 行、§8.7、i06）

- 誤り:
  - 「同じ link の間は繰り返さない」。timeout・DLC の切断・0xD3 のような一時の失敗も、許可の拒否と同じに扱っている。
  - OBEX の答えの時間は固定の 10 s（`obex.h:30` `BTD_OBEX_TIMEOUT_MS`）。スマホが許可の確認の画面を出している間、Connect の答えを待たせる機種では 10 s で切れる（推測、p008）。
  - MNS の DLC だけが閉じた時（MAS は生きている）の扱いが無い。MAP §4.1: "If the MNS connection is terminated by either device, then the message notification mode shall be set to default mode on MSE (i.e. the notification is inactive)"。
- 起こる状況:
  - 利用者がスマホで「許可」を押しても、zedBSD は link を切ってつなぎ直すまで MAP を始めない。
  - MNS が切れると、`messages=ready notify=1` のまま新しい SMS の event が来なくなる。
- 直し方:
  - MAS の Connect だけ答えの時間を長く（30〜60 s）。obex.c の op ごとの timeout を引数にする（`btd_obex_connect` の変更、i06 の範囲）。
  - 一時の失敗は間隔を伸ばしてやり直す（30 s → 10 分、許可の拒否 0xC1・0xC3 も 10 分ごとに 1 回）。
  - MNS の DLC が閉じたら NotificationRegistration（on）を 1 回出し直し、だめなら STATE `notify=0`。
  - 試験の台本に「Connect の答えが 20 s 後」「MNS の DLC だけ閉じる」を足す。

### M8. live の NewMessage の GET で既読になり、戻さない（§8.6 の 288 行、i06）

- 誤り: PAGE は「listing で read=no の entry は GET の後に UNREAD」（§8.5 の 279 行）だが、live の NewMessage は LOCATE → GET → `PHONE MESSAGE` だけで UNREAD が無い。
- 根拠: p001 §7.1.0（S6・S23）の目的は、zedBSD の取得でスマホの未読を消さない事。届いた直後の SMS が一番影響を受ける（スマホの通知と未読の印が、laptop が取った瞬間に消える）。
- 直し方: LOCATE の `read=no` なら GET の後に UNREAD。LOCATE で見つからない INBOX の NewMessage は未読とみて UNREAD（害は無い、§13 の表の理由と同じ）。queue の 16 の数え方に UNREAD を含める。

### M9. listing の無い item（LOCATE の失敗、`PHONE GET`）で key・time・folder が決まらない（§8.3 の 253 行、§8.6、§9.2・§9.3）

- 誤り:
  - key は「スマホの datetime の文字列」を含む。event 1.0 には datetime が無く（MAP 表 3.1、datetime は 1.1 以降）、bMessage 1.0 にも日時の property が無い（§3.1.3.1 の BNF）。
  - LOCATE で見つからない時（`zone=received`）と `PHONE GET`（handle だけ）には datetime が無く、key が定義できない。
  - `PHONE GET` の item の folder・dir・read・time の出どころも書かれていない。1.1 では handle で listing を引けない（FilterMessageHandle は 1.4 の物、§5.5.4.17）。
- 起こる状況: 後の PAGE で同じ SMS が別の key で来て、app に重複が残る（p004 の重ね合わせが効かない）。
- 直し方:
  - LOCATE を MaxListCount 8 → 32 にし、見つからなければ 1 回だけ待って再試行する。
  - それでも無い item は `key=` を出さず `partial=1` を付け、app は peer・本文・時刻の幅で重ねる、と §9.6 に書く。
  - `PHONE GET` は、live の本文が 16 KB で切れた時にも同じ 16 KB しか返さず意味が薄い。p003 から外すか、返す欄（`time=0 zone=none`、folder は bMessage の FOLDER）を決める。

### M10. ListingSize の出どころが仕様と合わない（§8.4 の COUNT の行、§8.5 の 278・280 行、i06）

- 誤り:
  - COUNT は最初の folder（inbox）に 1 回だけで、filter（FilterMessageType・FilterPeriodBegin）を付けない。
  - しかし次の page の判定 `o + k < min(ListingSize, 500)` は各 folder の ListingSize を要る。sent の ListingSize の出どころが無い。
- 根拠:
  - MAP §5.5.4.13: ListingSize は「filter を満たす件数」。MaxListCount=0 でも ListStartOffset・SubjectLength・ParameterMask だけを無視し、filter は効く。filter の無い COUNT は 30 日の外を含む件数になる。
  - 1.4.2 の表 5.11 は成功の答えの全部に ListingSize・MSETime を必須（C.3）とするが、1.1 時代の MSE が MaxListCount ≠ 0 の答えに ListingSize を付けるかは未確認（推測）。
- 直し方:
  - COUNT を folder ごとに、LIST と同じ filter で出す（MSETime はその答えから）。
  - ListingSize が無い答えでは `k < n` だけで folder を進め、500 は offset で切る、と書く。
  - 試験: ListingSize の無い LIST の台本。

### M11. `PHONE SEND` の長さ付きの入力で、エラーの時の N byte が要求の行として読まれる（§4.3 の 114〜115 行、§9.2 の 335 行、i02・i07）

- 誤り: N byte を読み捨てずに閉じるのは「N が上限を越える・0」の時だけ。`not-ready`・`no-send`・`permission`・`number` の ERROR、待ちの間（`waits_*`）に来た SEND の行の扱いが無い。
- 根拠: 今の `btd_read` は受けた buffer を改行ごとに `btd_line` に渡す（`main.c:858-871`）。待ちの間の行は捨てられるが（`main.c:903-907`）、後に続く本文の byte は次の行として解釈される。SEND の行と同じ `recv` で本文の先頭が buffer に入っている場合（同じ loop の中の残り）の移しも書かれていない。
- 起こる状況: 本文に改行と `FORGET …`・`PHONE LINK … off` のような行を含む SMS を、ready でない時に送ると、その行が同じ uid の要求として実行される。uid は同じなので権限は上がらないが、流れの区切りが崩れ、意図しない操作が走る。
- 直し方:
  - 文法として正しい `length=N`（1..8192）を読んだら、どの ERROR でも（待ちの間でも）まず N byte を読み捨ててから答える。
  - 行の loop は SEND の行で抜け、buffer の残りを本文の buffer へ移す、と書く。
  - 試験: not-ready の SEND で、本文の中の `SHOW` が実行されない。

### M12. HID の上限 5 の扱いが p002 の code と合わない（§3.4、i01・i03）

- 誤り: §3.4 は「記録が有効で enabled の間（在・不在に関わらず）5」。しかし p002 の `phone_ended` は link の終わりに 6 に戻す（`phone.c:1103`）。p003 でこの行を変える事が書かれていない。
- もう一つ: 記録が有効になった時（handoff、`PHONE LINK on`）に HID が既に 6 台つないでいると、`btd_hid_set_limit` は新しい接続を止めるだけ（`hid.c:573-581`、`hid_full` 3361-3373）。phone の page・受けで session の link が 8 本を越えうる（p001 §3.3 の「pairing 1 ＋ 断るための 1 ＋ HID 6」の枠を食う）。
- 直し方:
  - 上限は記録の状態（M2 の `set_record`）だけで決め、`phone_ended` では変えない。
  - page と Accept は `btd_hid_link_count ≤ 5` の時だけ。他は `why=busy-links` で待つ（間隔の段は進めない）。

## 3. minor

| # | 節 | 何が誤りか・抜けか | 根拠 | 直し方 |
| --- | --- | --- | --- | --- |
| m1 | §6.1 の 176 行 | datetime の文字列 20 byte では `YYYYMMDDTHHMMSS±hhmm`（20 文字）と NUL が入らない | MAP §3.1.10 | 21 byte 以上 |
| m2 | §6.2 | OBEX の time の `Z`（UTC）の末尾を受けない。読めない datetime の entry をどうするか（捨てるか `zone=received`）が無い | MAP §3.1.6.1 は OBEX の time の形を引く | `Z` を +0000 として受け、失敗の扱いを書く |
| m3 | §9.1 の 317 行 | 「1 行は 2048 byte 以下」では compositor が切る。読み手は 2047 byte の入れ物で、満ちたら閉じる | `bluetooth-zedbsd.c:732,760` | 改行を含めて 2047 byte 以下（中身 2046） |
| m4 | §4.1 | 今の `btd_write` の書式の buffer は `BTD_LINE_MAX + 4×BTD_NAME_MAX`（1508 byte）で、それより長い行を切る | `main.c:1969` | phoneio の行は `btd_outq_append` で直に足すと書く |
| m5 | §8.4 の LIST | body の上限 64 KB は、32 件で名前・番号が長く実体の escape が多いと越えうる。越えると EMSGSIZE で Abort し、毎回同じ offset で失敗して同期が進まない | §6.1 の値は 1024 byte まで | EMSGSIZE なら count を半分にして同じ offset をやり直す |
| m6 | §8.5 | cursor が `since` を含まない。違う since と古い cursor の組で offset がずれる | — | cursor に since を入れるか照合する |
| m7 | §8.6 | PushMessage の答え（Name＝handle）より先に MNS の SendingSuccess などが来ると「知らない handle は捨てる」で落ちる。MessageShift と SendingSuccess で `PHONE SENT state=sent` が 2 回出る | MAP §6.3.2（Put の headers は最後の packet）、MNS は別の DLC | PUSH の間は知らない handle の Sending・Delivery の event を数秒・8 個まで保留。同じ state は 1 回だけ |
| m8 | §8.3 | key の番号の正規化（空白と `-` を除く）では `(`・`)`・`.` が残る。listing の canonical（`+1-987-…`）と vCard の TEL の形が違うと key が変わる | MAP §3.1.6.1 の sender_addressing | `[0-9+*#]` 以外を除く。どの出どころの番号で key を作るかを固定する |
| m9 | §8.7 | (1) RFCOMM の server の登録 `btd_rfcomm_listen(rf, 16)` は session ごとに要る（`btd_rfcomm_init` が空にする、`rfcomm.h:145-171`）。どちらが session を開けても呼ぶ、と書く。(2) MNS の record の登録を MAP の始め（STEP 1）でなく、記録の messages が有効な間にするかを決める（pairing 直後に SDP を引く機種がある、推測）。(3) ServiceName を持つ record は LanguageBaseAttributeIDList（0x0006）を「should」で持つ | Core 5.4 Vol 3 Part B 5.1.8 | 3 つを書く |
| m10 | §10.1 | 対称の誤りを捕まえない所: (1) MNS の record を自前の `sdp.c` で読み戻すだけでは、型（channel を uint8 で書いているか）や属性 ID の誤りが両側で一致して通る。(2) bt-map-host-test の期待値が map.h の macro を使うと、tag の誤りが通る。(3) 偽の controller が Reject の後のローカルの Connection Complete（B3）や Accept の Command Status を出さなければ、交差の試験にならない | — | (1) record の期待値を手で書いた byte 列にする。(2) 試験の tag・値は表 6.6 から手で書いた定数にする（ParameterMask は `00 00 11 7E`、FilterMessageType は `0C`）。(3) Core 7.1.8・7.1.9 の事象の順を台本に入れる |
| m11 | §6.1 | 名前に `:` を許しつつ「namespace の扱いは失敗」は曖昧 | — | `:` を含む名前は無視（知らない属性）か失敗かを決める |
| m12 | §7.1 | recipient の vCard を「最も内の BENV」から取るが、MAP の図 3.1 では外側が最終の受け手、内側が最初の受け手 | MAP §3.1.3 の図 3.1 | SMS は 1 段で同じ。外側を使うと書く |
| m13 | §3.2・§3.3 | 同じ持ち主の再 pairing で profile が全部 1 に戻る。`user` の文字の制限（`[A-Za-z0-9._-]`）に合わない名前の扱いが無い | — | 既存の記録の enabled・profile を保つ。合わない名前は `why=store` で受けない |
| m14 | §3.2 | FORGET の順（bond と `.phone` のどちらを先に消すか）と、片方だけ失敗した時の扱いが無い | M3 | `.phone` を先に。bond の失敗は今の ERROR |
| m15 | §5.2 | page の条件に「POWER off」が無い。HID は `btd_powered_off` で止める | `main.c:2336-2339` | page と page scan の want を POWER off で 0 に |
| m16 | §8.4 | PAGE が queue を占める間、live の LOCATE・GET を間に挟むかが無い。room の待ちから起こす経路（outq が空いた後の map の pump）も無い | — | PAGE の sub-op の間に live の op を先に入れる。outq の flush の後に `btd_map_pump` |
| m17 | §4.3 | `ERROR length` の後にすぐ閉じると、outq の中の ERROR が届かない | — | flush してから閉じる（届かなくてよいなら、そう書く） |
| m18 | §4.1 | client を閉じた時の outq の free と、`dead` の時の扱い | — | `btd_client_close` で free と書く |
| m19 | §0 | 0x0001・0x0004・0x0005・0x0009・0x0100 は Core の値で、今ある `core.txt` で確かめられる。Assigned Numbers を待つのは 0x0315〜0x0317 と UUID だけ | Core 5.4 Vol 3 Part B 5.1 | 確かめて §0 を直す |
| m20 | §8.4 | FilterPeriodBegin の文字列に NUL を付けるかが無い。MSETime の読みが末尾の NUL を受けるかも無い | MAP 表 6.6（"String"） | 付けない形で送り、読みは末尾の NUL を許す |
| m21 | §5.5・§9 | p001 から変えた事が「変更」として記録されていない: suspend（p001 §8.2.1 は「入る時に閉じる」、p003 は「何もしない」）、本文の上限（64 KB → 16 KB、送信 8192）、`after=` → `cursor=`、event の `source` → `handle`・`key`。`PHONE PROBE` を消すので、p001 §12 と p002 §14 の「p008 で PROBE」も変わる | p001 §8.2.1・§8.3、p002 §14 | p003 に「p001 からの変更」の表を置き、p004・p008 の行を Q1 に頼む |
| m22 | §8.1 の 3・§13 | MAP 1.4.2 の MSE は MapSupportedFeatures の bit 19 を 1 にする「shall」（表 7.1 の注 2）。表 6.9 の C.1 は、その時 Connect の MapSupportedFeatures を必須とする。1.1 の MCE は後方互換の注で救われる見込みだが、厳しい MSE が Connect を断る危険がある（推測） | MAP §6.4.1 表 6.9、§7.1.1 | §13 の危険の表に足し、p008 で見る。Q16 (a) の変更が要るならユーザーの判断 |

その他の短い指摘:

- §9.4: 「空いた時に先に `PHONE DROPPED`」の「空いた」の閾値が無い。STATE が落ちた後に app が SHOW をやり直す事も §9.6 に書く。
- §8.6: MNS の Put に MASInstanceID が無い時の扱い（受けて捨てるか、0xC0 か）が無い。
- §5.7: `room(token)` が 32 KB 未満の待ちは loop の毎回の poll を 0 にしない事（`btd_timeout` に入れない）を書く。

## 4. i ごとの判定

| i | 判定 | 理由・条件 |
| --- | --- | --- |
| i01 phonerec・handoff の記録・LINK/SHOW・FORGET・HID の上限 | **直してから** | B1（持ち主の乗っ取り）、M2（記録の口と順）、M3（消せない記録）、M4（seat の人）、M12。直した後に短い再確認 |
| i02 outq・枠 16・長さ付きの入力 | **直してから（小）** | M11 を §4.3 に書けば GO。B2 の世代の番号は枠の構造体の仕事なので、ここで入れる |
| i03 link の一生 | **直してから** | B3（交差・Cancel・`phone_disconnect` の段）、M1、M5、M6、M12。短い再確認 |
| i04 mapxml と datetime | **GO** | m1・m2・m11 は実装の時に直してよい。試験の正解（1197518710、閏日と 60 秒）は python で確かめた |
| i05 bmsg | **GO** | LENGTH の形 1、`/END:MSG`、3 段、vCard 2.1 の VERSION と N は仕様どおり。m12 は実装の時に |
| i06 obex の response の hook・map.c・MNS の record | **直してから** | B2（cancel）、M7、M8、M9、M10。m5・m7・m9・m10 も設計に書く |
| i07 phoneio・main の PHONE と SUBSCRIBE | **直してから** | B2（token の寿命）、M11、m3・m4 |
| i08 T1 の依頼・記録 | i01〜i07 の後 | §10.2 の内容は妥当 |

## 5. 確かめて正しかった値（直さなくてよい）

- App Parameters の tag（MAP 表 6.6）: MaxListCount 0x01（2 byte）、ListStartOffset 0x02、FilterMessageType 0x03、FilterPeriodBegin 0x04、Attachment 0x0A、NotificationStatus 0x0E、MASInstanceID 0x0F、ParameterMask 0x10（4 byte）、ListingSize 0x12、Charset 0x14（1 = UTF-8）、StatusIndicator 0x17（0 = readStatus）、StatusValue 0x18（1 = yes）、MSETime 0x19。値は big-endian。
- ParameterMask の bit（§5.5.4.4 表 5.12）: 1 datetime、2 sender_name、3 sender_addressing、4 recipient_name、5 recipient_addressing、6 type、8 reception_status、12 read → 0x0000117E（python で確かめた）。
- FilterMessageType: bit 0 SMS_GSM、1 SMS_CDMA、2 EMAIL、3 MMS、4 IM で、1 は「除く」→ 0x0C は EMAIL と MMS を除く。SDP の SupportedMessageTypes は bit 0 EMAIL、1 SMS_GSM、2 SMS_CDMA（表 7.1）で §8.1 と合う。
- MapSupportedFeatures が無い MSE は 0x0000001F（表 7.1 の注 1）、bit 0・1・3 の意味。
- Target の UUID（表 6.5）、SetPath の flags 0x02（表 5.7）、GetMessage・SetMessageStatus・PushMessage の Name は 16 桁の 16 進と NUL（§5.6.2・§5.7.2・§5.8.2）、PushMessage の Name は子の folder で、答えの Name が handle、filler 0x30（§5.2.4・§5.7.5）。
- 何 packet かに分かれた Get の headers は最初の packet（§6.3.2）なので、§8.2 の `response` の hook は要る（今の `done` は最後の headers だけ、`obex.c:1215-1216`）。
- listing は新しい順（§3.1.6、§5.5.4.1）。ただし §3.1.6.1 の例は古い順に並んでいるので、§13 に「順は機種で確かめる」を足すとよい。
- bMessage の LENGTH は最初の `BEGIN:MSG` の `B` から最後の `END:MSG<CRLF>` の CRLF まで、escape で 1 増える、BENV は 3 段まで、PushMessage の FOLDER は空（§3.1.3）。
- HCI: Accept の Role 0x01 ＝ peripheral のまま（Core 7.1.8）、Reject の reason 0x0D〜0x0F、opcode 0x0405・0x040A・0x0411・0x0413・0x1408、status 0x06・0x23・0x2A、Create Connection の packet type 0xCC18 は HID と同じ（`hid.c:2427`）。
- `btd_keys_list` は `bredr.phone` を型として読まない（`keys.c:279-282` の `btd_address_type_parse`）。hidcache の掃除も `.hid` だけを見る（`hidcache.c:520`）。

## 6. 確かめていない事（推測）

- スマホが受けの接続で自分から認証・暗号化を始めるか、許可の画面の間 OBEX の Connect を待たせるか、profile の無い ACL を切るか（M5・M6・M7 の前提）。p008 で見る。
- 1.1 時代の MSE が MaxListCount ≠ 0 の答えに ListingSize・MSETime を付けるか（M10）。
- 厳しい 1.4 の MSE が MapSupportedFeatures の無い Connect を断るか（m22）。
- 試験・build は走らせていない（review だけ）。
