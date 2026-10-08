<!-- awesome-plan project=zedbsd record=ws169 -->
# WS169: メーラの app と compositor のメールの API

Status: incomplete（2026-10-08 q910 P2 の照合: p002〜p005 cleared（T1-298・302）、p000（mock）は T1-181 の撮影の後に p002 以降で実装され Q1 の判定、p006 は作らない、p007（規約）はベータ3。準正常系は WS177 の案 N）（2026-10-07 q831 で p001〜p005 を実装、T1 の QEMU 待ち。p006 は今回作らない、p007 は規約の見直し（後回し）。2026-10-05 追加。まず UI の mock（p000）を優先、q744。段は未定（ユーザーに確認）、見積もり 6 LW（Q1 の概算: 設計 1・API 1・IMAP/SMTP 2・app 2。Gmail・Outlook は別に））
Master: [master](../master.md)
Primary Milestone: MG006

## 由来

ユーザー（2026-10-05）「[メール] - メーラappを追加する - コンポジタにメールAPIを追加する - メーラappはコンポジタに受信を連絡する - 許可されたアプリはコンポジタからメール受信の通知を受け取れる - たとえばブラウザは認証メールの通知を受け取って自動入力できる - バックエンドはIMAP4とSMTPをまずは対応 - GmailとOutlookのバックエンド対応が目標」

## 優先（2026-10-05）

ユーザー（2026-10-05）「WS169も、夕方のUATにあるとうれしいですが、必須ではないです。」

## 目標（ユーザーの要件）

1. メーラの app を追加する。
2. compositor にメールの API を追加する。メーラは受信を compositor に知らせる。
3. 許可された app は compositor からメールの受信の通知を受け取れる（例: browser が認証のメールの通知を受けて code を自動で入力する）。
4. backend はまず IMAP4 と SMTP。目標は Gmail と Outlook の backend（OAuth2 の認証を含む）。

## 設計で決めること（p001）

- compositor の API の形: Keiland の system protocol の拡張（WS156 の通知の object と同じ作り、same-uid だけ）。受信の事象に何を載せるか（差出人・件名・本文の一部・認証 code の候補の抽出を誰がするか）。
- 許可の仕組み: どの app が受信の通知を受け取れるか（利用者が Settings で app ごとに許す、既定は拒否）。秘密（本文・code）の扱いと log に出さないこと。
- 認証 code の自動入力: browser の側の UI（候補を出して利用者が選ぶか、自動で入れるか）。WS156 の通知との関係。
- backend: IMAP4（IDLE で push、TLS は OpenSSL の package）、SMTP（submission、STARTTLS）。account と password・token の保存（WS163・WS162 の秘密の保存との整合）。Gmail・Outlook は OAuth2（XOAUTH2）で、browser を使う認可の流れ。
- メーラの app: 一覧・読む・書く・返信・添付、local の保存の場所（WS170 の「~/Documents/ は cloud backed」との整合）。
- 外部の package の要否（libetpan などか自前か）と license の監査。

## Phase

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| p000 | **UI の mock（夕方の UAT にあると嬉しい・必須でない）**: メーラの app の外側だけ。account・folder の一覧、メールの一覧、読む画面、書く画面（送信は「backend が無い」）。data は固定の試験 data。API・backend は作らない | in-progress（q744、P2。実装・host の PNG・build 済み、QEMU は T1 待ち。[phase](phase000/phase.md)） | — |
| p001 | 要件と設計（API・許可・backend・app） | cleared（q831、P2、[phase](phase001/phase.md)） | — |
| p002 | compositor のメールの API と許可（host の試験） | cleared（2026-10-07、T1-298） | p001 |
| p003 | IMAP4・SMTP の backend | cleared（2026-10-07、T1-298） | p001 |
| p004 | メーラの app（一覧・読む・書く） | cleared（2026-10-07、T1-298） | p002・p003 |
| p005 | browser の認証 code の自動入力 | cleared（2026-10-07） | p002・p004 |
| p006 | Gmail・Outlook（OAuth2） | planning（2026-10-06 夜 ユーザー「今は IMAP/SMTP だけ」: 今回は作らない、ベータ2 から外す） | p003 |
| p007 | 全文規約の見直し | planning | 上の全部 |
