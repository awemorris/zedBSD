# ws197-p003 詳細設計 第 2 版の短い再確認（review-2）

対象: [phase.md](phase.md)（branch `agent/p1-ws197`、commit 4397bcb31）。前回: [review-1.md](review-1.md)（blocker 3・major 12・minor 22）
日付: 2026-10-10（design-reviewer）
照合した物: `userland/base/bluetoothd/` の router.c（Connection Request・Link Key Request・Connection Complete の振り分け、`router_take`・`router_owner_of`・`btd_router_assign`）、linkmgr.c/h（`page_begin`・`page_end`・`connected`・`tick`、`BTD_LINKMGR_PAGE_MS` 15000）、session.c（command は同期: 答えを待つ間の他の event は queue に入り、command の後に配られる）、phone.c/h、pair.c（`pair_key_request`）、main.c（`btd_pair`・`btd_forget`・`btd_permitted`・`btd_accept`・`btd_read`・`btd_line`・`btd_write`・`btd_client_close`）、obex.c/h、rfcomm.h、sdps.c/h。MAP 1.4.2（§5.5.4.1・§5.5.4.13・表 5.10/5.11・表 6.6・§7.1.2 表 7.2）、Core 5.4 Vol 4 Part E 7.1.7・7.1.8・7.1.9。
code と設計文書は変えていない。行番号は phase.md（4397bcb31）の物。試験・build は走らせていない（review だけ）。

## 0. 結論

- review-1 の **blocker 3・major 12 は全部閉じた**。minor 22 も全部閉じた。
- 新しい指摘は **blocker 0・major 2・minor 9**。major はどちらも数行の書き直しで済み、再 review は要らない（Q1 が差分を見ればよい）。
  - N1（major）: M6 の直しで入れた「0x13・0x15 の切断で自動の page を止める」が、スマホの再起動・機内モード・Bluetooth の入れ直しの後、持ち主が logout/login するまで再接続しない状態を作る（直しが新しい誤りを生んだ）。
  - N2（major）: B1 の直しは**有効な記録**のある address だけを守る。持ち主の無い bond・無効な記録の bond に phone=1 で pairing すると、今の pair.c は保存の認証済みの鍵を黙って使うので、スマホの持ち主に何も見せずに持ち主になれる穴が残る。
- 判定（§4）: i02・i04・i05・i06・i07 は GO。i01 は N2、i03 は N1 を直してから（どちらも小）。i08 は i01〜i07 の後。

## 1. 閉じた指摘

