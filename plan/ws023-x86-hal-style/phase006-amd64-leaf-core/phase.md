# WS023-P006: Conform amd64 leaf and small core modules

Last updated: 2026-09-03

Phase ID: `ws023-p006`

Status: complete (`q067`)

Parent: [WS023](../ws.md)

Depends on: `ws023-p005`

## Objective

Establish the amd64 file/private-helper convention and style the small,
lower-risk HAL modules before touching the interrupt, MMU, and firmware paths.

## Scope

- `defs.h`, `asm.c/h`, `acpi-window.c/h`, `bsp.h`, and `clock.h`
- `cmain.c`, `descriptor.c/h`, `msi-source.c`, and `percpu.c/h`
- Small BSP policy/validation units:
  `bsp-pcat/early-init-policy.c/h`,
  `bsp-pcat/handoff-validation.c/h`,
  `bsp-pcat/mcfg.c`, and
  `bsp-pcat/timecounter-policy.c/h`

## Procedure

1. Add canonical envelopes and the duplicated private amd64
   `UNUSED_PARAMETER` helper.
2. Apply file ordering, prototypes, definition layout, leading declarations,
   intent comments, and explicit returns.
3. Preserve descriptor layouts, assembly wrappers, CPUID results, MCFG policy,
   and validation ordering exactly.

## Verification

- Run `git diff --check` and `make -j16 amd64-hal-compile`.
- Run ACPI-window, handoff-validation, early-init-policy, MCFG, MSI-source, and
  timecounter-policy focused fixtures.

## Completion conditions

- Every scoped file meets the WS023 contract.
- The focused fixtures and amd64 HAL compile gate pass without ABI changes.

## Execution result

The scoped leaf/core and policy modules now meet the canonical form.  ACPI
window, handoff, early-init, MCFG, MSI, and timecounter fixtures passed,
including applicable sanitizer variants; symbol/API/ABI comparison found no
delta.  See the [q067 result ledger](../tests/q067-results.md).
