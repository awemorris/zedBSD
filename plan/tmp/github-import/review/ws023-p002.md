# WS023-P002: Conform the i386 core and interrupt sources

Last updated: 2026-09-03

Phase ID: `ws023-p002`

Status: complete (`q067`)

Parent: [WS023](../ws.md)

Depends on: `ws023-p001`

## Objective

Apply the canonical style to the remaining i386 low-level core, interrupt, and
library implementation without altering machine-state transitions.

## Scope

- `asm.h`, `atomic.c`, `bsp.h`, `cmain.c`, `i386.h`
- `int.c/h`, `irq.c/h`, and `lib.c`

## Procedure

1. Apply the file envelope, section order, prototypes, definition layout, and
   function comments.
2. Hoist local declarations while leaving runtime initialization at its
   original execution point.
3. Split compound decisions and direct call returns only when evaluation order
   and result conventions remain exact.
4. Replace packed unused-argument casts with the private helper and remove the
   ordinary `//` comment.
5. Preserve atomic instruction sequences, interrupt masking/restoration,
   handler publication, vector mapping, and fatal paths exactly.

## Verification

- Run `git diff --check`, `hal-pcat-compile`, and
  `hal-pc98-compile` with `make -j16`.
- Run focused interrupt-source/MSI fixtures applicable to the shared x86
  boundary and inspect generated object symbols for linkage changes.

## Completion conditions

- All scoped C/header files meet the WS023 contract.
- No interrupt, atomic, symbol, or calling-convention behavior changes.
- Both configured i386 compile gates pass.

## Execution result

The core, interrupt, and library files now meet the canonical form.  Final
review restored the original user-frame-before-CR2 observation order in the
fault path.  Both configured i386 compile gates and the symbol comparison
passed; see the [q067 result ledger](../tests/q067-results.md).
