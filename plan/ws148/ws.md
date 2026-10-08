<!-- awesome-plan project=zedbsd record=ws148 -->

# WS148: Settings の Privacy の頁の検討（要らなければ削除、要るなら設計と実装）

<!-- awesome-plan-current:start -->
Status: completed（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.2）。旧: incomplete）
Primary Milestone: MG006
Parent: [Master](../master.md)
Queue: なし
<!-- awesome-plan-current:end -->

## 結果（2026-10-08 完了）

2026-10-06 ユーザーの決定 (a): Settings の Privacy の頁を削除し、最近の履歴の口（Files の Clear Recents、Storage の Keep recent items）を実装（ws148-p002、T1-271）。

## 制限・移管

run-host-files-recents.sh は WS177 の recents-p008.sh が使うので plan/tools/files/ へ移した（2026-10-08）。

## Phase

| Phase | 内容 | 最終の状態 |
| --- | --- | --- |
| ws148-p001 | 検討（上の観点、他の desktop の調べ、今の実体の有無）と結論の案、ユーザーの判断 | cleared（2026-10-06） |
| ws148-p002 | (a) 頁の削除と、最近の履歴の口（Files の Clear Recents、Storage の Keep recent items） | cleared（2026-10-08） |
| ws148-p003 | (b) の時の実装と回帰 | canceled（2026-10-06） |

Phase の directory とこの WS だけの試験は、完了の規則（AGENTS.md）で削除する（git の履歴に残る、削除は Q1）。
