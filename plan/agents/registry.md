# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P1 | kernel-and-driver-designer | ws291 p026 RTX の push buffer の設計 (q1838-i01) | `/home/awe/zedBSD-worktrees/p1-ws291p026` / `agent/p1-ws291p026` | in-progress（2026-11-01 20:2x 起動、10.0.10.3） | — |
| P2 | phase-runner-high | ws291 p020 VF2 の model の 50 fps (q1839-i01) | `/home/awe/zedBSD-worktrees/p2-ws291p020` / `agent/p2-ws291p020` | in-progress（2026-11-01 20:2x 起動、VF2） | — |
| P3 | phase-runner-high | ws291 p021 Pi 5 の model の 50 fps (q1840-i01) | `/home/awe/zedBSD-worktrees/p3-ws291p021` / `agent/p3-ws291p021` | in-progress（2026-11-01 20:2x 起動、Pi 5） | — |
| P4 | phase-runner-high | BUG-387 (q1841-i01 返却 9616367f5、次は q1842 板の試験) | `/home/awe/zedBSD-worktrees/p4-bug387` / `agent/p4-bug387` | 板の窓を待つ | — |

- 2026-11-01 20:20 の割り振り（ユーザーの指示）: P1 は RTX、P2 は VF2、P3 は Pi 5 の作業（どれも WS291 の描画の速さ、Q1 が選んだ）。Bug の修正は P4。
- 実機: 10.0.10.3（RTX 2070、SwitchBot 1 AC276E42B252）は P1、VF2（10.0.30.2、SwitchBot 3 AC276E432AE6）は P2、Pi 5（10.0.30.7、SwitchBot 2 14C19FA80F86）は P3。P4 の BUG-387 の実機の試験は、Q1 が P2・P3 から窓を取って渡す。
- WS074 は Claude のクラウドのセッションが担当（2026-11-01 ユーザー）。成果は origin/browser にあり、Q1 が main に merge する。P8 は立てない。
- WS335・WS336（Sonnet）・WS334（P7）の pending の Queue（q1835・q1836・q1837）は今回は起動しない。
- 試験の担当 T・ブラウザの B1 は今は立てていない。
