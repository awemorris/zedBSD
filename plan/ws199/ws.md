<!-- awesome-plan project=zedbsd record=ws199 -->

# WS199: セキュリティキーの管理の頁（Software Security Key を含む）と、ログイン画面のキーの自動のログイン

<!-- awesome-plan-current:start -->
Status: completed
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: q921（P1、2026-10-10）
Target: **ベータ2**（2026-10-10 ユーザー。走っている Bug と試験の後に着手）
Resume point: closed（ユーザーの完了・close 指示）。共有 projection と試験資材の整理は Q1 に引き継ぐ。
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

## 2026-10-10 UAT（ユーザー、5330）

「WS199のセキュリティーキーのページ、使えました。」「WS199のログイン画面のキー、使えました。ただし、PINなしとタッチなしはSettingsに項目がなく、PINが必須でした。NFCなのでタッチは不要でした。USBだと現状ではタッチ必須になると思います。Settingsの方の問題です。未実装？よって、タッチ不要は試験できませんでした。」
→ Q1: PIN 不要・タッチ不要の card は p002（77a40b51f、2026-10-10 夜に merge）で、ユーザーの image はその前の main で作られた見込み。次の image で試す。p002 の後の UP の検査の不具合（c786e6896 で直した）も次の image に入る。

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
| [p001](phase001/phase.md) | 調べ・設計（第 4.1 版、review-1〜3）と実装 i01 頁と popup・i02 NFC（BUG-286）・i03 鍵の情報・Set/Change PIN・Reset。i01（cfa5351a1）・i02（0b44c7008・5515a4dab）・i03（d8cb16814、KL_VERSION 77）を main に merge | cleared（実装 main、p004 の最終検証） |
| [p002](phase002/phase.md) | PIN 不要・タッチ不要の設定（options の行、set-options・auth-fido2、radio と警告）＝ i04 | cleared（77a40b51f） |
| [p003](phase003/phase.md) | greeter・lock の鍵のモード、user の自動の選択、0.5 秒、keypad、sleep で card を閉じる ＝ i05 | cleared（2f414c00e、T1-525・528） |
| [p004](phase004/phase.md) | host 試験の残り・style・T1 の AAT を 1 回で ＝ i06 | cleared（T1-523・525・528） |
| [p005](phase005/phase.md) | 5330 の UAT | cleared（ユーザー受け入れ） |

## 5330 の UAT の一覧（p005、ws199-p004 で 2026-10-10 P1）

ユーザーが YubiKey 5 NFC（USB と ACR1552 の NFC）で流す。手順の要約は plan/beta2.md の「次の UAT」の 4〜7。鍵の Insert・Reset・タッチ・KEYOWNER は QEMU に鍵が無いので、ここでしか確かめられない。

| # | 項目 | 手順 | 期待 |
| --- | --- | --- | --- |
| U1 | Add Key（USB、PIN 有り） | Settings → Security Keys → Add Key → password → 鍵を挿す → 名前（既定は鍵の名前か「Security Key」）→ 鍵の PIN → 触れる | Done、一覧に名前。PIN の誤りは password を保ったまま PIN の step へ |
| U2 | Add Key（PIN の無い鍵、NFC） | Reset 直後の鍵を reader に当てて Add Key | 「Set the key's PIN」の step（2 回の入力）→ そのまま登録 |
| U3 | 2 本以上 | 2 本挿して Add Key | 「More than one key is there.」、1 本にして Check Again で進む |
| U4 | Change PIN | Change PIN → 今の PIN → 新しい PIN を 2 回 | Done、新しい PIN で login できる。password は聞かない |
| U5 | Reset Key | 警告 → password → 抜いて挿し直す → 触れる | Done と消えた登録の数、一覧から消える。Touch の間の Cancel では消えない。挿し直しが遅いと「came back too late」 |
| U6 | Remove | 鍵の行の Remove → password | 一覧から消える。最後の鍵を消すと「Sign in with a security key」は「PIN and touch」に戻る |
| U7 | Software Security Key | Set Up PIN（password → 6 桁 2 回）、Change、Remove | lock の画面で PIN が出て解ける |
| U8 | login（USB・NFC） | login の画面で鍵を挿す／当てる | 自動で鍵の持ち主の user と鍵のモード。PIN and touch: 欄の下の keypad で PIN → タッチ。Touch only: タッチだけ。抜くと password の欄へ |
| U9 | unlock の 3 通り | 各設定で Super+L → swipe | PIN and touch: PIN とタッチ。Touch only: タッチだけ。No PIN no touch: 「Checking your security key...」を最低 0.5 秒出して解除 |
| U10 | 設定の変更の警告 | 「Touch only」「No PIN, and no touch to unlock」を選ぶ | password を求め、「鍵を持つ人は誰でも…」、後者は挿したまま・reader に置いたままの警告も |
| U11 | lock 中の sleep | 鍵の card を出したまま（または Settings の鍵の操作中に）lid を閉じる | 待っている問いが取り消されて眠る。起きた後に card は閉じている |
| U12 | NFC の置きっ放し | reader に鍵を置いたまま login・unlock | タッチと見なす（PIN の要る設定では PIN だけ） |
| U13 | console・SSH | console の login・su・sudo・SSH | password だけ（鍵・PIN は出ない） |

見積もり（2026-10-10）: p002 4 LW、p003 6 LW、p004 2 LW。使用量の都合で、p004 の T1 は 1 回にまとめる。

## 完了・close（2026-10-10、ユーザー受け入れ）

- ユーザー「また、WS199をcompleteでcloseしておいてください」を完成受け入れとして記録。Settings頁・Software Security Key・鍵の管理・PIN/タッチ設定、greeter/lockの自動選択とkeypadは main に実装済み。
- 最終検証は p004 の既存証拠（host回帰、48 C の全文規約、named build warning 0、Linux/FreeBSD build、T1-523・525・528）を採用。今回の終了操作で新しい試験を実行したとは主張しない。
- SSHで実機 `ge8adcdf` の sessiond 記録を追加確認: ENROLL fido2 ok、SETOPTIONS password ok 3回、UNLOCK fido2 ok 8回。ユーザーが現在imageで設定変更と解除に成功した証拠。ただしU1〜U13全項目それぞれの実施報告は無く、過去の未実施項目をPASSに書き換えない。
- 同日 GitHub の全issueをページ送りして logical marker・WS199 title を検索、対象issue無し。local記録のcloseを完了し、存在しないremote issueをcloseしたとは主張しない。公開は既存方針どおり保留。
- Q1引き継ぎ: master/Queueのcompleted projection、Phase directory・WS固有試験の整理（外部セッションの削除禁止規則のため本セッションでは削除しない）。継続回帰を残す場合は plan/tools へ移管しToolsに登録。source変更は無い。

## main統合確認（2026-10-10）

実装/検証/完了記録のsource commit `6bca6f2a2`をmainへmerge `bee41dded`で統合した。merge前のmain `5e178dd17`のbeta2.md更新を保持。master/共有Queueは未変更、push/公開は行わない。変更sourceは検証したprivate treeと同一。Q1向けの残り投影/整理は既述どおり。
