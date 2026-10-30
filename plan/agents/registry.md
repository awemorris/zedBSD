# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P1 | phase-runner-high | ws326 p024 (q1773-i01) | `/home/awe/zedBSD-worktrees/p1-ws326p024` / `agent/p1-ws326p024` | in-progress（2026-10-31 起動） | 5857d62a4（merge f31bbac2e、WS299） |
| P2 | phase-runner-high | VF2 の Bug の束 (q1772-i01: BUG-338・342・340・343・339) | `/home/awe/zedBSD-worktrees/p2-vf2bugs` / `agent/p2-vf2bugs` | in-progress（2026-10-31 起動、VF2） | b35939531（merge 9154dc6db） |
| P4 | phase-runner-high | ws326 p026 (q1769-i01) | `/home/awe/zedBSD-worktrees/p4-ws326p026` / `agent/p4-ws326p026` | in-progress（2026-10-31 起動） | 5b56cbaef（merge b0b4577c7） |
| P5 | kernel-and-driver-designer | ws330 p001 (q1763-i01) | `/home/awe/zedBSD-worktrees/p5-ws330` / `agent/p5-ws330` | in-progress（2026-10-31 起動） | c56fb91dc（merge 455a7d5b4、WS329 p001） |
| P6 | phase-runner-high | ws333 p016 → p021 (q1774-i01) | `/home/awe/zedBSD-worktrees/p6-ws333` / `agent/p6-ws333` | in-progress（2026-10-31 起動） | 93fb7eadd（merge 809dfe329） |
| P3 | phase-runner-high | ws291 p014 の続き (q1764-i01) | `/home/awe/zedBSD-worktrees/p3-ws291r2` / `agent/p3-ws291r2` | in-progress（2026-10-31 起動、10.0.10.3） | 8aca507b9（merge 1035ec0a0、WS331） |
| P8 | phase-runner-low | ws074 p260 自走 (q1727-i02) | `/home/awe/zedBSD-worktrees/p8-ws074` / `agent/p8-ws074` | in-progress（2026-10-31 起動し直し） | fccca00dc |
| P7 | phase-runner-high | ws333 p011 (q1770-i01) | `/home/awe/zedBSD-worktrees/p7-ws333b` / `agent/p7-ws333b` | in-progress（2026-10-31 起動） | 87a6d4ff7（merge 22c629663） |

- 同時に動かす数は 6（2026-10-29 ユーザーの P1〜P6 の指示）。VF2（主）と 5330（従）は P3、10.0.10.3 は空き（2026-10-30 09:13 P5 返却）。
- P6 の branch（merge しない）: agent/p6-ws296→p6-ws296b（2d17bf9ed から）→p6-ws296c（f31a4954d から）、agent/p6-ws282→p6-ws282b（1190b629c から）→p6-ws282c（c60cdbd44 から）、WS296 は agent/p6-ws296c 1809873bd で全 Phase 実装済み、agent/p6-ws243b（飛ばした記録 b13f3dae7）、agent/p6-ws223（未着手）。
- 試験の担当 T・ブラウザの B1 は今は立てていない。

- 10.0.10.3 の時間の枠（plan/tools.md）: 最初の枠は P1（06:20〜07:05）、次は P2（07:05〜07:50）、以後交代（ネットワーク優先）。
