# WS003 Phase 003: Latitude xHCI capability/MMIO bring-up

Last updated: 2026-08-26

WSID: `ws003`

Phase ID: `p003`

Combined ID: `ws003-p003`

Status: Partial (`q012` finished; BR-T33 uncleared)

Acceptance disposition: **PCI/BAR/capability accepted on hardware; USB device enumeration handed to `ws003-p004`**

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md)

Evidence: [Latitude xHCI evidence](../tests/latitude-xhci-evidence.md)

## Objective

Move the Latitude 5320 from U1 to U2 by making the PCI/xHCI capability path
observable and correct on the physical controllers, then attach the controller
which owns the boot USB device and enumerate that USB mass-storage device.

Root mount, writable-root I/O, and login are recorded when reached, but remain
BR-06/BR-T30--T31 acceptance rather than being hidden inside a controller
attach Phase.

## Baseline and confirmed boundary

The corrected-image physical run reaches all of these markers:

```text
A64 ENTRY PASS
A64 PAGING PASS
A64 ACPI RSDP PASS
A64 IRQ READY
A64 XMM CONTEXT PASS
boot: HAL initialized successfully. [cpu 8, memory 1024MB, timer 10ms]
```

It then assigns two 64-KiB BAR0 regions and fails both xHCI functions at the
same stage:

```text
pci: BAR0 assigned to f0800000 (64 KiB)
xhci: attach failed at capabilities (13)
pci: BAR0 assigned to f0810000 (64 KiB)
xhci: attach failed at capabilities (13)
```

`13` is zedBSD `ENODEV`. In `xhci_attach()` it is emitted at `capabilities`
only when the combined CAPLENGTH, HCIVERSION, slots, ports, runtime, doorbell,
or BAR-extent validation fails. No xHCI HCD is consequently registered, so the
later storage wait expires, physical disk count remains zero, and UUID lookup
returns `ENOENT` (`6`). Those VFS messages are downstream evidence, not a
separate boot-selector defect at this boundary.

The visible ENTRY/ACPI/IRQ/HAL markers prove the former high-RSDP stop is
cleared in three runs. BR-T32 and `ws003-p002` are complete.

## Root-cause hypotheses

| Priority | Hypothesis | Evidence and discriminator |
| --- | --- | --- |
| H1 | PCI Memory Space decoding is disabled during the first xHCI MMIO reads | `xhci_attach()` reads capability registers before `drv_pci_device_enable_memory()`. If firmware left `PCI_COMMAND.MEM` clear, real hardware can return an all-zero or all-one register image. Record COMMAND before/after enable and raw capability values. |
| H2 | The fallback BAR relocation does not produce a reachable, routed register window | Both `BAR0 assigned` lines prove the firmware BAR could not be mapped by the current amd64 fixed window and was moved into `0xf0000000`--`0xf1000000`. The fallback does not yet prove resource non-overlap, bridge-window coverage, or 64-bit BAR readback. Record the original BAR pair, assigned pair, ancestry/windows, and readback. |
| H3 | A valid controller is rejected by a capability compatibility or extent rule | The driver accepts only HCIVERSION 1.0/1.1 and reports nine predicates as one error. Record the raw snapshot and a reason mask before changing any rule. The Intel 500-series data for the expected integrated controller describes HCIVERSION 1.1, CAPLENGTH `0x80`, DBOFF `0x3000`, and RTSOFF `0x2000`, all fitting a 64-KiB BAR; another discovered controller must be identified rather than assumed identical. |
| H4 | IRQ, DMA/IOMMU, reset, or USB-storage code caused the photographed failure | These paths execute only after capability validation and therefore cannot cause the current `(13)`. They become next-stage hypotheses only if attach advances to their named stage. |

H1 is tested first because it is a direct ordering defect. H2 remains a
separate real-hardware risk and must not be masked with retries or fixed delays.
H3 is resolved from the physical raw register values, not by accepting every
version or removing bounds checks.

## Scope

- Add bounded, reason-coded PCI/xHCI attach diagnostics for each discovered
  controller.
- Ensure Memory Space decoding is active before the first BAR MMIO access and
  is balanced across every failure and detach path.
- Validate original and reassigned 32/64-bit BAR values, mapping extent, and
  routing assumptions.
- Separate malformed/all-zero/all-one register images from an unsupported
  HCIVERSION or an out-of-range runtime/doorbell layout.
- Support the compatible xHCI 1.x capability revisions actually justified by
  the specification and fixtures, including 1.2 if the implementation's
  register assumptions are verified.
