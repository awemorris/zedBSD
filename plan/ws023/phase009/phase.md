# WS023-P009: Conform the amd64 address-space implementation

Last updated: 2026-09-03

Phase ID: `ws023-p009`

Status: complete (`q067`)

Parent: [WS023](../ws.md)

Depends on: `ws023-p008`

## Objective

Apply the canonical style to the large amd64 VM implementation in an isolated
review so page-table ownership and shootdown behavior stay visible.

## Scope

- `space.c`
- `space.h`

## Procedure

1. Reorganize sections, declarations, prototypes, and public/static function
   definitions.
2. Hoist all nested declarations while preserving initialization order.
3. Split compound decisions and direct call returns into debuggable steps.
4. Add intent comments to traversal, mapping, rollback, detach, and shootdown
   paragraphs without changing their order or bounds.

## Verification

- Run `git diff --check`, `make -j16 amd64-hal-compile`, a full amd64
  build, and bounded BIOS/UEFI SMP QEMU login smokes.
- Compare ABI-bearing structures, emitted symbols, and mapping constants.

## Completion conditions

- The source/header pair meets the WS023 contract.
- VM ownership, mappings, and shootdowns remain behaviorally identical.

## Execution result

The complete VM source/header pair now meets the canonical form.  Independent
review covered rollback, lifetime, atomic, page-table, and shootdown order;
strict compilation, symbol comparison, full builds, and both amd64 firmware
runtime cells passed.  See the [q067 result ledger](../tests/q067-results.md).
