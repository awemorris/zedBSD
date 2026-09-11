# WS004 Phase 037: Intel WLAN identity and firmware boundary

Last updated: 2026-09-02

Phase ID: `ws004-p037`

Status: complete (`q061`); the user corrected the Intel target to the observed
exact AX211/CNVio2 function

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

Evidence: [HW-T37 q061 read-only intake](../tests/q061-intel-wlan-intake-evidence.md)

## Objective

Test the Intel Wi-Fi 6 AX201 hypothesis against the exact function installed
in the authorized machine and freeze the resulting device, firmware,
provenance, licensing, package, and update boundaries before any zedBSD Intel
register access or firmware upload. Q061 corrected the target to AX211/CNVio2.
This is a read-only intake Phase. It does not detach the host driver, change
radio state, start a scan, bind the device to QEMU, or implement a driver.

The scheduling dependency on p030 is deliberate: the first RTL8822BU automatic
lifecycle implementation completed in q060 before this second-WLAN-driver
intake began. P037 does not consume or replace p030/WS005 p008's later shared
physical and five-run acceptance. This ordering is not a claim that Intel and
Realtek share a hardware architecture.

## Design boundary

- Treat `Intel Wi-Fi 6 AX201` as a product-family hypothesis until the exact
  PCI identity, subsystem identity, revision, companion topology, and host
  driver report are captured. Do not bind a product string or a broad Intel
  vendor range.
- Reuse the already published p027 WLAN UAPI only as an external contract.
  Do not create an Intel/Realtek common transport, firmware, ring, descriptor,
  calibration, or register layer during intake.
- The initial `userland/firmware/intelax201/` reservation was conditional on
  the hypothesis. Q061 corrected it to `userland/firmware/intelax211/` for the
  accepted exact device and frozen firmware set.
- Keep firmware bytes outside the permissively licensed base system. A normal
  base build performs no firmware download and the kernel performs no runtime
  network fetch.
- Preserve independent implementation freedom. Similar names or operations in
  the RTL8822BU code are not evidence of a shared implementation boundary.

## Read-only test-machine inventory

Use the already authorized SSH-accessible test machine. Record commands and
raw, redacted output in a future `HW-T37` intake record. At minimum collect:

1. PCI domain/bus/device/function, vendor/device, subsystem vendor/device,
   class, revision, BARs, interrupt capability, IOMMU group, and upstream PCI
   topology from read-only PCI/sysfs data.
2. The bound host driver and module identity, module version, declared firmware
   names, and any device-family or transport identifiers exposed by the host.
3. The exact firmware filename and version actually requested or loaded for
   this function, using read-only kernel/module diagnostics. Distinguish a
   module's broad candidate list from the payload selected for this machine.
4. Firmware byte size and SHA-256, the installed host package/version which
   supplied it, upstream repository revision, WHENCE entry, and complete
   applicable license text.
5. Whether the device is an independently assignable PCI function or depends
   on platform/CNVio companion resources that prevent faithful isolated PCI
   passthrough. This is a fact for p038's execution method, not permission to
   detach or assign it in this Phase.

Redact hostname, account names, MAC addresses, BSSIDs, SSIDs, credentials,
leases, and unrelated PCI devices from the retained plan evidence. A stable
PCI hardware identity and firmware digest are required engineering evidence
and are not replaced by network identity.

## Firmware and license decision procedure

1. Locate the firmware's authoritative upstream record and license text.
   Use the official immutable upstream tag/release for provenance and package
   acquisition; do not float on a branch or substitute an unverified mirror.
2. Freeze exact acquisition revision, relative file path, byte size, SHA-256,
   firmware-reported version when available, WHENCE record, license file, and
   source-to-install mapping.
3. Define a default-off `userland/firmware/intelax211/` entry which downloads
   only when selected, verifies all frozen bytes, installs below
   `/lib/firmware/`, and installs the applicable license and package manifest.
   The Phase plans this entry; p038 implements it with the driver.
4. Record redistribution conditions verbatim enough to preserve notices and
   restrictions. Do not describe binary firmware as project source code or as
   covered by the zedBSD base license.
