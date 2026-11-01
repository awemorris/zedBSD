# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P1 | phase-runner-high | BUG-386 → BUG-385 (q1825-i01 → q1826) | `/home/awe/zedBSD-worktrees/p1-bug386` / `agent/p1-bug386` | in-progress（2026-11-01 08:24 起動、VF2） | 644eb3d98（merge dbf2059a7） |
| P2 | phase-runner-high | BUG-374 (q1827-i01) | `/home/awe/zedBSD-worktrees/p2-bug374` / `agent/p2-bug374` | in-progress（2026-11-01 08:24 起動、10.0.10.3 の RTX） | 7da994932（merge d8e39d789） |
| P4 | phase-runner-high | ws291 main の計測 (q1817-i01) | `/home/awe/zedBSD-worktrees/p4-ws291pi` / `agent/p4-ws291pi` | 返却（2026-11-01 06:53） | a54520ea8（merge 69109d3f2） |
| P5 | kernel-and-driver-designer | ws336 p001 設計の仕上げ (q1791-i02) | `/home/awe/zedBSD-worktrees/p5-ws336` / `agent/p5-ws336` | 返却（2026-11-01 04:53、cleared。実装は Sonnet） | 6848137f6（merge a1d9f18fb） |
| P6 | kernel-and-driver-designer | ws335 p001 設計の仕上げ (q1789-i02) | `/home/awe/zedBSD-worktrees/p6-ws335` / `agent/p6-ws335` | in-progress（2026-11-01 04:51 起動し直し） | f4d4662d3（main） |
| P3 | phase-runner-high | ws291 p024 RTX 最適化 (q1785-i03) | `/home/awe/zedBSD-worktrees/p3-ws291wsi` / `agent/p3-ws291wsi` | ラップアップ（2026-11-01 07:00、ユーザーの指示） | 6dd842057（merge 1e86f4502） |
| P8 | phase-runner-low | ws074 p260 自走 (q1727-i17) | `/home/awe/zedBSD-worktrees/p8-ws074` / `agent/p8-ws074` | in-progress（2026-11-01 09:06 起動し直し） | 26d1732d3（merge 00d5365c8） |
| P7 | phase-runner-high | ws334 p022 network (q1830-i01) | `/home/awe/zedBSD-worktrees/p7-ws334` / `agent/p7-ws334` | in-progress（2026-11-01 09:06 起動） | 37593500d（merge 8970a6e96） |

- 2026-11-01 08:2x の割り振り（ユーザーの指示）: Bug の修正を先に。VF2 の Bug に P1、RTX の Bug に P2、WS335・WS336・WS334（P7）・WS074（P8）に 1 人ずつ。実機は VF2 が P1、10.0.10.3 が P2。
- 同時に動かす数は 6（2026-10-29 ユーザーの P1〜P6 の指示）。VF2（主）と 5330（従）は P3、10.0.10.3 は空き（2026-10-30 09:13 P5 返却）。
- P6 の branch（merge しない）: agent/p6-ws296→p6-ws296b（2d17bf9ed から）→p6-ws296c（f31a4954d から）、agent/p6-ws282→p6-ws282b（1190b629c から）→p6-ws282c（c60cdbd44 から）、WS296 は agent/p6-ws296c 1809873bd で全 Phase 実装済み、agent/p6-ws243b（飛ばした記録 b13f3dae7）、agent/p6-ws223（未着手）。
- 試験の担当 T・ブラウザの B1 は今は立てていない。

- 10.0.10.3 の時間の枠（plan/tools.md）: 最初の枠は P1（06:20〜07:05）、次は P2（07:05〜07:50）、以後交代（ネットワーク優先）。
