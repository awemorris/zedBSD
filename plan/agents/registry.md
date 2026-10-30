# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P1 | phase-runner-high | ws291 p022 (q1731-i01) | `/home/awe/zedBSD-worktrees/p1-ws291i` / `agent/p1-ws291i` | in-progress（2026-10-30 起動、5330） | — |
| P2 | phase-runner-high | ws291 p020 (q1726-i01 返却) | `/home/awe/zedBSD-worktrees/p2-ws291v` / `agent/p2-ws291v` | returned（権限の判定で停止、ユーザーの判断待ち。branch は BUG-375 まで未 merge） | dc49bd786 |
| P4 | phase-runner-high | ws291 p021 (q1730-i01 返却) | `/home/awe/zedBSD-worktrees/p4-ws291pi` / `agent/p4-ws291pi` | returned（実機待ち、branch は未 merge） | e4f77b9e5 |
| P5 | phase-runner | ws327 (q1710-i01 cleared) | `/home/awe/zedBSD-worktrees/p5-ws327` / `agent/p5-ws327` | returned（WS327 完了） | cf2963051（merge a17e9386d） |
| P6 | phase-runner-high | ws307 (q1718-i01 返却) | `/home/awe/zedBSD-worktrees/p6-ws307` / `agent/p6-ws307` | returned（p007 の残り 2 つ） | ccd0c8693（merge 061230951） |
| P3 | phase-runner-high | ws291 p013 (q1728-i01) | `/home/awe/zedBSD-worktrees/p3-ws291r` / `agent/p3-ws291r` | in-progress（2026-10-30 起動、10.0.10.3） | — |
| P8 | phase-runner-low | ws074 p260 自走 (q1727-i01) | `/home/awe/zedBSD-worktrees/p8-ws074` / `agent/p8-ws074` | in-progress（2026-10-30 起動、100% まで自走） | 9df11eec9（merge 11888c436） |
| P7 | phase-runner-high | BUG-373・371・372 (q1732-i01) | `/home/awe/zedBSD-worktrees/p7-bug373` / `agent/p7-bug373` | in-progress（2026-10-31 起動） | — |

- 同時に動かす数は 6（2026-10-29 ユーザーの P1〜P6 の指示）。VF2（主）と 5330（従）は P3、10.0.10.3 は空き（2026-10-30 09:13 P5 返却）。
- P6 の branch（merge しない）: agent/p6-ws296→p6-ws296b（2d17bf9ed から）→p6-ws296c（f31a4954d から）、agent/p6-ws282→p6-ws282b（1190b629c から）→p6-ws282c（c60cdbd44 から）、WS296 は agent/p6-ws296c 1809873bd で全 Phase 実装済み、agent/p6-ws243b（飛ばした記録 b13f3dae7）、agent/p6-ws223（未着手）。
- 試験の担当 T・ブラウザの B1 は今は立てていない。

- 10.0.10.3 の時間の枠（plan/tools.md）: 最初の枠は P1（06:20〜07:05）、次は P2（07:05〜07:50）、以後交代（ネットワーク優先）。
