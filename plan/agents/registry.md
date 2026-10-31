# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P1 | phase-runner-high | 5330 の計測 (q1788-i01) | `/home/awe/zedBSD-worktrees/p1-ws299e` / `agent/p1-ws299e` | in-progress（2026-11-01 01:02 起動） | — |
| P2 | phase-runner-high | ws309 p015 (q1782-i02) | `/home/awe/zedBSD-worktrees/p2-ws309v2` / `agent/p2-ws309v2` | 返却（2026-10-31 20:23） | 7da994932（merge d8e39d789） |
| P4 | phase-runner-high | ws291 p025 Pi 4 (q1784-i03) | `/home/awe/zedBSD-worktrees/p4-ws291pi` / `agent/p4-ws291pi` | in-progress（2026-10-31 23:45 起動し直し） | 3ab10f5c5（merge d9846754e） |
| P5 | kernel-and-driver-designer | 返却（q1775 WS302 p001） | `/home/awe/zedBSD-worktrees/p5-ws302` / `agent/p5-ws302` | 返却（2026-10-31 06:4x） | e6d67143f（merge 896d3e3db） |
| P6 | kernel-and-driver-designer | ws335 p001 設計 (q1789-i01) | `/home/awe/zedBSD-worktrees/p6-ws335` / `agent/p6-ws335` | in-progress（2026-11-01 01:20 起動） | — |
| P3 | phase-runner-high | ws291 p024 RTX 最適化 (q1785-i02) | `/home/awe/zedBSD-worktrees/p3-ws291wsi` / `agent/p3-ws291wsi` | in-progress（2026-10-31 23:41 起動し直し） | 9cbb0e590（merge 7055ea569） |
| P8 | phase-runner-low | ws074 p260 自走 (q1727-i12) | `/home/awe/zedBSD-worktrees/p8-ws074` / `agent/p8-ws074` | in-progress（2026-11-01 00:16 起動し直し） | 5dadd7966（merge fd4449c07） |
| P7 | kernel-and-driver-designer | ws334 p001 設計 (q1787-i01) | `/home/awe/zedBSD-worktrees/p7-ws334` / `agent/p7-ws334` | 返却（2026-11-01 01:12） | 1deb49bff（merge b2a7e8454） |

- 同時に動かす数は 6（2026-10-29 ユーザーの P1〜P6 の指示）。VF2（主）と 5330（従）は P3、10.0.10.3 は空き（2026-10-30 09:13 P5 返却）。
- P6 の branch（merge しない）: agent/p6-ws296→p6-ws296b（2d17bf9ed から）→p6-ws296c（f31a4954d から）、agent/p6-ws282→p6-ws282b（1190b629c から）→p6-ws282c（c60cdbd44 から）、WS296 は agent/p6-ws296c 1809873bd で全 Phase 実装済み、agent/p6-ws243b（飛ばした記録 b13f3dae7）、agent/p6-ws223（未着手）。
- 試験の担当 T・ブラウザの B1 は今は立てていない。

- 10.0.10.3 の時間の枠（plan/tools.md）: 最初の枠は P1（06:20〜07:05）、次は P2（07:05〜07:50）、以後交代（ネットワーク優先）。
