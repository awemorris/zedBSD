<!-- awesome-plan project=zedbsd record=ws197 -->

# WS197: Bluetooth のスマホ連携（SMS の MAP、通話の HFP、連絡先の PBAP）

<!-- awesome-plan-current:start -->
Status: incomplete（ベータ2 の必須。code は branch agent/p1-ws197、区切りごとに main へ merge）
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: P1、WS200 の後（2026-10-10 ユーザー: N=1 のまま）
Target: **ベータ2**（2026-10-10 ユーザー「WS197はbeta2.mdで必須に入れておいてください。」）
Resume point: 下の「再開の手順」。p003 の i03 の途中（branch の head 2bf274a38、build は通る、host の試験の一部は未更新で通らない）。
<!-- awesome-plan-current:end -->

## 由来（2026-10-09 ユーザー）

「Bluetoothは、SMSのMAP, CallsのHFP, PBAP, を実装したいですが、見積もり規模はどうでしょうか。今はANCSは実装しなくていいですが、見積もりだけでも。」
「OBEX, MAP, Integration, PBAP, HFPの順で実装しますか。beta2.mdの必須が終わってからです。」

参考（Q1 の説明、2026-10-09）: Microsoft の Phone Link は iPhone では Bluetooth の HFP・MAP・PBAP・ANCS の標準だけを使い、Android では独自の app と IP（通話の音は HFP）。

## 再開の手順（2026-10-10 Q1、別の session への引き継ぎ）

- **code と詳細の記録は保留の branch `agent/p1-ws197`（head 2bf274a38）にある。** main には無い（10/17 の公開まで release の bluetoothd を変えないため、Q1 の判断）。p002・p003 の phase.md と review は 2026-10-10 に branch の f06bf4dbb の物を main にも写した（ユーザーの依頼）。branch で更新したら main へも写す（branch の main の merge で揃う）。試験（plan/ws197/tests/）は branch の code が要るので branch にだけある。
- 再開の時: (1) branch に main を merge する（`git switch agent/p1-ws197 && git merge main`。WS143・WS199 の bluetoothd・passkey の変更と衝突しうるので WS143 の host 試験 `plan/ws143/tests/bt-daemon-host-test.sh` と `plan/ws197/tests/bt-phone-host-test.sh` を流す）。(2) branch の plan/ws197/phase003/phase.md の「進み」の最後の行の**再開点**から続ける。(3) 区切りごとに main へ merge（ベータ2 に入れる、2026-10-10 ユーザー）。merge の前の T1 の HID の回帰はやめた（2026-10-10 ユーザー「流しすぎです。もう不要」）、host 試験と build で merge し実機の UAT で確かめる。
- 各 commit で: WS143 と WS197 の host 試験、target の bluetoothd の build warning 0。QEMU の HID の回帰（WS143 の 4 本）は流さない（2026-10-10 ユーザー）。

## 目標

- zedBSD の bluetoothd（WS143）にスマホ連携の profile を足す: MAP の MCE（SMS の一覧・受信の通知・送信）、PBAP の PCE（連絡先）、HFP の Hands-Free（通話の制御と通話の音）。
- [WS170](../ws170/ws.md) Phone の app の backend（compositor の API）につなぎ、Settings でスマホの pairing と許可を扱う。
- 制約: iPhone の MAP は読む・通知だけで送信できない。Android の MAP の送信は機種による。IP の経路（WS170）は別。ANCS は今は作らない（見積もり +8 LW）。

## Phase（2026-10-10 Q1、計 約 129 LW）

