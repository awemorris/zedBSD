# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P1 | phase-runner-high | ws324 p009b NVENC の関門 (q1884-i01) | `/home/awe/zedBSD-worktrees/p1-ws324` / `agent/p1-ws324` | in-progress（2026-11-02 01:07、10.0.10.3） | dce8421af（merge a43959d8e） |
| P2 | phase-runner-high | ws322 p007 速さと頑丈さ (q1896-i01) | `/home/awe/zedBSD-worktrees/p2-ws322` / `agent/p2-ws322` | in-progress（2026-11-02 01:55、VF2） | 6c49fc4f4（merge f9acea845） |
| P3 | phase-runner-high | BUG-393 (q1891-i01) | `/home/awe/zedBSD-worktrees/p3-bug393` / `agent/p3-bug393` | 返却（2026-11-02 02:20、fixed） | 594f9edc4（2026-11-02 02:20 に main へ merge） |
| P5 | phase-runner-high | ws334 p033 後半の Windows の口 (q1886-i01) | `/home/awe/zedBSD-worktrees/p5-ws334b` / `agent/p5-ws334b` | in-progress（2026-11-02 01:25 起動） | fb9ee2715（merge 04eb03f1d） |
| P6 | phase-runner-low | ws335 p015 OS の層 (q1897-i01) | `/home/awe/zedBSD-worktrees/p6-ws335b` / `agent/p6-ws335b` | in-progress（2026-11-02 02:04） | 6ef86e45f（merge f533fd4cd） |
| P7 | phase-runner-high | BUG-394 (q1892-i01) | `/home/awe/zedBSD-worktrees/p7-bug394` / `agent/p7-bug394` | 返却（2026-11-02 02:23、fixed） | 7366f119a（2026-11-02 02:23 に main へ merge） |
| P4 | phase-runner-high | BUG-390 (q1889-i01) | `/home/awe/zedBSD-worktrees/p4-bug390` / `agent/p4-bug390` | 返却（2026-11-02 01:42、fixed） | bf6c08971（merge 3b79cca9e） |
| P8 | phase-runner-high | BUG-391・392 (q1890-i01) | `/home/awe/zedBSD-worktrees/p8-bug391` / `agent/p8-bug391` | 返却（2026-11-02 02:09、fixed） | bd3a783d4（2026-11-02 02:09 に main へ merge） |

- 2026-11-01 20:20 の割り振り（ユーザーの指示）: P1 は RTX、P2 は VF2、P3 は Pi 5 の作業（どれも WS291 の描画の速さ、Q1 が選んだ）。Bug の修正は P4。
- 実機: 10.0.10.3（RTX 2070、SwitchBot 1 AC276E42B252）は P1、VF2（10.0.30.2、SwitchBot 3 AC276E432AE6）は P2、Pi 5（10.0.30.7、SwitchBot 2 14C19FA80F86）は P3。P4 の BUG-387 の実機の試験は、Q1 が P2・P3 から窓を取って渡す。
- WS074 は Claude のクラウドのセッションが担当（2026-11-01 ユーザー）。成果は origin/browser にあり、Q1 が main に merge する。P8 は立てない。
- WS335・WS336（Sonnet）・WS334（P7）の pending の Queue（q1835・q1836・q1837）は今回は起動しない。
- 試験の担当 T・ブラウザの B1 は今は立てていない。

| Sonnet | phase-runner-low | ws324 p006c (q1883-i01) | `/home/awe/zedBSD-worktrees/s-ws324-p006c` / `agent/s-ws324-p006c` | in-progress（2026-11-02 01:07 起動） | — |

| Sonnet | phase-runner-low | ws335 p026 (q1885-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p026` / `agent/s-ws335-p026` | 返却（2026-11-02、cleared） | 0b7351a88 |

| Sonnet | phase-runner-low | ws334 p034 (q1887-i01) | `/home/awe/zedBSD-worktrees/s-ws334-p034` / `agent/s-ws334-p034` | in-progress（2026-11-02 01:25 起動） | — |
| Sonnet | phase-runner-low | ws334 の TLS の誤りの文 (q1888-i01) | `/home/awe/zedBSD-worktrees/s-ws334-errstr` / `agent/s-ws334-errstr` | 返却（2026-11-02 02:20、cleared） | 9473e4c5e |

| Sonnet | phase-runner-low | ws335 p028 (q1893-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p028` / `agent/s-ws335-p028` | in-progress（2026-11-02 01:41 起動） | — |

| Sonnet | phase-runner-low | ws335 p017 (q1895-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p017` / `agent/s-ws335-p017` | in-progress（2026-11-02 01:47 起動） | — |
