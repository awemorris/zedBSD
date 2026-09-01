# WS004 Phase 037: Intel Wi-Fi 6 AX201 identity and firmware boundary

Last updated: 2026-09-01

Phase ID: `ws004-p037`

Status: planned; follows the `ws004-p030` automatic lifecycle milestone;
read-only investigation requires no current human decision and is not queued

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Identify the exact Intel Wi-Fi 6 AX201 function installed in the authorized
test machine and freeze the firmware, provenance, licensing, package, and
update boundaries before any zedBSD Intel register access or firmware upload.
This is a read-only intake Phase. It does not detach the host driver, change
radio state, start a scan, bind the device to QEMU, or implement a driver.

The scheduling dependency on p030 is deliberate: finish the first RTL8822BU
automatic lifecycle implementation before beginning the second WLAN driver.
P037 does not consume or replace p030/WS005 p008's later shared physical and
five-run acceptance. This ordering is not a claim that Intel and Realtek share
a hardware architecture.

## Design boundary

- Treat `Intel Wi-Fi 6 AX201` as a product-family hypothesis until the exact
  PCI identity, subsystem identity, revision, companion topology, and host
  driver report are captured. Do not bind a product string or a broad Intel
  vendor range.
- Reuse the already published p027 WLAN UAPI only as an external contract.
  Do not create an Intel/Realtek common transport, firmware, ring, descriptor,
  calibration, or register layer during intake.
- Reserve `userland/firmware/intelax201/` as the optional-package location, but
  let the observed device/host-driver evidence determine the exact firmware
  filenames and compatibility range.
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
   Prefer the official upstream repository for provenance and an immutable
   GitHub mirror/revision only as the package acquisition transport required by
   the project layout.
2. Freeze exact acquisition revision, relative file path, byte size, SHA-256,
   firmware-reported version when available, WHENCE record, license file, and
   source-to-install mapping.
3. Define a default-off `userland/firmware/intelax201/` entry which downloads
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
  upstream provenance, immutable GitHub acquisition revision, license text,
  install paths, and update rule are frozen.
- The base/optional-package boundary is explicit and mechanically testable.
- P038 knows whether safe isolated passthrough is plausible or a direct zedBSD
  boot will be required, without p037 performing either operation.
- No public WLAN UAPI or driver source is changed.

## Reconsideration boundary

Mark this Phase `uncleared` if the machine is not the expected AX201 family,
the exact firmware cannot be identified, the device depends on an unrecorded
platform companion, or firmware redistribution requires a policy/legal choice.
Do not broaden a PCI match, substitute a neighboring firmware payload, detach
the host device, or invent a common Intel/Realtek layer to bypass the missing
fact.