| # | 閉じた所（第 2 版） |
| --- | --- |
| B1 | §3.2 の 103 行（PAIR の始めの `owned`、root だけ例外）と 105 行の 7)（handoff の再検査）。§1.1 に変更として記録。残りは N2 |
| B2 | §4.2 の token ＝（枠, 世代）、`btd_client_close` から `btd_phone_cancel`。§8.4 の 342 行（実行中の操作の答えを捨て、PAGE を進めない）。§10.1 の main の配線の試験 |
| B3 | §5.3 の交差は Accept と 0x0408、他の状態の Reject とローカルの Connection Complete の吸い込み、§5.2 の 167 行の守りの切れの 0x0408 と CANCELLING、§5.4 の 196 行の切る経路。残りは minor n1〜n3 |
| M1 | §5.3 の 172 行（在なら state に関わらず wants＝1） |
| M2 | §2 の 75 行（phone.c が phonerec と keys を呼ぶ）、§3.2 の 105 行の 8)「記録を書いてから route」、109 行の load・link_set・forget |
| M3 | §3.1 の 97 行（掃除）、§3.2 の 105 行の 6)（有効な記録だけ）・9)、107 行（無効なら D8、`.phone` を先に、bond が無くても DONE） |
| M4 | §3.2 の 104 行（phone=1 は seat の人だけ、root も） |
| M5 | §5.4 の段の表（WAIT_PEER・AUTH・ENCRYPT・KEY_SIZE、0x0C で WAIT_PEER、暗号化の後の失敗を捨てる） |
| M6 | §5.2 の 164 行（READY だけでは戻さない、2 分）、§5.5 の短い link。ただし 0x13・0x15 の扱いが N1 |
| M7 | §8.1 の 300 行（Connect 60 s）、§8.2 の `btd_obex_set_timeout`、§8.1.1（間隔、permission の 600 s、MNS の出し直し） |
| M8 | §8.6 の 361 行（LOCATE の read=no と見つからない時の UNREAD） |
| M9 | §1 の 42 行と §1.1（`PHONE GET` を作らない）、§8.3 の 322 行（`key=-`・`partial=1`）、§8.6 の LOCATE 32 とやり直し。残りは minor n6 |
| M10 | §8.4 の COUNT（filter 付き、folder ごと）、§8.5 の 353 行（ListingSize の無い答え） |
| M11 | §4.3（文法が正しければ必ず N byte を読む、buffer の残りを移す、待ちの間も） |
| M12 | §3.4 の 120〜121 行（`phone_ended` で変えない、`busy-links`）。残りは minor n5 |
| m1 | §6.1 の 235 行（24 byte） |
| m2 | §6.2 の 242〜243 行（`Z`、読めない時は `zone=received`） |
| m3 | §9.1 の 394 行（2047 byte） |
| m4 | §4.1 の 128 行（`btd_outq_append` で直に） |
| m5 | §8.4 の 341 行（n を半分に） |
| m6 | §8.5 の 350 行（cursor に since） |
| m7 | §8.6 の 367〜368 行（保留と 1 回だけ） |
| m8 | §8.3 の 321 行（`[0-9+*#]`、出どころの順を固定） |
| m9 | §5.8 の 224 行（session ごとに listen）、§8.7 の 375 行（READY で登録）、0x0006 の行 |
| m10 | §10.1 の 444 行（手の定数）、§8.7 の 388 行（手の byte）、457 行（Core の事象の順）。ただし n2 |
| m11 | §6.1 の 233 行（`:` の属性は無視、要素は失敗） |
| m12 | §7.1 の 253 行（一番外の BENV） |
| m13 | §3.1 の 94 行・§3.2 の 105 行の 8)。ただし n8 |
| m14 | §3.2 の 107 行 |
| m15 | §3.3 の 114 行（POWER off は不在） |
| m16 | §4.1 の 129 行・§8.4 の 343 行 |
| m17・m18 | §4.1 の 131 行 |
| m19 | §0 の 26 行 |
| m20 | §6.2 の 242・245 行、§8.4 の 339 行 |
| m21 | §1.1 の表 |
| m22 | §13 の 495 行 |
| その他 3 件 | §9.4 の 428 行（192 KB／64 KB の閾値）と §9.6 の 438 行（SHOW のやり直し）、§8.6 の 369 行（MASInstanceID の無い Put）、§5.8 の 226 行（room は `btd_timeout` に入れない） |

確かめて正しかった新しい記述: Core 7.1.7 の引用（Cancel の成功で page の Connection Complete は 0x02、Connection Complete が既に出ていれば Command Complete 0x0B、無い Create Connection への Cancel は 0x02、page の Connection Complete は必ず出て Cancel の Command Complete の後）、MAP §5.5.4.13（ListingSize は filter 後の件数）、表 6.6 の FilterMessageType 0x0C、§7.1.2 表 7.2（MNS の record の ServiceName は M、GoepL2CapPsm・MapSupportedFeatures は 1.4 の M だが注 1 で無い時は 0x1F を仮定）。`btd_router_assign` は既にある route の owner を変えるだけ（`router.c:241-246`）なので §5.4 の 184 行の二重の assign は害が無い。RFCOMM の DLC の T1 は 60 s（`rfcomm.h:57`）で、§8.1 の Connect の 60 s と合う。

## 2. major

### N1. 0x13・0x15 の切断で自動の page を「次の合図まで」止めると、スマホの再起動・機内モードの後に再接続しない（§5.5 の 204 行、§5.6、i03）

