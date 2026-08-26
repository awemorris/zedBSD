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
- The active Queue Book is `plan/queue.md`; completed Queue records use
  `plan/queue-qNNN.md`. Queue IDs are permanent and use `qNNN`.
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
4. [queue.md](queue.md) owns the finite execution manifest currently proposed
   or authorized by the user. It points to P books and does not replace them.
5. A WS `tests/README.md` owns the index of shared test cases and executable
   test paths for that WS.
6. WS001 `ws.md` additionally owns the POSIX compliance ledger.

The same fact may be summarized upward, but detail is edited at its owning
level. The master links downward instead of duplicating phase design.

M/W/P are the planning hierarchy. Q is the execution boundary. The existence
or approval of an M, W, or P book does not authorize implementation.

## 3. Queue Book and authorization

The four planning/execution books answer different questions:

| Book | Local file | Responsibility |
| --- | --- | --- |
| M | `plan/master.md` | Where the project is going |
| W | `plan/wsXXX-name/ws.md` | What outcome the workstream must achieve |
| P | `plan/wsXXX-name/phaseYYY-name/phase.md` | How a bounded phase is implemented and verified |
| Q | `plan/queue.md` | What the agent may execute in the current cycle |

Queue construction considers dependencies, priority, timebox, risk, unresolved
human decisions, and whether results can be verified. A Queue must be finite.
It references P books rather than copying their detailed procedures.

Before code changes begin, the proposed Queue and timebox are presented to the
user. Implementation starts only after explicit Queue approval. A planned
Phase that is absent from the approved Queue is not executable.

Every Queue item uses exactly one lowercase state:

| State | Meaning |
| --- | --- |
| `pending` | Selected for this Queue but not started; execution requires Queue approval |
| `in-progress` | Currently being implemented or verified |
| `completed` | Its P-book completion conditions have evidence |
| `uncleared` | This cycle could not safely or reasonably complete it |

An `uncleared` item records its reason, facts learned, remaining work, and
resume condition. It is returned to P/W/M instead of being forced to pass. A
Queue may be `finished` with uncleared items: Queue completion means all
authorized items were processed as far as reasonable, not that all succeeded.

Historical records `q001` through `q007` were mechanically migrated from the
pre-Q terminology without renumbering their execution cycles.

## 4. W/P status vocabulary

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

## 5. Required `ws.md` fields

Each workstream plan records:

- WSID, title, status, last verified Phase, and resume point;
- objective, scope, non-goals, and cross-WS dependencies;
- Phase registry with combined ID, status, result, and Phase link;
- accumulated design decisions and reconsideration boundaries;
- shared-test index link;
- WS completion definition and remaining handoffs.

## 6. Required `phase.md` fields

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

## 7. Pause protocol

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

## 8. Evidence rules

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

## 9. Physical-run coordination

Each implementation-stage request for user-operated physical-hardware work
covers exactly one boot, probe, or other bounded operation. A final acceptance
campaign may contain an explicit consecutive-run count, but it is identified
as final repeatability work rather than an implementation dependency.

Before the user starts, the request states all of the following in one place:

- the purpose of this one run and the boundary it is intended to prove;
- the WS/Phase and test-case ID, plus this run's ordinal within any planned
  repetition count;
- a clickable repository link to the exact image or executable artifact;
- the artifact SHA-256, and whether it must be written as a raw image;
- one bounded action to perform;
- the exact success markers or measurements to capture;
- the evidence requested, such as one full-screen photograph; and
- known later errors which are outside the run's acceptance boundary and do
  not invalidate it.

Terms such as "the corrected image", "the current image", or "boot it again"
are not sufficient without the artifact link and purpose. A physical result is
recorded before issuing the next single-run request.

### Scheduling repeatability work

For behavior which is expected to be deterministic, one successful physical
observation is provisional confirmation: record it, then continue safe
implementation and automated verification. Do not require N physical successes
before proceeding to the next implementable Phase.

Repeated physical boots or probes are deferred to the final acceptance stage
after all safe agent-executable implementation and automated verification have
been exhausted. The default final campaign is five consecutive successful runs
of one frozen artifact. A failure breaks the consecutive sequence, is recorded,
and is analyzed before a new final sequence begins. A Phase may choose another
count only with an explicit risk or cost rationale.

Earlier repetition is justified only when the defect is intermittent or
probabilistic, the sample count is itself needed to evaluate the correction,
or safety/data-integrity risk makes continued implementation unsound without
repeat evidence. The Phase states that exception explicitly. Otherwise, one
successful run unblocks continued work and repeatability counts belong to a
final acceptance campaign rather than the feature's implementation critical
path.
