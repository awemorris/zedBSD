# WS003 Phase 002: Latitude UEFI memory-map normalization

Last updated: 2026-08-26

WSID: `ws003`

Phase ID: `p002`

Combined ID: `ws003-p002`

Status: Complete

Acceptance disposition: **Cleared**

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md)

## Objective

Make the production amd64 `BOOTX64.EFI` accept and safely normalize the Dell
Latitude 5320 firmware memory map, exit UEFI boot services, and enter the
kernel with a valid, bounded handoff.

## Physical finding

The target loads the zedBSD USB image and displays:

```text
A64 UEFI ENTRY
A64 UEFI ELF
A64 UEFI READY
Normalize memory map: 0x8000000000000001
```

The visible status is the loader's locally selected `EFI_LOAD_ERROR`, not a raw
status returned by firmware. Source inspection shows that normalization rejects
at least these conditions: a short/misaligned descriptor stream, page-count or
address overflow, descriptors not monotonically ordered, overlapping ranges,
more than `MAX_MEMORY_RANGES`, or an empty result. The photo alone does not
distinguish them.

The next physical run advanced past that rejection but displayed only
`A64 UEFI READY`. Source review found that the final map was normalized between
`GetMemoryMap()` and `ExitBootServices()`. On firmware which changes its map
asynchronously, the sort/merge delay can invalidate the key on every attempt;
the old three-failure path then halted without a visible post-READY message.
The final normalization is now deferred until after a successful exit, leaving
only the required two firmware calls in the retry interval.

The 2026-08-26 physical acceptance run with that corrected sequencing still
stops visibly at exactly `A64 UEFI READY`. The sequencing defect was real, but
the physical result falsifies it as a sufficient explanation. The loader emits
all later diagnostics only to QEMU debug port `0xe9`, and the kernel can also
halt in `bsp_boot_init()` before its framebuffer console is initialized.
Consequently the current screen cannot distinguish final-map failure,
`ExitBootServices()` failure, bootstrap page-table/CR3 failure, kernel-entry
failure, or an early silent handoff rejection.

The q011 diagnostic hardware run resolved that uncertainty. The Latitude
reported `RSDP=0x64ffe014`, `CR4=0x668`, 173 firmware descriptors, and 135
normalized ranges. Four persistent GOP blocks prove successful
`ExitBootServices()`, final normalization, bootstrap-CR3 execution, and the
first kernel instruction. LA57 is clear and the handoff capacity is not
exhausted.

The first proven stop was therefore the old `bsp_boot_init()` validation which
rejected every RSDP at or above 1 GiB before console initialization. The
Latitude address exceeds that limit.

The corrected-image hardware runs display `ENTRY`, `PAGING`, ACPI
RSDP, IRQ, XMM, eight-CPU HAL initialization, and the timer. This proves the
former boundary is corrected. The image subsequently fails both physical
xHCI functions at capability validation and therefore enumerates no boot USB
disk. That downstream boundary is planned separately as `ws003-p003`; this
Phase completed its declared BR-T32 gate 3/3.

## Scope

- Capture descriptor size/version/count, RSDP and framebuffer addresses, and
  CR4.LA57 state using bounded diagnostics that do not dump sensitive firmware
  data.
- Add post-READY visual stage markers written directly through the GOP
  framebuffer, without calling firmware after `ExitBootServices()`, for:
  successful boot-services exit, final-map normalization, bootstrap CR3 load,
  and the first kernel instruction.
- Compare the Latitude map shape with the UEFI contract and QEMU/OVMF control.
- Accept valid maps regardless of descriptor ordering by sorting/merging a
  loader-owned representation when required, while rejecting real overlaps,
  integer overflow, malformed descriptor sizing, and capacity exhaustion.
- Keep `GetMemoryMap`/`ExitBootServices` retry semantics valid: do not allocate
  memory after obtaining the final map key.
- Verify that normalized ranges retain reserved/runtime/MMIO distinctions
  required by the amd64 HAL and do not expose unavailable memory.

## Non-goals

- Continuing past malformed or overlapping firmware maps by silently dropping
  descriptors.
- Secure Boot enablement, kernel xHCI bring-up, or root-filesystem repair.
- Treating the attached photo as a complete hardware inventory.

## Expected files and subsystems

- `bootloader/uefi/bootx64.c`
- amd64 handoff memory-range definitions and validation, if evidence requires
- WS003 host fixtures and QEMU/Latitude evidence

## Ordered work packages

- [x] Make post-READY failure stages observable on physical GOP without relying
      on port `0xe9` or boot services.
- [x] Add host fixtures for sorted, unsorted-valid, adjacent, overlapping,
      malformed-size, overflow, and range-capacity maps.
