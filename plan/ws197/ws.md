<!-- awesome-plan project=zedbsd record=ws197 -->

# WS197: Bluetooth のスマホ連携（SMS の MAP、通話の HFP、連絡先の PBAP）

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: 未割当（plan/beta2.md の必須が終わってから P1、2026-10-09 ユーザー）
Target: ベータ2 の必須の後に着手（code は 10/17 の公開まで main に入れず保留の branch で、Q1 の判断）
Resume point: p001（設計）から。
<!-- awesome-plan-current:end -->

## 由来（2026-10-09 ユーザー）

「Bluetoothは、SMSのMAP, CallsのHFP, PBAP, を実装したいですが、見積もり規模はどうでしょうか。今はANCSは実装しなくていいですが、見積もりだけでも。」
「OBEX, MAP, Integration, PBAP, HFPの順で実装しますか。beta2.mdの必須が終わってからです。」

参考（Q1 の説明、2026-10-09）: Microsoft の Phone Link は iPhone では Bluetooth の HFP・MAP・PBAP・ANCS の標準だけを使い、Android では独自の app と IP（通話の音は HFP）。

## 目標

- zedBSD の bluetoothd（WS143）にスマホ連携の profile を足す: MAP の MCE（SMS の一覧・受信の通知・送信）、PBAP の PCE（連絡先）、HFP の Hands-Free（通話の制御と通話の音）。
- [WS170](../ws170/ws.md) Phone の app の backend（compositor の API）につなぎ、Settings でスマホの pairing と許可を扱う。
- 制約: iPhone の MAP は読む・通知だけで送信できない。Android の MAP の送信は機種による。IP の経路（WS170）は別。ANCS は今は作らない（見積もり +8 LW）。

## 見積もり（2026-10-09 Q1、1 LW ≈ エージェントの実時間 20 分）

| Phase | 内容 | LW |
| --- | --- | --- |
| p001 | 設計（各 profile の役割、bluetoothd の構造、WS170 との API、試験の方法）。design-reviewer を通す | 4 |
| p002 | RFCOMM（OBEX と HFP の下、L2CAP の上の多重化と credit の流量制御、SDP の検索）と OBEX（client・server、Connect/Get/Put、header、app parameter） | 22 |
| p003 | MAP（MAS: folder・message の一覧・取得・送信の bMessage、MNS: 通知の server と SDP の record） | 16 |
| p004 | Integration（WS170 Phone の app の backend: SMS・通話・連絡先の compositor の API、Settings のスマホの pairing と許可） | 12 |
| p005 | PBAP（電話帳の取得、vCard 2.1/3.0 の parser、連絡先の store） | 8 |
| p006 | HFP の制御（AT の SLC、indicator、応答・終話・発信、発信者、割り込み、codec の交渉） | 12 |
| p007 | HFP の音（SCO）: xHCI の isochronous の転送（今は無い）、usb-bt の isochronous の interface、SCO の link、CVSD・mSBC、audiod の mic と speaker の経路 | 25 |
| p008 | 実機の試験と debug（Android と iPhone） | 20 |
| p009 | 規約の全文の見直し | 2 |
| 計 | | **約 121 LW**（L2CAP ERTM は後回し +10、ANCS は作らない +8） |

順はユーザーの指示（OBEX → MAP → Integration → PBAP → HFP）。OBEX の下に RFCOMM が要るので p002 に含めた。HFP の中で一番不確かなのは p007 の SCO（USB の isochronous）。