| Phase | 内容 | LW | Status |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計（profile の役割、bluetoothd の構造、WS170 の API、試験）、review 3 回、ユーザーの決定 Q1〜Q16 | 4 | cleared |
| p002 | RFCOMM・OBEX・SDP の server と client・phone.c・phone の pairing・linkmgr・drop の回復・WS143 の変更（i01〜i08）。詳細は branch の phase002/phase.md | 22 | **cleared**（host 約 2,500 checks、T1-518 HID の回帰 PASS、2026-10-10 Q1） |
| p003 | MAP（MCE の MAS と MNS）。2026-10-10: i01〜i07 実装済み、c56043c2b（i06）まで main に merge（T1-526 PASS）、i07＋owner の純粋な関数 5c6219e7a も T1-527 PASS（p004 の 1 回目の Just Works の CONSENT は前からの flake、T1-502 と同じ）で main に merge。**p003 は cleared**（実機の MAP は p008）。、phone link の持ち主・記録・再接続、socket の PHONE。詳細設計 第 2.1 版（review 2 回）、i01〜i08。詳細は branch の phase003/phase.md | 24 | **in-progress**: i01（phonerec、PAIR の検査、PHONE LINK・SHOW、FORGET）・i02（outq、client の枠、長さ付きの入力）済み。i03（phone link の一生）の途中。i04 mapxml・i05 bMessage・i06 map.c と MNS・i07 phoneio と SUBSCRIBE・i08 T1 は未 |
| [p004](phase004/phase.md) | SMS の層の interface の設計（Phone app・libkeiland・compositor・libkeiland-backend・bluetoothd、v3.1、review 3 回）。ユーザーの決定 P1〜P8 | — | cleared（設計、2026-10-10） |
| p004a | libkeiland-backend の phone-zedbsd.c（bluetoothd の socket）・compositor の phone-shell.c の bluetooth の backend・libkeiland の追加（phase004 §3〜§5・§11.2〜§11.4） | 6.5 | cleared 候補（2026-10-10、main に merge 4faf17473、KL_VERSION 79・manager 27、host 試験 PASS。実機は下の UAT。詳細は [phase004](phase004/phase.md) の「実装の進み」） |
| p004b | Phone の app の保存と同期（目印・merge・E.164 の key・送信の状態） | 3.5 | cleared 候補（2026-10-10、main に merge 778c1e377、host 試験 PASS。region の設定が無いので国番号は既定 81（P8）） |
| p004c | Settings の「Use as phone」と通知（WS156 の lock_text の変更は P1 が同じ Phase で行う、Q1） | 1.5 | cleared 候補（2026-10-10 P1、e7a478d33、host 試験 PASS、merge 待ち。実機は下の UAT） |
| [p005](phase005/phase.md) | PBAP（電話帳、vCard 2.1/3.0、電話帳の写しと名前の引き、通話の履歴）。詳細設計 第 3 版（review 2 回）、i01〜i07 | 11.5 | **planning**（2026-10-11: 第 3.1 版、i01〜i05 実装済み（i05 の中継は KL_VERSION 80・manager 28）、i06 は前半（目印・消しの計画・写しの消しの判断）だけ。i06 の後半と i07 Settings が残り（phase.md の「再開の手順」）。ユーザーの判断 Pc1〜Pc6 待ち、推しを仮に入れた） |
| p006 | HFP の制御（AT の SLC、indicator、応答・終話・発信、発信者、割り込み、codec の交渉） | 12 | planned |
| p007 | HFP の音: p007a xHCI の isochronous・usb-bt の interface 1・SCO の口、p007b SCO・audiod・CVSD の後に mSBC（Q9 の SCO の UAPI は p007a の設計の後にユーザーに聞く） | 25 | planned |
| p008 | 実機（Android が先、iPhone は HFP の後、Q13）。PHONE PROBE は p003 で消すので MAP の操作で確かめる | 20 | planned |
| p009 | 規約の全文の見直し | 2 | planned |

関連の Bug: [BUG-282](../bugs/BUG-282.md)（WS143 の hid.c の page の途中の Connection Request の取り違え、p003 i03 と同じ形で直す）。Future Work: fw-bt-goep2（ERTM・GOEP 2.0、MAP 1.4・PBAP 1.2）、F-086（SUBSCRIBE の phone の分は p003）。

## 2026-10-10 Q1: p002 の判定

p002（RFCOMM・OBEX・SDP・phone.c と WS143 の変更、i01〜i08）は保留の branch agent/p1-ws197 の 92157604f で実装済み。host 試験は全部 PASS、T1-518（WS143 の HID の回帰 4 本、bt-loopback-p002・bt-daemon-p003・bt-pair-p004・bt-hid-p005）が全部 PASS。**p002 は cleared**（code は 10/17 の後に main へ merge）。branch の phase002/phase.md への反映は P1。

## 2026-10-10 T1-524（HID の回帰、883abd4b8）