5. Treat every firmware update as a separately reviewed revision/digest change
   with its own hardware regression, never a floating download from a branch.

If the exact terms clearly permit the proposed unmodified optional-package
distribution, p037 completes without human judgment. If redistribution,
notice, patent, modification, or acquisition terms are ambiguous, complete
the device/provenance inventory, mark p037 `uncleared`, and ask for the narrow
firmware/license decision. Do not queue p038's firmware package or upload path
while that boundary is unresolved.

## Verification contract (`HW-T37`)

- The raw read-only inventory and a normalized summary agree on exact PCI and
  subsystem identity, revision, driver, topology, and firmware selection.
- A neighboring or family-only PCI identity does not satisfy the exact match.
- The selected firmware bytes reproduce the recorded size and digest from the
  immutable acquisition source.
- The matching WHENCE/license records are retained and the proposed package
  layout contains no base-system blob, silent ordinary-build fetch, or kernel
  network fetch.
- The evidence distinguishes facts observed on the test machine, facts from
  primary upstream records, and implementation assumptions to be tested in
  p038.
- No host driver unbind/rebind, PCI assignment, radio operation, firmware
  upload, or zedBSD physical run occurs in this Phase.

## Completion conditions

- Exact device/subsystem/revision/topology identity and the supported binding
  tuple are frozen from the authorized read-only host evidence.
- The exact selected firmware set, compatibility evidence, size, digest,
  upstream provenance, immutable official acquisition revision, license text,
  install paths, and update rule are frozen.
- The base/optional-package boundary is explicit and mechanically testable.
- P038 knows whether safe isolated passthrough is plausible or a direct zedBSD
  boot will be required, without p037 performing either operation.
- No public WLAN UAPI or driver source is changed.

## Q061 read-only result

The authorized function is PCI `8086:51f0`, subsystem `8086:4090`, revision
`01`: an Alder Lake-P Root Complex Integrated Endpoint reported independently
by PCI and `iwlwifi` diagnostics as Intel Wi-Fi 6E AX211 160 MHz, Garfield
Peak. It is not AX201. The exact BAR, interrupt/MSI-X, singleton IOMMU group,
FLR, CNVio2 topology, driver/module, firmware, package, digest, WHENCE, and
license evidence is retained in the linked HW-T37 record without machine or
network identity.

The kernel selected `iwlwifi-so-a0-gf-a0-89.ucode` and reported firmware
version `89.735b75a4.0`; the family PNVM is also pinned. The host bytes match
official `linux-firmware` tag `20260410` at dereferenced commit
`dc85ccedc9c973682fbcf4d628ca61174bcc3120`. Tagged WHENCE reports
`86.735b75a4.0` for the same `-89` bytes, so the record preserves both the
upstream metadata and runtime report rather than hiding the discrepancy.

The Intel terms clearly permit an unmodified, separately installed binary
package with the complete required notice under the documented OSI-approved
base-license combination. License ambiguity is not the blocker. No package
policy decision remains. P038 owns the default-off
`userland/firmware/intelax211/` entry for the exact frozen `-89.ucode` and
PNVM bytes.

The observed function is a CNVio2 Root Complex Integrated Endpoint backed by
platform CNVi and a companion RF module. Although it occupies a singleton
IOMMU group, faithful generic PCI passthrough is not established; a retargeted
exact-device checkpoint would use bounded direct zedBSD boot.

The user accepted AX211 as the corrected exact target. Resume at the retargeted
standalone AX211 p038 Phase using only PCI `8086:51f0`, subsystem `8086:4090`,
revision `01`, the frozen firmware bytes, and bounded direct zedBSD boot.
P039 remains unstarted behind completion of that exact-device normal path.

## Reconsideration boundary

Before the q061 target decision, an unexpected device, unidentified firmware,
unrecorded platform companion, or ambiguous redistribution term required this
Phase to stop as `uncleared`. Q061 found AX211/CNVio2 and the user explicitly
accepted that exact device as the corrected target, so the mismatch is
resolved without broadening a PCI match. Future evidence must still stop
rather than substitute a neighboring identity or firmware payload, detach the
host device, or invent a common Intel/Realtek layer to bypass a missing fact.
