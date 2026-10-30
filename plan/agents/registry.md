# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P1 | phase-runner-high | ws299 (q1744-i01) | `/home/awe/zedBSD-worktrees/p1-ws299e` / `agent/p1-ws299e` | in-progress（2026-10-31 起動、5330 専用） | 6e6c26b8c（merge d26c657c7） |
| P2 | phase-runner-high | Bug の 4 つ目の束 (q1761-i01: BUG-312・173・238・199・281・308) | `/home/awe/zedBSD-worktrees/p2-bugs4` / `agent/p2-bugs4` | in-progress（2026-10-31 起動、QEMU） | b3aa69fa2（merge 612a2ea04） |
| P4 | phase-runner-high | Bug の 2 つ目の束 (q1753-i01: BUG-351・349・362・368・359) | `/home/awe/zedBSD-worktrees/p4-bugs` / `agent/p4-bugs` | in-progress（2026-10-31 起動、QEMU） | b2e2993de（merge 54a134140。agent/p4-ws291pi は f2f87ac1b まで未 merge） |
| P5 | kernel-and-driver-designer | ws329 p001 (q1758-i01) | `/home/awe/zedBSD-worktrees/p5-ws329` / `agent/p5-ws329` | in-progress（2026-10-31 起動） | e4ce407a1（merge 85fbeb091、WS318 p001） |
| P6 | phase-runner-high | ws333 p005 の前半 (q1762-i01) | `/home/awe/zedBSD-worktrees/p6-ws333` / `agent/p6-ws333` | in-progress（2026-10-31 起動） | 4c6c4de88（merge 39fc879c5、WS332） |
| P3 | phase-runner-high | q1737 WS331 p006（q1736 WS307 は cleared、q1738 は P6 へ、q1739 は canceled） | `/home/awe/zedBSD-worktrees/p3-seq` / `agent/p3-seq` | in-progress（2026-10-31 起動） | 823ce7bde（merge ccd7bf2d2） |
| P8 | phase-runner-low | ws074 p260 自走 (q1727-i02) | `/home/awe/zedBSD-worktrees/p8-ws074` / `agent/p8-ws074` | in-progress（2026-10-31 起動し直し） | fccca00dc |
| P7 | phase-runner-high | ws333 p003 → p006 (q1754-i01) | `/home/awe/zedBSD-worktrees/p7-ws333` / `agent/p7-ws333` | in-progress（2026-10-31 起動） | cb2f410a4（merge 52aa284ba） |

- 同時に動かす数は 6（2026-10-29 ユーザーの P1〜P6 の指示）。VF2（主）と 5330（従）は P3、10.0.10.3 は空き（2026-10-30 09:13 P5 返却）。
- P6 の branch（merge しない）: agent/p6-ws296→p6-ws296b（2d17bf9ed から）→p6-ws296c（f31a4954d から）、agent/p6-ws282→p6-ws282b（1190b629c から）→p6-ws282c（c60cdbd44 から）、WS296 は agent/p6-ws296c 1809873bd で全 Phase 実装済み、agent/p6-ws243b（飛ばした記録 b13f3dae7）、agent/p6-ws223（未着手）。
- 試験の担当 T・ブラウザの B1 は今は立てていない。

- 10.0.10.3 の時間の枠（plan/tools.md）: 最初の枠は P1（06:20〜07:05）、次は P2（07:05〜07:50）、以後交代（ネットワーク優先）。