- Correct attach-path specification defects exposed by the audit: scratchpad
  count high/low field composition, relative xECP traversal, legacy ownership
  cleanup, and a bounded wait for `USBSTS.CNR` after controller reset.
- Preserve the QEMU xHCI/USB-root baseline and the high-RSDP UEFI matrix.
- On the Latitude, identify both xHCI functions and reach U2 on the function
  carrying the boot media.

## Non-goals

- General USB HID, USB4/Thunderbolt policy, SuperSpeed performance, hotplug,
  suspend/resume, or every external dock topology.
- NVMe, WLAN, i915, root-overlay repair, or a VFS/UUID redesign.
- Treating the intentional current 1-GiB general allocator/direct-map limit as
  an xHCI failure.
- Enabling bus mastering before DMA setup merely to test capability MMIO.
- Replacing evidence with an unconditional retry, delay, version wildcard, or
  unchecked BAR relocation.
- A general PCI resource rebalance unless H2 proves that a bounded mapping of
  the firmware-assigned BAR cannot solve the target case.

## Dependencies

- Completed `ws003-p002` high-RSDP path and kernel-console entry.
- WS004 PCI/ECAM/MSI and xHCI QEMU milestones (`ws004-p002`--`p004`).
- A disposable boot USB device and one user-operated Latitude boundary
  confirmation during this implementation Phase.
- Bounded PCI inventory for each xHCI function: segment/BDF, vendor, device,
  subsystem, revision, parent bridge, command register, and BAR0.

The complete `ws003-p001` inventory is not a prerequisite for the bounded
xHCI facts above.

## Fixed decisions

- Turn on PCI Memory Space decoding after BAR selection/mapping is prepared but
  before *any* capability or ownership MMIO access.
- Keep bus mastering separate and enable it only when DMA/controller startup
  requires it.
- Preserve or explicitly balance the original PCI command state on attach
  failure and detach; no successful probe may leak a half-enabled device.
- Reject all-zero/all-one capability images distinctly and retain arithmetic
  and BAR-extent checks.
- Identify both physical functions. Completion requires the boot-media
  controller to attach; the other must either attach or have a BDF-specific,
  evidence-backed unsupported reason.
- Prefer the firmware-assigned BAR when it can be safely mapped. A low-BAR
  fallback is allowed only with assignment readback, collision checks, and
  applicable bridge-window coverage.
- If arbitrary high MMIO mapping is required, use a bounded, uncached,
  supervisor-only, writable, NX PCI MMIO mapping with symmetric unmap. Do not
  reuse the read-only ACPI discovery window.
- If correct PCI resource routing requires a general allocator/bridge rebalance,
  stop this Phase as Partial and extract that work to WS004.

## Expected files and subsystems

- `drivers/pci-xhci.c`
- `drivers/pci.c` and `include/drivers/pci.h`, if balanced command-state or BAR
  inspection support is required
- `drivers/usb.c` and `include/drivers/usb.h` for checked xHCI quiesce and
  fail-closed unregister
- `drivers/pci-pcat.c`
- amd64 MMIO page mapping code only if H2 is confirmed
- `plan/ws003-bringup/tests/` fixtures, runbooks, and evidence
- WS004 cross-reference only if a common PCI resource Phase is extracted

## Ordered work packages

- [x] Record the BR-T32 photographs and establish xHCI capability
      validation as the earliest downstream failure.
- [x] Emit one bounded diagnostic record per controller containing
      segment/BDF, vendor/device/revision, PCI COMMAND before and after Memory
      Space enable, BAR type/size/original low-high pair, assignment readback,
      parent bridge, and final mapping extent.
- [x] Refactor capability validation into a reason-coded snapshot containing
      CAPLENGTH, HCIVERSION, HCSPARAMS1/2, HCCPARAMS1, DBOFF, RTSOFF, mapping
      size, and the exact failed predicate. Detect all-zero/all-one snapshots.
- [x] Add BR-T25 host fixtures for valid xHCI 1.0/1.1/1.2 layouts, zero/all-one
      images, every extent failure, scratchpad field composition, relative
      xECP walking, and `PCI_COMMAND.MEM=0` sequencing/rollback.
- [x] Move balanced Memory Space enable before the first MMIO read. Keep bus
      master, IRQ allocation, controller registration, and teardown ordered and
      independently diagnosed.
- [x] Make 32/64-bit BAR assignment transactional: disable and verify
      I/O/MEM/BME, program and read back both halves, restore COMMAND only on
      success, and roll back or remain decode-off after a partial failure.
- [x] Add an optional checked USB HCD quiesce barrier. xHCI unregister/detach
      releases IRQ, DMA, BAR, and controller memory only after `HCHalted`; a
      failed stop remains PCI-bound and quarantined against root/URB work and
      reprobe.
