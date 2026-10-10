<!-- awesome-plan project=zedbsd record=ws197-p005-review-1 -->

# ws197-p005 第 1 版の review（design-reviewer、agent ad782c2d4ddaad97c、2026-10-11）

対象: [phase.md](phase.md) 第 1 版（4ee8730b7）。PBAP の byte 列（tag・長さ・PropertySelector `…00 20 00 87`・`…10 00 00 87`、Target、MaxListCount=0、0.vcf、履歴の順）は仕様どおり。問題は今の code と合わない前提と抜け。

## blocker

- **B1（§7.1・§7.3）**: スマホの連絡先を `ph_contact` として store に混ぜる形は今の store と合わない。item は `messages/<連絡先の id>/` に書かれ（store.c:2232-2236・339）、id `bt-<addr>/<key>` は `/` を含み、`store_load_conversations`（store.c:975-1060）は `n`・`a` しか読まないので開き直すと見えない。連絡先の消し・改名で folder が孤立。上限 1024 は全体の配列にかかる（store.c:57・1275）。view の request は配列の index（phone.h の `struct ph_request`、main.c:1110-1168）なので同期中の消し・並べ替えで別の人に送り得る。複数の番号の送り先が最初の TEL。→ **直し: スマホの連絡先は store の配列に入れず別の表（key → 名前・番号の鍵）にし、item は番号の会話か手元の連絡先の folder に書く。スマホの連絡先は名前と一覧の重ねにだけ使う。**

## major

| # | 節 | 内容 | 直し |
| --- | --- | --- | --- |
| M1 | §5.2・§6 | backend の `phone_field`（phone-zedbsd.c:1699-1740）は引用符の中を区別せず、`name=` が `length=` の前にあると FN の ` length=5` で長さを偽れる。今の `PHONE MESSAGE` も同じ（既存の欠陥） | **直した（eee2a5d12）**: phone_field は引用の外の欄だけを見る。phone-backend-host-test に偽の欄の例 |
| M2 | §7.2・§12 | 全体の同期の後の消しが生きている連絡先を消す（page の間の 1 件の増で offset がずれる、`o >= PhonebookSize` の食い違い、一時的な空の電話帳で全部消し） | 終わりは「受けた件数 < MaxListCount」、終わりに SIZE を取り直し違えば消さない、件数の大きな減り（半分以下・0）では消さない、2 回続けて無い時だけ消す。試験に入れる |
| M3 | §8 | handoff は記録を profiles=0x07（m,c,h）で書く（phone.c:3077-3104、phonerec.h:43）。PAIR から LINK の間や CLI の pairing で PBAP が始まり許可の画面が出る | handoff の既定を MESSAGES に。§8 の既存の記録の扱いを 0x07 も含めて書き直す（Pc4） |
| M4 | §6 | compositor の `phone_item` は what を MESSAGES に固定（phone-shell.c:1817）、`kl_backend_phone_item`・`_state` に what・contacts が無い（keiland-backend.h:957-973） | backend の struct と phone-shell の送りの変更を §2・§6 に書く |
| M5 | §6 | zone の値の意味が今の定義（0 phone、1 MSE、2 local、3 received）と食い違う。time 0 は 1970 年に並ぶ | `Z` は 0、local は 2。時刻無しは zone 3 で同期の時刻か partial の印。keiland.h の説明も |
| M6 | §6 | `kl_phone_link` に欄を足すと今の libkeiland が短い size を拒む（system-view.c:844・758、p004 §3 の規則は未実装）。link の event は signature が固定（system.c:157・4181） | i05 で「最初の版の size 以上なら知っている所まで写す」と新しい event（版で分ける） |
| M7 | §6・§8 | contacts の why が libkeiland まで届かない | `contacts_why[KL_PHONE_WHY_MAX]` を足す |
| M8 | §3・§5.1 | PCE の SDP record が抜けている（p002 は p005 が登録と決めた、PBAP §7.1.1 は M） | 版 0x0101 の record の byte、登録の時期、置き場所（MNS は phone.c の `phone_mns_update`、§3 の「phone.c を変えない」と衝突）、試験 |
| M9 | §5.1 | Connect に App Parameters を載せる API が無い（obex.c:366-399）。1.1 を名乗るなら付けないのが筋 | (a) 付けない か (b) obex.c に追加の header の引数。§0・§5.1 を合わせる |
| M10 | §3 | 自分で開いた DLC が DM で断られると `opened` 無しで `closed` だけ（rfcomm.c:1167-1179・1543-1567）。mux の表で行き先が無い。MAP も同じ穴（map.c:488-505、OPENING に期限無し） | 表に無い dlci の closed は `dlci >> 1` の server channel から子を引く。子は CONNECTING で自分の channel の closed を失敗に。期限を持つ。試験 |
| M11 | §5.1・§5.3 | 「idle で OFF」と「切断で failed」が矛盾。cursor の session が OBEX の Connect ごとに変わるので、page の間に切られると stale で同期が終わらない | 実行中の操作が無い時の相手の切断だけ idle。cursor の照合を同期の通し番号に（PBAP の offset は session に結ばれない）か、同期の間は接続を保つ。自分から切る時期 |
| M12 | §5.1 | Android は RFCOMM の受けで許可を聞き、拒否で DLC を閉じる見込み（推測）。refused の 30〜600 s の繰り返しで許可の画面が何度も出る | READY に届かずに DLC が断られた・閉じた時も permission と同じ扱い。UAT に「拒否の後に繰り返し出ない」 |
| M13 | §9 | 試験が上の欠陥を捕まえない | B1・M1・M2・M10・M11・M8・M9・SIZE の答えの分割を試験に。消しの判断を純粋な関数に寄せる |

