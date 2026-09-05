# Queue: completed-producer documentation follow-up

Last updated: 2026-09-05

QID: `q072`

Queue status: complete

Queue finished: **Yes**

Authorization: the user explicitly requested that `ws009-p006` be placed in
the Queue and executed. This Queue is limited to the current execution cycle
and contains no later implementation Phase.

Parent: [master plan](master.md)

Previous Queue: [q071](queue-q071.md)

## Purpose

Bring public documentation forward to the already accepted WLAN, physical USB
HID, Intel Mac image-Variant, Noct 2.0.1, and project LLVM state before the next
implementation wave. Planned removals, new UAPIs, and manually blocked work
remain visibly distinct from installed behavior.

## Execution registry

| Priority | WS / Phase | Status | Purpose / dependency |
| --- | --- | --- | --- |
| 1 | [`ws009-p006`](ws009-documentation/phase006-completed-producer-follow-up/phase.md) | completed (`q072`) | Published current completed-producer behavior; completed WS004/WS005/WS006/WS008/WS020/WS021 results |

## Execution policy

- Change product documentation and its WS009 planning/evidence records only;
  do not implement WS006 p009, WS011, WS017, WS019, or WS022 behavior.
- Derive command examples and claims from current production source and the
  completed producer evidence rather than from planned contracts alone.
- Do not expose real credentials, retained `.internal/` paths, or private test
  data. Examples use obvious placeholders.
- Keep current, experimental, deprecated, planned, and manually blocked
  behavior distinct in status banners and prose.
- Run the native Noct relative-link validator, focused documentation command
  and example checks, and `git diff --check`. Do not run aggregate
  `make check`.

## Completion boundary

Q072 completes when `ws009-p006` satisfies its P-book completion conditions
and its actual evidence is synchronized into the P, W, M, test index, and this
Queue. Any newly discovered producer mismatch outside documentation scope is
recorded and returned to its owning WS rather than silently implemented here.

## Completion result

- Added the managed-WLAN command, profile, authority, state, recovery, and
  failure reference from the accepted implementation boundary.
- Reconciled evdev/USB HID, Intel Mac image variants, Noct 2.0.1, and project
  LLVM 23.1.0 documentation while keeping deprecated and planned behavior
  explicit.
- Added a focused completed-producer documentation checker and made the native
  relative-link validator ignore only disposable `temp/` and prohibited
  `.internal/` trees.
- `DOC-T70--T72 completed-producer documentation: PASS (11 status banners)`.
- `Markdown relative-link check: PASS (1803 links)`.
- `git diff --check` passed. No producer source, runtime configuration, or
  image behavior changed in this Queue.
