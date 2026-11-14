# 担当の台帳

今の担当だけ（[運用](protocol.md)）。返却した担当の行は、次の担当を起動する時に Q1 が消す。2026-10-26 までの台帳は
[history/agents-registry-through-20261026.md](../history/agents-registry-through-20261026.md)。

| 担当 | 定義 | 仕事（Queue） | worktree / branch | 状態 | 最後の merge |
| --- | --- | --- | --- | --- | --- |
| P2 | phase-runner | ws347 p010 RTL の reset cp ほか (q2080-i01) | `/home/awe/amaterrace-worktrees/p2-reset` / `p2-reset` | 動いている（2026-11-14） | — |
| P4 | phase-runner | ws347 p010 RTL の reset su・vf・tu (q2081-i01) | `/home/awe/amaterrace-worktrees/p4-reset` / `p4-reset` | 動いている（2026-11-14） | — |
| P5 | phase-runner | ws347 p010 RTL の reset rx・rop (q2082-i01) | `/home/awe/amaterrace-worktrees/p5-reset` / `p5-reset` | 動いている（2026-11-14） | — |
| P2 | phase-runner | BUG-435 古い build の変数 (q2079-i01) | `/home/awe/zedBSD-worktrees/p2-bug435` / `agent/p2-bug435` | 返却（2026-11-14、1 段目 cleared） | c0603e0fcc（merge 7d8ef71552） |
| P2 | phase-runner-high | BUG-433 ws131 host 試験 (q2077-i01) | `/home/awe/zedBSD-worktrees/p2-bug433` / `agent/p2-bug433` | 返却（2026-11-14、fixed） | 07bef9d24d |
| P1 | phase-runner-high | ws353 p012 cursor (q2076-i01) | `/home/awe/zedBSD-worktrees/p1-ws353k` / `agent/p1-ws353k` | 動いている（2026-11-14） | — |
| P2 | phase-runner-high | BUG-432 stdout の flush (q2075-i01) | `/home/awe/zedBSD-worktrees/p2-bug432` / `agent/p2-bug432` | 返却（2026-11-14、fixed） | 97706198bb（merge 8ba44ca444） |
| P1 | phase-runner-high | ws353 p019 printers (q2074-i01) | `/home/awe/zedBSD-worktrees/p1-ws353j` / `agent/p1-ws353j` | 返却（2026-11-14、cleared） | 810dba2742（merge 8dddb662f6） |
| P2 | phase-runner-high | ws353 p010 shown・client の buffer (q2073-i01) | `/home/awe/zedBSD-worktrees/p2-ws353e` / `agent/p2-ws353e` | 返却（2026-11-14、merge 保留: ユーザーの判断待ち） | f129cf3a30 |
| P1 | phase-runner-high | ws353 p018 入力と sessiond (q2072-i01) | `/home/awe/zedBSD-worktrees/p1-ws353i` / `agent/p1-ws353i` | 返却（2026-11-14、cleared） | e8b9a14a0d（merge f4d831e0ea） |
| P3 | conformance-reviewer | ws335 p047 規約の見直し (q2071-i01) | `/home/awe/zedBSD-worktrees/p3-ws335p047` / `agent/p3-ws335p047` | 返却（2026-11-14、cleared） | a681a1e68f |
| P4 | phase-runner | BUG-430 Makefile の案 (q2070-i01) | `/home/awe/zedBSD-worktrees/p4-bug430` / `agent/p4-bug430` | 返却（2026-11-14） | 57ab1210b3 |
| P3 | phase-runner | ws335 p046 統合 (q2069-i01) | `/home/awe/zedBSD-worktrees/p3-ws335p046` / `agent/p3-ws335p046` | 返却（2026-11-14、cleared） | 9730bb1c22（merge 2f0b7eed5f） |
| P1 | phase-runner-high | ws353 p008 main の poll -1 (q2068-i01) | `/home/awe/zedBSD-worktrees/p1-ws353h` / `agent/p1-ws353h` | 返却（2026-11-14、cleared） | 4f4112c981（merge d795c0b9a4） |
| P2 | phase-runner-high | ws353 p011 render の slot (q2067-i01) | `/home/awe/zedBSD-worktrees/p2-ws353d` / `agent/p2-ws353d` | 返却（2026-11-14、cleared） | 1a9c8d8901 |
| P2 | phase-runner-high | ws353 p015 network・WSI (q2066-i01) | `/home/awe/zedBSD-worktrees/p2-ws353c` / `agent/p2-ws353c` | 返却（2026-11-14、cleared） | afbbfeed48 |
| P1 | phase-runner-high | ws353 p007 締め切り B (q2065-i01) | `/home/awe/zedBSD-worktrees/p1-ws353g` / `agent/p1-ws353g` | 返却（2026-11-14、cleared） | a6eafe010b |
| P2 | phase-runner-high | ws353 p014 refresh の fence (q2064-i01) | `/home/awe/zedBSD-worktrees/p2-ws353b` / `agent/p2-ws353b` | 返却（2026-11-14、cleared） | f3c6bb3e9a（merge 2498bc8853） |
| P1 | phase-runner-high | ws353 p006 締め切り A (q2062-i01) | `/home/awe/zedBSD-worktrees/p1-ws353f` / `agent/p1-ws353f` | 返却（2026-11-14、cleared） | d375b9db1b |
| P2 | phase-runner-high | ws353 p009 present の fence (q2063-i01) | `/home/awe/zedBSD-worktrees/p2-ws353a` / `agent/p2-ws353a` | 返却（2026-11-14、cleared） | 8a7503373a |
| P1 | phase-runner-high | ws353 p005 timer ほか (q2061-i01) | `/home/awe/zedBSD-worktrees/p1-ws353e` / `agent/p1-ws353e` | 返却（2026-11-14、cleared） | cff19670bf（merge 6b1c2de16a） |
| P1 | phase-runner-high | ws353 p004 hotplug の thread (q2060-i01) | `/home/awe/zedBSD-worktrees/p1-ws353d` / `agent/p1-ws353d` | 返却（2026-11-14、cleared） | e7584e6425 |
| P1 | phase-runner-high | ws353 p003 libvulkan (q2059-i01) | `/home/awe/zedBSD-worktrees/p1-ws353c` / `agent/p1-ws353c` | 返却（2026-11-14、cleared） | e5039925e0 |
| P2 | phase-runner-high | BUG-431 venus の display の事象 (q2058-i01) | `/home/awe/zedBSD-worktrees/p2-bug431` / `agent/p2-bug431` | 返却（2026-11-14、fixed） | 5ca7945a36 |
| P1 | phase-runner-high | ws353 p002 kernel の hotplug の fence (q2057-i01) | `/home/awe/zedBSD-worktrees/p1-ws353b` / `agent/p1-ws353b` | 動いている（2026-11-13） | — |
| P1 | phase-runner-high | ws353 p001 設計 (q2056-i01) | `/home/awe/zedBSD-worktrees/p1-ws353` / `agent/p1-ws353` | 動いている（2026-11-13 22:2x） | — |
| Sonnet | phase-runner-low | ws335 p043 (q2053-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p043` / `agent/s-ws335-p043` | 動いている（2026-11-13 22:3x） | — |
| P1 | phase-runner-high | ws352 p001 の仕上げ (q2046-i02) | `/home/awe/zedBSD-worktrees/p1-ws352` / `agent/p1-ws352` | 返却（2026-11-13 22:1x、未 merge） | — |
| P3 | phase-runner-high | ws334 p040 winpthreads を外す (q2052-i01) | `/home/awe/zedBSD-worktrees/p3-ws334c` / `agent/p3-ws334c` | 返却（2026-11-13 22:2x、cleared） | bf80fa75b3（merge 4688966210） |
| P2 | phase-runner | ws335 p042 compat を前提の設計 (q2051-i01) | `/home/awe/zedBSD-worktrees/p2-ws335f` / `agent/p2-ws335f` | 返却（2026-11-13 22:0x） | f4b46e6251（merge 2ae171867d） |
| P1 | phase-runner-high | ws333 thermal の権限 (q2015-i01) | `/home/awe/zedBSD-worktrees/p1-ws333e` / `agent/p1-ws333e` | 返却（2026-11-12） | 03f66fffcf（merge b1ade10787） |
| P2 | phase-runner-high | ws347 Gemini の修正の監査 (q2014-i01) | `/home/awe/zedBSD-worktrees/p2-ws347a` / `agent/p2-ws347a` | 返却（2026-11-12） | 5f117d149e（merge b2ecd73d78） |
| P1 | phase-runner-high | ws333 の UAPI の実装 (q2013-i01) | `/home/awe/zedBSD-worktrees/p1-ws333d` / `agent/p1-ws333d` | 返却（2026-11-12） | 54ae40fb23（merge 10f86ae6e8） |
| P1 | phase-runner-high | ws333 の再開 (q2012-i01) | `/home/awe/zedBSD-worktrees/p1-ws333c` / `agent/p1-ws333c` | 返却（2026-11-12） | ea64e3349f（merge c1d2505046） |
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
| Sonnet | phase-runner-low | ws335 p020 (q1912-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p020` / `agent/s-ws335-p020` | 返却（2026-11-02、cleared） | 46ff949c8 |
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
| P1 | phase-runner-high | ws324 p007 (q1922-i01) | `/home/awe/zedBSD-worktrees/p1-ws324` / `agent/p1-ws324` | 返却（2026-11-02 11:17、cleared。今日の WS324 の作業は終わり、encode は次に再開） | 3f03eb54a（2026-11-02 11:17 に main へ merge） |
| P3 | phase-runner-high | ws321 p001〜p003 (q1923-i01) | `/home/awe/zedBSD-worktrees/p3-ws321` / `agent/p3-ws321` | 返却（2026-11-02 10:17、cleared） | fa5992dd0（2026-11-02 10:17 に main へ merge） |
| P8 | phase-runner-high | BUG-402〜407 (q1924-i01) | `/home/awe/zedBSD-worktrees/p8-uat2` / `agent/p8-uat2` | 返却（2026-11-02 11:38、5 件 fixed） | 9b4b9f627（2026-11-02 11:38 に main へ merge） |
| Sonnet | phase-runner-low | ws335 p039 (q1925-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p039` / `agent/s-ws335-p039` | 返却（2026-11-02 10:21、cleared） | 6cb4f008b |
| P4 | phase-runner-high | ws319 H.265 decode (q1926-i01) | `/home/awe/zedBSD-worktrees/p4-ws319` / `agent/p4-ws319` | 返却（2026-11-02 12:03、cleared） | c36151f73（2026-11-02 12:03 に main へ merge） |
| Sonnet | phase-runner-low | ws335 p022 (q1927-i01) | `/home/awe/zedBSD-worktrees/s-ws335-p022` / `agent/s-ws335-p022` | in-progress（2026-11-02 10:10 起動） | — |
| P6 | phase-runner-high | ws335 p029 (q1928-i01) | `/home/awe/zedBSD-worktrees/p6-ws335d` / `agent/p6-ws335d` | 返却（2026-11-02、cleared） | c158243c2 |
| P7 | phase-runner-high | ws335 p038 (q1929-i01) | `/home/awe/zedBSD-worktrees/p7-ws335e` / `agent/p7-ws335e` | 返却（2026-11-02 11:50、cleared） | 76b8017b2 |
| P3 | phase-runner-high | BUG-409 (q1930-i01) | `/home/awe/zedBSD-worktrees/p3-bug409` / `agent/p3-bug409` | 返却（2026-11-02 10:41、fixed） | 708fb3d9c（2026-11-02 10:41 に main へ merge） |
| P3 | phase-runner-high | BUG-410 (q1931-i01) | `/home/awe/zedBSD-worktrees/p3-bug410` / `agent/p3-bug410` | 返却（2026-11-02 11:03、fixed） | fbeea4f29（2026-11-02 11:03 に main へ merge） |
| P5 | phase-runner-high | ws336 p008 (q1932-i01) | `/home/awe/zedBSD-worktrees/p5-ws336` / `agent/p5-ws336` | in-progress（2026-11-02 10:52 起動） | — |
| Sonnet | phase-runner-low | ws336 p009 (q1933-i01) | `/home/awe/zedBSD-worktrees/s-ws336-p009` / `agent/s-ws336-p009` | in-progress（2026-11-02 10:52 起動） | — |
| Sonnet | phase-runner-low | ws336 p011 (q1934-i01) | `/home/awe/zedBSD-worktrees/s-ws336-p011` / `agent/s-ws336-p011` | in-progress（2026-11-02 10:52 起動） | — |
| Sonnet | phase-runner-low | ws336 p014 (q1935-i01) | `/home/awe/zedBSD-worktrees/s-ws336-p014` / `agent/s-ws336-p014` | in-progress（2026-11-02 10:52 起動） | — |
| P3 | phase-runner-high | BUG-412 (q1936-i01) | `/home/awe/zedBSD-worktrees/p3-bug412` / `agent/p3-bug412` | 返却（2026-11-02 13:15、再現せず） | 45099f582（2026-11-02 13:15 に main へ merge） |
| P3 | phase-runner-high | BUG-412 の log (q1945-i01) | `/home/awe/zedBSD-worktrees/p3-bug412` / `agent/p3-bug412` | 返却（2026-11-02 13:31、cleared） | 8e9c773ad（2026-11-02 13:31 に main へ merge） |
| P3 | phase-runner-high | BUG-412 の直し (q1955-i01) | `/home/awe/zedBSD-worktrees/p3-bug412` / `agent/p3-bug412` | 返却（2026-11-02 14:27、uncleared） | 82f7aba10（2026-11-02 14:27 に main へ merge） |
| P3 | phase-runner-high | BUG-412 の常時の記録 (q1967-i01) | `/home/awe/zedBSD-worktrees/p3-bug412` / `agent/p3-bug412` | 返却（2026-11-02 15:29、cleared） | 15e61e1cf（2026-11-02 15:29 に main へ merge） |
| P3 | phase-runner-high | BUG-412 の直しの候補 (q1970-i01) | `/home/awe/zedBSD-worktrees/p3-bug412` / `agent/p3-bug412` | 返却（2026-11-02 15:44、実機の確かめ待ち） | — |
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
| P1 | phase-runner-high | ws335 p033 (q1971-i01) | `/home/awe/zedBSD-worktrees/p1-ws335` / `agent/p1-ws335` | 返却（2026-11-02 17:43、cleared） | f31fbecbd（2026-11-02 17:43 に main へ merge） |
| P1 | phase-runner-high | ws335 p040 (q1981-i01) | `/home/awe/zedBSD-worktrees/p1-ws335` / `agent/p1-ws335` | 返却（2026-11-02 18:41、cleared） | ddc678f69（2026-11-02 18:41 に main へ merge） |
| P1 | phase-runner-high | ws335 p034 (q1986-i01) | `/home/awe/zedBSD-worktrees/p1-ws335` / `agent/p1-ws335` | 返却（2026-11-02 21:35、cleared） | a37f972c7（2026-11-02 21:35 に main へ merge） |
| P1 | phase-runner-high | ws335 p035 (q1992-i01) | `/home/awe/zedBSD-worktrees/p1-ws335` / `agent/p1-ws335` | 返却（2026-11-03 00:05、ラップアップ） | 5a7359a3a（2026-11-03 00:27 に main へ merge） |
| P6 | phase-runner-high | ws336 p016 (q1938-i01) | `/home/awe/zedBSD-worktrees/p6-ws336` / `agent/p6-ws336` | 返却（2026-11-02 12:37、cleared） | e8b178482（2026-11-02 12:37 に main へ merge） |
| P6 | phase-runner-high | ws336 p017 (q1944-i01) | `/home/awe/zedBSD-worktrees/p6-ws336` / `agent/p6-ws336` | 返却（2026-11-02 14:36、cleared） | 9f1fdd60f（2026-11-02 14:36 に main へ merge） |
| P6 | phase-runner-high | ws336 p018 (q1965-i01) | `/home/awe/zedBSD-worktrees/p6-ws336` / `agent/p6-ws336` | 返却（2026-11-02 16:35、cleared） | 1d7df5834（2026-11-02 16:35 に main へ merge） |
| P6 | phase-runner-high | ws336 p019 (q1975-i01) | `/home/awe/zedBSD-worktrees/p6-ws336` / `agent/p6-ws336` | 返却（2026-11-02 18:04、cleared） | 024a472c7（2026-11-02 18:04 に main へ merge） |
| P6 | phase-runner-high | ws336 p020 (q1983-i01) | `/home/awe/zedBSD-worktrees/p6-ws336` / `agent/p6-ws336` | 返却（2026-11-02 20:23、cleared） | 0aaa5801c（2026-11-02 20:23 に main へ merge） |
| P6 | phase-runner-high | ws336 p021 (q1989-i01) | `/home/awe/zedBSD-worktrees/p6-ws336` / `agent/p6-ws336` | 返却（2026-11-02 21:15、cleared） | 54778d48a（2026-11-02 21:15 に main へ merge） |
| P6 | phase-runner-high | ws336 p022 (q1991-i01) | `/home/awe/zedBSD-worktrees/p6-ws336` / `agent/p6-ws336` | 返却（2026-11-02 23:27、cleared） | 07cf884f5（2026-11-02 23:27 に main へ merge） |
| P6 | phase-runner-high | ws336 p023 (q1998-i01) | `/home/awe/zedBSD-worktrees/p6-ws336` / `agent/p6-ws336` | 返却（2026-11-02 23:40、cleared） | 6e6fd8452（2026-11-02 23:40 に main へ merge） |
| P6 | phase-runner-high | ws336 p024 (q1999-i01) | `/home/awe/zedBSD-worktrees/p6-ws336` / `agent/p6-ws336` | 返却（2026-11-03 00:05、ラップアップ） | 58f18a624（2026-11-03 00:05 に main へ merge） |
| P2 | phase-runner-high | ws343 p002 (q1990-i01) | `/home/awe/zedBSD-worktrees/p2-ws343` / `agent/p2-ws343` | 返却（2026-11-02 22:20、cleared） | b8c4adaf6（2026-11-02 22:20 に main へ merge） |
| P2 | phase-runner-high | ws344 p001 (q1939-i01) | `/home/awe/zedBSD-worktrees/p2-ws344` / `agent/p2-ws344` | 返却（2026-11-02 13:21、cleared） | ffa491347（2026-11-02 13:21 に main へ merge） |
| P2 | phase-runner-high | ws344 p017 (q1966-i01) | `/home/awe/zedBSD-worktrees/p2-ws344` / `agent/p2-ws344` | 返却（2026-11-02 15:22、cleared） | 0fd7fe84d（2026-11-02 15:22 に main へ merge） |
| P2 | phase-runner-high | ws344 p019 (q1969-i01) | `/home/awe/zedBSD-worktrees/p2-ws344` / `agent/p2-ws344` | 返却（2026-11-02 16:35、cleared） | 15ec0a6de（2026-11-02 16:35 に main へ merge） |
| P2 | phase-runner-high | ws344 p020 (q1976-i01) | `/home/awe/zedBSD-worktrees/p2-ws344` / `agent/p2-ws344` | 返却（2026-11-02 17:29、cleared） | 9424d21b0（2026-11-02 17:29 に main へ merge） |
| P2 | phase-runner-high | ws344 p021 (q1979-i01) | `/home/awe/zedBSD-worktrees/p2-ws344` / `agent/p2-ws344` | 返却（2026-11-02 17:56、cleared） | 2d115efe8（2026-11-02 17:56 に main へ merge） |
| P2 | phase-runner-high | ws344 p022 (q1982-i01) | `/home/awe/zedBSD-worktrees/p2-ws344` / `agent/p2-ws344` | 返却（2026-11-02 18:41、cleared） | b114f4ef2（2026-11-02 18:41 に main へ merge） |
| P2 | phase-runner-high | WS344 の完了 (q1987-i01) | `/home/awe/zedBSD-worktrees/p2-ws344` / `agent/p2-ws344` | 返却（2026-11-02 20:23、cleared） | 166e6f675（2026-11-02 20:23 に main へ merge） |
| P5 | phase-runner-high | ws336 p015 (q1940-i01) | `/home/awe/zedBSD-worktrees/p5-ws336` / `agent/p5-ws336` | 返却（2026-11-02 13:46、cleared） | 00a9f4ccc（2026-11-02 13:46 に main へ merge） |
| P8 | phase-runner-high | BUG-411 ほか (q1941-i01) | `/home/awe/zedBSD-worktrees/p8-uat3` / `agent/p8-uat3` | 返却（2026-11-02 13:52、一部） | e58b58424（2026-11-02 13:52 に main へ merge） |
| P8 | phase-runner-high | ws345 p001 (q1959-i01) | `/home/awe/zedBSD-worktrees/p8-ws345` / `agent/p8-ws345` | 返却（2026-11-02 14:46、cleared） | 87feb4707（2026-11-02 14:46 に main へ merge） |
| P8 | phase-runner-high | ws345 p002 (q1968-i01) | `/home/awe/zedBSD-worktrees/p8-ws345` / `agent/p8-ws345` | 返却（2026-11-02 15:59、cleared） | 4a4b68de0（2026-11-02 15:59 に main へ merge） |
| P8 | phase-runner-high | ws345 p003 (q1972-i01) | `/home/awe/zedBSD-worktrees/p8-ws345` / `agent/p8-ws345` | 返却（2026-11-02 16:47、cleared） | 6bfb56328（2026-11-02 16:47 に main へ merge） |
| P8 | phase-runner-high | BUG-414・415 (q1977-i01) | `/home/awe/zedBSD-worktrees/p8-uat3` / `agent/p8-uat3` | 返却（2026-11-02 17:56、cleared） | cb162ceca（2026-11-02 17:56 に main へ merge） |
| P3 | phase-runner-high | BUG-416 (q1978-i01) | `/home/awe/zedBSD-worktrees/p3-bug416` / `agent/p3-bug416` | 返却（2026-11-02 17:56、cleared） | de9fd4b3f（2026-11-02 17:56 に main へ merge） |
| P5 | phase-runner-high | ws345 p004 (q1973-i01) | `/home/awe/zedBSD-worktrees/p5-ws345` / `agent/p5-ws345` | 返却（2026-11-02 17:33、cleared） | db42ea1a8（2026-11-02 17:33 に main へ merge） |
| P5 | phase-runner-high | ws345 p005 (q1980-i01) | `/home/awe/zedBSD-worktrees/p5-ws345` / `agent/p5-ws345` | 返却（2026-11-02 18:28、cleared） | 283f5600c（2026-11-02 18:28 に main へ merge） |
| P8 | phase-runner-high | ws345 p006 (q1984-i01) | `/home/awe/zedBSD-worktrees/p8-ws345` / `agent/p8-ws345` | 返却（2026-11-02 20:23、cleared） | 0ac211b5c（2026-11-02 20:23 に main へ merge） |
| P8 | phase-runner-high | ws345 p007 (q1988-i01) | `/home/awe/zedBSD-worktrees/p8-ws345` / `agent/p8-ws345` | 返却（2026-11-02 22:31、cleared） | 31980ffef（2026-11-02 22:31 に main へ merge） |
| P5 | phase-runner-high | ws345 p008 (q1985-i01) | `/home/awe/zedBSD-worktrees/p5-ws345` / `agent/p5-ws345` | 返却（2026-11-02 21:40、cleared） | d8517c082（2026-11-02 21:40 に main へ merge） |
| P5 | phase-runner-high | ws345 p009 (q1993-i01) | `/home/awe/zedBSD-worktrees/p5-ws345` / `agent/p5-ws345` | 返却（2026-11-02 22:58、cleared） | 78d8be32d（2026-11-02 22:58 に main へ merge） |
| P5 | phase-runner-high | ws345 p010 (q1995-i01) | `/home/awe/zedBSD-worktrees/p5-ws345` / `agent/p5-ws345` | 返却（2026-11-03 00:05、ラップアップ） | 9de0527c8（2026-11-03 00:05 に main へ merge） |
| P8 | phase-runner-high | ws345 p014 (q1996-i01) | `/home/awe/zedBSD-worktrees/p8-ws345` / `agent/p8-ws345` | 返却（2026-11-03 00:05、ラップアップ） | 031c1ef34（2026-11-03 00:05 に main へ merge） |
| P3 | phase-runner-high | BUG-412 の締め (q1997-i01) | `/home/awe/zedBSD-worktrees/p3-bug412` / `agent/p3-bug412` | 返却（2026-11-02 23:40、cleared） | c4d563f88（2026-11-02 23:40 に main へ merge） |
| P3 | phase-runner-high | BUG-412 hot-plug (q1994-i01) | `/home/awe/zedBSD-worktrees/p3-bug412` / `agent/p3-bug412` | 返却（2026-11-02 22:31、再現せず） | — |
| P4 | phase-runner-high | ws345 p013 (q1974-i01) | `/home/awe/zedBSD-worktrees/p4-ws345` / `agent/p4-ws345` | 返却（2026-11-02 17:29、cleared） | 481b0ef40（2026-11-02 17:29 に main へ merge） |
| P1 | phase-runner-high | BUG-413 (q1942-i01) | `/home/awe/zedBSD-worktrees/p1-bug413` / `agent/p1-bug413` | 返却（2026-11-02 12:20、fixed） | 28ad1130b（2026-11-02 12:20 に main へ merge） |