- 誤り: 「0x13 Remote User Terminated・0x15 … Power Off → 自動の page を止める。止めを解くのは不在 → 在、sleep.end、`PHONE LINK on`。スマホからの接続はいつでも受ける」。review-1 の M6 の直し方をそのまま入れたが、その直し方自体が次の場面を見落としていた。
- 根拠:
  - MAP では MAS の接続を始めるのは MCE（zedBSD）だけで、MSE（スマホ）は MAS のために自分から ACL を張らない（MNS は MCE の NotificationRegistration の後だけ）。p003 の範囲（HFP は p006）では、スマホから接続してくる理由が無い。したがって「スマホからの接続はいつでも受ける」は再接続の経路にならない。
  - スマホの Bluetooth を切る・機内モード・再起動では、スマホは切る前に Disconnect を出す（reason 0x13 か 0x15）のが普通（推測、p008）。
  - Android が profile の無い ACL を切る時の reason も 0x13 の見込み（推測、p008）。
- 起こる状況: 持ち主が seat に居るまま、スマホを再起動する（機内モードを入れて切る）→ 0x15/0x13 → 自動の page が止まる → スマホが戻っても、持ち主が logout/login するか suspend から戻るか `PHONE LINK on` を出すまで SMS が届かない。STATE に理由も出ない（§9.2 の `why=` の値に無い）。Q14（seat に戻ったら再接続）の期待と違い、利用者からは「時々つながらなくなる」に見える。
- 直し方（どれか、または組み合わせ）:
  1. 0x15 は 0x08 と同じ扱い（次の段の間隔で page）。電源を切ったスマホへの page は Page Timeout で失敗するだけで、段の間隔（最大 600 s）が嵐を防ぐ。
  2. 0x13 は止めずに、段を 600 s まで一気に進める（「切断」を押した利用者には 10 分ごとの再接続、`PHONE LINK off` が本当の止め方、と §9.6・p004 に書く）。止めるのは「READY の後 2 分より前の 0x13 が 3 回続いた」時だけにする。
  3. 止めている間は STATE に `why=peer-closed` を出し、app が「再接続」の操作を出せるようにする（`PHONE LINK on` で解ける、は今のまま）。
  4. 試験: 0x15 の後に段の間隔で page する。0x13 の後は 600 s で page する。短い 0x13 が 3 回で止まり、`PHONE LINK on` で解ける。§10.3 の実機の手順に「スマホの再起動・機内モードの後に戻る」を足し、reason の値を記録する。

### N2. 持ち主の無い bond・無効な記録の bond に phone=1 で pairing すると、保存の鍵が黙って使われ、スマホの持ち主に確認が出ない（§3.2 の 103〜105 行、§1.1 の 53 行、i01）

- 誤り: B1 の直しは「**有効な記録**があり uid が違えば `ERROR owned`」。§1.1 の 53 行は「今の pair.c は保存の認証済みの鍵を使い、スマホに何も出ない」を理由に挙げたが、その性質は有効な記録の無い address でもそのまま残る。
- 根拠:
  - `pair_key_request`（`pair.c:1099-1108`）は phone=1 でも、保存の鍵が認証済み（0x05・0x08）なら Link Key Request Reply を返す（Negative Reply は Just Works の鍵の時だけ）。
  - 記録が無効になる道: 持ち主の account が消えた・名前が変わった（§3.1 の 95 行）、bond が残ったまま `.phone` だけ消えた（手での削除）、普通の PAIR（phone 無し）で作った認証済みの bond。無効な記録の FORGET は D8 の人に許される（§3.2 の 107 行）ので、bond を残して `.phone` だけを消す事もできる（`.phone` を先に消し、bond の消しが失敗した時など）。
  - Android の MAP の許可は相手の機器ごとに覚えられる（「次回から確認しない」、推測、p008）。以前の持ち主が許可を残していれば、MAP の Connect でもスマホに何も出ない。
