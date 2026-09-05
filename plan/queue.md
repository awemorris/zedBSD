# Queue: confirmed-commit overlay publication correction

Last updated: 2026-09-05

QID: `q075`

Queue status: proposed

Queue finished: **No**

Authorization: pending explicit user approval; no implementation or additional
QEMU execution is authorized by this proposal.

Parent: [master plan](master.md)

Previous Queue: [q074](queue-q074.md)

## Purpose

Correct the target hybrid-overlay atomic-publication stop exposed by q074's
NCOM-T021 cell. Establish the exact blocking stage, make only the responsible
bounded correction, then rerun T021 once so p007 can either complete or retain
a narrower honest residual.

## Execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [`ws011-p009`](ws011-net-config/phase009-confirmed-commit-overlay-publication/phase.md) | pending | Diagnose and correct the post-reconcile/pre-DISARM atomic publication stop, then perform one T021-only corrective acceptance; depends on q074 evidence and completed p006 |

## Why this is the next bounded unit

- Q074 accepted T020 but exhausted p007's two-cell boundary when T021 failed to
  return from atomic publication. Repeating that attempt without new evidence
  is prohibited.
- The ten admitted post-commit requests and absence of DISARM constrain the
  problem to one production interval: temporary write/flush/close/overlay
  replace and backing synchronization.
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
- After deterministic correction gates pass, run exactly one fresh T021-only
  acceptance cell with q074's restricted NE2000 topology and real one-minute
  timeout. Do not rerun accepted T020.
- Rerun NCOM-T001--T012, directly affected VFS/storage fixtures, existing
  WS011/ZNV2/Wi-Fi regressions, maintained amd64/i386 target builds,
  documentation validators, and `git diff --check`. Do not run aggregate
  `make check`.
- Do not begin p008 physical work, add a remote service, change public
  grammar/protocol, begin VLAN/bridge, or broaden into a VFS/storage redesign.

## Important uncertainty

Current evidence proves the stop interval but not whether file `fsync`, close,
overlay rename, or backing synchronization is responsible. P009 permits one
instrumented target cell for that distinction. If the result requires broader
filesystem architecture, q075 finishes with p009 uncleared and a new Phase;
it does not force a speculative fix or consume repeated QEMU retries.

## Approval request

Approve q075 to execute only `ws011-p009` within the boundary above. Planning
changes and this proposal may be committed, but production correction and any
additional QEMU execution wait for explicit approval.
