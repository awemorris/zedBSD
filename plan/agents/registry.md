# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P1 | phase-runner-high | ws324 p009a NVENC の channel (q1880-i01) | `/home/awe/zedBSD-worktrees/p1-ws324` / `agent/p1-ws324` | in-progress（2026-11-02 00:41 起動、10.0.10.3） | 6b136a2ae（merge e1eaa6761） |
| P2 | phase-runner-high | ws322 p004 H.264 decode の道 (q1869-i01) | `/home/awe/zedBSD-worktrees/p2-ws322` / `agent/p2-ws322` | in-progress（2026-11-01 23:51 起動、VF2） | d1af4687c（merge 361044f95） |
| P3 | phase-runner-high | ws291 p027 の回帰 amd64・Venus (q1859-i01) | `/home/awe/zedBSD-worktrees/p3-ws291reg` / `agent/p3-ws291reg` | 返却（2026-11-01 23:19、PASS） | 73669b953（merge 6f49769e7） |
| P5 | phase-runner-high | ws334 p032 Wine の確かめ (q1874-i01) | `/home/awe/zedBSD-worktrees/p5-ws334b` / `agent/p5-ws334b` | in-progress（2026-11-02 00:19 起動） | 22e46046c（merge f9c278fa5） |
| P6 | phase-runner-low | ws335 p012 RSA (q1870-i01) | `/home/awe/zedBSD-worktrees/p6-ws335b` / `agent/p6-ws335b` | in-progress（2026-11-01 23:51 起動） | f14b5b34a（merge 2d7097dd3） |
| P7 | phase-runner-high | ws334 p027 の guest の確かめ (q1858-i01) | `/home/awe/zedBSD-worktrees/p7-ws334g` / `agent/p7-ws334g` | 返却（2026-11-01 22:26、PASS） | f45a3e728（merge f58a8d93f） |
| P4 | phase-runner-high | ws291 p027 の回帰 arm64・riscv64 (q1860-i01) | `/home/awe/zedBSD-worktrees/p4-ws291reg` / `agent/p4-ws291reg` | 返却（2026-11-01 22:48、PASS） | 42ae7d081（merge b29f3a5ae） |
| P8 | phase-runner-high | menuconfig の host 試験の直し (q1873-i01) | `/home/awe/zedBSD-worktrees/p8-menutest` / `agent/p8-menutest` | 返却（2026-11-02 00:41、PASS） | c2d396e24（merge cc506bb26） |

- 2026-11-01 20:20 の割り振り（ユーザーの指示）: P1 は RTX、P2 は VF2、P3 は Pi 5 の作業（どれも WS291 の描画の速さ、Q1 が選んだ）。Bug の修正は P4。
- 実機: 10.0.10.3（RTX 2070、SwitchBot 1 AC276E42B252）は P1、VF2（10.0.30.2、SwitchBot 3 AC276E432AE6）は P2、Pi 5（10.0.30.7、SwitchBot 2 14C19FA80F86）は P3。P4 の BUG-387 の実機の試験は、Q1 が P2・P3 から窓を取って渡す。
- WS074 は Claude のクラウドのセッションが担当（2026-11-01 ユーザー）。成果は origin/browser にあり、Q1 が main に merge する。P8 は立てない。
- WS335・WS336（Sonnet）・WS334（P7）の pending の Queue（q1835・q1836・q1837）は今回は起動しない。
- 試験の担当 T・ブラウザの B1 は今は立てていない。
| Sonnet | phase-runner-low | ws335 p009 (q1876-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p009` / `agent/s-ws335-p009` | in-progress（2026-11-02 00:24 起動） | — |
| Sonnet | phase-runner-low | ws335 p016 (q1877-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p016` / `agent/s-ws335-p016` | in-progress（2026-11-02 00:24 起動） | — |
| Sonnet | phase-runner-low | ws335 p025 (q1878-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p025` / `agent/s-ws335-p025` | in-progress（2026-11-02 00:24 起動） | — |
| Sonnet | phase-runner-low | ws324 p006b (q1879-i01) | `/home/awe/zedBSD-worktrees/s-ws324-p006b` / `agent/s-ws324-p006b` | in-progress（2026-11-02 00:33 起動） | — |
