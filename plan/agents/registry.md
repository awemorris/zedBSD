# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P1 | phase-runner-high | ws291 p022 (q1731-i01) と ws333 p002 (q1735-i01) | `/home/awe/zedBSD-worktrees/p1-ws291i` / `agent/p1-ws291i` | in-progress（2026-10-31、5330、p022 に戻った） | 6314c5b94（merge f740839e5） |
| P2 | phase-runner-high | ws291 p020 (q1733-i01) | `/home/awe/zedBSD-worktrees/p2-ws291v` / `agent/p2-ws291v` | in-progress（2026-10-31 起動し直し、VF2 の SSH・sudo の許可） | dc49bd786 |
| P4 | phase-runner-high | q1742 BUG-374（Pi 5 の p021 は実機待ち） | `/home/awe/zedBSD-worktrees/p4-bug374` / `agent/p4-bug374` | in-progress（2026-10-31 起動） | e4f77b9e5（ws291p021 は未 merge） |
| P5 | kernel-and-driver-designer | ws333 p001 (q1734-i01) | `/home/awe/zedBSD-worktrees/p5-ws333` / `agent/p5-ws333` | in-progress（2026-10-31 起動） | — |
| P6 | phase-runner-high | q1740 BUG-370 → q1741 WS291 p014 | `/home/awe/zedBSD-worktrees/p6-ws291` / `agent/p6-ws291` | in-progress（2026-10-31 起動） | 93f7ec9d3 |
| P3 | phase-runner-high | q1736 WS307 → q1737 WS331 p006 → q1738 WS332 → q1739 WS326 | `/home/awe/zedBSD-worktrees/p3-seq` / `agent/p3-seq` | in-progress（2026-10-31 起動） | 823ce7bde（merge ccd7bf2d2） |
| P8 | phase-runner-low | ws074 p260 自走 (q1727-i02) | `/home/awe/zedBSD-worktrees/p8-ws074` / `agent/p8-ws074` | in-progress（2026-10-31 起動し直し） | fccca00dc |
| P7 | phase-runner-high | BUG-373・371・372 (q1732-i01) | `/home/awe/zedBSD-worktrees/p7-bug373` / `agent/p7-bug373` | in-progress（2026-10-31 起動） | — |

- 同時に動かす数は 6（2026-10-29 ユーザーの P1〜P6 の指示）。VF2（主）と 5330（従）は P3、10.0.10.3 は空き（2026-10-30 09:13 P5 返却）。
- P6 の branch（merge しない）: agent/p6-ws296→p6-ws296b（2d17bf9ed から）→p6-ws296c（f31a4954d から）、agent/p6-ws282→p6-ws282b（1190b629c から）→p6-ws282c（c60cdbd44 から）、WS296 は agent/p6-ws296c 1809873bd で全 Phase 実装済み、agent/p6-ws243b（飛ばした記録 b13f3dae7）、agent/p6-ws223（未着手）。
- 試験の担当 T・ブラウザの B1 は今は立てていない。

- 10.0.10.3 の時間の枠（plan/tools.md）: 最初の枠は P1（06:20〜07:05）、次は P2（07:05〜07:50）、以後交代（ネットワーク優先）。
