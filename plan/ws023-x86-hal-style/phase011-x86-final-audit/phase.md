# WS023-P011: Complete the x86 HAL style and regression audit

Last updated: 2026-09-03

Phase ID: `ws023-p011`

Status: complete (`q067`)

Parent: [WS023](../ws.md)

Depends on: `ws023-p001` through `ws023-p010`

## Objective

Audit all 88 files against the canonical style and prove that the complete
source-only rewrite preserves the supported x86 HAL behavior.

## Procedure

1. Re-enumerate every maintained C/header file and account for it against the
   Phase registry.
2. Check exact modelines/headers, file section order, forward declarations,
   function layouts/comments, declaration placement, semantic paragraphs,
   loops/switches, braces, returns, and supported extension exceptions.
   Run the Phase-owned `tests/x86-hal-style-audit.noct` for the mechanically
   decidable subset and review the remaining semantic rules manually.
3. Verify no compact multi-statement line, prohibited `goto`, declaration in
   a `for` initializer, or meaningful direct-call return remains.
4. Review the combined diff specifically for API/ABI/layout, volatile access,
   lock/IRQ state, timing, ownership, and observable-result changes.
5. Run the final focused, configured-build, and runtime matrix. Do not run the
   aggregate `make check` target.

## Verification

- `git diff --check`.
- Focused ACPI-window, early-init, handoff, MCFG/MSI, timecounter, console,
  input ownership, and PC-98 PIC/cursor gates used by the preceding Phases.
- Fresh `make -j16` builds with separate build roots for:
  `config/ci/config-pcat.mk`, `config/ci/config-pc98.mk`,
  `config/ci/config-amd64.mk`, and `config/ci/config-intelmac.mk`.
- Bounded runtime cells: i386 PC/AT login, maintained i386 PC-98 login,
  amd64 BIOS SMP login, and amd64 UEFI SMP login.

## Completion conditions

- The audit accounts for all 88 files with no unrecorded exception.
- All focused tests, four configured builds, and four QEMU cells pass.
- M/W/P/Q agree and WS023 may be marked complete.

## Execution result

The mechanical audit passed for all 88 files, manual semantic/API/ABI review
completed, and the final focused, four-build, and four-runtime matrix passed.
The only combined-wrapper failure was its unrelated obsolete 128-MiB amd64
fixture capacity; direct production-image BIOS and UEFI cells both passed.
Exact evidence and retained baseline risks are in the
[q067 result ledger](../tests/q067-results.md).
