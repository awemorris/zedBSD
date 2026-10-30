# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P1 | phase-runner-high | ws299 (q1744-i01) | `/home/awe/zedBSD-worktrees/p1-ws299e` / `agent/p1-ws299e` | in-progress（2026-10-31 起動、5330 専用） | 6e6c26b8c（merge d26c657c7） |
| P2 | phase-runner-high | Bug の一括 (q1750-i01: BUG-363・344・366・354・328・352) | `/home/awe/zedBSD-worktrees/p2-bugs` / `agent/p2-bugs` | in-progress（2026-10-31 起動、QEMU） | 806ff2ca2（merge 62eeb340e、WS291 p020） |
| P4 | phase-runner-high | Bug の 2 つ目の束 (q1753-i01: BUG-351・349・362・368・359) | `/home/awe/zedBSD-worktrees/p4-bugs` / `agent/p4-bugs` | in-progress（2026-10-31 起動、QEMU） | b2e2993de（merge 54a134140。agent/p4-ws291pi は f2f87ac1b まで未 merge） |
| P5 | kernel-and-driver-designer | ws318 p001 (q1752-i01) | `/home/awe/zedBSD-worktrees/p5-ws318` / `agent/p5-ws318` | in-progress（2026-10-31 起動） | 3f1e3c655（merge 843847185、WS306 p001） |
| P6 | phase-runner-high | q1740 BUG-370 → q1741 WS291 p014 | `/home/awe/zedBSD-worktrees/p6-ws291` / `agent/p6-ws291` | in-progress（2026-10-31 起動） | 93f7ec9d3 |
| P3 | phase-runner-high | q1736 WS307 → q1737 WS331 p006 → q1738 WS332 → q1739 WS326 | `/home/awe/zedBSD-worktrees/p3-seq` / `agent/p3-seq` | in-progress（2026-10-31 起動） | 823ce7bde（merge ccd7bf2d2） |
| P8 | phase-runner-low | ws074 p260 自走 (q1727-i02) | `/home/awe/zedBSD-worktrees/p8-ws074` / `agent/p8-ws074` | in-progress（2026-10-31 起動し直し） | fccca00dc |
| P7 | phase-runner-high | ws333 p003 → p006 (q1754-i01) | `/home/awe/zedBSD-worktrees/p7-ws333` / `agent/p7-ws333` | in-progress（2026-10-31 起動） | cb2f410a4（merge 52aa284ba） |

- 同時に動かす数は 6（2026-10-29 ユーザーの P1〜P6 の指示）。VF2（主）と 5330（従）は P3、10.0.10.3 は空き（2026-10-30 09:13 P5 返却）。
- P6 の branch（merge しない）: agent/p6-ws296→p6-ws296b（2d17bf9ed から）→p6-ws296c（f31a4954d から）、agent/p6-ws282→p6-ws282b（1190b629c から）→p6-ws282c（c60cdbd44 から）、WS296 は agent/p6-ws296c 1809873bd で全 Phase 実装済み、agent/p6-ws243b（飛ばした記録 b13f3dae7）、agent/p6-ws223（未着手）。
- 試験の担当 T・ブラウザの B1 は今は立てていない。

- 10.0.10.3 の時間の枠（plan/tools.md）: 最初の枠は P1（06:20〜07:05）、次は P2（07:05〜07:50）、以後交代（ネットワーク優先）。
