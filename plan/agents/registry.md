# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P1 | phase-runner-high | ws313 p003 firmware (q1714-i01) | `/home/awe/zedBSD-worktrees/p1-ws313` / `agent/p1-ws313`、firmware: `/home/awe/zedBSD-worktrees/p1-c5-firmware` / `agent/p1-c5-firmware` | in-progress（2026-10-30 起動） | e0b52c642（merge 0384466f5） |
| P2 | phase-runner | ws331 p001 (q1713-i01) | `/home/awe/zedBSD-worktrees/p2-ws331` / `agent/p2-ws331` | in-progress（2026-10-30 起動） | — |
| P5 | phase-runner | ws327 (q1710-i01、WS の完了で返却) | `/home/awe/zedBSD-worktrees/p5-ws327` / `agent/p5-ws327` | in-progress（2026-10-30 起動） | — |
| P6 | phase-runner | ws307 (q1711-i01、WS の完了で返却) | `/home/awe/zedBSD-worktrees/p6-ws307` / `agent/p6-ws307` | in-progress（2026-10-30 起動） | — |
| P8 | phase-runner-low | ws074 p260 (q1708-i01) | `/home/awe/zedBSD-worktrees/p8-ws074` / `agent/p8-ws074` | in-progress（2026-10-30 13:50 起動） | — |
| P7 | phase-runner-high | ws291 (q1712-i01、WS の完了で返却) | `/home/awe/zedBSD-worktrees/p7-ws291` / `agent/p7-ws291` | in-progress（2026-10-30 起動、10.0.10.3） | — |

- 同時に動かす数は 6（2026-10-29 ユーザーの P1〜P6 の指示）。VF2（主）と 5330（従）は P3、10.0.10.3 は空き（2026-10-30 09:13 P5 返却）。
- P6 の branch（merge しない）: agent/p6-ws296→p6-ws296b（2d17bf9ed から）→p6-ws296c（f31a4954d から）、agent/p6-ws282→p6-ws282b（1190b629c から）→p6-ws282c（c60cdbd44 から）、WS296 は agent/p6-ws296c 1809873bd で全 Phase 実装済み、agent/p6-ws243b（飛ばした記録 b13f3dae7）、agent/p6-ws223（未着手）。
- 試験の担当 T・ブラウザの B1 は今は立てていない。

- 10.0.10.3 の時間の枠（plan/tools.md）: 最初の枠は P1（06:20〜07:05）、次は P2（07:05〜07:50）、以後交代（ネットワーク優先）。
