# WS003 Phase 020: Panasonic CF-SV7 early ACPI/interrupt bring-up

Last updated: 2026-08-30

Phase ID: `ws003-p020`

Status: Completed (`q033`, 2026-08-30)

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md), especially `BR-T52`

## Objective

Advance the Panasonic CF-SV7 from its current amd64 UEFI baseline,
`A64 ACPI RSDP PASS`, through the early interrupt boundary:

```text
A64 IRQ READY
A64 XMM CONTEXT PASS
boot: HAL initialized successfully.
```

This Phase owns only firmware handoff through early ACPI, local/IO APIC,
descriptor/IDT, timer, and console-IRQ initialization.  xHCI, USB storage,
root mounting, and later CF-SV7 device support are separate follow-up Phases
selected from the first subsequent stop.

## Baseline evidence

The 2026-08-30 physical photograph shows:

```text
zedBSD amd64 HAL
A64 ENTRY PASS
A64 PAGING PASS
A64 ACPI RSDP PASS rev=2 rsdt=7A396028 xsdt=00000000:7A3960C0
```

No later diagnostic is visible.  This proves UEFI loader handoff, kernel
entry, and the new page-table installation, but it does **not** prove that
ACPI table discovery completed.  The RSDP marker is printed immediately after
RSDP validation and before XSDT/MADT lookup.

The unmarked interval currently contains:

1. XSDT/RSDT lookup, MADT/MCFG discovery, MADT topology parsing, and ECAM
   mapping;
2. Local APIC mapping and mode initialization;
3. bootstrap-CPU topology publication;
4. GDT/TSS and IDT installation;
5. legacy PIC masking and I/O APIC discovery/routing;
6. Local APIC timer calibration through PIT channel 2; and
7. keyboard IRQ registration.

The photograph therefore selects this interval, not one assumed component,
as the initial failure boundary.

## Confirmed implementation hazards

### x2APIC-to-xAPIC transition

The current `amd64_lapic_init()` rewrites `IA32_APIC_BASE` in one operation
to `EN=1, EXTD=0`.  If firmware handed off with x2APIC enabled
(`EN=1, EXTD=1`), this attempts the architecturally invalid direct transition
from x2APIC to xAPIC.  Intel specifies that it raises a general-protection
exception; the valid legacy transition is x2APIC -> disabled -> xAPIC.  The
current IDT is installed later, so such an exception can appear as a silent
stop or reset loop.

Reference: Intel 64 and IA-32 Architectures Software Developer's Manual,
Volume 3A, “Local x2APIC State Transitions”:
<https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3a-part-1-manual.pdf>

This diagnostic Phase does not change the processor's APIC mode.  It checks
CPUID and the raw `IA32_APIC_BASE` state on the bootstrap processor and every
application processor before xAPIC MMIO.  Only an already-active xAPIC
(`EN=1, EXTD=0`) with an 8-bit-addressable APIC ID is accepted.  A disabled
APIC, x2APIC handoff, legacy-xAPIC lockout, or a larger APIC ID stops with an
explicit diagnostic and makes this Phase `uncleared`; the result then
authorizes design of complete APIC initialization or an x2APIC MSR backend.
This avoids a seemingly simple mode change whose complete register state,
LINT/NMI routing, and per-CPU reinitialization contract are not implemented
today.

### Unbounded PIT wait

`pit_wait_10ms()` polls port `0x61` bit 5 without a timeout.  It is the only
unbounded hardware wait in the selected interval.  A platform without the
expected PIT channel-2 behavior will spin forever immediately before
`A64 IRQ READY`.  The wait must become bounded and report a distinct failure.
On failure it must stop and mask the LAPIC timer and restore the original port
`0x61` gate/speaker state before returning the error, so a later retry or
future fallback does not inherit half-configured hardware.
Selecting and implementing an alternative calibration source is a follow-up
decision only if the bounded CF-SV7 evidence proves that it is needed.

## Fixed execution decisions

- Install a valid early GDT/TSS and IDT before firmware-dependent ACPI/MSR/MMIO
  accesses wherever current initialization dependencies permit.  An early
  `#GP` or `#PF` must print its vector, instruction address, and relevant fault
  address instead of becoming a silent triple fault.
- Add concise permanent boundary diagnostics rather than a table dump:
  ACPI root/MADT/MCFG completion, APIC mode and base, LAPIC completion, IDT,
  IOAPIC completion, timer calibration begin/end, and console IRQ completion.
