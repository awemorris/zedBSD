# ws197-p004 詳細設計 第 1 版の review（review-1）

対象: [phase.md](phase.md)（agent/p1、commit 274cc2c20）。bluetoothd の側は branch `agent/p1-ws197` の f06bf4dbb。
日付: 2026-10-10（design-reviewer）。code と設計文書は変えていない。build・試験・QEMU は走らせていない（review だけ）。(推測) は確かめられなかった物。
答え: 第 2 版で全部を本文に入れた（各節の `[B1]`・`[M1]`・`[m]` の印）。

## 結論

全体はまだ実装に入れない。blocker 3・major 13・minor 約 20。bluetoothd の文法（§5.1〜§5.2）は B3 を入れれば p003 i07 に使える。libkeiland・compositor・backend・store はそれぞれ、2 つの session が違って決めうる未決の interface を 1 つ以上持つ。

## Blockers

- **B1. 新しい request の誤りが app に届く道が無い（§3.3・§3.5・§4）。** `struct kl_phone_event` に error の欄が無く大きさは変えない。mark_read の「state 0、error だけ」は未定義。`failed`（opcode 7）は箇条だけで表と §11.3 に無い。§3.5 の errno をどの呼びで見るか不明。send_text の result・QUEUED・failed の順が不明。直し: request ごとの一生の表（来る event・順・終わり）。request の errno は今の result の ring（`kl_system_take_result`、Phone の app は既に読む）へ。`KL_PHONE_STATUS` は SENT・DELIVERED・FAILED だけ。opcode 7 を表に。
- **B2. 終わりの event が落ちうる。同期が永久に EBUSY、送信の状態が消える（§3.3・§4・§6.4）。** (a) `kwl_emit` は client の出力が 1 MB を越えると全 event を ENOBUFS で捨てる（`page_end` も `dropped` も）。(b) libkeiland の phone の ring は 16 で古い物を黙って捨てる。item ごとに 1 つ積むと 32 item の page で STATUS・LINK が消える。(c) p001 S7 の通し番号を `dropped` で置き換えたが §10 に無い。直し: compositor が `page_end`・`failed`・`dropped` を借りにして送り直す。`KL_PHONE_ITEM` は 1 つの印に。ring の溢れは `KL_NOTIFY_LOST` と同じ作りで `KL_PHONE_DROPPED`。同期の EBUSY は page_end・failed・object の喪失・期限で解く。S7 を戻すか §10 に記録。
- **B3. 持ち主が変わった後も前の持ち主の SUBSCRIBE が新しい持ち主の SMS を受ける（§5.1・§5.4、p003 §9.4）。** SUBSCRIBE は始めに 1 度だけ確かめ、emit は全部の SUBSCRIBE へ。直し: emit の時に今の有効な記録の uid と root だけに送る。記録の変化で持ち主でない SUBSCRIBE を閉じる。host 試験を足す。

## Major

