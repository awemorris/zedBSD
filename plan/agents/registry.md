# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。
| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P2 | conformance-reviewer | ws336 p026 試験の C (q2094-i01) | `/home/awe/zedBSD-worktrees/p2-ws336p026` / `agent/p2-ws336p026` | 動いている（2026-11-14 23:2x） | — |
| P7 | phase-runner-mid | ws334 p037 と不具合 (q2095-i01) | `/home/awe/zedBSD-worktrees/p7-ws334b` / `agent/p7-ws334b` | 動いている（2026-11-14 23:2x） | — |
| P8 | phase-runner | ws343 fetch の https (q2096-i01) | `/home/awe/zedBSD-worktrees/p8-ws343` / `agent/p8-ws343` | 返却（2026-11-14、merge は Makefile の案の承認待ち） | — |
| P5 | phase-runner | ws329 p002 共通化 (q2092-i01) | `/home/awe/zedBSD-worktrees/p5-ws329b` / `agent/p5-ws329b` | 動いている（2026-11-14 23:3x） | — |
| P6 | phase-runner-high | ws302 p003 sched の口 (q2099-i01) | `/home/awe/zedBSD-worktrees/p6-ws302b` / `agent/p6-ws302b` | 動いている（2026-11-14 23:4x） | — |
| P3 | phase-runner-mid | ws345 p011 受け入れ (q2097-i01) | `/home/awe/zedBSD-worktrees/p3-ws345p011` / `agent/p3-ws345p011` | 動いている（2026-11-14 23:3x） | — |
| P4 | phase-runner | ws328 p001 review の反映 (q2098-i01) | `/home/awe/zedBSD-worktrees/p4-ws328b` / `agent/p4-ws328b` | 動いている（2026-11-14 23:2x） | — |
| P1 | phase-runner | ws353 p016 試験の再開 (q2088-i01) | `/home/awe/zedBSD-worktrees/p1-ws353n` / `agent/p1-ws353n` | 動いている（2026-11-14 21:3x、5330 を割当） | — |
| Sonnet | phase-runner-low | ws335 p043 (q2053-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p043` / `agent/s-ws335-p043` | 動いている（2026-11-13 22:3x） | — |
| P1 | phase-runner-high | ws347 p006 RTL の CAPS (q2010-i01) | `/home/awe/amaterrace-worktrees/p1-ws347rtl` / `ws347-rtl`（amaterrace-gpu） | 停止（2026-11-11 12:37 ユーザーの指示、未 commit の RTL の変更あり、merge しない） | — |
| P2 | phase-runner-high | ws347 p003 driver の追従 (q2011-i01) | `/home/awe/zedBSD-worktrees/p2-ws347` / `agent/p2-ws347` | 停止（2026-11-11 12:37 ユーザーの指示、未 commit の変更あり） | — |

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
| Sonnet | phase-runner-low | ws324 p006d (q1899-i01) | `/home/awe/zedBSD-worktrees/s-ws324-p006d` / `agent/s-ws324-p006d` | 返却（2026-11-02 03:26、cleared） | 0703840c5 |
| Sonnet | phase-runner-low | ws324 p006e (q1900-i01) | `/home/awe/zedBSD-worktrees/s-ws324-p006e` / `agent/s-ws324-p006e` | 返却（2026-11-02、cleared） | 5cca24183 |
| Sonnet | phase-runner-low | ws324 p006f (q1901-i01) | `/home/awe/zedBSD-worktrees/s-ws324-p006f` / `agent/s-ws324-p006f` | 返却（2026-11-02 03:16、cleared） | 51cd471c2 |
| Sonnet | phase-runner-low | ws335 p018 (q1902-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p018` / `agent/s-ws335-p018` | 返却（2026-11-02 06:09、cleared） | 8d598df28 |
| Sonnet | phase-runner-low | ws335 p019 (q1903-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p019` / `agent/s-ws335-p019` | 返却（2026-11-02 06:09、cleared） | 861875a51 |
| Sonnet | phase-runner-low | ws335 p021 (q1905-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p021` / `agent/s-ws335-p021` | 返却（2026-11-02、cleared） | e594bc29e |
| Sonnet | phase-runner-low | ws324 p006g (q1906-i01) | `/home/awe/zedBSD-worktrees/s-ws324-p006g` / `agent/s-ws324-p006g` | 返却（2026-11-02 06:09、cleared） | e4045b157 |
| Sonnet | phase-runner-low | ws335 p023 (q1908-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p023` / `agent/s-ws335-p023` | 返却（2026-11-02 06:09、cleared） | 14a11d50d |
| Sonnet | phase-runner-low | ws335 p020 (q1912-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p020` / `agent/s-ws335-p020` | 返却（2026-11-02、cleared） | 46ff949c8 |
| Sonnet | phase-runner-low | ws335 p024 (q1913-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p024` / `agent/s-ws335-p024` | 返却（2026-11-02 09:40、cleared） | 1a82b52f6 |
| P8 | phase-runner-high | BUG-395・396 (q1914-i01) | `/home/awe/zedBSD-worktrees/p8-bug395` / `agent/p8-bug395` | in-progress（2026-11-02 06:16 起動） | — |
- 2026-11-02 07:18（ユーザーの決定）: ブラウザ（WS334・WS074・libbrowser）の作業は当面 P8。P7・P4 は今の Bug の後、ブラウザの作業を P8 に寄せる。
| Sonnet | phase-runner-low | ws335 p039 (q1925-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p039` / `agent/s-ws335-p039` | 返却（2026-11-02 10:21、cleared） | 6cb4f008b |
| Sonnet | phase-runner-low | ws335 p022 (q1927-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p022` / `agent/s-ws335-p022` | in-progress（2026-11-02 10:10 起動） | — |
| P5 | phase-runner-high | ws336 p008 (q1932-i01) | `/home/awe/zedBSD-worktrees/p5-ws336` / `agent/p5-ws336` | in-progress（2026-11-02 10:52 起動） | — |
| Sonnet | phase-runner-low | ws336 p009 (q1933-i01) | `/home/awe/zedBSD-worktrees/s-ws336-p009` / `agent/s-ws336-p009` | in-progress（2026-11-02 10:52 起動） | — |
| Sonnet | phase-runner-low | ws336 p011 (q1934-i01) | `/home/awe/zedBSD-worktrees/s-ws336-p011` / `agent/s-ws336-p011` | in-progress（2026-11-02 10:52 起動） | — |
| Sonnet | phase-runner-low | ws336 p014 (q1935-i01) | `/home/awe/zedBSD-worktrees/s-ws336-p014` / `agent/s-ws336-p014` | in-progress（2026-11-02 10:52 起動） | — |
| Sonnet | phase-runner-low | ws344 p002 (q1946-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p002` / `agent/s-ws344-p002` | 返却（2026-11-02 13:39、cleared） | c9339766d（2026-11-02 13:39 に main へ merge） |
| Sonnet | phase-runner-low | ws344 p012 (q1952-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p012` / `agent/s-ws344-p012` | 返却（2026-11-02 13:46、cleared） | 17414580f（2026-11-02 13:46 に main へ merge） |
| Sonnet | phase-runner-low | ws344 p013 (q1954-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p013` / `agent/s-ws344-p013` | 返却（2026-11-02 13:58、cleared） | c7e335d80（2026-11-02 13:58 に main へ merge） |
| Sonnet | phase-runner-low | ws344 p014 (q1958-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p014` / `agent/s-ws344-p014` | 返却（2026-11-02 14:07、cleared） | 06940dba1（2026-11-02 14:07 に main へ merge） |
| Sonnet | phase-runner-low | ws344 p015 (q1960-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p015` / `agent/s-ws344-p015` | 返却（2026-11-02 14:19、cleared） | d3219af90（2026-11-02 14:19 に main へ merge） |
| Sonnet | phase-runner-low | ws344 p018 (q1963-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p018` / `agent/s-ws344-p018` | 返却（2026-11-02 14:30、cleared） | 017987380（2026-11-02 14:30 に main へ merge） |
| Sonnet | phase-runner-low | ws344 p003 (q1947-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p003` / `agent/s-ws344-p003` | 返却（2026-11-02 13:36、cleared） | 2098ad02d（2026-11-02 13:36 に main へ merge） |
| Sonnet | phase-runner-low | ws344 p004 (q1950-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p004` / `agent/s-ws344-p004` | 返却（2026-11-02 13:55、cleared） | 614bf75dd（2026-11-02 13:55 に main へ merge） |
| Sonnet | phase-runner-low | ws344 p005 (q1957-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p005` / `agent/s-ws344-p005` | 返却（2026-11-02 14:17、cleared） | d6d5b1249（2026-11-02 14:17 に main へ merge） |
| Sonnet | phase-runner-low | ws344 p007 (q1962-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p007` / `agent/s-ws344-p007` | 返却（2026-11-02 14:32、cleared） | b667e13b1（2026-11-02 14:32 に main へ merge） |
| Sonnet | phase-runner-low | ws344 p008 (q1964-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p008` / `agent/s-ws344-p008` | 返却（2026-11-02 14:42、cleared） | d89d4073f（2026-11-02 14:42 に main へ merge） |
| Sonnet | phase-runner-low | ws344 p009 (q1948-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p009` / `agent/s-ws344-p009` | 返却（2026-11-02 13:29、cleared） | c4dd16ad0（2026-11-02 13:29 に main へ merge） |
| Sonnet | phase-runner-low | ws344 p010 (q1949-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p010` / `agent/s-ws344-p010` | 返却（2026-11-02 13:37、cleared） | 2c8e3429a（2026-11-02 13:37 に main へ merge） |
| Sonnet | phase-runner-low | ws344 p011 (q1951-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p011` / `agent/s-ws344-p011` | 返却（2026-11-02 13:55、cleared） | d014edfd5（2026-11-02 13:55 に main へ merge） |
| Sonnet | phase-runner-low | ws344 p006 (q1956-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p006` / `agent/s-ws344-p006` | 返却（2026-11-02 14:14、cleared） | 48cf51ccd（2026-11-02 14:14 に main へ merge） |
| Sonnet | phase-runner-low | ws344 p016 (q1961-i01) | `/home/awe/zedBSD-worktrees/s-ws344-p016` / `agent/s-ws344-p016` | 返却（2026-11-02 14:41、cleared） | 5dd9aa50c（2026-11-02 14:41 に main へ merge） |
| Sonnet | phase-runner-low | ws335 p030 (q1937-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p030` / `agent/s-ws335-p030` | 返却（2026-11-02 12:21、cleared） | 37f9fa0dd（2026-11-02 12:21 に main へ merge） |
| Sonnet | phase-runner-low | ws335 p031 (q1943-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p031` / `agent/s-ws335-p031` | 返却（2026-11-02 13:41、cleared） | 9336ee6f5（2026-11-02 13:41 に main へ merge） |
| Sonnet | phase-runner-low | ws335 p032 (q1953-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p032` / `agent/s-ws335-p032` | 返却（2026-11-02 15:49、cleared） | 0bd943451（2026-11-02 15:49 に main へ merge） |
