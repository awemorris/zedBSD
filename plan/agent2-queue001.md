# Agent 2 Queue 001: base style adoption and bounded POSIX utilities

Last updated: 2026-08-31

QID: `agent2-q001`

Queue status: completed (`2026-08-31`)

Queue finished: **Yes**

Authorization: on 2026-08-31 the user approved execution of this Agent 2
Queue after asking Agent 2 to apply [`coding-style.md`](coding-style.md) to
`userland/base`, then expand bounded POSIX.1-2024 command work.

Timebox: one finite four-Phase cycle.  The style Phase establishes an
incremental enforcement boundary rather than rewriting all 238 current C and
header files in one review.  The three utility Phases are individually
stoppable and do not share semantic implementation changes.

Parent: [master plan](master.md)

Canonical Queue isolation: this file does not replace or edit `plan/queue.md`.
It is the independent Agent 2 proposal requested by the user.

## Purpose

Make the new C style contract operative for base-system work without hiding a
58,780-line historical conversion inside a functional change.  Then replace
the deliberate `lp` stub with the requested direct-PDF LPD model and close two
small, high-confidence P1 utility gaps (`cmp` and `tee`).

## Candidate registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws001-p015` | [Phase](ws001-posix/phase015-base-c-style-adoption/phase.md) | completed | `plan/coding-style.md` becomes an executable `userland/base` change gate, representative shared code is normalized without behavior changes, and historical debt is inventoried rather than bulk-rewritten |
| 2 | `ws001-p016` | [Phase](ws001-posix/phase016-direct-lpd-printing/phase.md) | completed milestone | Native `lp` and `lpr` submit PDF jobs directly to an explicitly selected LPD destination without a persistent local spool queue; guest-to-LPD networking remains a runtime handoff |
| 3 | `ws001-p017` | [Phase](ws001-posix/phase017-cmp-conformance/phase.md) | completed milestone | `cmp` gains `-l`, `-s`, skip operands, correct unequal-read handling, robust I/O, and exact 0/1/>1 status behavior |
| 4 | `ws001-p018` | [Phase](ws001-posix/phase018-tee-conformance/phase.md) | completed milestone | `tee` gains `-i`, removes its 32-output ceiling, and handles partial I/O, failed outputs, signals, and close errors truthfully |

## Selection rationale

- The style document explicitly treats historical exceptions as debt rather
  than precedent.  P015 therefore enforces the complete style on new and
  refactored base files while recording untouched files; a whole-tree rewrite
  requires later ownership-sized Queues.
- The master priority list explicitly asks for the no-spool direct-LPD
  `lp`/`lpr` model.  P016 is placed before opportunistic utility cleanup.
- `cmp` is 49 lines and has a precise P1 defect: independently sized reads can
  produce a false difference.  Its complete option/status surface is bounded.
- `tee` is 63 lines and similarly isolated.  Its fixed descriptor array and
  incomplete output-failure behavior can be tested without a kernel or locale
  design change.
- Large language, traversal, locale, account, and shell items (`awk`, `cp`,
  `grep`, `sh`, `wc`, `xargs`) remain outside this Queue because they require
  broader semantic dependencies or several independently reviewable Phases.

## Dependency and deferral rules

- P015 must complete before functional source changes begin.  Every file added
  or substantially refactored by p016--p018 must pass the p015 style gate.
- P016 may use transient, unlinked staging for stdin or unknown-length input
  solely to obtain the LPD byte count.  It must not create a durable queue,
  retry daemon, scheduler, or background spool service.
- P016's proposed destination spelling is
  `host[:port]/queue`, selected by `lp -d`, `lpr -P`, `LPDEST`, or `PRINTER`.
  Approval of this Queue approves that spelling; if the user wants
  `/etc/printcap`, IPP, discovery, or another namespace, p016 must be revised
  before execution.
- P016 submits only PDF payloads and uses the LPD raw-data path.  Printer-side
  PDF interpretation is an acceptance prerequisite, not a local conversion
  feature.
- P017 and p018 are dependency-independent after p015.  If p016 becomes
  uncleared on physical-printer availability, continue with them.
- Do not import external command source or copied test suites.  Standards and
  protocol documents may inform independently written code and cases.
- An unexpected libc/kernel gap is returned to the WS001 ledger and does not
  silently expand the affected utility Phase.

## Verification rules

- Use Phase-owned host tests linked to production sources, direct standalone
  package builds, `make -j16`, and `git diff --check`.
- Use a deterministic fake LPD server for p016 protocol/failure coverage and a
  disposable amd64 image for the native command smoke test.
- Do not use aggregate `make check` or repository `.internal/` material.
- Do not combine style-only and semantic changes in the same commit/diff
  checkpoint, even when they affect one file.
- Do not commit unless the user separately authorizes commits.

## Completion definition

This proposed Queue is finished when every candidate is `completed` or
`uncleared`, its Phase and WS001 ledger evidence are synchronized, and no
historical whole-tree style debt is misreported as resolved.  Queue completion
does not claim general POSIX conformance or style normalization of untouched
`userland/base` files.

## Execution result

All four bounded implementation milestones completed. The final style
inventory contains 7 compliant and 234 historical C/header files; it does not
represent the historical files as newly normalized. Production-linked host
tests report `BASE-STYLE-T001` through
`T003`, `LPD-T001`, `CMP-T001`, and `TEE-T001` as passing. The four native
amd64 ELFs pass the repository checker, the full configured `make -j16` image
build passes, and the disposable guest reports `AGENT2-Q001` PASS.

P016 deliberately remains a PDF-only product profile rather than a POSIX
`lp` conformance claim. Issue 8 requires text input and `-w` print-completion
notification, neither of which can be represented truthfully by the approved
bare-LPD/PDF contract. The fake receiver proves the host protocol and all five
acknowledgement refusals; a guest-to-receiver network transaction is retained
as a runtime integration handoff because the selected guest config has no
QEMU-reachable Ethernet driver enabled.
