# WS001 Phase 015: base C coding-style adoption

Last updated: 2026-08-31

WSID: `ws001`

Phase ID: `p015`

Combined ID: `ws001-p015`

Status: Complete (`agent2-q001`, 2026-08-31)

Parent: [WS001](../ws.md)

Style contract: [C coding style](../../coding-style.md)

Tests: [WS001 test index](../tests/README.md)

## Objective

Make `plan/coding-style.md` the operative rule for new and refactored C/header
files under `userland/base`, with repeatable checks for objective rules and an
explicit inventory for rules that require human review.  Establish this
boundary before further POSIX command implementation.

## Baseline and scope

The current tree contains 238 C/header files and 58,780 lines under
`userland/base`.  None currently carries the required Emacs modeline, and the
style includes semantic rules that a formatter cannot safely infer: function
ownership/order, purpose comments, declaration lifetime, fallible-call
sequencing, loop intent, and debugger-friendly control flow.

This Phase does not bulk-rewrite every historical file.  The style document
itself says historical exceptions are not precedents and defines the canonical
form for new and refactored code.  P015 applies that policy by establishing a
change gate, fully normalizing a small representative shared boundary, and
recording the untouched debt for later ownership-sized work.

Expected changed implementation scope:

- `userland/base/common/command.c` and `command.h` as the representative shared
  command boundary;
- focused style-audit tooling and fixtures owned by WS001;
- documentation/build hooks needed to invoke the focused gate.

## Fixed decisions

- Use `/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */` for a
  one-line C-file modeline; header comment blocks may carry the same payload on
  their second line.
- A file newly added or substantially refactored after p015 must comply as a
  complete file.  Untouched historical files are inventory debt, not passing
  evidence.
- Automated checks cover only mechanically decidable properties.  They must
  not pretend to prove comment usefulness, ownership, lifetime, or evaluation
  order.
- Style-only changes preserve behavior and are reviewed separately from later
  command semantics.
- Do not add test-only environment switches to production code.

## Work packages

1. Inventory every base C/header file and classify objective versus manual
   style checks without modifying production behavior.
2. Add a focused runner with positive and negative fixtures for modelines,
   tabs, obvious one-line case bodies, known loop/switch purpose-comment
   shapes, and other reliably parseable rules.
3. Normalize `common/command.c` and `command.h` completely, including public
   and static comments, declaration placement, split calls, loops, returns,
   and the modeline.
4. Compile the production shared command object and run its existing consumers'
   focused host smoke coverage.
5. Record the historical file inventory and the precise enforcement rule in
   the WS001 test index and ledger.

## Completion conditions

- the style contract is discoverable from WS001 and has a maintained focused
  invocation;
- objective checker fixtures prove both acceptance and rejection without
  false claims about semantic style rules;
- the representative shared command boundary fully complies and behaves
  identically;
- every later Phase can name exactly which changed files must pass; and
- focused builds, `make -j16`, and `git diff --check` pass.

## Execution result

The objective audit, its negative fixtures, and the production-linked shared
command test are maintained as:

```sh
plan/ws001-posix/tests/base-c-style-audit-test.sh
plan/ws001-posix/tests/run-base-command-host-test.sh
plan/ws001-posix/tests/base-c-style-inventory.sh
```

`userland/base/common/command.c` and `command.h` now use the required modeline,
function layout, comments, declaration groups, guarded calls, loop intent, and
semantic spacing. The host fixture passes parsing, mode, full-write/copy, and
dynamic line-read behavior. The inventory reports every base C/header file as
either `compliant` or `historical`; it does not promote untouched debt.

Recorded evidence:

```text
BASE-STYLE-T001 objective source audit: PASS
BASE-STYLE-T002 objective checker fixtures: PASS
BASE-STYLE-T003 shared command behavior: PASS
standalone -Wall -Wextra -Werror compile: PASS
git diff --check: PASS
```

The final repository-wide `make -j16` gate was shared with the remaining Queue
items and passed after their source set was complete. The disposable amd64
guest cell also passed without changing the production image.

## Reconsideration boundary

Stop and revise the plan if the intended requirement is an immediate rewrite
of all 58,780 lines.  That outcome needs several subsystem-owned Queues and
cannot be hidden inside this adoption Phase.
