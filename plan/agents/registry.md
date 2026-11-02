# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P1 | phase-runner-high | ws324 p009b NVENC の関門 (q1884-i01) | `/home/awe/zedBSD-worktrees/p1-ws324` / `agent/p1-ws324` | 返却（2026-11-02 06:28、uncleared） | be70e1303（2026-11-02 06:28 に main へ merge） |
| P2 | phase-runner-high | ws322 p007 速さと頑丈さ (q1896-i01) | `/home/awe/zedBSD-worktrees/p2-ws322` / `agent/p2-ws322` | 返却（2026-11-02 03:45、cleared） | 6d842c319（2026-11-02 03:45 に main へ merge） |
| P3 | phase-runner-high | BUG-393 (q1891-i01) | `/home/awe/zedBSD-worktrees/p3-bug393` / `agent/p3-bug393` | 返却（2026-11-02 02:20、fixed） | 594f9edc4（2026-11-02 02:20 に main へ merge） |
| P5 | phase-runner-high | ws334 p033 後半の Windows の口 (q1886-i01) | `/home/awe/zedBSD-worktrees/p5-ws334b` / `agent/p5-ws334b` | 返却（2026-11-02 02:44、cleared） | 7c8295924（2026-11-02 02:44 に main へ merge） |
| P6 | phase-runner-low | ws335 p015 OS の層 (q1897-i01) | `/home/awe/zedBSD-worktrees/p6-ws335b` / `agent/p6-ws335b` | 返却（2026-11-02 03:02、cleared） | 56a60f716（2026-11-02 03:02 に main へ merge） |
| P7 | phase-runner-high | BUG-394 (q1892-i01) | `/home/awe/zedBSD-worktrees/p7-bug394` / `agent/p7-bug394` | 返却（2026-11-02 02:46、fixed） | 471d71a4e（2026-11-02 02:46 に main へ merge） |
| P4 | phase-runner-high | BUG-390 (q1889-i01) | `/home/awe/zedBSD-worktrees/p4-bug390` / `agent/p4-bug390` | 返却（2026-11-02 01:42、fixed） | bf6c08971（merge 3b79cca9e） |
| P8 | phase-runner-high | BUG-391・392 (q1890-i01) | `/home/awe/zedBSD-worktrees/p8-bug391` / `agent/p8-bug391` | 返却（2026-11-02 02:09、fixed） | bd3a783d4（2026-11-02 02:09 に main へ merge） |

- 2026-11-01 20:20 の割り振り（ユーザーの指示）: P1 は RTX、P2 は VF2、P3 は Pi 5 の作業（どれも WS291 の描画の速さ、Q1 が選んだ）。Bug の修正は P4。
- 実機: 10.0.10.3（RTX 2070、SwitchBot 1 AC276E42B252）は P1、VF2（10.0.30.2、SwitchBot 3 AC276E432AE6）は P2、Pi 5（10.0.30.7、SwitchBot 2 14C19FA80F86）は P3 → 2026-11-02 02:23 から P7（BUG-394 の計測）。P4 の BUG-387 の実機の試験は、Q1 が P2・P3 から窓を取って渡す。
- WS074 は Claude のクラウドのセッションが担当（2026-11-01 ユーザー）。成果は origin/browser にあり、Q1 が main に merge する。P8 は立てない。
- WS335・WS336（Sonnet）・WS334（P7）の pending の Queue（q1835・q1836・q1837）は今回は起動しない。
- 試験の担当 T・ブラウザの B1 は今は立てていない。

| Sonnet | phase-runner-low | ws324 p006c (q1883-i01) | `/home/awe/zedBSD-worktrees/s-ws324-p006c` / `agent/s-ws324-p006c` | 返却（2026-11-02、cleared） | 76579427e |

