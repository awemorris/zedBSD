# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P1 | conformance-reviewer | 返却（q1778 WS326 p025） | `/home/awe/zedBSD-worktrees/p1-ws326p025` / `agent/p1-ws326p025` | 返却（2026-10-31 06:4x） | 181a071ac（merge 1b37841bf） |
| P2 | phase-runner-high | ws309 p015 (q1782-i02) | `/home/awe/zedBSD-worktrees/p2-ws309v2` / `agent/p2-ws309v2` | 返却（2026-10-31 20:23） | 7da994932（merge d8e39d789） |
| P4 | phase-runner-high | ws291 p025 Pi 4 (q1784-i01) | `/home/awe/zedBSD-worktrees/p4-ws291pi` / `agent/p4-ws291pi` | in-progress（2026-10-31 20:20 起動） | — |
| P5 | kernel-and-driver-designer | 返却（q1775 WS302 p001） | `/home/awe/zedBSD-worktrees/p5-ws302` / `agent/p5-ws302` | 返却（2026-10-31 06:4x） | e6d67143f（merge 896d3e3db） |
| P6 | kernel-and-driver-designer | 返却（q1779 WS310 p001） | `/home/awe/zedBSD-worktrees/p6-ws310` / `agent/p6-ws310` | 返却（2026-10-31 07:1x） | 38f8c85eb（merge a2aa89c4f） |
| P3 | phase-runner-high | ws291 p024 RTX 最適化 (q1785-i01) | `/home/awe/zedBSD-worktrees/p3-ws291wsi` / `agent/p3-ws291wsi` | in-progress（2026-10-31 21:04 起動） | 6bb35e399（main） |
| P8 | phase-runner-low | ws074 p260 自走 (q1727-i11) | `/home/awe/zedBSD-worktrees/p8-ws074` / `agent/p8-ws074` | in-progress（2026-10-31 22:51 起動し直し） | 2084d3b0d（merge 11159cbbe） |
| P7 | phase-runner-high | BUG-379 (q1786-i01) | `/home/awe/zedBSD-worktrees/p7-bug379` / `agent/p7-bug379` | 返却（2026-10-31 23:23） | 7300658b2（main） |

- 同時に動かす数は 6（2026-10-29 ユーザーの P1〜P6 の指示）。VF2（主）と 5330（従）は P3、10.0.10.3 は空き（2026-10-30 09:13 P5 返却）。
- P6 の branch（merge しない）: agent/p6-ws296→p6-ws296b（2d17bf9ed から）→p6-ws296c（f31a4954d から）、agent/p6-ws282→p6-ws282b（1190b629c から）→p6-ws282c（c60cdbd44 から）、WS296 は agent/p6-ws296c 1809873bd で全 Phase 実装済み、agent/p6-ws243b（飛ばした記録 b13f3dae7）、agent/p6-ws223（未着手）。
- 試験の担当 T・ブラウザの B1 は今は立てていない。

- 10.0.10.3 の時間の枠（plan/tools.md）: 最初の枠は P1（06:20〜07:05）、次は P2（07:05〜07:50）、以後交代（ネットワーク優先）。
