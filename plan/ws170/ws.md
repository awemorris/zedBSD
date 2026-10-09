<!-- awesome-plan project=zedbsd record=ws170 -->
# WS170: Phone の app（連絡先・SMS/MMS/RCS・VoIP を統合したタイムライン）

Status: incomplete（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: p000 を cleared。2026-10-08 q910 P2 の照合: p002〜p004 cleared（T1-298）、p000（mock）は T1-177b の撮影の後に実装され Q1 の判定、p005（規約）はベータ3。準正常系は WS177 の案 P）（2026-10-07 q831 で p001〜p004 を実装、T1 の QEMU 待ち。p005 は規約の見直し（後回し）。2026-10-05 追加。まず UI の mock（p000）を優先、q743。段は未定（ユーザーに確認）。最初の範囲（連絡先からタイムラインの表示まで）の見積もり 4 LW（Q1 の概算: 設計 1・連絡先と保存 1・タイムラインの UI 1.5・API の骨格 0.5）。本物の送受信・通話は後の Phase）
Master: [master](../master.md)
Primary Milestone: MG006

## 由来

ユーザー（2026-10-05）:

「[Phone app]
- 連絡先、SMS/MMS/RCS、VoIPを統合した総合phoneソフト
- 連絡先ごとにiOSのメッセージ風のタイムラインがある
- 連絡先のタイムラインからメッセージか電話ができる
- コンポジタにメッセージを操作するAPIを追加する
- コンポジタバックエンドがサポートしていればSMSを送受信可能
- バックエンドはローカルのモデムをサポートできるし、連携スマホアプリ経由で送受信も可能
- スマホ連携はBluetoothではなくIP経由も可能で、SMS, MMS, 通話をブリッジできる
- 連携するスマホは持ち歩いてもいいし、家に固定しておいてもいい
- 家の中でスマホを持ち歩かず部屋で充電しておいて、Keilandタブレットを家中持ち歩くユースケース
- キャリア回線のSMS, MMS, 通話は、キャリア通信を融合する目的
- 将来はキャリア系の技術でなくインターネット技術に移行することを見越した世界観
- キャリア技術、インターネット技術をブリッジして１つのアプリに融合し統一操作できる世界観
- メッセージや添付ファイルは失われないように、クラウドストレージにバックされたローカルフォルダに入れて管理する(~/Documents/以下はクラウドbackedとみなす)
- VoIPは対応プロバイダをバックエンドで追加可能。電話番号対応のゲートや、VoIPのみのゲートがあると思う。ただこれはオプション
- チャット/メッセンジャーは無理に統合せず、サービスのネイティブのWebやアプリを推奨しつつ、APIバックエンドで一部統合できるようにするオプション機能
- メッセージングは流行り廃りがあり、今だとRCSにフォーカスするのがいいと思うが、将来はキャリア技術でなくインターネット技術の標準ができるかもと思う
- まずは連絡先からタイムラインを表示するところまで実装
- 本物のメッセージ送受信や通話はあとで実装」

## 優先（2026-10-05）

ユーザー（2026-10-05 11 時前）「WS170は、ガワのインタフェースのモックだけでよいので、優先して作ってみてください。12時に予定しているUATのあとでいいです。おそらく17時か18時頃にもう一度UATをやるので、そのときにあればうれしいですが、必須ではないです。」

## 目標

最終の目標（世界観）: キャリアの技術（SMS・MMS・RCS・回線の通話）とインターネットの技術（VoIP、将来の標準）をブリッジし、1 つの app で連絡先ごとに統一の操作で扱う。

1. 連絡先ごとに iOS のメッセージ風のタイムライン。タイムラインからメッセージか電話ができる。
2. compositor にメッセージを操作する API を足す。compositor の backend が対応していれば SMS を送受信できる。
3. backend: local のモデム、または連携するスマホの app 経由（Bluetooth でなく IP でも可、SMS・MMS・通話をブリッジ。スマホは持ち歩いても家に固定してもよい。家ではスマホを部屋で充電し Keiland のタブレットを持ち歩く使い方）。
4. メッセージと添付は失われないよう、cloud storage に back された local の folder（`~/Documents/` 以下は cloud backed とみなす）に置いて管理する。
5. VoIP は対応の provider を backend で足せる（電話番号の gateway・VoIP だけの gateway）。option。
6. chat・messenger は無理に統合しない。native の Web・app を勧め、API の backend で一部を統合できる option。
7. messaging は今は RCS に焦点。将来のインターネットの標準に移れる構造にする。