| Sonnet | phase-runner-low | ws335 p026 (q1885-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p026` / `agent/s-ws335-p026` | 返却（2026-11-02、cleared） | 0b7351a88 |

| Sonnet | phase-runner-low | ws334 p034 (q1887-i01) | `/home/awe/zedBSD-worktrees/s-ws334-p034` / `agent/s-ws334-p034` | 返却（2026-11-02 06:09、cleared） | d29225d46 |
| Sonnet | phase-runner-low | ws334 の TLS の誤りの文 (q1888-i01) | `/home/awe/zedBSD-worktrees/s-ws334-errstr` / `agent/s-ws334-errstr` | 返却（2026-11-02 02:20、cleared） | 9473e4c5e |

| Sonnet | phase-runner-low | ws335 p028 (q1893-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p028` / `agent/s-ws335-p028` | 返却（2026-11-02、cleared） | fff699939 |

| Sonnet | phase-runner-low | ws335 p017 (q1895-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p017` / `agent/s-ws335-p017` | 返却（2026-11-02、cleared） | 42514404b |
| P3 | phase-runner-high | Pi 5 の desktop の動画 (q1898-i01) | `/home/awe/zedBSD-worktrees/p3-pi5rec` / `agent/p3-pi5rec` | 返却（2026-11-02 03:03、cleared） | b0016818d（2026-11-02 03:03 に main へ merge） |
| Sonnet | phase-runner-low | ws324 p006d (q1899-i01) | `/home/awe/zedBSD-worktrees/s-ws324-p006d` / `agent/s-ws324-p006d` | 返却（2026-11-02 03:26、cleared） | 0703840c5 |
| Sonnet | phase-runner-low | ws324 p006e (q1900-i01) | `/home/awe/zedBSD-worktrees/s-ws324-p006e` / `agent/s-ws324-p006e` | 返却（2026-11-02、cleared） | 5cca24183 |
| Sonnet | phase-runner-low | ws324 p006f (q1901-i01) | `/home/awe/zedBSD-worktrees/s-ws324-p006f` / `agent/s-ws324-p006f` | 返却（2026-11-02 03:16、cleared） | 51cd471c2 |
| Sonnet | phase-runner-low | ws335 p018 (q1902-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p018` / `agent/s-ws335-p018` | 返却（2026-11-02 06:09、cleared） | 8d598df28 |
| Sonnet | phase-runner-low | ws335 p019 (q1903-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p019` / `agent/s-ws335-p019` | 返却（2026-11-02 06:09、cleared） | 861875a51 |
| P6 | phase-runner-high | ws335 p037 (q1904-i01) | `/home/awe/zedBSD-worktrees/p6-ws335c` / `agent/p6-ws335c` | 返却（2026-11-02 03:59、cleared） | c6cce5b28（2026-11-02 03:59 に main へ merge） |
| Sonnet | phase-runner-low | ws335 p021 (q1905-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p021` / `agent/s-ws335-p021` | 返却（2026-11-02、cleared） | e594bc29e |
| Sonnet | phase-runner-low | ws324 p006g (q1906-i01) | `/home/awe/zedBSD-worktrees/s-ws324-p006g` / `agent/s-ws324-p006g` | 返却（2026-11-02 06:09、cleared） | e4045b157 |
| P2 | phase-runner-high | ws322 p007b (q1907-i01) | `/home/awe/zedBSD-worktrees/p2-ws322` / `agent/p2-ws322` | 返却（2026-11-02 06:09、cleared） | a8733e219（2026-11-02 06:09 に main へ merge） |
| Sonnet | phase-runner-low | ws335 p023 (q1908-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p023` / `agent/s-ws335-p023` | 返却（2026-11-02 06:09、cleared） | 14a11d50d |
| P5 | phase-runner-high | ws334 p038 (q1909-i01) | `/home/awe/zedBSD-worktrees/p5-ws334c` / `agent/p5-ws334c` | 返却（2026-11-02 06:57、cleared） | 1aa077e57（2026-11-02 06:57 に main へ merge） |
| P6 | phase-runner-high | ws324 p006h (q1910-i01) | `/home/awe/zedBSD-worktrees/p6-ws324h` / `agent/p6-ws324h` | 返却（2026-11-02 07:40、cleared） | 22eed36ea（2026-11-02 07:40 に main へ merge） |
| P2 | phase-runner-high | ws322 p005 (q1911-i01) | `/home/awe/zedBSD-worktrees/p2-ws322` / `agent/p2-ws322` | 返却（2026-11-02 06:45、cleared） | 1987af84c（2026-11-02 06:45 に main へ merge） |
| Sonnet | phase-runner-low | ws335 p020 (q1912-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p020` / `agent/s-ws335-p020` | in-progress（2026-11-02 06:10 起動） | — |
| Sonnet | phase-runner-low | ws335 p024 (q1913-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p024` / `agent/s-ws335-p024` | 返却（2026-11-02 09:40、cleared） | 1a82b52f6 |
| P8 | phase-runner-high | BUG-395・396 (q1914-i01) | `/home/awe/zedBSD-worktrees/p8-bug395` / `agent/p8-bug395` | in-progress（2026-11-02 06:16 起動） | — |
| P4 | phase-runner-high | BUG-397・399 (q1915-i01) | `/home/awe/zedBSD-worktrees/p4-bug397` / `agent/p4-bug397` | 返却（2026-11-02 08:12、fixed） | 3a026d9ef（2026-11-02 08:12 に main へ merge） |
| P7 | phase-runner-high | BUG-398 (q1916-i01) | `/home/awe/zedBSD-worktrees/p7-bug398` / `agent/p7-bug398` | 返却（2026-11-02 07:50、fixed） | 305b1ffe9（2026-11-02 07:50 に main へ merge） |
| P1 | phase-runner-high | ws324 p005 (q1917-i01) | `/home/awe/zedBSD-worktrees/p1-ws324` / `agent/p1-ws324` | 返却（2026-11-02 08:22、cleared） | 5c141459c（2026-11-02 08:22 に main へ merge） |
| P5 | phase-runner-high | BUG-400 (q1918-i01) | `/home/awe/zedBSD-worktrees/p5-ws334d` / `agent/p5-ws334d` | 返却（2026-11-02 08:12、fixed） | faa1a9c71（2026-11-02 08:12 に main へ merge） |
- 2026-11-02 07:18（ユーザーの決定）: ブラウザ（WS334・WS074・libbrowser）の作業は当面 P8。P7・P4 は今の Bug の後、ブラウザの作業を P8 に寄せる。
| P2 | phase-runner-high | ws322 p006 (q1919-i01) | `/home/awe/zedBSD-worktrees/p2-ws322` / `agent/p2-ws322` | 返却（2026-11-02 09:46、cleared。今日の WS322 の作業は終わり、encode は次に再開） | d19b5ff6d（2026-11-02 09:46 に main へ merge） |
| P8 | phase-runner-high | BUG-396 の残り (q1920-i01) | `/home/awe/zedBSD-worktrees/p8-bug396b` / `agent/p8-bug396b` | 返却（2026-11-02 07:55、fixed） | 960c63e4a（2026-11-02 07:55 に main へ merge） |
| P8 | phase-runner-high | BUG-401 (q1921-i01) | `/home/awe/zedBSD-worktrees/p8-bug401` / `agent/p8-bug401` | 返却（2026-11-02 08:37、fixed） | 60998cbd5（2026-11-02 08:37 に main へ merge） |
| P1 | phase-runner-high | ws324 p007 (q1922-i01) | `/home/awe/zedBSD-worktrees/p1-ws324` / `agent/p1-ws324` | in-progress（2026-11-02 08:22、RTX） | 5c141459c |
| P3 | phase-runner-high | ws321 p001〜p003 (q1923-i01) | `/home/awe/zedBSD-worktrees/p3-ws321` / `agent/p3-ws321` | in-progress（2026-11-02 08:32 起動、Pi 5） | — |
| P8 | phase-runner-high | BUG-402〜407 (q1924-i01) | `/home/awe/zedBSD-worktrees/p8-uat2` / `agent/p8-uat2` | in-progress（2026-11-02 08:37 起動） | 60998cbd5 |
| Sonnet | phase-runner-low | ws335 p039 (q1925-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p039` / `agent/s-ws335-p039` | in-progress（2026-11-02 09:40 起動） | — |
| P4 | phase-runner-high | ws319 H.265 decode (q1926-i01) | `/home/awe/zedBSD-worktrees/p4-ws319` / `agent/p4-ws319` | in-progress（2026-11-02 10:07 起動、5330） | — |
