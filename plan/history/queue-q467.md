<!-- awesome-plan project=zedbsd record=queue-history q467 -->

# Queue q467: session の残した object の解放（ws031-p050）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-26）
Active Queue: q467
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-26 ユーザー「残っている解放漏れを対処してください。」

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q467-i01 | [ws031-p050](../ws031/phase050/phase.md) | cleared（close で残った object を解放。実機で殺した client と閉じた mview の後も zwl が合成） |

依存: ws035-p067（cleared）。

Upcoming Work Outlook: ws035-p068（zdesktop-terminal）、ws035-p069（App Home の PoC）。どちらも 2026-09-26 のユーザーの指示。
