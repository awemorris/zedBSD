<!-- awesome-plan project=zedbsd record=beta2-triage -->

# ベータ2 の残り作業（2026-10-10 Q1 更新）

公開は 10/17（OSC 当日、朝から会場）なので、**10/16 中にリリースの準備を終える**。
体制: P1（実装・debug）＋T1（QEMU と 5330 の試験）＋ユーザー（UAT・判断）。この表は計画で、Queue の承認ではない。
見積もりは LW（1 LW ≈ エージェントの実時間 20 分）。完了した項目は消す。Q1 は進むたびに更新する。

## 日程

| 日 | やること |
| --- | --- |
| 10/10〜10/12 | 走っている Bug の直しと試験を終える → WS199（セキュリティキー）→ WS200 |
| **10/13** | 機能の凍結の目標。ベータなので UAT の Bug は直し切れなくてよく、code freeze はぎりぎりまで行わないこともある |
| 10/14〜10/15 | 最終回帰（QEMU）と 5330 の UAT、出た Bug を「直す／既知の問題に書く」で仕分け |
| **10/16** | 最終の image・配布物・license の一覧・release notes を確定。Vulkan Video は既定で ON（2026-10-11 ユーザー）。公開の手順の確認（公開はユーザーの指示で） |

## 今の状況

- 2026-10-11: [BUG-287](bugs/BUG-287.md)（WS197 のスマホ連携が「Use as phone」の後に「Phone messages connecting...」のまま）を P1 が調べている。5330 で btsnoop を取った（pairing は 2 回目で PAIRED だが `l2cap=0`、その後 MAP の接続が進まない）。P1 の直しが来たら merge し、次の UAT の #11 で確かめる。
- WS199・WS200 は実装と QEMU の試験が済み、残りは次の UAT（#4〜#8）。
- WS083（Vulkan Video）は既定で ON にした（2026-10-11 ユーザー）。残りは p007 の hang の実機の確かめ。libavcodec なしで libmedia だけで mp4（H.264・AAC）と .m4a を再生する [WS202](ws202/ws.md) は設計だけを書き、実装は別のセッション。
- T1 の WS143 の HID の回帰はもう流さない（ユーザー）。P2 は WS083 を終えて退いた。
- 残る既知の Bug は USB LAN の遅さ（BUG-222、4.9 MB/s）。

## 必須

| 項目 | 状態 | LW | 担当 |
| --- | --- | --- | --- |
| [WS199](ws199/ws.md) セキュリティキーの管理の頁（Software Security Key を含む）とログイン画面のキーの自動のログイン | p001〜p004 を merge（host 試験・Linux の build まで）。T1-523（2026-10-10）: security-keys・passkey-p002・wheel-card・FreeBSD の build PASS。FAIL: key-keypad の 6（login の log に key owner の行が無い）、fido2-p003 の step 3（鍵が無い時に NFC の待ちで reason=timeout になり no-key にならない）→ P1 が直す。p005 は 5330 の UAT（U1〜U13 は ws.md） | 0.5 | P1・T1 |
| [WS197](ws197/ws.md) Bluetooth のスマホ連携（SMS の MAP・通話の HFP・連絡先の PBAP）（2026-10-10 ユーザー「WS197はbeta2.mdで必須に入れておいてください。」） | p001・p002 cleared、p003 MAP は i01〜i07 実装（i06 まで main に merge、i07 は T1-527）、p004 の SMS の interface の設計は cleared（判断 P1〜P8 は推しどおり）。p004a〜c（SMS を Phone の app で）を 2026-10-10 夜に main に merge（bc4c7f9f6、host 試験 PASS、実機は UAT）。次 p005 PBAP → p006・p007 HFP → p008 実機 | 約 64 | P1 |
| [WS200](ws200/ws.md) Users の頁のパスワード変更のウィザードと認証方式の選択 | p001 cleared（QEMU の T1-525・528 PASS）。POSIX（p045）も PASS。Sign-in Methods は Users の頁の card の switch（押すと password の popup） | 0.5 | T1・ユーザー |
| [WS083](ws083/ws.md) Vulkan Video（H.264） | p001〜p006・p008 cleared（実機で全 stream 一致、1080p 相当 1 frame 約 5 ms）。2026-10-11 から既定で ON（ユーザー）。残り: p007 の hang の回復の実機の確かめ（hang の kernel を 1 回置く、手順は phase007、ユーザーの判断） | 0.5 | ユーザー・Q1 |
| [WS129](ws129/ws.md) p005・p013 release notes・既知の問題・利用の手引き | 下書き済み（[notes](../docs/release/zedbsd-1.0.0-beta2.md)・[known issues](../docs/release/zedbsd-1.0.0-beta2-known-issues.md)・[guide](../docs/release/zedbsd-1.0.0-beta2-guide.md)）。**ユーザーの review 待ち**。WS199・WS200 の機能を足し、RC で review の comment を消す | 1.5 | ユーザー・P1 |
| [WS129](ws129/ws.md) p006 最終回帰（release の image） | 10/14 | 3 | T1 |
| [WS129](ws129/ws.md) p008 公開の準備（tag・CI・配布物） | 手順は用意済み。10/16、公開はユーザーの指示 | 0.5 | Q1・P1 |
| 次の UAT で出る Bug の枠 | — | 5 | P1 |
| **計** | | **約 88 LW**（約 29 時間、うち WS197 が約 75） | |

