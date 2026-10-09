<!-- awesome-plan project=zedbsd record=ws199 -->

# WS199: セキュリティキーの管理の頁（Software Security Key を含む）と、ログイン画面のキーの自動のログイン

<!-- awesome-plan-current:start -->
Status: planned
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: q921（P1、2026-10-10）
Target: **ベータ2**（2026-10-10 ユーザー。走っている Bug と試験の後に着手）
Resume point: p001 から。
<!-- awesome-plan-current:end -->

## 由来（2026-10-10 ユーザーの UAT）

「セキュリティーキーの管理はSettingsに独立ページにしてほしいです。PIN入力もポップアップがいいですね。Add Keyでウィザードが起動、デバイス名をデフォルトの名前にして編集可能にする、PINは入力を促される、PINが設定されていないなら、PIN設定のステップも入れる、PINがわからないときのために初期化もできるようにする、名前の入力もウィザード、確認中や処理中は、ポップアップを操作できない表示にする。セキュリティキーのページには、システムに登録済みのキーの名前一覧がリスト表示。あと、キーの初期化ボタン、PIN変更のボタンもあり。」

## 仕様の変更（2026-10-10 ユーザー、ベータ2）

「Settingsのページとログイン画面の変更は、やりましょう。USB LAN以外のバグがないからです。」
- ログイン画面で、キーが刺さっていれば自動でキーでのログインモードになる。
- Settings に「ログインにキーの PIN 入力を不要」「ログインにキーのタッチを不要」のチェック。
- キーが刺さっていてタッチ不要なら、キーチェック中のメッセージを出し、完了すればデスクトップへ。キーの確認を最低 0.5 秒表示してから遷移。
- キーが刺さっていて PIN 不要なら、タッチを促す。PIN が必要なら、PIN 入力とタッチを促す。PIN 入力はオンスクリーンキーボードを入力欄のすぐ下に表示。
- Settings にセキュリティーキー管理のページ。物理デバイスの無い今の PIN（/etc/passkey）も残し、同じページで「Software Security Key」として管理。
- passkey と fidoctl は今の設計のまま（ユーザー「現在のあなたの設計がすぐれているので、そのままにします」）。

決定（2026-10-10 ユーザー、クリック、全部推し）:
- ロック画面の「タッチ不要」は、ロックの後に挿したキーだけ（挿しっぱなしならタッチを促す）。
- 組み合わせは 3 通り: PIN＋タッチ（既定）・タッチだけ・どちらも不要（「タッチ不要」は「PIN 不要」が前提）。
- ログイン画面でキーを挿すと、キーの credential の持ち主のユーザーを自動で選ぶ。
- 「PIN 不要」「タッチ不要」を入れる時はパスワードを求め、「キーを持つ人は誰でもログインできる」の警告を出す。
- console・su・sudo・SSH は password だけのまま。

着手の順（ユーザー「現在走っているバグ修正、テストをすべて終わらせてから、セキュリティキーの新規実装に移ります。」）: 走っている Bug の直しと T1 の試験が全部終わってから。

## 設計の review の後の決定（2026-10-10 ユーザー）

- 「Loginはタッチが必要。Unlockはタッチ不要。Unlockでは、スライドしないと認証画面に入れないので、キーが刺さったままでも自動認証される問題はない。」→ 「タッチ不要」は lock の解除だけに効き、login（greeter）は常にタッチが要る。lock では「lock の後に挿した鍵」の判定（復帰の USB の数え直しの除外）は作らない（前の決定を置き換える）。挿したままの鍵でも、スライドで認証の画面に入った後はタッチ無しで解除される（Q1 の注: 鍵を挿したままにすると、機械の前の人は誰でもスライドで解除できる。設定の警告にこれを書く）。
- PIN の入力: greeter が PIN の欄のすぐ下に小さな keypad（数字＋ABC）を描く（推し）。
- 範囲:「全部ベータ2」（自動の鍵のモード・PIN 不要・タッチ不要も含む。5330 の YubiKey で鍵の振る舞いを先に確かめる）。
- 他の推しは「全部推しどおり」: WS200 の Sign-in Methods と PIN 不要・タッチ不要を /etc/passkey の 1 行の設定にまとめ両方の頁が読む、鍵の PIN の変更は鍵の PIN だけで password 無し、Reset で消えた登録はこの機械からも消す、Add の password は最初に聞く、独立の頁。

## R3 の決定（2026-10-10 ユーザー、クリック「置きっ放しもタッチ」）

NFC の reader に載せたままの鍵も、そのまま「タッチ」と見なす（当て直しを求めない）。Q1 の注: reader に鍵を置いておくと、PIN を求めない設定の人は誰も触らずに login・解除できる。Settings の警告と release notes に書く。

## 目標

- Settings に「Security Keys」の独立の頁（今は Users の頁の中の欄）。
- 頁: 登録済みの鍵の名前の一覧、Add Key、鍵の初期化（Reset）、PIN の変更の button。
- Add Key のウィザード（popup）: 鍵を挿す → 名前（既定の名前、編集可）→ 鍵に PIN が無ければ PIN の設定の step → PIN の入力 → 触れる → 完了。PIN が分からない時の初期化への案内。
- 確認中・処理中は popup を操作できない表示（busy）。
- 初期化は鍵の全部の credential を消す（CTAP2 authenticatorReset、挿してから数秒の内・触れる、の制約を案内）。警告と確認。
- 認証の経路は今の sessiond → /sbin/passkey・passkey-fido2（docs/architecture/security.md）を保つ。Settings は特権を持たない。

## 完了の条件

- QEMU の AAT（鍵は QEMU の CTAP2 の模擬か passkey の試験の道具）で頁とウィザードの流れ、PNG をユーザーに。
- 5330 の UAT（YubiKey）。

## Phase

| Phase | 目的 | Status |
| --- | --- | --- |
| p001 | 調べ・設計（第 4 版、review-1〜3）と実装 i01〜i06（i01 popup と頁、i02 NFC（BUG-286）、i03 KEYINFO・KEYPIN・KEYRESET と Insert/Set PIN/Change PIN/Reset、i04 options と auth-fido2、i05 KEYOWNER と greeter・lock の鍵のモードと keypad、i06 試験と T1）、約 23 LW。2026-10-10: i01（cfa5351a1）と i02（0b44c7008）を main に merge | in-progress |
| p002 | T1 の AAT と 5330 の UAT | planning |
