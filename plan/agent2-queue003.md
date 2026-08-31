# Agent 2 Queue 003: complete userland C-style conformance

Last updated: 2026-08-31

QID: `agent2-q003`

Queue status: uncleared

Queue finished: **Yes -- terminal uncleared result recorded**

Authorization: on 2026-08-31 the user explicitly approved execution after
reviewing the complete 258-file Queue proposal.

Timebox: one exhaustive inventory and one complete style-only refactoring
Phase, executed in subsystem-sized batches, followed by focused checks and one
configured `make -j16` build. No POSIX feature implementation is included.

Parent: [master plan](master.md)

Canonical Queue isolation: this file does not replace or edit `plan/queue.md`.
It is the independent Agent 2 Queue requested for `/home/awe/zedBSD-2`.

## Candidate registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws001-p020` | [Phase](ws001-posix/phase020-complete-userland-c-style/phase.md) | uncleared | Structural rules pass over all 258 files; function-body semantic and 731 mechanical residuals remain explicitly uncleared |

## Why a new Phase is required

- `ws001-p015` intentionally normalized only
  `userland/base/common/command.c` and `command.h`; it classified the other
  files as historical debt.
- Its objective audit currently checks only the modeline, trailing whitespace,
  and statements on `case` labels. It cannot support a whole-tree style claim
  and does not check the function-order or static-declaration rules identified
  by the user.
- The current scope is 214 `.c` files and 44 `.h` files comprising 72,481
  lines. The 11 `.S` files retain the completed Phase 19 header treatment, but
  C function/declaration rules do not apply to assembler.
- The work changes organization and presentation only. Observable behavior,
  call order, object lifetime, error handling, ABI, public declarations, and
  build configuration remain fixed.

## Execution boundary

- Begin with an exhaustive structural inventory; do not infer compliance from
  the old `compliant`/`historical` modeline classification.
- Refactor every C implementation and header, including base, X11, and package
  runtime source. No subtree is grandfathered as historical.
- Process cohesive batches so a failed compile can be attributed and repaired
  before continuing. A file is complete only after both mechanical and manual
  checklist evidence pass.
- No functional command changes, feature work, generated artifacts, imported
  source, or unrelated planning work are in scope.
- No commit, staging operation, stash, reset, or cleanup is authorized.
- Do not use aggregate `make check` or repository `.internal/` material.

## Verification rules

- Use the Phase-owned whole-tree structural audit, fixtures, and per-file
  checklist inventory.
- Run applicable existing focused host tests for refactored shared components
  and commands, then the configured `make -j16` gate.
- Run the existing disposable amd64 base-utility smoke if any transformation
  goes beyond declaration/comment/whitespace/function-block movement.
- Run the Phase 19 exact-header/body-boundary audit and `git diff --check`.
- Do not mark the Queue complete while any file is merely `historical`,
  unaudited, or exempted without a rule-based non-applicability reason.

## Completion definition

This Queue is finished after `ws001-p020` is `completed` or `uncleared`, its
actual evidence and residuals are synchronized to WS001, and the result states
truthfully whether all applicable rules—not only mechanically convenient
ones—were applied.

## Execution result

The user-identified structural defect is fixed across the whole inventory:
2,454 function definitions have public-before-static order, reader-facing
top-down static order, one-line prototypes for every normal static function,
split definition headers, and the required function comments. All 1,454
loops/switches now have adjacent intent comments, and 63 same-line `case`
statements were split. Safe declaration/assignment normalization was also
applied without moving VLA evaluation or changing call order.

The Queue is terminal `uncleared`, not complete. The body audit still reports
731 mechanically visible residuals: 654 initialized automatic declarations
outside the direct `if`/`else` exception and 77 declarations in `for`
initializers. All 86 malformed multi-line comment openings were normalized.
Debugger-friendly call decomposition, allocation/check grouping, semantic
paragraphs, final-return spacing, and environment-switch intent also require
per-function semantic review. The 258-row ledger records these facts instead
of presenting the zero-diagnostic structural audit as whole-style proof.

Passing evidence: structural audit and fixtures, exact 269-file header audit
and fixtures, `-Wdeclaration-after-statement -Werror` compilation of the
configured userland targets, configured `make -j16`, and `git diff --check`.
