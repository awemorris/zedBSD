<!-- awesome-plan project=zedbsd record=ws187 -->

# WS187: ロック画面の大きな時計（縦長の display でもきれいに）

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

## 到達目標と受け入れ

- lock の画面（compositor、ws035-p102 の lock）に時計を大きく、画面の中央より上に出す。縦長（portrait、例 1080x1920）と横長（1920x1080・5330 の 1920x1080）のどちらでも配置が崩れない（寸法は論理 px、Guardrail の「寸法の単位」）。
- 認証の入力（PIN・password、後の WS172 p007 の方式の選択）は時計と重ならない。
- 確認: build warning 0、配置の host 試験（縦長・横長）、QEMU の PNG（縦長と横長）は T1。

## Phase

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | lock の画面の時計の配置と描画（縦長・横長）、host 試験、T1 の PNG | planned | — |