- [x] Correct scratchpad composition and xECP/legacy handoff walking, then wait
      for `USBSTS.CNR` to clear after reset before programming operational
      registers.
- [x] Use the first diagnostic image to choose the H2 branch. If the reassigned
      BAR reads back and returns a valid snapshot, do not broaden amd64 MMIO.
      If it remains zero/all-one, map the validated firmware BAR at its original
      address; extract general PCI rebalance if bridge/resource repair is
      required. The physical snapshot is valid through the relocated BAR, so
      no high-MMIO or general PCI rebalance branch is selected.
- [x] Run the focused fixtures, existing WS004 xHCI controls, `make -j16`, and
      `git diff --check`. Do not use `make check` or `.internal/` material.
- [x] Re-run BR-T24 with one production image at 4, 8, and 16 GiB and the
      legacy-BIOS q35/xHCI USB control.
- [x] Run BR-T33 once on the Latitude. Record both xHCI functions, the active
      controller's successful attach/start, and boot USB mass-storage
      enumeration. One success provisionally clears U2 and allows subsequent
      implementation to continue. Both controllers attached, but EP0 device
      enumeration failed before mass storage; BR-T33 therefore remains failed.
- [x] Record U3/login if reached. If the first new stop is controller reset,
      DMA, IRQ, port, mass-storage, or VFS, leave its exact stage and evidence
      as a bounded handoff rather than expanding this Phase silently. The new
      EP0 transfer/timeout/cancellation boundary is handed to `ws003-p004`.

## Acceptance cases

- `BR-T25`: the host capability/BAR/MEM-decode fixture accepts valid 1.0--1.2
  samples and rejects malformed, zero, all-one, overflow, and out-of-BAR
  samples with distinct reasons; enable/rollback ordering is asserted.
- `BR-T26`: production USB-core teardown retains the same registered bus and
  rejects new URBs when checked xHCI quiesce fails; a later successful retry
  performs stop and removes the bus.
- `BR-T21`: QEMU q35 xHCI still enumerates USB mass storage.
- `BR-T24`: one production image still reaches `login:` under OVMF USB boot at
  4, 8, and 16 GiB, plus the existing legacy-BIOS control.
- `BR-T33`: one Latitude boundary-confirmation boot attaches the boot-media
  xHCI controller and enumerates the intended USB mass-storage device without
  `attach failed at capabilities`, `boot-storage wait expired`, or
  `physical disks=0`. The legacy `devices=0` field is not itself a failure:
  successful QEMU USB-only boots print it before reporting `physical disks=1`.
- Focused tests, existing applicable WS004 xHCI tests, `make -j16`, and
  `git diff --check` pass.

## Completion conditions

- The exact BDF/IDs, command state, original/final BAR, and raw capability
  snapshot for both Latitude xHCI functions are stored as evidence.
- Valid capability images are accepted with bounds checks; zero, all-one,
  malformed offsets/extents, and unsupported revisions have distinct failures.
- PCI Memory Space, BAR mapping, ownership, bus master, IRQ, and controller
  startup have ordered, balanced failure/detach handling.
- All discovered xHCI functions either attach or have a specific documented
  unsupported reason, and the function carrying the boot media attaches.
- One Latitude boundary-confirmation boot enumerates the intended USB
  mass-storage device and reaches U2 without a boot-storage timeout caused by
  an absent HCD.
- BR-T25, BR-T26, BR-T21, BR-T24 at 4/8/16 GiB, the legacy-BIOS control,
  `make -j16`, and `git diff --check` pass without regression.

Reaching U3, login, and safe root I/O is desirable evidence but not required to
declare this controller-focused Phase complete. Those gates remain BR-06,
BR-T30, and BR-T31. Repeatability is not an intermediate p003 gate: after U4
implementation is frozen, BR-T30 performs the final five-consecutive-boot
Latitude acceptance campaign.

## Actual results and evidence

Execution was authorized as the sole item in `q012` on 2026-08-26. The driver
now enables and verifies PCI Memory Space before its first capability MMIO,
keeps Bus Master disabled until DMA startup, validates BAR assignment
readback, emits BDF/ID/COMMAND/BAR/raw-capability diagnostics, and restores
the saved PCI enable state and original BAR on failure or detach. Generic PCI
BAR assignment now performs checked, transactional 32/64-bit updates and
fails closed with decode/BME disabled after partial failure.

