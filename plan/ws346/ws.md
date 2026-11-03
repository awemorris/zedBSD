<!-- awesome-plan project=zedbsd record=ws346 -->

# WS346: libbrowser の style と layout の差分の再計算（layout cache）

Status: incomplete
Primary Milestone: MG006
Related Milestones: MG002
Objectives: O1, O2
Parent: [Master](../master.md)
Owner: Q1
Queue: [q2001](../queue.md)（p001 設計）→ [q2002](../queue.md)（p002 から、Q1 が直に実装）

## 目標

DOM が変わるたびに style と layout を document の全体で作り直している今の形（[ws074 design.md §6.5](../ws074/design.md)）を、
**変わった所だけ計算し直す**形に変える。結果（layout の木・描画）は全体を作り直した時と同じであること。
script が DOM を変えては幾何を読む頁（amazon.co.jp、store.google.com）の待ち時間を大きく減らす。

## ユーザーの決定（要点）

- 2026-11-03 06:17 ユーザーから、layout cache を実装したいので設計書を外部設計まで作ってほしい、実装は Sonnet が設計書を元に行う、
  という趣旨の指示を受けた。設計は Q1 が直に行う（同日のユーザーの指示で、このセッションはサブエージェントを使わない）。
- 外部の標準仕様に基づく物ではないので「標準廉価実装手順」の適用の条件（§1 の 1）を満たさない。手順の形（設計の文書・header の
  契約・Phase ごとの phase.md・Sonnet の 1 回の起動で済む大きさ・Q1 の受け入れ）だけを借りる（Q1 の判断）。

## 背景（測定、2026-11-03 Q1）

- amazon.co.jp の 1 回の読み込み（host、`--render --async --follow=4`、uprobe の計数）: 全体の layout（`page_layout`）392 回、
  `layout_build` 522 回、**style の engine の作り直し 508 回**、`css_engine_compute` 82 万回・`cascade_compute` 142 万回、
  `getComputedStyle` 6 万回、幾何の問い 6.4 万回。画面の描画（`view_update`）は 7 回だけ。layout のほぼ全部は script が強いた物。
- 同じ読み込みの CPU（`perf`）: style の cascade 約 22%、JS の VM 約 22%、GC・確保 約 10%、layout と text 約 11%、libc
  （style の struct の copy・sort を含む）約 16%。
- 原因: DOM の変更は document の generation を進め、`page_update_styles` が **engine を作り直して全 element の style の cache を捨てる**。
  続く幾何の問いが `page_layout` で box の木を全部作り直す。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [ws346-p001](phase001/phase.md) | 設計（要求・外部設計・Phase の分割）[design.md](design.md) | cleared（2026-11-03 06:40、ユーザーの決定 3 件） | — |
| [ws346-p002](phase002/phase.md) | DOM の変更の記録・切り替え・計数の土台（振る舞いは変えない） | cleared（2026-11-03、q2002） | p001 |
| [ws346-p003](phase003/phase.md) | 照合（verify）の道具と bench | cleared（2026-11-03、q2003） | p002 |
| [ws346-p004](phase004/phase.md) | selector の特徴の表と変更の影響の範囲（css） | cleared（2026-11-03、q2003） | p002 |
| [ws346-p005](phase005/phase.md) | style の無効化の統合（engine を保つ） | cleared（2026-11-03、q2003） | p003, p004 |
| [ws346-p006](phase006/phase.md) | layout の再利用 A: 幾何の保存・候補の判定・intrinsic の持ち越し | planned | p005 |
| [ws346-p007](phase007/phase.md) | layout の再利用 B: 部分木の接ぎ木 | planned | p006 |
| [ws346-p008](phase008/phase.md) | 統合の試験・速さの測定・既定の判定 | planned | p007 |
| [ws346-p009](phase009/phase.md) | 規約の全文の見直し | planned | p008 |

- 2026-11-03 06:40 ユーザーから、design.md §12 の 3 件（既定は目標を満たしたら on・live の頁を照合の mode で約 20 回・範囲の外は別の WS）を推しのとおりにしてよい、という趣旨の答えを受けた。
- 2026-11-03 06:39 ユーザーから、定義した Phase をこのメインのエージェント（Sonnet 5.5）で実装し、サブエージェントは使わない、という趣旨の指示を受けた。

## Resume point

p002 を実装中（Q1、main の checkout）。p002 → p003・p004 → p005 → p006 → p007 → p008 → p009 の順。
