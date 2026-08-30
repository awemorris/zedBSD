# Queue: Panasonic CF-SV7 early ACPI/interrupt bring-up

Last updated: 2026-08-30

QID: `q033`

Queue status: in-progress

Queue finished: **No**

Authorization: the user approved execution of the proposed `ws003-p020`
single-Phase Queue on 2026-08-30.

Timebox: no fixed wall-clock limit. Complete the automated implementation and
QEMU gates in this session, then stop at the one declared CF-SV7 physical
acceptance checkpoint. A newly proven architecture decision makes the Phase
`uncleared`; do not guess through it.

Parent: [master plan](master.md)

Previous Queue: [q032](queue-q032.md)

## Purpose

Replace the Panasonic CF-SV7's silent stop after
`A64 ACPI RSDP PASS` with bounded early-HAL behavior and, when the existing
xAPIC architecture is sufficient, reach `A64 IRQ READY`,
`A64 XMM CONTEXT PASS`, and kernel HAL readiness.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws003-p020` | [Phase](ws003-bringup/phase020-cf-sv7-acpi-irq-bringup/phase.md) | in-progress | Early exceptions and ACPI/APIC/timer boundaries are observable; invalid APIC mode changes and unbounded PIT waits are removed; automated regressions pass; one final CF-SV7 image is handed off |

## Frozen execution boundary

- The current photograph is the physical baseline. Do not request another
  physical boot until the complete software batch and automated regressions
  are ready.
- Install the minimum safe early GDT/TSS/IDT before firmware-dependent
  ACPI/MSR/MMIO operations.
- Accept only an already-active, validated xAPIC state. A disabled APIC,
  x2APIC state, legacy-xAPIC lockout, or an APIC ID outside the existing
  8-bit destination contract is a visible `BOUNDARY-CAPTURED` result, not
  permission to add a partial x2APIC backend.
- Remove the current mode-changing `IA32_APIC_BASE` write from the accepted
  path. Validate CPUID, mode, base, and readback before xAPIC MMIO on every
  CPU.
- Bound PIT channel-2 calibration. On timeout, stop/mask the LAPIC timer and
  restore the original port `0x61` state before reporting failure.
- Keep diagnostics concise and permanent enough to identify ACPI completion,
  APIC mode/base, LAPIC, IDT, IOAPIC, timer begin/end, and console IRQ.
- Do not weaken ACPI physical-range validation or fold xHCI/storage/root work
  into this Queue.

## Execution rules

- Do not inspect or modify `.internal/` or `userland/noct/NoctLang`.
- Preserve unrelated work. Use `make -j16`, focused fixtures, and
  `qemu-system-x86_64`; do not use the aggregate `make check` target.
- Run the established 4/8/16-GiB OVMF q35/xHCI USB matrix and the supported
  amd64 BIOS regression before physical handoff.
- A single CF-SV7 observation is `PASS` only if all Phase objective markers
  appear. A new bounded diagnostic is `BOUNDARY-CAPTURED`, leaves the Phase
  `uncleared`, and supplies the resume condition for a later Phase.
- Synchronize actual results into P/W/M/Q. Commit `WIP` and push after this
  Queue reaches its software/physical handoff state.

## Automated checkpoint

The q033 software batch is complete and the Queue remains `in-progress` only
for its one physical observation.  The frozen artifact is
`/home/awe/zedBSD/build/amd64/hdd-image.img`, 203,423,744 bytes, SHA-256
`38e1d8e4ccfb6ce7d1c37082818f76546a6e07dbf8e86e551e654a5f2b3ca9e8`.

- The host policy fixture and its ASan/UBSan build pass.
- Disposable-kernel QEMU injection proves bounded early `#UD`, `#GP`, and
  instruction-fetch `#PF` handling after IDT installation.
- The PIT-disabled negative cell stops at the required OUT-low timeout.
- SeaBIOS q35/xHCI USB with four CPUs reaches `login:`.
- The final OVMF q35/xHCI USB matrix reaches `login:` with four CPUs at 4, 8,
  and 16 GiB (3/3 pass).
- `make -j16`, `make check-disk-image`, shell syntax, and
  `git diff --check` pass; aggregate `make check` was not used.

Evidence is preserved under `plan/ws003-bringup/temp/q033-final/`.  The next
and only action in q033 is the physical `BR-T52` observation described in the
Phase book.  No intermediate or repeatability boot is requested.

## Completion definition

q033 is finished when `ws003-p020` is either:

- `completed`, with host/QEMU evidence and one CF-SV7 boot reaching IRQ, XMM,
  and HAL readiness; or
- `uncleared`, with all safe automated work complete, a uniquely identified
  physical image, and one new bounded hardware result or an explicit
  architecture decision recorded.

The Queue may pause at `awaiting-physical` after its automated gates. That is
not a second implementation item and does not authorize repeated human boots.
