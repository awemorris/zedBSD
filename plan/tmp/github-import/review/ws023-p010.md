# WS023-P010: Conform the amd64 console and input implementation

Last updated: 2026-09-03

Phase ID: `ws023-p010`

Status: complete (`q067`)

Parent: [WS023](../ws.md)

Depends on: `ws023-p009`

## Objective

Apply the canonical style to the large amd64 PC/AT console source, including
all production and focused-test conditional branches.

## Scope

- `bsp-pcat/cons.c`

## Procedure

1. Reorganize constants, tables, variables, declarations, public definitions,
   and static definitions without changing conditional compilation.
2. Convert every declared-`for` initializer and nested declaration to the
   leading ANSI declaration group.
3. Replace two cleanup `goto` paths with explicit ownership helpers.
4. Style output locking/rendering first, keyboard/input second, and
   initialization/test hooks last.
5. Preserve key tables, designated table initialization where positional
   replacement would reduce safety, console locking, cursor state, and evdev
   publication.

## Verification

- Run `git diff --check`, `make -j16 amd64-hal-compile`, the amd64 console
  output host fixture, and the input-ownership fixture so all conditional
  variants compile.
- Run a full amd64 UEFI build and bounded login/input smoke.

## Completion conditions

- `cons.c` meets the WS023 contract in every compiled variant.
- Output, keyboard, locking, and input-publication focused tests and runtime
  smoke pass.

## Execution result

The complete console/input implementation now meets the canonical form in its
production and fixture variants.  Original short-circuit device-read order was
retained; ordinary, ASan, UBSan, input-ownership, full-build, and UEFI runtime
gates passed.  See the [q067 result ledger](../tests/q067-results.md).
