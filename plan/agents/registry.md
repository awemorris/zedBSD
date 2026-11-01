# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P1 | kernel-and-driver-designer | ws324 p001 RTX の video の設計 (q1854-i01) | `/home/awe/zedBSD-worktrees/p1-ws324` / `agent/p1-ws324` | in-progress（2026-11-01 22:10 起動）。前の agent/p1-ws291p026 は未 merge | 7973c156f（main） |
| P2 | phase-runner-high | ws322 p002 VF2 の VPU の probe (q1852-i01) | `/home/awe/zedBSD-worktrees/p2-ws322` / `agent/p2-ws322` | in-progress（2026-11-01 21:35 起動、VF2） | 5b8debe04（merge 261624739） |
| P3 | phase-runner-high | ws291 p021 Pi 5 の model の 50 fps (q1840-i02) | `/home/awe/zedBSD-worktrees/p3-ws291p021` / `agent/p3-ws291p021` | 返却（2026-11-01 21:17、cleared） | d32bcb9f5（merge 9b865b038） |
| P5 | phase-runner-high | ws334 p026 libmedia の Windows (q1853-i01) | `/home/awe/zedBSD-worktrees/p5-ws334b` / `agent/p5-ws334b` | in-progress（2026-11-01 21:45 起動） | 30bd64129（merge f5aad61a8） |
| P6 | phase-runner-low | ws335 p007 整数と Montgomery (q1851-i01) | `/home/awe/zedBSD-worktrees/p6-ws335b` / `agent/p6-ws335b` | in-progress（2026-11-01 21:33 起動） | 556988325（merge 72bb7908a） |
| P7 | phase-runner-high | ws334 p027 の guest の確かめ (q1858-i01) | `/home/awe/zedBSD-worktrees/p7-ws334g` / `agent/p7-ws334g` | in-progress（2026-11-01 22:07 起動、QEMU） | — |
| Sonnet | phase-runner-low | ws334 p024 (q1855-i01) | `/home/awe/zedBSD-worktrees/s-ws334-p024` / `agent/s-ws334-p024` | in-progress（2026-11-01 21:48 起動） | — |
| Sonnet | phase-runner-low | ws334 p030 (q1857-i01) | `/home/awe/zedBSD-worktrees/s-ws334-p030` / `agent/s-ws334-p030` | in-progress（2026-11-01 21:48 起動） | — |
| P4 | phase-runner-high | BUG-387 の残り (q1850-i01) | `/home/awe/zedBSD-worktrees/p4-bug387` / `agent/p4-bug387` | 返却（2026-11-01 21:28） | 42ae7d081（merge b29f3a5ae） |

- 2026-11-01 20:20 の割り振り（ユーザーの指示）: P1 は RTX、P2 は VF2、P3 は Pi 5 の作業（どれも WS291 の描画の速さ、Q1 が選んだ）。Bug の修正は P4。
- 実機: 10.0.10.3（RTX 2070、SwitchBot 1 AC276E42B252）は P1、VF2（10.0.30.2、SwitchBot 3 AC276E432AE6）は P2、Pi 5（10.0.30.7、SwitchBot 2 14C19FA80F86）は P3。P4 の BUG-387 の実機の試験は、Q1 が P2・P3 から窓を取って渡す。
- WS074 は Claude のクラウドのセッションが担当（2026-11-01 ユーザー）。成果は origin/browser にあり、Q1 が main に merge する。P8 は立てない。
- WS335・WS336（Sonnet）・WS334（P7）の pending の Queue（q1835・q1836・q1837）は今回は起動しない。
- 試験の担当 T・ブラウザの B1 は今は立てていない。
