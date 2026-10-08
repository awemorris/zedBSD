<!-- awesome-plan project=zedbsd record=ws165 -->
# WS165: 手書きの入力

Status: incomplete（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: p001 を cleared。2026-10-08 q910 P2 の照合: p002・p003・p005 cleared（p005 の表を直した）、p001（設計）は Q1 の判定、p004（実機の UAT と規約）は UAT と規約（ベータ3））（2026-10-05 追加、**ベータ2**（2026-10-05 ユーザー）、見積もり 4 LW。p001 の設計の第 1 版あり）
Master: [master](../master.md)
Primary Milestone: MG006

## 由来

ユーザー（2026-10-05）「手書き入力（ゴール設定が難しいので段階化する）」

## 範囲（案、p001 の設計で確定する）

段階化: 段 1 の goal を設計で決める（例: 英数字の単一の文字の認識 → 日本語のひらがな・漢字 → 連続の筆記）。WS102 のスクリーンキーボードの手書きと WS095 の IME との関係を整理する。

## Phase

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws165-p001](phase001/phase.md) | 要件と段の設計（段 1: 1 文字ずつの英数字・かな・記号、点群の照合） | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: H1〜H5 決定済み、p002・p003・p005 が cleared。旧: planning（設計の第 1 版、2026-10-05 P1 q739。判断 H1〜H4 待ち）） | — |
| [ws165-p002](phase002/phase.md) | 手本の data と点群の照合、host 試験 | cleared（2026-10-06 ユーザー H5「いったんacceptして、追加のフェーズを第2段でやりましょう。」: top-1 88%（形が同じ組を 1 字に数えて 91.7%）、top-4 98.3% で受け入れ） | p001、H1〜H3 |
| [ws165-p003](phase003/phase.md) | compositor の `kwl_hand_recognize`、ink の記録、T1 | cleared（2026-10-07、T1-280） | p002 |
| ws165-p004 | 実機の UAT、全文規約 | planned | p003 |
| [ws165-p005](phase005/phase.md) | 第 2 段: 区別できない字の組を分けて認識率を上げる（2026-10-06 Q1 が作成、ユーザー H5） | cleared（2026-10-07 Q1 の判定: T1-318 (c) で osk-guest: PASS（templates count=228、認識、第 1 候補「-」が key で ime-probe に届く）。host の top-1 90.6%）（2026-10-08 q910 P2 の照合で phase.md に合わせた。旧: in-progress（2026-10-07 P2: 書く面の大きさと位置で、そのままの top-1 90.6〜91.7%・top-4 98.4〜98.6%（h…） | p003 |
