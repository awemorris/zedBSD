# WS023-P003: Conform the i386 VM and task sources

Last updated: 2026-09-03

Phase ID: `ws023-p003`

Status: complete (`q067`)

Parent: [WS023](../ws.md)

Depends on: `ws023-p002`

## Objective

Style the i386 address-space and task/context implementation as a separate
review unit because declaration movement and cleanup paths can otherwise hide
MMU or ownership changes.

## Scope

- `space.c/h`
- `task.c/h`

## Procedure

1. Reorder file sections and add every required forward declaration/comment.
2. Hoist nested declarations and retain assignments at the original semantic
   points.
3. Expand Boolean decisions and returns into debugger-visible steps without
   changing page-table traversal, TLB invalidation, signal-frame layout,
   context registers, or release order.
4. Remove compressed statements and document each mapping/cleanup phase.

## Verification

- Run `git diff --check`, both configured i386 HAL compile gates, and a full
  PC/AT image build with `make -j16`.
- Inspect ABI-bearing structures and task-frame offsets before and after.
- Run one bounded i386 PC/AT QEMU login smoke.

## Completion conditions

- Both source/header pairs meet the WS023 contract.
- Object-visible ABI/layout and MMU/task behavior are unchanged.
- The PC/AT build and runtime smoke pass.

## Execution result

The address-space and task/context sources were expanded and reviewed without
an ABI or ownership change.  Original counter-assignment, segment-register,
and signal-validation order was retained.  The compile, configured-build, and
PC/AT runtime gates passed; see the
[q067 result ledger](../tests/q067-results.md).