- 起こる状況: 以前の持ち主 B の account が消えた後（または B のスマホを普通の pairing で bond しただけの時）、seat の人 A が `PAIR <B のスマホ> bredr phone=1` → スマホが近くに居れば保存の鍵で無言で認証 → handoff の 6)・7) は有効な記録が無いので通る → 9) で B の無効な `.phone` を消す → A が持ち主になり、B のスマホの SMS を読める。Q2（持ち主だけが見る）と p001 S4 の前提（スマホがもう一度許可を求める）に反する。
- 直し方: phone=1 の pairing では保存の鍵を**常に**使わない（`pair_key_request` で `pair->phone` なら Negative Reply、SSP の数値比較をやり直させる。スマホの画面に確認が出る）。phone=1 の pairing は持ち主の初回の登録と作り直しだけなので、毎回の確認は利用者の負担にならない。§3.2 の表と §1.1 の 53 行に書き、`bt-phone-link-host-test`（か pair の host の試験）に「認証済みの保存の鍵がある address の phone=1 で、Link Key Request に Negative Reply」を足す。

## 3. minor

| # | 節 | 何が誤りか・抜けか | 根拠 | 直し方 |
| --- | --- | --- | --- | --- |
| n1 | §5.3 の 176 行 | `reject_pending` が status 0x0D で照合すると読める（「address と reason を覚えて」）。Core 7.1.9 が Reason を入れると定めるのは**相手**の Connection Complete の status だけで、ローカルの Connection Complete の status は定めていない。review-1 の B3 (b) の書き方（「status が自分の出した reason 0x0D」）も同じ誤り | Core 5.4 Vol 4 Part E 7.1.9（"The Status parameter of the HCI_Connection_Complete event, which is sent to the Host of the device attempting to make the connection, will contain the Reason"） | address だけで照合し、1 回・数秒で消す。偽の controller の台本はローカルの status を 0x0D 以外（例 0x0F・0x16）でも流す |
| n2 | §5.3 の 176 行 | CANCELLING で同じ address の Connection Request を Reject する。phone は接続したいのに断り、その Reject のローカルの Connection Complete が router で linkmgr の token を終える（`router.c:462-464`、address が page の物と同じ）。CANCELLING の規則（「Connection Complete（どの status でも）で NONE」）とも重なり、page の本当の Connection Complete を待たずに NONE になる | `linkmgr.c:147-164` | CANCELLING は PAGING と同じく Accept（Cancel は出し直さない）→ ACCEPTING（`page_outstanding` は 1 のまま） |
| n3 | §5.2 の 167 行 | 「Command Complete の 0x0B（既に接続、その Connection Complete は届いている）」は、この daemon では逆の順になる。command は同期で、答えを待つ間に来た Connection Complete は queue に入り command の後に配られる（`session.c:8-18`、`session_command`）。結果（NONE にし、後の成功の Connection Complete は claims 0 で router が切る、`router.c:1029-1033`）は意図どおりだが、「成功なら `phone_disconnect` で切る」の枝は実際には通らない。また phone の守り 15 s と linkmgr の page の満了 15 s（`linkmgr.h:40`、`linkmgr.c:205`）が同じなので、どちらの tick が先かで CANCELLING の間に token が外れ得る | session.c、linkmgr.c | 0x0B と 0x02 の Command Complete は log だけにし、CANCELLING を出るのは Connection Complete（と 5 s の守り）だけ、と書く。phone の PAGING の守りを linkmgr より短く（例 12 s）。成功の Connection Complete は切らずに SECURING へ進めてもよい（どちらかに決める） |
| n4 | §5.3 の 175 行 | 交差で Accept と Cancel を「出し」の順が書かれていない。Core 7.1.7 の「baseband が既に接続を作っていれば切って Success」の「接続」を、controller が同じ BD_ADDR の受けた接続と区別するかは実装次第（推測） | Core 7.1.7 | Cancel を先に出し（同期で Command Complete まで待つ、数 ms）、次に Accept。Cancel が 0x0B なら page の接続が既にあるので Accept せずに Reject。p008 で見る |
| n5 | §3.3・§5.3・§5.4 の 196 行 | (1) 在 → 不在・FORGET・`LINK off` の時、PAGING・ACCEPTING・CANCELLING で何をするかが無い（`phone_disconnect` は handle が要り、ACCEPTING・CANCELLING には handle が無いので 196 行の列挙は実際には SECURING だけ）。(2) ACCEPTING の 15 s の守りの切れの扱いが無い。(3) HID が 6 台の時の Connection Request は「待つ」ではなく断る必要がある（Accept の timeout まで放置すると controller が断るまで link の枠を食う） | `phone.c:1053-1055` | PAGING は Cancel、ACCEPTING・CANCELLING は「止める」印を立てて成功の Connection Complete で切る。HID が 6 台の時の Connection Request は Reject（0x0D、n1 の吸い込み） |
| n6 | §6.1 の 237 行・§8.4 の 332 行・§13 の 499 行 | 「map は順に頼らない」とあるが、LOCATE（offset 0 から 32 件）は新しい順に頼る。MAP §5.5.4.1 は「most recent messages in chronological descending order」と規定しているので仕様には合うが、順の違う MSE では live の item が全部 `partial=1` になる | MAP §5.5.4.1 | LOCATE に FilterPeriodBegin（今から 1 時間前、§6.2 の offset）を付けて順に頼らなくする。§13 の「listing の順」の行に LOCATE を書く |
| n7 | §8.7 の 375 行 | MNS の record を登録する口が無い。phone は `const struct btd_sdps_db *` を持つ（`phone.h:119`、`phone.c:132`）ので `btd_sdps_register` を呼べない。db は main の `btd_records`（`main.c:217`） | sdps.h:112 | phone の db を const でなくするか、main の hook（register・unregister）を `struct btd_map_hooks` か phone の hook に足す |
| n8 | §3.2 の 105 行の 8) | 同じ uid の再 pairing で enabled を保つと、`LINK off` にした後に phone=1 で pairing し直した持ち主のスマホがつながらないまま（利用者の意図は「使う」） | — | phone=1 の pairing は enabled を 1 にし、profile だけ保つ |
| n9 | §9.2 の 403・408 行 | SHOW の `link=` に ACCEPTING・CANCELLING・CLOSING の写しが無い。SEND の答えの `state=queued` は PushMessage の成功の後なので名前が合わない | — | `link=none|paging|securing|ready|closing`（ACCEPTING は securing、CANCELLING は paging に寄せる、と書く）。`state=pushed` か `accepted` |

