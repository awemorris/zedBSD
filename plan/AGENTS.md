# zedBSD agent workflow

This repository uses the MWP-Q Agentic Coding Method described by
[MWP-Q AGENTS-ja.md](https://github.com/awemorris/MWP-Q-Agentic-Coding-Method/blob/main/AGENTS-ja.md).
The local contracts below adapt that method without renumbering existing zedBSD
workstreams or phases.

## Planning books and execution boundary

- M book: `plan/master.md` — project goals, milestones, workstreams, and status.
- W book: `plan/wsXXX-name/ws.md` — one workstream's outcome, scope,
  dependencies, phases, and completion conditions.
- P book: `plan/wsXXX-name/phaseYYY-name/phase.md` — one bounded phase's
  procedure and verification contract.
- Q book: `plan/queue.md` — the finite set of phases authorized for the current
  execution cycle.

M/W/P describe planned work. They are not implementation authorization. Code
work must cross the Q boundary: present the proposed Queue, obtain user
approval, then execute only that scope. Planning approval and execution
approval are separate.

Existing identifiers and slugged directories are immutable. This repository
continues to use combined phase IDs such as `ws004-p006`. Queue IDs use `qNNN`;
closed Queue Books are retained as `plan/queue-qNNN.md`.

## Queue construction

Before placing work in a Queue, inspect the M, relevant W/P books, actual code,
and prior Queue results. Select only a finite, timeboxed set whose dependencies,
major scope/product/risk decisions, and verification path are known.

Do not put an unresolved human decision and dependent implementation into one
Queue item. Keep judgment-dependent work in M/W/P until the decision has been
made and reflected in its Phase. A Queue entry points to its P book instead of
copying the detailed plan.

Before implementation:

1. confirm or request the intended timebox;
2. present the Queue proposal with phase ID, purpose, reason, dependencies, and
   important uncertainty;
3. wait for explicit execution approval;
4. set the Queue and selected item to `in-progress` only after approval.

## Queue states

Every Queue item uses exactly one state:

- `pending`: selected for this Queue but not started; execution still requires
  Queue-level user approval;
- `in-progress`: currently being implemented or verified;
- `completed`: all P-book completion conditions have evidence;
- `uncleared`: this cycle could not safely or reasonably complete the item.

An `uncleared` result records the reason, facts learned, remaining work, and a
concrete resume condition. It is a normal outcome. Do not force completion,
hide uncertainty, or repeat the same failed attempt without new information.

The Queue itself may be `finished` even when an item is `uncleared`. Finished
means that every authorized item has been processed as far as reasonable, not
that every item succeeded.

## Execution and feedback loop

For each approved Queue item:

1. read its P book;
2. execute within the approved scope;
3. verify against the P-book completion conditions;
4. mark it `completed` or `uncleared`;
5. continue to the next dependency-ready Queue item;
6. synchronize actual results back into P, W, and M.

Unexpected findings may be handled locally only when they fit the current
Phase safely. Otherwise, do not silently expand scope. Record the finding,
return residual work to the appropriate P/W/M book, and create a new Phase
when needed.

## Tests and temporary material

Reusable phase/WS fixtures belong under `plan/wsXXX-name/tests/`. Disposable
diagnostics belong under a WS `temp/` directory and must remain untracked.
Do not consume repository `.internal/` material; copy an explicitly selected
test into the owning WS plan area before use.

Project-specific execution constraints remain in force:

- do not commit unless the user changes that instruction;
- do not use the aggregate `make check` target;
- use `make -j16` for the supported build gate;
- use `qemu-system-x86_64` for amd64 runtime verification;
- use disposable image copies for destructive storage tests.
