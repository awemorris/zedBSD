<!-- awesome-plan project=zedbsd record=ws192 -->

# WS192: 右上の状態の島をタップで開く glass の操作パネル（タブレット向け）

<!-- awesome-plan-current:start -->
Status: planned
Primary Milestone: MG006
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: q917（P1、2026-10-09）
Target: **ベータ2**（RC 10/13。2026-10-09 ユーザー、クリック「ベータ2（RC 10/13 まで）」）
Resume point: p001 から。
<!-- awesome-plan-current:end -->

## 由来（2026-10-09 ユーザー）

「右上の通知アイコン領域は、タブレットでは個別のアイコンのタッチが難しかったです。そこで、通知アイコンの島をクリックやタッチすると、ポップアップが画面右上に表示されて、そこに大きめのメニューで、WiFiボタン、音量スライダー、IMEアイコン、などを表示して、操作可能にしたいです。ポップアップはglassエフェクトがいいです。」

## 目標

- 右上の状態の icon の島（WiFi・音量・IME・電池・Bluetooth など）を click・tap すると、画面の右上に操作パネルの popup が開く。
- popup は glass の effect（既存の glass の描画を再利用）で、指で押せる大きさの項目を並べる: WiFi のボタン（on/off と一覧への入口）、音量の slider、IME の切り替え、その他の既存の icon の操作（Bluetooth・電池の表示など、今の島にある物）。
- 外を tap・Esc・もう一度島を tap で閉じる。マウスでも同じに使える。
- 個別の icon の今の動作（click の popup 等）との関係を p001 で決める（既定案: 島の tap はパネルを開くに統一し、パネルの中から各機能へ）。

## 完了の条件

- QEMU の AAT（touch と mouse）でパネルの開閉と各項目の操作が効く。PNG をユーザーに見せる。
- 実機（5330 の touch）での確認はユーザーの UAT。
- 変えた C の規約の全文の見直し（最後の Phase）。

## Phase

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 今の島・各 icon の popup・glass の描画の調べと、パネルの設計（項目・寸法・開閉・入力）と実装・host 試験 | planned | — |
| p002 | AAT（T1）とユーザーの UAT の依頼 | planning | p001 |
| p003 | 規約の全文の見直し | planning | p001 |