注（実装の時に）: 今の `phone_event` は handle が `phone->handle` でない event を捨てる（`phone.c:636-640`）。PAGING・ACCEPTING・CANCELLING で受ける Connection Complete（失敗の物は handle に意味が無い）は address で振り分ける必要がある。設計の誤りではないが i03 の実装で落としやすい。

## 4. i ごとの判定

| i | 判定 | 理由・条件 |
| --- | --- | --- |
| i01 phonerec・handoff・LINK/SHOW・FORGET・HID の上限 | **直してから（小）** | N2（phone=1 は保存の鍵を使わない）を §3.2・§1.1 に書けば GO。n8 は実装の時に |
| i02 outq・枠 16・世代・長さ付きの入力 | **GO** | B2・M11・m3・m4・m17・m18 は閉じた |
| i03 link の一生 | **直してから（小）** | N1（0x13・0x15）を §5.5 に書けば GO。n1〜n5 は実装の時に直してよい（設計の文も合わせて直す） |
| i04 mapxml と datetime | **GO** | 変わらず |
| i05 bmsg | **GO** | m12 は閉じた |
| i06 obex の hook・map.c・MNS の record | **GO** | n6・n7 は実装の時に |
| i07 phoneio・main の PHONE と SUBSCRIBE | **GO** | n9 は実装の時に |
| i08 T1 の依頼・記録 | i01〜i07 の後に GO | §10.1 の偽の controller に n1（ローカルの status を変える）を足す |

N1・N2 は数行の書き直しで、再 review は要らない（Q1 が差分を確かめればよい）。

## 5. 確かめていない事（推測）

- スマホの再起動・機内モード・Bluetooth の入れ直し・profile の無い ACL の切断で、Android が出す Disconnect の reason（N1 の前提）。p008。
- Android の MAP の許可が相手ごとに覚えられ、bond が同じなら再び聞かないか（N2 の前提）。p008。
- 交差で Cancel が受けた接続を切る controller があるか（n4）。p008。
- 試験・build は走らせていない（review だけ）。