- **M1.** 利用者の全部の app が SMS の本文を受ける: libkeiland は全 client に phone object を作り、v1 の `received` は全 object へ。listen は Phone だけと言うが強制が無い。Mail は app_id "mailer" の gate。直し: backend 2 では v1 の received を送らない。listen・sync・mark_read を app_id で gate するか、Q2 の「持ち主だけ」が uid の全 process の意味だと記録する（P7）。
- **M2.** 同期の上限と遅い SMS で永久に取りこぼす: bluetoothd は毎回 folder ごとに 500 で止め、PAGE-END は止めた事を言わない。目印 − 24 時間より古い datetime で遅く届いた物は以後取らない。直し: PAGE-END に `capped`、上限は最初の同期だけか app が渡す、定期の深い同期、P1 の保証の範囲を正しく。
- **M3.** §6.3 の重ねが曖昧で重複を作る: partial と key 付きの重ね、live の dir=out と自分の sending、`n090…` と `n+8190…` の別の folder、rename するか、1 対 1 か。直し: 索引と候補の手順を algorithm として書く。
- **M4.** SENT が 2 本の socket の間で失われるか誤る: map は pushed・DONE の後に SUBSCRIBE へ SENT。backend が SUBSCRIBE を先に読むと n が未登録。bluetoothd の再起動で `next_request` が 1 から。直し: result を SENT より先に、知らない n を 10 秒保留、繋ぎ直しで表を空に、`next_request` を乱数から。
- **M5.** backend の API が名前の一覧で interface でない: state・item・result の struct、changed の bit、SENT の state の値、cancel・fd が無い。Settings の LINK・SHOW が `kl_backend_bluetooth` か `kl_backend_phone` か未定。`PAIR phone=1` の WS143 の変更の持ち主。
- **M6.** 新しい公開の struct が伸ばせない（p005・p006 で欄が要る）。直し: size の引数か予約の欄。定数に名前。
- **M7.** 同時の数と接続の枠の主張が code と矛盾: request の接続 1 本なら同期は 1 つ。SEND が 32 の GET の page や 30 秒の room の待ちの後ろに並ぶ。mark_read が queue 8 で EBUSY。「予約 4 の中で足りる」は誤り（compositor は 6 本、予約は空き 4 以下の時だけ非 seat を断る。誰でも 12 本を持てる）。直し: 直列を明記するか SEND・READ に 2 本目、mark_read の歩調、予約を 6 か uid ごとの上限。
- **M8.** 送信の途中の link の喪失で同じ SMS を 2 度送らせうる: `lost` を failed と見せると利用者が再送。直し: lost・ECONNRESET・timeout は「分からない」で sending のまま、同期か 1 時間で決める。failed は確かな拒否だけ。
- **M9.** notify=0（MNS 無し）の時に live が来ない: 定期の同期が無い。送信は 1 時間で failed に。直し: notify=0 の間は 5 分ごと、1 時間の規則の前に同期。
- **M10.** mark_read の handle を持たない、ESTALE で全同期を起こす。直し: memory の key → handle の表、無い時は次の同期まで延ばす、mark_read の ESTALE で同期しない。
- **M11.** `phone.backend` の実行中の変更を扱わない、設定の範囲 0〜1、`_BLUETOOTH` の定数が無い、greeter が開かないこと。直し: 設定の変化で開け閉め、close で ENODEV と link、file を §11.3 に。
- **M12.** banner と lock の画面の interface が無い、app も通知するので二重。直し: notify の拡張（全文と lock の文）と持ち主、bluetooth の item では app は通知しないか、「前面」の定義。
- **M13.** v1 の send と版の gate が無い: backend 2 での v1 send、新しい event は版 N 以上の object だけ、今の app は QUEUED を FAILED にする。

## Minor（要約）

SHOW の権限の文言（D8）、loopback の矛盾（sync の偽の page）と link の値、errno と why の語の表の欠け、send_text の入力の検査（length 0 で bluetoothd が接続を閉じる、番号の正規化の持ち主、SMS 以外は ENOTSUP）、同期の全 page で since が同じ、libkeiland の page_end の保持・queue の順・item を先に取る、同期のやり直しの上限と dropped の後の 1 回と起動の時の 2 重、SUBSCRIBE の拒否の語とやり直し、backend の書きの buffer と見張り、store の知らない header の保持と `s<key>` での重複除き、key の衝突の限界、§11.4 の file の欠け、規約の見直しの Phase と merge の危険、定数の値の重なり（QUEUED 6 と DROPPED 6）、試験の欠け（SENT が pushed より先、page_end の ENOBUFS、ring の溢れ、持ち主の変更の間の SUBSCRIBE、番号の衝突、SEND の length 0）、Phase の分け（p004a・b・c）。

## 確かめて合っていた物

wire の item 約 17 KB は 65532 以内。handle 26・key 17・cursor 約 40・datetime 24 は欄に収まる。peer・name の 128 と NUL は 132 以内。daemon の行 2047 は backend の 2048 以内。送りの 8192 と受けの 16384 は 3 層で同じ。kind（3〜6）と QUEUED は今の値と重ならない。cursor の形は map.c と合う。partial は map.c で必ず `key=-`。

## 判定

GO: §0、§5.1〜§5.2（B3 と SHOW の文言を直して）、§6.1（M9 の定期の同期を足して）、§10（S7 の記録を足して）、§11.1、§11.2 の file の置き場。
直してから: §3・§4（B1・B2・M1・M6・M13）、§5.3（M5・M7・M4）、§2・§6.2〜§6.4（M2・M3・M10）、§7（M12）、§8（M8）、§11.3〜§11.6（M11・M5）。§9 の P1〜P6 は P1 の保証を直し（M2）、P7（M1）を足してからユーザーへ。
