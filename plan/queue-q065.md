# Queue: AX211/CNVio2 VFIO hardware path

Archived after bounded execution as `queue-q065.md`.

Last updated: 2026-09-03

QID: `q065`

Queue status: finished

Queue finished: **Yes**

Authorization: the user explicitly requested that the exact AX211 on
`10.0.10.25` be tested through QEMU PCI passthrough and that the corresponding
Phase be queued and executed.

Timebox: none. Execute the single finite resumed Phase below. A passthrough
failure is diagnosed only while the exact failing boundary remains observable;
do not guess proprietary CNVio2 behavior.

Parent: [master plan](master.md)

Previous Queue: [q064](queue-q064.md)

## Purpose

Resume `ws004-p038` on the exact `8086:51f0`, subsystem `8086:4090`, revision
`01` AX211/CNVio2 device by assigning its singleton IOMMU group to a disposable
QEMU zedBSD guest. Establish the deepest reproducible physical checkpoint from
PCI ownership through firmware/PNVM, NVM/MCC-constrained 2.4/5-GHz RF scan,
WPA2/CCMP and useful IP traffic, then restore the host device to `iwlwifi`.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p038` | [standalone AX211 normal path](ws004-hardware/phase038-intel-ax211-standalone-driver/phase.md) | uncleared (`q065`) | Safe VFIO assignment on `10.0.10.25`, bounded QEMU hardware execution with exact-stage evidence, and verified host-driver restoration; retain one direct-boot result as the final Phase closure |

## Accepted execution policy

- Confirm the SSH route does not use AX211, IOMMU group 11 contains only
  `0000:00:14.3`, VFIO is available, and no other QEMU owns the device before
  changing host binding.
- Use a disposable copy of the configured amd64 UEFI USB image. Do not modify
  host storage or the repository `.internal/` area.
- Bind only `0000:00:14.3` through per-device `driver_override`; do not use a
  broad vendor/device override.
- Install a bounded cleanup path before unbinding. On every ordinary exit,
  stop QEMU, clear the override, reprobe `iwlwifi`, and verify the original
  host network device exists. SSH remains on the separate USB Ethernet path.
- Record no SSID passphrase, machine credential, or other network secret in
  M/W/P/Q books, tracked tests, process listings, or captured logs.
- Treat PCI attach, firmware/PNVM ALIVE, scan, association, DHCP, ping, fetch,
  disconnect and down as distinct gates. A failure records the last passing
  gate and first exact diagnostic.
- QEMU passthrough is a development and strong physical-device evidence path;
  it does not replace the final one-run direct zedBSD boot because virtual PCI,
  ACPI and power topology differ from the host platform.

## Completion definition

Q065 finishes when the passthrough attempt either supplies its bounded gate
evidence or is honestly uncleared with an exact resume condition, the AX211 is
verified back on `iwlwifi`, and P/W/M/Q records agree. P038 itself remains
uncleared until its separately retained direct-boot completion checkpoint also
passes.

## Execution result

Q065 finished with `ws004-p038` uncleared and a precise continuation boundary.

- The exact `8086:51f0`, subsystem `8086:4090`, revision `01` device was passed
  through safely. Firmware and PNVM reached ALIVE, the runtime completed a
  regulatory-constrained 2.4/5-GHz scan, and the intended 5-GHz BSS was found.
- Association then failed deterministically at the first legacy
  `MAC_CONTEXT_CMD` ADD. Firmware reported UMAC `ADVANCED_SYSASSERT`; the last
  host command was group 1/opcode `0x28`. Supplying the reference-consistent
  initial DTIM interval did not change this boundary.
- The pinned `-89` firmware advertises `MLD_API_SUPPORT`; its matching command
  family is `MAC_CONFIG`, `LINK_CONFIG`, `STA_CONFIG`, and the corresponding
  key path. The current legacy MAC/binding/station sequence is therefore an API
  generation mismatch, not evidence for another legacy payload guess.
- The VFIO runner stopped the guest, cleared the per-device override, restored
  `0000:00:14.3` to `iwlwifi`, and verified that SSH still used the independent
  USB Ethernet route. The remote q065 staging tree, including credential-bearing
  temporary material and logs, was removed.
- The run also exposed the need for a real fixed-frequency HAL counter. The
  agreed API and initial calibration work remain uncommitted carry-over for the
  next Queue; amd64 must not publish it before the complete admitted CPU set is
  validated.

Resume through q066: finish `ws003-p025`, then replace the complete legacy
association family with the exact `-89` MLD family and rerun p038.