## 次の UAT で確認してほしい事項（5330、WS199・WS200 の後の image）

| # | 項目 | 手順 | 期待 |
| --- | --- | --- | --- |
| 4 | [WS199](ws199/ws.md) セキュリティキーの頁 | Settings → Security Keys で鍵の一覧、Add Key のウィザード（名前・PIN の設定・初期化・PIN の変更）、Software Security Key（今の PIN） | ウィザードで登録・PIN の変更・初期化ができる。処理中は操作できない表示 |
| 5 | WS199 ログイン画面のキー | 鍵を挿す（(a) 既定、(b)「PIN 不要」、(c)「PIN 不要」＋「タッチ不要」の設定で） | 自動で鍵のモードとその鍵の user に。(a) PIN（欄の下に OSK）とタッチ、(b) タッチだけ、(c) 「確認中」の後に最低 0.5 秒「確認した」を出してデスクトップへ |
| 6 | WS199 ロック画面のタッチ不要 | (c) の設定で lock。鍵を挿したまま／抜いて挿し直す | 挿したままならタッチを促す。lock の後に挿した鍵ならタッチ無しで解除 |
| 7 | WS199 設定の変更 | 「PIN 不要」「タッチ不要」を入れる | パスワードを求め、「鍵を持つ人は誰でもログインできる」の警告が出る |
| 8 | [WS200](ws200/ws.md) Users の頁 | Change Password のウィザード、Sign-in Methods の card の switch（Password・PIN・Security Key、押すと password の popup） | パスワードを変えられる。外した方式は lock・greeter に出ない。console・SSH は password のまま |
| 11 | [WS197](ws197/ws.md) スマホの SMS（Android） | 手順は ws197/ws.md の「5330 の UAT の手順」: Settings → Bluetooth でスマホを pairing して「Use as phone」→ Phone の app で受信・送信・同期、app を閉じている間の通知 | SMS が送受信でき、履歴が同期される |
| 9 | [BUG-222](bugs/BUG-222.md) USB LAN の速さ | 2026-10-10 は 4.9 MB/s（前は 950 KB/s） | 既知の問題に書くか close はユーザー |

## 既知の問題に書いて出す（ベータ3 以降）

| 項目 | 理由 |
| --- | --- |
| BUG-222（USB LAN の遅さ、直らなければ）、BUG-280（App Home への遷移の fps）、BUG-217（最大化の session の状態）、BUG-223（動画の全画面）、BUG-205（太字の font） | 設計の変更・調べが要る |
| BUG-255（蓋を閉じた間の HDMI）、BUG-159（電池で 5 fps）、BUG-145（AX211 の DHCP）、BUG-165（5330 の DSDT） | 調査が長い・実機の時間が要る |
| WS201（/home の暗号化）、WS195（/opt/keiland）、WS196（useradd 等）、WS198（self-build）、[WS001 p045](ws001/phase045/phase.md) POSIX の header、[WS126](ws126/ws.md) Python | ベータ3 の列 |
| 規約の全文の見直しの Phase（各 WS） | ユーザーの決定でベータ3 |

## 運用

- 凍結の目標の 10/13 の後も、ベータなので UAT の Bug の直しは 10/16 の準備に間に合う範囲で続ける。新しい仕様の変更は「ベータ3 に回すか」を Q1 がユーザーに聞く。
- Q1 は進むたびにこの表を更新し、完了した項目を消す（2026-10-09 ユーザー「都度、beta2.mdを更新していただけると、進捗がわかって助かります」）。
