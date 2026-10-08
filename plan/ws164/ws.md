<!-- awesome-plan project=zedbsd record=ws164 -->
# WS164: OS の起動時の Welcome の画面

Status: incomplete（2026-10-08 q910 P2 の照合: p002 cleared（T1-269、表を直した）、p001（設計、H1〜H4 決定済み）は Q1 の判定、p003（規約）はベータ3。実機は UAT。旧: planning（2026-10-05 追加、**ベータ2**（2026-10-05 ユーザー）、見積もり 1 LW。p001 の設計の第 1 版あり））
Master: [master](../master.md)
Primary Milestone: MG006

## 由来

ユーザー（2026-10-05）「OS起動時のWelcome画面（すでにあるStartでもいいかも。要検討）」

## 範囲（案、p001 の設計で確定する）

最初の起動（または毎回）の Welcome の画面。既存の Start（Files の Today など）で足りるかの検討から始める。

## Phase

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws164-p001](phase001/phase.md) | 要件と設計 | planning（設計の第 1 版、2026-10-05 P1 q732。2026-10-05 夜 H1〜H4 決定済み、cleared の判定は Q1） | — |
| [ws164-p002](phase002/phase.md) | 設定の key・compositor の起動・Settings の welcome の mode・host 試験・T1 | cleared（2026-10-06 Q1 判定: T1-269 の QEMU で期待どおり。実機は UAT）（2026-10-08 q910 P2 の照合で phase.md に合わせた。旧: in-progress（2026-10-06 q821 P2: Settings の側と compositor の起動を実装、host PASS。QEMU は …） | p001、H1〜H4 |
| ws164-p003 | 全文規約の見直し | planned | p002 |
