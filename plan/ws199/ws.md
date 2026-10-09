<!-- awesome-plan project=zedbsd record=ws199 -->

# WS199: Settings のセキュリティキーの独立の頁とウィザード

<!-- awesome-plan-current:start -->
Status: planned
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: q921（P1、2026-10-10）
Target: **ベータ2**（2026-10-10 ユーザー、クリック「ベータ2 に入れる」）
Resume point: p001 から。
<!-- awesome-plan-current:end -->

## 由来（2026-10-10 ユーザーの UAT）

「セキュリティーキーの管理はSettingsに独立ページにしてほしいです。PIN入力もポップアップがいいですね。Add Keyでウィザードが起動、デバイス名をデフォルトの名前にして編集可能にする、PINは入力を促される、PINが設定されていないなら、PIN設定のステップも入れる、PINがわからないときのために初期化もできるようにする、名前の入力もウィザード、確認中や処理中は、ポップアップを操作できない表示にする。セキュリティキーのページには、システムに登録済みのキーの名前一覧がリスト表示。あと、キーの初期化ボタン、PIN変更のボタンもあり。」

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
| p001 | 今の Users の頁の鍵の欄・fidoctl・libpasskey・sessiond の要求の調べ、頁とウィザードの設計（画面の流れ）、実装、host 試験 | planned |
| p002 | T1 の AAT と 5330 の UAT | planning |
