# WS023-P007: Conform amd64 interrupt, page, SMP, and task code

Last updated: 2026-09-03

Phase ID: `ws023-p007`

Status: complete (`q067`)

Parent: [WS023](../ws.md)

Depends on: `ws023-p006`

## Objective

Apply the canonical style to amd64 core execution paths while preserving
interrupt, page-table, SMP, and context-switch semantics.

## Scope

- `int.c/h`, `irq.c/h`, `lib.c`
- `page.c`, `pic.h`, `smp.c/h`
- `task.c/h`

## Procedure

1. Add required prototypes/comments and put public definitions before static
   definitions.
2. Hoist declarations, expand compact bodies, split meaningful calls and
   compound results, and use explicit result exits.
3. Preserve IRQ state, atomic/lock ordering, page-table mutation, TLB actions,
   AP admission, task-frame layout, and release ownership exactly.

## Verification

- Run `git diff --check`, `make -j16 amd64-hal-compile`, and a full amd64
  configured build.
- Run bounded amd64 SMP BIOS and UEFI QEMU login smokes because the Phase
  touches interrupt, page, task, and AP paths.

## Completion conditions

- All scoped files meet the WS023 contract.
- Both SMP firmware modes boot with unchanged CPU admission and login behavior.

## Execution result

The interrupt, page, SMP, library, and task group now meets the canonical
form.  Final review retained atomic-target-before-current-CPU observation in
the task switch.  Strict compilation, full linking, and four-CPU BIOS and UEFI
runtime gates passed; see the [q067 result ledger](../tests/q067-results.md).