bt-loopback-p002・bt-daemon-p003（SHOW・BONDS）・bt-pair-p004 は PASS。**bt-hid-p005 が 2 回とも FAIL**: 自動の接続の後に HOG の mouse が open にならない（state=waiting）、keyboard の EVDEV の node が出ない、controller が去った時の KEY_B の release、controller が戻った時の HOG の mouse。→ WS197 i01〜i03 の WS143 の変更（linkmgr の page の枠・session・router）による回帰の見込み。**main への merge は止める**。P1 が直す。

## 5330 の UAT の手順（p004a〜c、スマホの SMS、2026-10-10 P1）

ユーザー「実機でテストするので詳細なQEMUテストは不要です」（2026-10-10）。p004a〜c は host 試験と build だけで、動きは Latitude 5330 の実機で確かめる。Android のスマホ 1 台（Q13: Android が先）。iPhone の MAP は送信ができない（読む・通知だけ）。

前提: main に merge した後の image（WS197 の bluetoothd と desktop を含む）を 5330 に入れ、ユーザーで sign in。スマホの Bluetooth を on、SMS を送ってくれる別の電話を用意。

1. **pairing と Use as phone**: Settings → Bluetooth を開く（Bluetooth を on）。
   - まだ pair していないスマホ: Other Devices にスマホ（種類 Phone）が出る。**Use as phone** を押す → PAIR phone=1 の pairing（両方に同じ数字が出たら両方で確認）→ スマホが「メッセージへのアクセス」を聞いたら**許可**。
   - pair 済みのスマホ: My Devices のスマホの行の **Use as phone** を押す。
   - 期待: 行の下の文が「Messages connecting...」→「Messages connected」（1 分ほど）、page の下に「The phone is used for messages.」。desktop の設定 phone.backend が 2。許可しなかった時は「Allow access to messages on the phone」。
2. **最初の同期**: Phone の app を開く → 過去 30 日（folder ごとに最大 500 通）の SMS が番号（または相手の名前）の会話で出る。500 通を越えた時は「Some older messages were not brought in.」が 1 回。同期の item では通知が出ない。
3. **受信（app が開いている時）**: 別の電話から SMS を送る → 数秒で会話に入り、app の通知「Message from <名前>」。その会話を開いていれば既読になり、スマホの側でも既読になる。
4. **受信（app が閉じている時）**: Phone の app を閉じて SMS を送る → desktop の通知（Phone、題は相手の名前か番号、本文は 1 行目）。通知の本文を押すと Phone の app がその会話で開く（`phone --peer <番号>`）。開いた時の同期でその SMS が入る（重複しない）。
5. **lock の画面**: 画面を lock して SMS を送る → lock の画面に「New message from <名前>」だけの板が出る（本文は出ない）。app が開いている時と閉じている時の両方。
6. **送信**: 会話で文を書いて送る → 「Sending」→「Sent」→（スマホが報告すれば）「Delivered」。相手の電話に届く。app を閉じて開き直しても 1 通のまま（スマホの送信済みの写しと重なる、`s<key>.txt` に名前が変わる）。スマホが送信を受け付けない機種では送信の button が灰色で「The phone does not take texts to send.」。
7. **離れて戻る**: スマホの Bluetooth を off にする（またはスマホを離す）→ Settings の行が「Used as phone (not connected)」などに。その間に届いた SMS は、on に戻した後の同期で入る。
8. **送信の途中で切れる**: 送信の直後にスマホの Bluetooth を off → その 1 通は「Sending」のまま（unknown）。1 時間後に同期を 1 回してもスマホの写しと重ならなければ「Not delivered」。
9. **やめる**: Settings の **Stop using as phone** → 「The phone is no longer used.」、phone.backend が 0。Phone の app には新しい SMS が来ない。

集める物: 失敗した時は、compositor の log の `KWL PHONE` の行、Phone の app の stderr の `PHONE` の行（番号・本文は出ない、長さだけ）、bluetoothd の log、`~/Documents/Phone/messages/` と `~/Documents/Phone/sync/bt-<address>.state` の中身、Settings の画面の写真。

限界（記録）: 番号の国番号の既定は 81（Settings に地域の設定が無い、P8）。app が閉じている間の SMS は保存されず、次の起動の同期で入る（P1、Q2）。
