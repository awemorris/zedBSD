# Planning governance

Last updated: 2026-08-25

Parent: [master plan](master.md)

## 1. Identity and paths

- A workstream has a permanent three-digit WSID and directory
  `plan/wsXXX-name/`.
- Its authoritative workstream plan is `ws.md`.
- A Phase has a three-digit local ID and directory
  `phaseYYY-name/phase.md`.
- The globally unambiguous Phase ID is `wsXXX-pYYY`.
- Phase IDs are never reused. Historical numbering is preserved; WS001 uses
  `p085` to represent the legacy “Phase 8.5”.
- Shared test specifications and test indexes live in the WS `tests/`
  directory.

Executable tests may remain in the repository-level `tests/` tree when the
build system or multiple implementation areas use them. The WS test index must
name those files, their owner Phase, environment, and acceptance role. New
planning-only fixtures or test specifications belong under the WS `tests/`.

## 2. Sources of truth

1. [master.md](master.md) owns WS identity, program priority, dependencies,
   current WS state, and resume point.
2. `ws.md` owns the WS objective, Phase registry, internal dependencies,
   accumulated decisions, and WS-level handoffs.
3. `phase.md` owns one bounded implementation/design unit, its state,
   work-package checklist, evidence, interruption record, and next action.
4. A WS `tests/README.md` owns the index of shared test cases and executable
   test paths for that WS.
5. WS001 `ws.md` additionally owns the POSIX compliance ledger.

The same fact may be summarized upward, but detail is edited at its owning
level. The master links downward instead of duplicating phase design.

## 3. Status vocabulary

| Status | Meaning |
| --- | --- |
| Proposed | Scope exists, but decisions or dependencies remain open |
| Planned | Scope and broad acceptance are known |
| Ready | Inputs are available and implementation can start |
| In progress | Work is actively changing the implementation |
| Verification | Implementation is ready for the declared test matrix |
| Paused | Work intentionally stopped at a documented resumable point |
| Blocked | No safe progress is possible until a named condition changes |
| Partial | Useful result exists, but one or more acceptance gates remain open |
| Complete | All declared acceptance gates have evidence |
| Deferred | Intentionally postponed before implementation |
| Superseded | Replaced by a linked Phase or decision |

A WS may be paused while its last Phase is complete. A Phase may also be paused
mid-implementation, but only after recording its exact safe state.

## 4. Required `ws.md` fields

Each workstream plan records:

- WSID, title, status, last verified Phase, and resume point;
- objective, scope, non-goals, and cross-WS dependencies;
- Phase registry with combined ID, status, result, and Phase link;
- accumulated design decisions and reconsideration boundaries;
- shared-test index link;
- WS completion definition and remaining handoffs.

## 5. Required `phase.md` fields

Each Phase records:

- WSID, Phase ID, combined ID, status, dates, and parent WS;
- objective, baseline, scope, non-goals, and dependencies;
- fixed decisions and files/subsystems expected to change;
- ordered work packages with completion state;
- acceptance cases linked to the WS test index;
- actual results and evidence;
- interruption/resumption record;
- remaining debt and handoff items.

Historical aggregate plans may be retained under `history/`, but every Phase
must have a `phase.md` that states its own scope and result. The historical
document is supporting evidence, not the active state record.

## 6. Pause protocol

Before pausing an active Phase:

1. stop at a buildable or explicitly described non-buildable checkpoint;
2. update each work package as complete, partial, or not started;
3. list the last passing commands and every failing or unrun acceptance case;
4. record uncommitted-file assumptions and generated artifacts that matter;
5. state one concrete next action and any decision needed before it;
6. update the parent `ws.md` and [master.md](master.md).

Resumption begins by checking those recorded assumptions and rerunning the last
passing focused gate. It does not infer completion from elapsed time or from a
newer unrelated Phase.

## 7. Evidence rules

- Host/unit tests prove separable logic only.
- QEMU evidence records the full device model/command and does not prove
  physical hardware behavior.
- Hardware evidence records the laptop/firmware/device IDs, discovery and error
  logs, repeated cold boots where relevant, and safe test media.
- “Complete in QEMU” and “complete on hardware” are separate acceptance claims.
- A probe-only driver or success-returning stub is partial.
- Sensitive WLAN credentials and device secrets are redacted from stored logs.

Every implementation Phase uses the supported build command, focused tests,
bounded `qemu-system-x86_64` tests where relevant, formatting for changed
userland C/header files, and `git diff --check`. The aggregate `make check`
target is not required.

