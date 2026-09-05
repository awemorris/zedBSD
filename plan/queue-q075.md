# Queue: confirmed-commit overlay publication correction

Last updated: 2026-09-05

QID: `q075`

Queue status: finished

Queue finished: **Yes**

Authorization: the user approved correction on 2026-09-05 ("では修正してください").
The user then expanded verification to ten slightly varied T021 trials and
explicitly allowed clearance if the stop does not recur in any of them.
One diagnostic cell passed, then normal case 06 reproduced the stop. The user
subsequently explicitly approved thorough optimization of the identified FAT
path. That failure is preserved; the four remaining cells all passed post-fix
acceptance, beginning with the failing procedure. T020 was not rerun.

Parent: [master plan](master.md)

Previous Queue: [q074](queue-q074.md)

## Purpose

Correct the target hybrid-overlay atomic-publication stop exposed by q074's
NCOM-T021 cell. Establish the exact blocking stage, make only the responsible
bounded correction, then complete T021 verification so p007 can either complete or retain
a narrower honest residual.

## Execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [`ws011-p009`](ws011-net-config/phase009-confirmed-commit-overlay-publication/phase.md) | completed | Reproduced and corrected FAT backing-file traversal cost; cost/fault/build gates and all four post-fix T021 cases pass; p007 automatic acceptance completed |

## Why this is the next bounded unit

- Q074 accepted T020 but exhausted p007's two-cell boundary when T021 failed to
  return from atomic publication. Repeating that attempt without new evidence
  is prohibited.
- The ten admitted post-commit connections do not prove final DNS completion:
  authentication is logged before request dispatch. Localization must cover
  the final DNS request/response/close, subsequent atomic publication, and
  DISARM connection setup rather than assuming a specific syscall failed.
- P009 defines finite stage instrumentation, lower-only overlay replacement
  tests, strict durability/order invariants, and a maximum QEMU budget. It can
  stop uncleared if the cause requires a material filesystem redesign.
- P008 cannot proceed until automatic T021 passes, and the active WS019 wave
  follows WS011. This correction therefore remains the first dependency-ready
  work.

## Execution and timebox boundary

- One Queue item and one corrective Phase only.
- Begin with static and host stage/order/failure evidence. Use at most one
  instrumented diagnostic amd64 QEMU cell only if target-only localization is
  still required.
- Make only a bounded fix in the responsible netconf, generic sync, overlay, or
  directly backing-filesystem path. Do not weaken file synchronization, atomic
  replacement, error propagation, or publication-before-DISARM ordering.
- User-amended boundary: run ten fresh T021-only cells using normal optimized
  binaries, q074's restricted NE2000 topology, and the real one-minute timeout.
  Vary one procedure/timing feature per case as recorded in P009. Preserve
  failed guest state and investigate any recurrence; do not treat tracing or
  frame-pointer builds as these ten acceptance trials. Do not rerun T020.
- Rerun NCOM-T001--T012, directly affected VFS/storage fixtures, existing
  WS011/ZNV2/Wi-Fi regressions, maintained amd64/i386 target builds,
  documentation validators, and `git diff --check`. Do not run aggregate
  `make check`.
- Do not begin p008 physical work, add a remote service, change public
  grammar/protocol, begin VLAN/bridge, or broaden into a VFS/storage redesign.

## Important uncertainty

Q074 alone did not locate the exact stage. Q075 case 06 now identifies a
running net write/flush through UFS-in-FAT, not a lock deadlock: the healthy
32 MiB backing file has 16,384 clusters, and repeated Floyd validation and
per-sector seeks amplify block-cache work. The correction uses a single
forward validation and operation-local write position, without persistent
validation trust or skipped synchronization. Deterministic old/new cost and
fault gates and cases 07--10 passed. This is a causal-correction result, not ten
successful non-reproductions. A broader architecture change or further runtime
campaign remains outside this completed Queue.

## Approval record

The user's correction request authorizes `ws011-p009` within the boundary
above. Standing user permission permits a `WIP` commit and push.

## Completion result

- Normal case 06 reproduced excessive FAT traversal during the startup writer's
  flush, with a healthy 16,384-cluster DATA.IMG and no held cache lock. Preserve
  its failure; case 01's unrelated host predicate defect was revalidated offline.
- A single forward Brent validation also locates the starting cluster and old
  tail. Operation-local cursors serve data, zero-fill, and growth without
  repeating initial seeks. Every write still validates the entire chain; no
  persistent trust cache, public ABI, synchronization, or rollback weakening.
- The exact 32 MiB cost fixture drops 4 KiB backing writes from 16,399/17,423
  backend reads to 138/138. Old code fails the enforced cost gate. Final cost,
  cursor, and maintained FAT suites pass in ordinary and sanitizer modes,
  including 55 in-loop growth write-failure positions with retry/remount.
- All ten normal cells were consumed: cases 01--05 passed guest observations,
  case 06 failed before correction, and cases 07--10 all passed after correction.
  The latter prove publication, no late rollback after 70 seconds, rebooted
  persistence, address/route/DNS/gateway connectivity, and input integrity.
- NCOM-T001--T012, WS011/ZNV2/Wi-Fi, atomic writer and overlay fault regressions,
  maintained amd64/i386 builds, document validators, and whitespace gates pass.
  Production config and production image remain unchanged.
- P007 and p009 are complete. P008 awaits only its physical transport/topology/
  recovery decisions; p004 remains held. The next dependency-ready wave is
  WS019, subject to a separately approved Queue.

Exact commands, source identities, counts, and observations are retained in the
[q075 evidence](ws011-net-config/tests/q075-confirmed-commit-evidence.md).
