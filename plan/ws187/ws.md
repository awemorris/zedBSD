<!-- awesome-plan project=zedbsd record=ws187 -->

# WS187: ロック画面（大きな時計、スワイプ・ホイールでの解除、認証の方式の選択）

<!-- awesome-plan-current:start -->
Status: planned
Primary Milestone: MG006
Objectives: O2
Parent: [Master](../master.md)
Focused goal: fg019（ベータ2）
Queue: q864（P2 に予約）
Resume point: [p001](phase001/phase.md)
<!-- awesome-plan-current:end -->

## 由来（2026-10-08 ユーザー）

「ロック画面は縦長のディスプレイでもきれいに見えるよう、時計を中央より上に大きく表示してほしいです。」（WS182 の D1 の確認の返事に添えて）

追加（2026-10-08 ユーザー）:「画面の下部（下端でなくてよい）から上にスワイプするとロック解除でき、手動ロックされた場合を除き、ロックから一定時間ならスワイプのみで認証不要、認証する場合はスワイプのあとパスワード、PIN、ハードウェアパスキーが選択できる入力画面。タッチパッドもタッチスクリーンもない場合のために、マウスホイールを上方向に回転でロック解除も実装。」（ws172-p007 の lock の画面の分はここへ移した）

## 到達目標と受け入れ

- lock の画面（compositor、ws035-p102 の lock）に時計を大きく、画面の中央より上に出す。縦長（portrait、例 1080x1920）と横長（1920x1080・5330 の 1920x1080）のどちらでも配置が崩れない（寸法は論理 px、Guardrail の「寸法の単位」）。
- 認証の入力（PIN・password、後の WS172 p007 の方式の選択）は時計と重ならない。
- 解除の操作: 画面の下部（下端に限らない、例えば下 1/3）から上へのスワイプ（touchscreen・touchpad）、または mouse の wheel を上へ回す。
- 猶予: 自動の lock（idle・蓋など）から一定の時間の内は、スワイプ（wheel）だけで解除し認証しない。手動の lock（利用者が lock を選んだ時）は常に認証する。時間の既定は設計で決めてユーザーに確かめる（案: 5 分、Settings で変えられる形は WS148）。
- 認証: 猶予の外は、スワイプの後に入力の画面を出し、Password・PIN・Hardware Key（hardware passkey）を選べる。登録の無い方式は出さない。Hardware Key は sessiond（/sbin/passkey の STYLES）が返した時だけ出す（FIDO2 の key が登録済みで使える時。ws172-p003 の段 A は実装済み、2026-10-08 P2 の指摘で Q1 の「出さない」を訂正）。
- 確認: build warning 0、配置の host 試験（縦長・横長）、QEMU の PNG（縦長と横長）は T1。

## Phase

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | lock の画面の時計の配置と描画（縦長・横長）、host 試験、T1 の PNG | in-progress（q864、P2） | — |
| p002 | 解除の操作（下部から上へのスワイプ・wheel の上）と猶予（自動の lock の後の一定時間は認証なし、手動の lock は常に認証） | planned | p001 |
| p003 | 認証の入力の画面: Password・PIN・Hardware Key の選択（登録の無い方式は出さない、Hardware Key は sessiond が返した時だけ） | planned | p002、PIN は ws172-p002 |
