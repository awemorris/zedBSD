# WS005 Phase 012: Wi-Fi command and operation-story repairs

Date: 2026-09-06
Combined ID: `ws005-p012`
Status: completed (`q085`)

Current result (2026-09-06): following the user's explicit acceptance release,
P048 is implemented and all 30 saved stories pass ordinary and ASan/UBSan runs.
Applicable regressions, actual host command/daemon integration and serialized
amd64/PCAT/PC98 builds pass. See [individual outcomes and limits](results.md).
Physical RF and QEMU were not exercised in this cycle. Earlier review decisions
remain in [review disposition](review2-response.md) and
[implementation history](implementation.md).

Execution correction from the user: first rebuild the managed implementation
as a coherent whole. The saved 30 stories are acceptance tests after this
structural repair, not a test-first driver for accumulating local patches.
Do not spend the implementation stage building or repeatedly running the
30-story harness. Use compilation and narrow checks when needed to validate
the structural changes, then execute acceptance and regression gates.

## Objective and execution authority

The user reports working direct association on RTL8822BU and AX211 but unusable
managed command sequences: enable before adding an automatic key does not
connect, repeated enable destabilizes interface state, and `wifi wlan0 list`
does not show SSIDs. The user explicitly authorizes static investigation,
primitive repair, then exactly 30 command-story scenarios and corrections until
all pass. This request supplies implementation authorization for Q085.
Review progress every 90 active minutes; continue necessary authorized repairs
and retain unresolved failures until their actual acceptance conditions pass.

## Scope and fixed behavior

User scope extension: Q085 also includes WS004 p047's explicitly requested
AX211/RTL8822BU review corrections. P012 acceptance stays pending until that
implementation checkpoint; the earlier fixed-driver scope is superseded only
for those two drivers and the common lifecycle contracts they use.

Preserve the six public global `net wifi` forms, euid-selected credential store,
one managed connection, direct interface-specific `wifi` recovery path and
current drivers. WS005 owns this follow-up; WS011's confirmed-commit and held
VLAN/bridge scopes remain separate.

Preserve direct `wifi list` as a nonblocking snapshot operation, including an
empty result while scanning. The user explicitly confirmed this behavior.
Validate the findings in `plan/old/net-wifi-report.md` against production code,
focusing on managed lifecycle, shared deadlines, child completion and request
responsiveness. Repair machine connect progress only where its stream violates
the daemon's bounded record contract.

The normal story is save an automatic profile, enable, list/observe connected
L2 plus DHCP, disconnect with automatic selection suppressed, reconnect or
re-enable, then disable. Enable before set-key-auto must converge without a
second enable. Repetition, invalid commands, key corrections and failed work
must leave recoverable state. Unknown/invalid input fails before mutation;
manual disconnect suppresses auto; changing profiles while disabled does not
enable Wi-Fi. A working selected connection must not be corrupted by listing or
repeating enable. At most one owned L2/L3 connection may exist at any step.

## Execution sequence

1. Freeze the current command sources and prior fixtures; statically inspect
   direct ioctl/list handling and managed scheduling/preparation/retirement.
   Record concrete faults and test blind spots before fixing them.
2. Rebuild request dispatch, policy intent, operation deadlines, cancellation,
   connection ownership and retryable retirement as one consistent design.
   Preserve direct snapshot semantics and the public six-command grammar.
3. Save 30 numbered command stories with preconditions, ordered public commands,
   expected state/SSID/interface/L2/L3, error outcome and a recovery endpoint.
   Include both normal orders, reordered operations, repeated commands, corrected
   keys, missing/partial/dual radios, scan/auth/DHCP/retirement failures and owner
   isolation. Use synthetic SSIDs/keys, never the prior physical credentials.
4. After the structural implementation and the requested external audit are
   complete, and the user resumes work, execute stories through actual command dispatch and production daemon policy,
   child parsing and primitive handling as feasible. Model only external radio,
   clock, process and L3 boundaries; do not implement a parallel policy model or
   count individual assertions as 30 scenarios. Require exact command results,
   observed state and bounded completion at every step; retain initial failures.
5. Correct source-localized defects and rerun affected stories, then the complete
   30-story normal/sanitized set and applicable existing command/protocol/store/
   child/managed/retirement/wired regression gates. Review fixture boundaries
   against production contracts to prevent optimistic doubles.
6. Run serialized `make -j16` for explicit amd64/PCAT/PC98 CI configurations.
   Run a practical integration smoke with the actual command/daemon boundary;
   clearly distinguish deterministic scenario coverage from real RF acceptance.
7. Record all 30 individual outcomes, before/after evidence, commands and limits;
   synchronize Q/P/W/M only when all required gates actually pass.

No commits, aggregate `make check`, unrelated kernel refactor, new model of Wi-Fi
security or private `.internal` reads. Prior secrets must not appear in scenarios,
source or retained logs. Current authorization does not require another approval
before routine local repairs/tests. Any unavailable physical verification is
reported as a coverage limit, never converted into simulated hardware success.