- Validate CPUID APIC capabilities, MADT and `IA32_APIC_BASE`
  alignment/base agreement, and the initial APIC mode before any LAPIC MMIO.
  Do not attempt to clear EXTD on an x2APIC handoff in this Phase, and do not
  relax ACPI physical-range validation merely to make a table dereference
  succeed.
- Guard `IA32_ARCH_CAPABILITIES` with its CPUID enumeration before reading it,
  and read `IA32_XAPIC_DISABLE_STATUS` only when the architecture reports that
  facility. Do not issue the current broad low-bit-clearing mode write. An
  already-active xAPIC must retain its base, BSP, and mode fields unchanged and
  pass readback/base validation before MMIO.
- Make every early hardware poll finite.  A timeout is a visible error, not a
  degraded success.
- Preserve the working Latitude 5320 implementation and the 4/8/16-GiB OVMF
  USB matrix. This Phase does not request an additional Latitude boot.
- Use the current photograph as the baseline.  After the complete software
  batch and automated regressions, request one CF-SV7 boot only.  Do not gate
  each intermediate change on another human boot; five-run repeatability is
  reserved for final WS acceptance.

## Ordered work packages

1. Add a host-testable per-CPU APIC initial-state policy covering disabled,
   xAPIC, x2APIC, unsupported/locked legacy mode, MSR readback failure, and
   APIC-ID limits. Only already-active xAPIC is accepted by this Phase;
   disabled and x2APIC states are diagnosed without a mode write.
2. Move or introduce the minimum safe early descriptor/IDT boundary and prove
   that its handlers do not require initialized kernel services.
3. Add bounded diagnostics across ACPI discovery, LAPIC, IOAPIC, and timer
   initialization.
4. Remove the invalid direct x2APIC-to-xAPIC write. Preserve the existing
   xAPIC backend only for an already-active validated xAPIC and fail visibly,
   before MMIO, when complete APIC initialization or an x2APIC backend is
   required.
5. Bound PIT channel-2 calibration and preserve the observed port/LAPIC state
   in its timeout diagnostic.
6. Run focused host fixtures and the established amd64 OVMF q35/xHCI USB
   4/8/16-GiB login matrix, plus the supported BIOS regression.
7. Produce one clearly identified `build/amd64/hdd-image.img` and request one
   CF-SV7 physical boot.  Record the last marker and all displayed state.
8. If the three target markers pass, complete this Phase and extract the next
   CF-SV7 USB/device boundary.  Otherwise mark it `uncleared`, preserve the new
   exact boundary, and create a follow-up Phase without guessing through it.

## Verification contract

- `BR-T52` host fixtures exercise APIC initial-state policy and bounded timeout
  paths without privileged host MSR or port access.
- Fault-injection or mocked early-init fixtures show that a `#GP`, `#PF`,
  malformed ACPI table, APIC-mode mismatch, invalid IOAPIC topology, and PIT
  timeout each produce a distinct bounded result.
- The production amd64 UEFI image continues to reach `login:` under
  `qemu-system-x86_64` with q35/xHCI at 4, 8, and 16 GiB; the supported amd64
  BIOS path remains passing.
- One CF-SV7 boot uses the exact image linked in the handoff request and either
  reaches all three objective markers or supplies one new unambiguous bounded
  failure marker. Only the first result is `PASS`; the latter is
  `BOUNDARY-CAPTURED` and leaves this Phase `uncleared`.
- `make -j16` and `git diff --check` pass.  The aggregate `make check` target
  is not used.

## Automated checkpoint (2026-08-30)

The implementation batch is complete and frozen for the single physical
observation.  The handoff artifact is:

| Property | Value |
| --- | --- |
| Image | `/home/awe/zedBSD/build/amd64/hdd-image.img` |
| Size | 203,423,744 bytes |
| Build time | 2026-08-30 15:45:54 +0900 |
| SHA-256 | `38e1d8e4ccfb6ce7d1c37082818f76546a6e07dbf8e86e551e654a5f2b3ca9e8` |

Implemented behavior:

- GDT/TSS and IDT are active after paging and before ACPI, MSR, or APIC MMIO.
  The disposable-kernel injection runner produced bounded vector 6, 13, and
  14 diagnostics; the page fault reported `CR2=0xdeadbeef`.
- The old `IA32_APIC_BASE` mode-changing write is gone.  Bootstrap and
  secondary CPUs accept only an already-active xAPIC whose CPUID ID, MADT ID,
  MSR base, and MMIO ID agree.  Rejected APs publish a reason and park without
  APIC access; NMI panic broadcast becomes available only after every AP has
  reached ready.