**最初の範囲（ユーザー）**: 連絡先からタイムラインを表示するところまで。本物のメッセージの送受信と通話は後。

## 設計で決めること（p001）

- 保存の形: 連絡先（vCard か独自か）、会話とメッセージ（`~/Documents/` の下の file の配置、1 メッセージ 1 file か会話ごとの log か、添付の置き方、同期の衝突に強い形）。
- compositor のメッセージの API: Keiland の system protocol の拡張（WS156・WS169 と同じ作り）。backend（モデム・スマホの bridge・VoIP・chat の API）の plug-in の口と、app への事象（受信・送信の状態・着信）。
- スマホの bridge の protocol（IP の上、認証と暗号、家の LAN と外から）。スマホ側の app は別の WS（将来）。
- タイムラインの UI: 連絡先の一覧 → 連絡先のタイムライン（メッセージの吹き出し・通話の記録・添付）、送信の欄と電話の button（最初は backend が無い旨を出す）。
- 試験: 試験用の偽の backend（タイムラインに出す data）。

## Phase

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| p000 | **UI の mock（優先、夕方の UAT が目安・必須でない）**: Phone の app の外側だけ。連絡先の一覧 → 連絡先ごとの iOS のメッセージ風のタイムライン（吹き出し・通話の記録・添付の表示）、送信の欄と電話の button（押すと「backend が無い」）。data は app の中の固定の試験 data。保存・API・backend は作らない | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-177b の撮影の後、p002〜p004 で実装・cleared。旧: in-progress（q743、P2。実装・host の PNG・build 済み、QEMU は T1 待ち。[phase](phase0…） | — |
| p001 | 要件と設計（保存の形・API・backend の口・UI。世界観の全体と最初の範囲を分ける） | cleared（q831、P2、[phase](phase001/phase.md)） | — |
| p002 | 連絡先と会話の保存（`~/Documents/`）、host の試験 | cleared（2026-10-07、T1-298） | p001 |
| p003 | Phone の app: 連絡先の一覧とタイムラインの表示（保存から、送受信と通話は p004 の口で） | cleared（2026-10-07、T1-298） | p002・p004 |
| p004 | compositor のメッセージの API の骨格と偽の backend | cleared（2026-10-07、T1-298） | p001 |
| 後（別の Phase、範囲の外） | 本物の SMS・MMS・RCS の送受信、モデム、スマホの bridge、VoIP、chat の API の統合。Bluetooth のスマホの bridge（SMS の MAP、連絡先の PBAP、通話の HFP）の層の流れと interface は [WS197 p004](../ws197/phase004/phase.md)（2026-10-10 ユーザーの指示で P1 が確定） | — | — |
| p005 | 全文規約の見直し（最初の範囲） | planning | p002〜p004 |

## スマホ連携の SMS の流れ（2026-10-10、WS197 p004）

本物の SMS（Bluetooth の MAP でスマホ経由）の層の流れ（Phone の app → libkeiland → compositor → libkeiland-backend → bluetoothd）、各境界の interface、保存の持ち主（Phone の app だけ）、同期（pull、目印は app）、WS170 の store・app・compositor・libkeiland の変更の一覧は [ws197-p004](../ws197/phase004/phase.md) が正。p001 §3 の API の拡張と p002 の store の変更（Source・番号の key の会話・同期の目印）はそこに従う。

## 2026-10-06 UAT のフィードバック

- BUG-218 本体の padding を Settings と同じに、慣性の開始の遅れ。BUG-203・204 の IME と OSK は直し済み・T1-217 待ち

## 2026-10-10 WS197 p004b による変更（Q1）

WS197 p004b（778c1e377）が userland/desktop/phone を変えた: 保存（番号の key は E.164、既定の国番号 81、会話の file、Source ごとの key の index、自分の送信の写しの重ね、知らない header を保つ）と同期（sync/bt-<address>.state の目印、§6.1・§6.2 の同期の手順、送信、既読の handle、`--peer NUMBER`）。設計は [WS197 p004](../ws197/phase004/phase.md) §6。試験は plan/ws197/tests/phone-store-host-test.sh。