Capability arithmetic is shared with BR-T25. It accepts only xHCI 1.0, 1.1,
and 1.2, rejects zero/all-one and malformed layouts with reason bits, fixes
scratchpad Hi/Lo composition and relative xECP walking, performs byte-wide
legacy ownership handoff with SMI cleanup/rollback, and waits for CNR both
before initial operational writes and after HCRST.

Teardown now has a checked xHCI quiesce boundary. USB core removes a bus only
after `HCHalted`; otherwise it blocks new root/URB work and retains the bus.
An all-ones `USBSTS` read is treated as unreachable MMIO, never as a halted
controller, so DMA and IRQ resources remain retained on that fail-closed path.
Attach-time failures which cannot quiesce, or cannot restore PCI state, remain
bound to the PCI device in a quarantine state so DMA, IRQ, mapping, BAR claim,
and controller ownership cannot be freed or reprobed prematurely.

The following software gates pass:

- BR-T25 capability/MMIO and PCI enable-state fixture;
- BR-T26 checked HCD quiesce/unregister fixture;
- existing WS004 xHCI model, PCI rescan, PCIe capability, and PCI MSI fixtures;
- `make -j16`;
- BR-T24 OVMF USB-root boots at 4, 8, and 16 GiB;
- one legacy-BIOS q35/xHCI USB-root control reaching `login:`; and
- `git diff --check`.

The recorded q012 candidate is `build/amd64/hdd-image.img`, 135266304 bytes, SHA-256
`4b346ec9d303c557c4b810f2a5b3ea430964c7e6e9a98fc7a572410f2ba667f4`.
QEMU reports `reject=00000000:ok`, attaches xHCI, enumerates `sda`, resolves
the root, and reaches `login:` in every required software case. Physical U2
has not been claimed.

The one BR-T33 Latitude run used the recorded image and cleared the original
physical boundary. Both `0000:00:0d.0` and `0000:00:14.0` report
`version=0120` and `reject=00000000:ok`; both register an xHCI HCD. The latter
also proves that BAR0 relocation to `0xf0810000`, Memory Space enable, and the
64-KiB MMIO mapping are usable. H1 and H3 are resolved, while H2 does not
require a high-MMIO or general PCI-resource branch.

BR-T33 itself failed after controller start. `usb1` reported EP0 transfer
completion 6 followed by `EPIPE (44)`, completion 4 followed by `EIO (5)`, an
`ETIMEDOUT (42)`, and Stop Endpoint command 15 completion 19 during timeout
cancellation. The photograph contains no `usb-storage: sda` and stops at the
boot-storage wait. The first new boundary is therefore xHCI device/control
endpoint enumeration, before the mass-storage class driver. It is extracted
to [ws003-p004](../phase004-latitude-xhci-device-enumeration/phase.md). Code
review also found that the failed-cancel path loses the durable request/DMA
owner and can later free endpoint resources after an unchecked Disable Slot
failure. Treat the current artifact as diagnostic-only; p004 must close this
P1 before another physical run.

## Interruption / resumption

q012 is finished with this Phase Partial and its sole item uncleared. Resume
the bring-up only by placing `ws003-p004` in a future approved Queue. Its first
action is the focused control-TRB and EP0-state fixture; no further p003
physical repetition is required.

## Remaining debt and handoff

- Full root continuity, writable I/O, and login remain BR-06/BR-T30--T31.
- A proven need for generic high-physical-address PCI MMIO or bridge resource
  assignment becomes a separate WS004 Phase.
- The early-boot legacy BIOS ownership wait is bounded by MMIO poll count, not
  a real one-second monotonic deadline; a pre-scheduler HAL monotonic source is
  required before that specification detail can be closed.
- The PC/AT fallback BAR cursor is not reclaimed when a temporary assignment
  is restored, so repeated detach/reprobe can consume the small fallback
  window. General PCI resource reclamation belongs in WS004.
- The PCI enable-state token is intentionally an exclusive attach/detach lease;
  nested tokens or mixing it with the older enable counter remains unsupported.
- Returning xHCI legacy ownership before restoring a relocated BAR can race
  firmware which immediately reacquires the device. A later lifecycle Phase
  must define firmware hand-back explicitly; normal boot attach is unaffected.
- A raw BAR rollback write can itself fail. Decode/BME remain disabled and the
  cached address is refreshed, but richer recovery/diagnostics remain PCI-core
  work.
- A later failure after controller start is classified by its first stage and
  extracted if it exceeds this Phase's U2 boundary.

## References

- [Intel xHCI specification and current revisions](https://www.intel.com/content/www/us/en/products/docs/io/universal-serial-bus/universal-serial-bus-specifications.html)
- [Intel 500 Series PCH datasheet, volume 2](https://cdrdv2-public.intel.com/631120/631120-002.pdf)