## minor

1. PBAP の cancel・check・pump・tick・deadline の main の配線（main.c:1062・2617・3411・2321・3418・3422）、`waits_phone` にどの profile か。
2. PhonebookSize は `response` の hook で最初の packet から（§6.2.2、obex.h:126-138）。
3. 「Get は 0x83 だけ」の出典は §6.2.2（§6.1 ではない）。
4. 履歴の新しい順は should（§3.1.5.3）。since で打ち切ると順の違う機種で取りこぼす。UAT に。
5. 時刻の無い履歴の key の衝突、非通知の通話の置き場所（`ph_store_conversation` は空の番号で失敗、store.c:576-591）。
6. `store_find_key` は `:map:`・`s<key>.txt` に固定（store.c:1983-1984）。`ph_store_sync_save`（store.c:822-849）は 2 行を書き直す。
7. 名前の引きは 5000 × 8 の線形の比較が遅い。鍵の表を前もって作る。
8. vCard 2.1 の折り返しの違い、CHARSET を UTF-8 だけにしたのは p001 §7.2 からの変更（§1.1 に）。
9. skipped の意味が混ざる（0.vcf は数えない方がよい）。
10. `BTD_PHONE_PENDING_DLCS` 2（phone.h:95）。HFP の前に増やす。
11. `btd_phone_profile_ok` は MAP の up でしか呼ばれない。contacts だけの時。
12. 5000 件は 157 回の Get。bluetoothd が大きく取り memory から 32 件ずつ渡す形も検討。
13. §8 の文が読みにくい。off の時の profiles の値。
14. mch を読むとスマホの不在着信の印が消える機種（推測）。UAT に。

## 追加のユーザーの判断（Pc1・Pc2 の他）

- Pc3: Stop using as phone・contacts の off・ペアの解除・別のスマホの時に `contacts/bt-<addr>/` と通話の履歴を残すか消すか。
- Pc4: CLI で作った m,c,h の記録で、更新の時に自動で contacts を始めてよいか。
- Pc5: mch を読むとスマホの不在着信の印が消える場合でも読むか。
- Pc6: 手元とスマホの連絡先で番号が一部だけ重なる時、残りの番号の会話の名前。

## 判定

| i | 判定 |
| --- | --- |
| i01 vcard.c | **GO**（M5・minor 5・8 は実装の中で） |
| i02 phonemux | 直してから（M10・M8・minor 10） |
| i03 pbap の始め | 直してから（M9・M11・M12・M8） |
| i04 page と socket | 直してから（M1・M2・M11・minor 1・2・9） |
| i05 中継 | 直してから（M4・M5・M6・M7・M1） |
| i06 Phone の app | 直してから（B1・M2・minor 6・7・Pc1・Pc3・Pc6） |
| i07 Settings | 直してから（M3・M7・minor 13・Pc4） |
