# WS023-P001: Restore compressed i386 HAL core source

Last updated: 2026-09-03

Phase ID: `ws023-p001`

Status: complete (`q067`)

Parent: [WS023](../ws.md)

## Objective

Restore the visibly compressed i386 controller, SMP, per-CPU, page, and timer
code to canonical human-readable form first. Refactor, rather than exempt or
discard, the five inherited edits committed as `b4be6eb`.

## Scope

- `defs.h`, `acpi.c`, `apic-topology.h`, `mps.c`
- `ioapic.c/h`, `lapic.c/h`
- `percpu.c/h`, `smp.c/h`, `page.c`
- `multiboot.h`, `pic.h`, `clock.h`
- `bsp-pcat/pit.c` and `bsp-pc98/pit.c`

## Procedure

1. Record the `b4be6eb` diff for the five inherited files and distinguish its
   partial expansion from the additional canonical rewrite.
2. Add the canonical file envelopes and the private i386
   `UNUSED_PARAMETER` helper.
3. Expand every packed signature, body, declaration, decision, and return.
4. Reorder declarations/definitions and add intent comments while preserving
   CPUID order, topology publication, MMIO/port-I/O order, IRQ state, barriers,
   locks, page-table writes, and RTC/PIT sampling.
5. Review the staged-origin lines separately so the final diff accounts for
   both inherited and Phase-authored changes.

## Verification

- Run `git diff --check` and manually compare all volatile/I/O/control paths.
- Build `hal-pcat-compile` and `hal-pc98-compile` with the WS021 project
  configurations and `make -j16`.
- Run the existing timecounter configuration-stamp and early-init focused
  fixtures that compile the touched clock/controller boundaries.

## Completion conditions

- No compact one-line implementation remains in the scoped files.
- The scoped files satisfy the WS023 style/boundary contract.
- Both i386 HAL compile gates and focused controller/time tests pass.
- No inherited committed edit is lost, reset, or attributed ambiguously.

## Execution result

The complete scoped group, including all five inherited `b4be6eb` files, now
uses the canonical source form.  PC/AT and PC-98 HAL compilation, controller
and timecounter fixtures, and the final builds passed.  Detailed evidence is
recorded in the [q067 result ledger](../tests/q067-results.md).
