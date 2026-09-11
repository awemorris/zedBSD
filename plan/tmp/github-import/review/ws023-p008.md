# WS023-P008: Conform amd64 boot, APIC, and time sources

Last updated: 2026-09-03

Phase ID: `ws023-p008`

Status: complete (`q067`)

Parent: [WS023](../ws.md)

Depends on: `ws023-p007`

## Objective

Style the firmware handoff, ACPI/APIC, console-independent clock, and
timecounter implementations as one hardware-order-sensitive review unit.

## Scope

- `bsp-pcat/acpi.c/h`, `boot.c`, and `clock.c`
- `bsp-pcat/ioapic.c/h`, `lapic.c/h`, and `pic.c`
- `bsp-pcat/timecounter.c/h`

## Procedure

1. Apply every structural and semantic style rule without changing firmware
   selection, table bounds, interrupt routing, timer calibration, or
   publication state.
2. Replace the existing cleanup `goto` paths with small helpers or explicit
   reverse-order cleanup whose behavior is reviewed branch by branch.
3. Preserve volatile register accesses and all timing sample/order barriers.
4. Record any construct that must remain as a supported compiler extension.

## Verification

- Run `git diff --check`, `make -j16 amd64-hal-compile`, ACPI/early-init,
  timecounter policy/configuration, and handoff focused fixtures.
- Run the retained positive and negative amd64 SMP counter QEMU cells and one
  full UEFI boot.

## Completion conditions

- All scoped files meet the WS023 contract with no `goto`.
- Firmware/APIC/timecounter focused and runtime evidence remains unchanged.

## Execution result

The firmware, APIC, boot, clock, and timecounter group now meets the canonical
form with no `goto`.  Strict compilation, firmware/timecounter fixtures, full
builds, and amd64 BIOS/UEFI runtime passed; see the
[q067 result ledger](../tests/q067-results.md).
