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
| **10/16** | 最終の image・配布物・license の一覧・release notes を確定。Vulkan Video は T1-435 が PASS なら ON、でなければ OFF。公開の手順の確認（公開はユーザーの指示で） |

## 今の状況

- 2026-10-10 の UAT は一通り済んだ。残る既知の Bug は USB LAN の遅さ（BUG-222、最悪ベータ2 では遅くてよい）。
- 順（ユーザー）: 走っている Bug の直しと試験を全部終える → WS199（Vulkan Video より優先）→ WS200。
- P1: WS199 の設計を新しい仕様で書き直し design-reviewer に通している（code は試験が片付いてから）。
- T1: T1-521（ロック画面・Ethernet の頁）・T1-522（PIN の登録の後の解除）は PASS（BUG-283・284・285 は QEMU で確認、実機は次の UAT）。今 T1-435（Vulkan Video の 5330、5330 は使えない）。
- WS197（Bluetooth のスマホ連携）はベータ3、10/17 まで main に入れない branch で止めてある。

## 必須

| 項目 | 状態 | LW | 担当 |
| --- | --- | --- | --- |
| [WS199](ws199/ws.md) セキュリティキーの管理の頁（Software Security Key を含む）とログイン画面のキーの自動のログイン | 設計の書き直しと review の後に実装（走っている試験は済んだ） | 26 | P1・T1 |
| [BUG-286](bugs/BUG-286.md) NFC の YubiKey で登録と login が失敗（passkey-fido2 が USB の鍵だけを開く） | WS199 の中で | 3 | P1・ユーザー |
| [WS200](ws200/ws.md) Users の頁のパスワード変更のウィザードと認証方式の選択 | WS199 の後 | 6 | P1・T1 |
| [WS083](ws083/ws.md) Vulkan Video（H.264） | release の config は OFF。T1-435（5330）が PASS なら ON の 1 行。直しは WS199 の後 | 2 | T1・P1 |
| [WS129](ws129/ws.md) p005・p013 release notes・既知の問題・利用の手引き | 下書き済み（[notes](../docs/release/zedbsd-1.0.0-beta2.md)・[known issues](../docs/release/zedbsd-1.0.0-beta2-known-issues.md)・[guide](../docs/release/zedbsd-1.0.0-beta2-guide.md)）。**ユーザーの review 待ち**。WS199・WS200 の機能を足し、RC で review の comment を消す | 1.5 | ユーザー・P1 |
| [WS129](ws129/ws.md) p006 最終回帰（release の image） | 10/14 | 3 | T1 |
| [WS129](ws129/ws.md) p008 公開の準備（tag・CI・配布物） | 手順は用意済み。10/16、公開はユーザーの指示 | 0.5 | Q1・P1 |
| 次の UAT で出る Bug の枠 | — | 5 | P1 |
| **計** | | **約 47.5 LW**（約 16 時間） | |

## 次の UAT で確認してほしい事項（5330、WS199・WS200 の後の image）

| # | 項目 | 手順 | 期待 |
| --- | --- | --- | --- |
| 1 | [BUG-283](bugs/BUG-283.md) ロック画面の button | PIN か鍵を登録した状態で lock | Password・PIN・Security Key の button が高く押しやすい |
| 2 | [BUG-284](bugs/BUG-284.md) Ethernet の頁 | Settings → Network → Ethernet | wlan0 が出ず、ue0 だけが Connected |
| 3 | [BUG-285](bugs/BUG-285.md) PIN の登録の直後 | autologin のまま Settings で PIN を登録 → lock | lock の画面に PIN が出て PIN で解除できる |
| 4 | [WS199](ws199/ws.md) セキュリティキーの頁 | Settings → Security Keys で鍵の一覧、Add Key のウィザード（名前・PIN の設定・初期化・PIN の変更）、Software Security Key（今の PIN） | ウィザードで登録・PIN の変更・初期化ができる。処理中は操作できない表示 |
| 5 | WS199 ログイン画面のキー | 鍵を挿す（(a) 既定、(b)「PIN 不要」、(c)「PIN 不要」＋「タッチ不要」の設定で） | 自動で鍵のモードとその鍵の user に。(a) PIN（欄の下に OSK）とタッチ、(b) タッチだけ、(c) 「確認中」の後に最低 0.5 秒「確認した」を出してデスクトップへ |
| 6 | WS199 ロック画面のタッチ不要 | (c) の設定で lock。鍵を挿したまま／抜いて挿し直す | 挿したままならタッチを促す。lock の後に挿した鍵ならタッチ無しで解除 |
| 7 | WS199 設定の変更 | 「PIN 不要」「タッチ不要」を入れる | パスワードを求め、「鍵を持つ人は誰でもログインできる」の警告が出る |
| 8 | [WS200](ws200/ws.md) Users の頁 | Change Password のウィザード、Sign-in Methods の Password・PIN・Security Key | パスワードを変えられる。外した方式は lock・greeter に出ない。console・SSH は password のまま |
| 10 | [BUG-286](bugs/BUG-286.md) NFC の YubiKey | Settings で NFC の reader に当てて登録、NFC のタッチで login・解除 | USB と同じに使える |
| 9 | [BUG-222](bugs/BUG-222.md) USB LAN の速さ | 別の PC から ue0 経由で大きい file を scp | 速さを教えてください（10/06 は 950 KB/s、TCP の直しの後の値） |

## 既知の問題に書いて出す（ベータ3 以降）

| 項目 | 理由 |
| --- | --- |
| BUG-222（USB LAN の遅さ、直らなければ）、BUG-280（App Home への遷移の fps）、BUG-217（最大化の session の状態）、BUG-223（動画の全画面）、BUG-205（太字の font） | 設計の変更・調べが要る |
| BUG-255（蓋を閉じた間の HDMI）、BUG-159（電池で 5 fps）、BUG-145（AX211 の DHCP）、BUG-165（5330 の DSDT） | 調査が長い・実機の時間が要る |
| WS201（/home の暗号化）、WS197（Bluetooth のスマホ連携）、WS195（/opt/keiland）、WS196（useradd 等）、WS198（self-build）、[WS001 p045](ws001/phase045/phase.md) POSIX の header、[WS126](ws126/ws.md) Python | ベータ3 の列 |
| 規約の全文の見直しの Phase（各 WS） | ユーザーの決定でベータ3 |

## 運用

- 凍結の目標の 10/13 の後も、ベータなので UAT の Bug の直しは 10/16 の準備に間に合う範囲で続ける。新しい仕様の変更は「ベータ3 に回すか」を Q1 がユーザーに聞く。
- Q1 は進むたびにこの表を更新し、完了した項目を消す（2026-10-09 ユーザー「都度、beta2.mdを更新していただけると、進捗がわかって助かります」）。
