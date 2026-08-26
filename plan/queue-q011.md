# Queue: Latitude post-READY localization and high-RSDP correction

Last updated: 2026-08-26

QID: `q011`

Queue status: finished

Queue finished: **Yes**

Authorization: approved by user on 2026-08-26

Timebox: no fixed duration

Parent: [master plan](master.md)

Previous Queue: [q010](queue-q010.md)

## Purpose

Resume only `ws003-p002`. Make every boundary after `A64 UEFI READY` visible on
the Latitude without firmware console calls, use one physical diagnostic boot
to identify the first failing boundary, and correct only the failure supported
by that evidence.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws003-p002` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase002-uefi-memory-map/phase.md), [BR-T32 evidence](ws003-bringup/tests/latitude-uefi-evidence.md) | completed | Four physical GOP markers identify the old 1-GiB RSDP rejection; the bounded correction passes 4/8/16-GiB OVMF USB boots and BR-T32 passes 3/3 on the Latitude |

## Dependencies and uncertainty

- The diagnostic physical run is complete: four blocks prove kernel entry and
  the measured RSDP is `0x64ffe014`.
- The correction does not broaden the general direct map or allocator. It uses
  an early-only, bounded 16-MiB sparse ACPI mapping window.
- QEMU/OVMF acceptance requires the same image to reach `login:` at 4, 8, and
  16 GiB with an RSDP above 1 GiB.
- Three user-operated Latitude cold boots of the corrected image reach
  ACPI/IRQ/HAL; BR-T32 is complete.
- The first downstream failure is both xHCI functions rejecting their
  capability image. It is planned as `ws003-p003`, which is not part of q011
  and is not authorized for implementation by this Queue.

## Final physical request result

- Purpose: confirm that the q011 high-RSDP correction reproducibly crosses the
  UEFI-to-kernel boundary and reaches initialized ACPI/IRQ/HAL state.
- Scope: `ws003-p002`, BR-T32, run 3 of 3.
- Artifact: [build/amd64/hdd-image.img](../build/amd64/hdd-image.img)
- SHA-256:
  `5d6900b49f2edf51a742b94491783f1f6d7c5809ea57cd43d982140a825a0dd8`
- Preparation: if the USB medium does not already contain this exact artifact,
  write it to the whole USB device as a raw disk image rather than copying it
  into a filesystem.
- Action performed: the image was booted once from complete power-off on the
  Latitude.
- Evidence: the supplied full-screen photograph contains the latest visible
  output.
- Pass markers: `A64 ENTRY PASS`, `A64 PAGING PASS`, `A64 ACPI RSDP PASS`,
  `A64 IRQ READY`, `A64 XMM CONTEXT PASS`, successful HAL initialization, and
  `A64 TIMER TICK`.
- Outside this run: the later `xhci: attach failed at capabilities (13)`, boot
  storage timeout, and UUID failure are the planned `ws003-p003` handoff and do
  not fail BR-T32 once all pass markers above are present.
- Result: PASS; BR-T32 run 3/3 and q011 are complete.

## Execution contract

- Do not call UEFI console or any boot service after successful
  `ExitBootServices()`.
- Keep the marker implementation independent of port `0xe9`; the latest stage
  must remain recognizable in a photograph if execution halts.
- Preserve existing debug-port text for OVMF automation.
- Correct a defect inside the UEFI-to-early-amd64 boundary only when the marker
  and bounded facts support it. Extract an address-space/LA57 redesign instead
  of silently broadening this Queue.
- Use focused tests, `make -j16`, QEMU OVMF USB control, and
  `git diff --check`. Do not use `make check`, `.internal/`, or commits.

## Execution record

Execution was approved and completed on 2026-08-26. The diagnostic physical
run, correction, software gates, and corrected-image Latitude acceptance are
complete.

- Final corrected image: `build/amd64/hdd-image.img`
- Image SHA-256:
  `5d6900b49f2edf51a742b94491783f1f6d7c5809ea57cd43d982140a825a0dd8`
- BR-T23 memory-map fixture: PASS
- Physical diagnostic: RSDP `0x64ffe014`, CR4 `0x668`, 173 descriptors,
  135 normalized ranges, and four GOP blocks; U1 proved
- Confirmed cause: the old pre-console `rsdp >= 0x40000000` handoff rejection
- ACPI sparse-window fixture: PASS
- BR-T24 q35/OVMF/xHCI/SMP=4 USB boot at 4/8/16 GiB: 3/3 PASS;
  every run observed high RSDP, ACPI/IRQ readiness, four CPUs, and `login:`
- PC/AT BIOS xHCI USB control: 1/1 PASS
- 2026-08-26 requested revalidation: both focused fixtures PASS; `make -j16`
  reports the production image up to date; image SHA-256 remains
  `5d6900b49f2edf51a742b94491783f1f6d7c5809ea57cd43d982140a825a0dd8`
- Revalidation BR-T24 under QEMU 10.0.11: 4/8/16-GiB cases PASS in
  12/11/11 seconds with RSDP `0x000000007f77e014`; legacy-BIOS q35/xHCI USB
  control PASS 1/1 in 11 seconds
- Physical corrected-image run 1/3: PASS through kernel ACPI/IRQ/HAL and timer;
  the previous high-RSDP boundary is cleared in this run
- Physical corrected-image run 2/3: PASS through `ENTRY`, `PAGING`, ACPI RSDP,
  IRQ, XMM, eight-CPU HAL initialization, and `A64 TIMER TICK`; image SHA-256
  matches the q011 production image
- Physical corrected-image run 3/3: PASS through the same UEFI/kernel boundary
  and all BR-T32 markers; the later xHCI/storage failure is outside p002
- Downstream handoff: both xHCI functions fail at `capabilities (13)`, followed
  by zero physical disks and UUID `ENOENT`; analysis and the proposed solution
  are in [ws003-p003](ws003-bringup/phase003-latitude-xhci-capability-mmio/phase.md)
- Physical disposition: BR-T32 PASS 3/3; q011 finished
