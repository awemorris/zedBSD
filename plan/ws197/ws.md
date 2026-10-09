<!-- awesome-plan project=zedbsd record=ws197 -->

# WS197: Bluetooth のスマホ連携（SMS の MAP、通話の HFP、連絡先の PBAP）

<!-- awesome-plan-current:start -->
Status: incomplete（ベータ3。code は保留の branch だけ）
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: 未割当（ベータ3、10/17 の公開の後に再開）
Target: **ベータ3**
Resume point: 下の「再開の手順」。p003 の i03 の途中（branch の head 2bf274a38、build は通る、host の試験の一部は未更新で通らない）。
<!-- awesome-plan-current:end -->

## 由来（2026-10-09 ユーザー）

「Bluetoothは、SMSのMAP, CallsのHFP, PBAP, を実装したいですが、見積もり規模はどうでしょうか。今はANCSは実装しなくていいですが、見積もりだけでも。」
「OBEX, MAP, Integration, PBAP, HFPの順で実装しますか。beta2.mdの必須が終わってからです。」

参考（Q1 の説明、2026-10-09）: Microsoft の Phone Link は iPhone では Bluetooth の HFP・MAP・PBAP・ANCS の標準だけを使い、Android では独自の app と IP（通話の音は HFP）。

## 再開の手順（2026-10-10 Q1、別の session への引き継ぎ）

- **code と詳細の記録は保留の branch `agent/p1-ws197`（head 2bf274a38）にある。** main には無い（10/17 の公開まで release の bluetoothd を変えないため、Q1 の判断）。main のこの ws.md と phase001 は要約。p002・p003 の phase.md・review・試験（plan/ws197/tests/）は branch にだけある。
- 再開の時: (1) branch に main を merge する（`git switch agent/p1-ws197 && git merge main`。WS143・WS199 の bluetoothd・passkey の変更と衝突しうるので WS143 の host 試験 `plan/ws143/tests/bt-daemon-host-test.sh` と `plan/ws197/tests/bt-phone-host-test.sh` を流す）。(2) branch の plan/ws197/phase003/phase.md の「進み」の最後の行の**再開点**から続ける。(3) i03 が終わったら main へ merge してよいか Q1 が判断（10/17 の後なら可）。
- 各 commit で: WS143 と WS197 の host 試験、target の bluetoothd の build warning 0。QEMU に Bluetooth の実機は無いので、HID の回帰は T1（T1-518 の手順: branch の commit を detach で build-bt-image.sh、WS143 の 4 本の試験）。

## 目標

- zedBSD の bluetoothd（WS143）にスマホ連携の profile を足す: MAP の MCE（SMS の一覧・受信の通知・送信）、PBAP の PCE（連絡先）、HFP の Hands-Free（通話の制御と通話の音）。
- [WS170](../ws170/ws.md) Phone の app の backend（compositor の API）につなぎ、Settings でスマホの pairing と許可を扱う。
- 制約: iPhone の MAP は読む・通知だけで送信できない。Android の MAP の送信は機種による。IP の経路（WS170）は別。ANCS は今は作らない（見積もり +8 LW）。

## Phase（2026-10-10 Q1、計 約 129 LW）

| Phase | 内容 | LW | Status |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計（profile の役割、bluetoothd の構造、WS170 の API、試験）、review 3 回、ユーザーの決定 Q1〜Q16 | 4 | cleared |
| p002 | RFCOMM・OBEX・SDP の server と client・phone.c・phone の pairing・linkmgr・drop の回復・WS143 の変更（i01〜i08）。詳細は branch の phase002/phase.md | 22 | **cleared**（host 約 2,500 checks、T1-518 HID の回帰 PASS、2026-10-10 Q1） |
| p003 | MAP（MCE の MAS と MNS）、phone link の持ち主・記録・再接続、socket の PHONE。詳細設計 第 2.1 版（review 2 回）、i01〜i08。詳細は branch の phase003/phase.md | 24 | **in-progress**: i01（phonerec、PAIR の検査、PHONE LINK・SHOW、FORGET）・i02（outq、client の枠、長さ付きの入力）済み。i03（phone link の一生）の途中。i04 mapxml・i05 bMessage・i06 map.c と MNS・i07 phoneio と SUBSCRIBE・i08 T1 は未 |
| p004 | Integration（WS170 Phone の app の backend: SMS・通話・連絡先の compositor の API、Settings のスマホの pairing と許可）。p003 §1.1 の変更（suspend、本文 16 KB、PAGE の cursor、PHONE GET を作らない）を前提に | 12 | planned |
| p005 | PBAP（電話帳、vCard 2.1/3.0、連絡先の store） | 8 | planned |
| p006 | HFP の制御（AT の SLC、indicator、応答・終話・発信、発信者、割り込み、codec の交渉） | 12 | planned |
| p007 | HFP の音: p007a xHCI の isochronous・usb-bt の interface 1・SCO の口、p007b SCO・audiod・CVSD の後に mSBC（Q9 の SCO の UAPI は p007a の設計の後にユーザーに聞く） | 25 | planned |
| p008 | 実機（Android が先、iPhone は HFP の後、Q13）。PHONE PROBE は p003 で消すので MAP の操作で確かめる | 20 | planned |
| p009 | 規約の全文の見直し | 2 | planned |

関連の Bug: [BUG-282](../bugs/BUG-282.md)（WS143 の hid.c の page の途中の Connection Request の取り違え、p003 i03 と同じ形で直す）。Future Work: fw-bt-goep2（ERTM・GOEP 2.0、MAP 1.4・PBAP 1.2）、F-086（SUBSCRIBE の phone の分は p003）。

## 2026-10-10 Q1: p002 の判定

p002（RFCOMM・OBEX・SDP・phone.c と WS143 の変更、i01〜i08）は保留の branch agent/p1-ws197 の 92157604f で実装済み。host 試験は全部 PASS、T1-518（WS143 の HID の回帰 4 本、bt-loopback-p002・bt-daemon-p003・bt-pair-p004・bt-hid-p005）が全部 PASS。**p002 は cleared**（code は 10/17 の後に main へ merge）。branch の phase002/phase.md への反映は P1。