- PIT calibration observes bounded OUT-low and OUT-high stages, restores port
  `0x61` on every return, and masks/stops the LAPIC timer on failure.  QEMU
  with `q35,pit=off` now stops in one second at
  `A64 TIMER CAL TIMEOUT stage=out-low` and cannot reach IRQ/HAL readiness.
- I/O APIC all-ones/invalid versions, impossible pin counts, GSI overflow, and
  overlapping ranges fail with a distinct topology diagnostic.

Automated evidence:

| Gate | Result | Preserved evidence |
| --- | --- | --- |
| APIC/PIT/I/O-APIC host policy, ordinary and ASan/UBSan | PASS | `tests/early-init-policy-test.c` |
| Early IDT `#UD/#GP/#PF` injection | PASS 3/3 | `temp/q033-final/early-idt/` |
| PIT-disabled negative QEMU cell | PASS | `temp/q033-final/pit-off/` |
| SeaBIOS q35/xHCI USB, SMP=4 | PASS to `login:` | `temp/q033-final/bios/` |
| OVMF q35/xHCI USB, SMP=4, 4/8/16 GiB | PASS 3/3 | `temp/q033-final/br-t24/` |
| `make -j16`, `make check-disk-image`, shell syntax, `git diff --check` | PASS | current worktree |

The aggregate `make check` target was not used.  Direct injection of a
malformed firmware ACPI table or architectural-MSR read, AP reject
publication/park, the PIT OUT-high timeout, and exact port/LVT rollback remains
impractical without compiling a production test hook, replacing the ACPI
firmware tables, or adding a hardware-I/O shim.  Those paths were
source-audited and the shared pure policies were executed, but they are not
misreported as direct runtime injections.  This does not add another human
checkpoint; the next action is the one physical CF-SV7 boot below.

## Physical handoff (one observation)

Boot the exact frozen image above on the Panasonic CF-SV7 once.  A photograph
or transcription must include the last visible `A64` lines.  The result is:

- `PASS` only if `A64 IRQ READY`, `A64 XMM CONTEXT PASS`, and
  `boot: HAL initialized successfully.` all appear; or
- `BOUNDARY-CAPTURED` if a new explicit ACPI/APIC/I/O-APIC/timer diagnostic is
  the last boundary.  Record that result as `uncleared` and extract a separate
  follow-up Phase rather than expanding q033.

## Physical result (2026-08-30)

`BR-T52` passed on its first consolidated CF-SV7 observation. The photographed
system had already advanced beyond all three objective markers and showed
xHCI enumeration, USB-storage publication, boot parameters, and VFS
initialization. Those later subsystems cannot execute before
`A64 IRQ READY`, `A64 XMM CONTEXT PASS`, and
`boot: HAL initialized successfully.`, so the downstream photograph is valid
evidence that the early ACPI/interrupt boundary is cleared.

The first new stop is outside this Phase:

```text
gpt: sda rejected: invalid protective MBR (3)
vfs: boot0 selector resolution failed (error 6)
VFS initialization failed (6); entering idle.
```

It is caused by copying the 397,312-sector fixed GPT image onto a
60,549,120-sector USB device. It is owned by
[ws003-p021](../phase021-portable-gpt-image-extent/phase.md); q033 and p020 do
not absorb the GPT/root-continuity change.

## Completion conditions

- No hardware wait between `A64 PAGING PASS` and `A64 IRQ READY` is unbounded.
- Firmware x2APIC state cannot trigger an invalid direct transition or an
  unobservable pre-IDT exception; it is either absent or reported as the
  explicit resume condition for a separate backend Phase.
- The CF-SV7 reaches `A64 IRQ READY`, `A64 XMM CONTEXT PASS`, and
  `boot: HAL initialized successfully.` in the single Phase acceptance boot.
- The declared automated regressions remain passing; the Phase does not insert
  an additional Latitude hardware checkpoint.
- The next CF-SV7 stop, if any, is outside this Phase and is recorded as a new
  bounded Phase rather than folded into early interrupt bring-up.

All completion conditions are satisfied. Remaining direct-injection gaps
listed in the automated checkpoint remain accurately recorded as test debt;
they did not prevent the production path from clearing the physical objective.

## Reconsideration boundary

Return to planning if CF-SV7 requires a full x2APIC MSR backend, 32-bit APIC
destinations, a new timer source, or relaxed firmware-memory ownership rules.
Those are architecture or policy expansions; this Phase must not silently
introduce them merely to clear one physical marker.
