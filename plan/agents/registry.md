# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P1 | phase-runner | ws009 (q1701-i01 DOC-54 を標準廉価実装手順に) | `/home/awe/zedBSD-worktrees/p1-ws009` / `agent/p1-ws009` | running（2026-10-30） | 13c603132（merge b9c381154） |
| P2 | phase-runner | ws327 (q1697-i02 p003 の guest → p007 委譲の設計) | `/home/awe/zedBSD-worktrees/p2-ws327` / `agent/p2-ws327` | running（2026-10-30 再起動、標準廉価実装手順に組み直し fb2d34dc0） | b73eb1fa7（未 merge） |
| P3 | phase-runner | ws315 (q1696-i01 返却) | `/home/awe/zedBSD-worktrees/p3-ws315` / `agent/p3-ws315` | returned（p006 の引き継ぎから、未 merge b2e867978） | 4559631d9（merge 145280b3e） |
| P4 | phase-runner | ws307 (q1698-i01 返却) | `/home/awe/zedBSD-worktrees/p4-ws307o` / `agent/p4-ws307o` | returned（次は ws307 の Resume point から） | 520360247（merge fbdd143a8） |
| P5 | phase-runner | ws291 NVIDIA (q1700-i01 返却) | `/home/awe/zedBSD-worktrees/p5-ws291n` / `agent/p5-ws291n` | returned（次は phase010 の q1700-i01 節から） | c59e5a60e（merge 12a2cc467） |
| P6 | phase-runner＋Sonnet | ws314 p011・p012 (q1691-i01 cleared) | `/home/awe/zedBSD-worktrees/p6-ws314doc` / `agent/p6-ws314doc` | returned | 64f09ca82（merge d4ba04b96） |
| P7 | phase-runner | ws313 (q1687-i01 返却) | `/home/awe/zedBSD-worktrees/p7-ws313e` / `agent/p7-ws313e` | returned（実機の結果待ち） | b35b6240c（merge b6649f09f） |

- 同時に動かす数は 6（2026-10-29 ユーザーの P1〜P6 の指示）。VF2（主）と 5330（従）は P3、10.0.10.3 は空き（2026-10-30 09:13 P5 返却）。
- P6 の branch（merge しない）: agent/p6-ws296→p6-ws296b（2d17bf9ed から）→p6-ws296c（f31a4954d から）、agent/p6-ws282→p6-ws282b（1190b629c から）→p6-ws282c（c60cdbd44 から）、WS296 は agent/p6-ws296c 1809873bd で全 Phase 実装済み、agent/p6-ws243b（飛ばした記録 b13f3dae7）、agent/p6-ws223（未着手）。
- 試験の担当 T・ブラウザの B1 は今は立てていない。

- 10.0.10.3 の時間の枠（plan/tools.md）: 最初の枠は P1（06:20〜07:05）、次は P2（07:05〜07:50）、以後交代（ネットワーク優先）。