- [x] Implement safe canonical ordering/merging at the loader boundary.
- [x] Verify QEMU/OVMF still emits `A64 UEFI EXIT` and reaches kernel/login.
- [x] Re-verify QEMU/OVMF after moving final normalization past
      `ExitBootServices`.
- [x] Rebuild the diagnostic USB image and verify its marker path reaches login
      under q35/OVMF/xHCI/SMP=4.
- [x] Record the latest visible stage and bounded address/state facts on the
      Latitude.
- [x] Correct the first proven failing boundary. If the evidence requires a
      broader physical-map or LA57 redesign, leave this Phase Uncleared and
      extract that design rather than guessing inside this Phase.
- [x] Boot the same production image through OVMF q35/xHCI USB with 4, 8, and
      16 GiB; require a greater-than-1-GiB RSDP, ACPI/IRQ readiness, four CPUs,
      `login:`, and no fatal/storage error.
- [x] Verify the Latitude passes normalization, `ExitBootServices`, CR3
      transition, and kernel entry on three cold boots (result: 3/3).
- [x] Record the new highest U-tier and hand off any next physical failure
      without claiming shell acceptance prematurely.
- [x] Run focused tests, `make -j16`, and `git diff --check`; do not use
      `make check` or `.internal/` material.

## Completion conditions

- Valid descriptor maps in arbitrary order normalize deterministically; invalid
  size, overflow, overlap, and capacity cases fail with distinct diagnostics.
- QEMU/OVMF retains its current UEFI boot behavior.
- One production image passes the BR-T24 OVMF q35/xHCI USB matrix at 4, 8,
  and 16 GiB with an RSDP above 1 GiB, full ACPI/IRQ initialization, four
  CPUs, and `login:`.
- The Latitude passes `Normalize memory map`, exits boot services, and reaches
  an unambiguous kernel-entry marker on three cold boots from the USB image.
- The handoff contains non-overlapping, ordered, correctly typed ranges within
  the fixed handoff capacity.
- Focused tests, `make -j16`, and `git diff --check` pass.

## Actual result

All completion conditions pass. The same production image passes the focused
fixtures, the 4/8/16-GiB OVMF USB matrix, the legacy-BIOS control, and BR-T32
3/3 on the Latitude. Each physical run reaches `ENTRY`, `PAGING`, ACPI RSDP,
IRQ, XMM, eight-CPU HAL initialization, and the timer. The later xHCI
capability failure is outside this Phase and is handed to `ws003-p003`.

## Interruption / resumption

The BR-T23 fixture, q011 diagnostic run, bounded ACPI-window correction,
BR-T24 software gates, and BR-T32 physical acceptance are complete. The exact
old stop was selected from physical evidence rather than inferred from OVMF.
Resume WS003 by selecting planned `ws003-p003` in a new Queue; the downstream
xHCI failure was not implementation scope for q011.

The q011 diagnostic image prints RSDP, GOP, low-bootstrap, CR4, map size,
descriptor size/version, and normalized range count before `READY`. It reserves
the top-right GOP panel for a persistent unary stage code:

- one large block: `ExitBootServices()` succeeded;
- two: the accepted final memory map normalized successfully;
- three: execution continued under the bootstrap CR3; and
- four: the first kernel instruction executed.

An ordinary failure returned by the final `GetMemoryMap()` or
`ExitBootServices()` is printed through the still-live firmware console. If
final normalization fails after exit, one large block remains and 1--7 small
lower blocks encode the `zbl_uefi_map_result` value.

The Latitude photograph showed all four blocks and `RSDP=0x64ffe014`, proving
U1 and selecting the old 1-GiB RSDP rejection as the exact first stop. The
kernel now retains its 1-GiB general direct map and allocator but maps validated
ACPI pages through a dedicated persistent 16-MiB sparse window. The window is
early-boot-only, read-only, NX, supervisor-only, non-global, and bounded to
4096 aggregate unique pages. Physical spans are page-rounded and checked
against CPU MAXPHYADDR and, for UEFI, ACPI reclaim/NVS/reserved map entries.
Legacy BIOS retains its historical sub-1-GiB readable extent because its v1
handoff does not describe SeaBIOS's reserved ACPI-table gap.

BR-T24 passes 3/3 using the same production image at 4, 8, and 16 GiB. Every
case placed the RSDP at `0x7f77e014` and reached ACPI validation, IRQ readiness,
four CPUs, USB-root init, and `login:` without fatal or storage errors. A
legacy-BIOS q35/xHCI USB control also passes 1/1. BR-T32 passes 3/3 with the
corrected final image. All three runs crossed the old boundary and exposed
`xhci: attach failed at capabilities (13)`
on both physical xHCI functions; see
[latitude-xhci-evidence.md](../tests/latitude-xhci-evidence.md).
