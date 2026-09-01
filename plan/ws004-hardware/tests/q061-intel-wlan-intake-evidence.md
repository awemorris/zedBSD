# HW-T37: q061 Intel WLAN read-only intake evidence

Last updated: 2026-09-02

Queue: `q061`

Phase: [`ws004-p037`](../phase037-intel-ax201-intake-firmware/phase.md)

Result: complete; the AX201 hypothesis was corrected by user decision to the
observed exact AX211/CNVio2 target

## Safety and redaction boundary

The inventory used only read-only PCI, sysfs, module, kernel-log, package, and
firmware-file inspection. It did not unbind or reload a driver, change radio or
network state, scan, assign a function to QEMU, upload firmware, install a
package, or write a remote file.

This retained record excludes the machine hostname, account name, IP address,
MAC address, BSSID, SSID, credentials, leases, and unrelated PCI functions.
The stable PCI identity and firmware digests below are required engineering
evidence rather than network identity.

## Observed hardware identity

| Field | Read-only result |
| --- | --- |
| PCI function | `0000:00:14.3` |
| Class | `0280` network controller |
| Vendor/device | `8086:51f0` |
| Subsystem vendor/device | `8086:4090` |
| PCI revision | `01` |
| Product report | Alder Lake-P PCH CNVi WiFi; Intel Wi-Fi 6E AX211 160 MHz, Garfield Peak |
| Kernel corroboration | `Detected Intel(R) Wi-Fi 6E AX211 160MHz`; `PCI dev 51f0/4090, rev=0x370` |
| PCI topology | Root Complex Integrated Endpoint directly below `pci0000:00`; no upstream PCI bridge |
| BAR 0 | `0x6055294000`--`0x6055297fff`, 16 KiB, 64-bit non-prefetchable |
| Legacy interrupt route | pin A, IRQ 16 |
| MSI | one-vector capability present, disabled in the observed state |
| MSI-X | enabled; 16-vector capability, 14 observed active vectors |
| IOMMU | group `11`, containing only `0000:00:14.3` |
| Reset | PCIe Function Level Reset capability present |
| Bound transport driver | `iwlwifi` |
| Operational module | `iwlmvm` |
| Kernel/module package | `linux-modules-6.19.13+deb13-amd64` `6.19.13-1~bpo13+1` |

The in-tree module has no separate module-version field. Its observed vermagic
is `6.19.13+deb13-amd64 SMP preempt mod_unload`. Relevant broad module metadata
declares `iwlwifi-so-a0-gf-a0-100.ucode` and
`iwlwifi-so-a0-gf-a0.pnvm`; those declarations are not evidence that API 100
was selected on this machine.

## Selected firmware and installed compatibility files

The boot log positively identifies the loaded payload as
`iwlwifi-so-a0-gf-a0-89.ucode`, reports operational mode `iwlmvm`, and reports
firmware version `89.735b75a4.0`.

| Role | Installed relative path | Size | SHA-256 | Selection evidence |
| --- | --- | ---: | --- | --- |
| Loaded operational firmware | `intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode` | 1,736,748 | `c569c4b0ffe2054a1cedd5affccff2da8515325eeb23f788c7abe9463d1a1514` | Positively named by the kernel boot log |
| Device-family PNVM | `intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm` | 55,176 | `efa9726d4a9d44b83fc9a14cedcf306a4e439e9de919802eb9e92df4ec032b2a` | Declared by the bound module and installed for this family; the retained boot log has no separate PNVM-loaded line |
| Installed fallback, not selected | `intel/iwlwifi/iwlwifi-so-a0-gf-a0-72.ucode` | 1,560,532 | `69fd0b6f3165da435cb67bdd7fce504978780b69506a8c48a597d5c87bc99e73` | Present on the host, but not the payload selected in this boot |

The compatibility links below `/lib/firmware/` and `/usr/lib/firmware/` point
to the `intel/iwlwifi/` files. Debian package `firmware-iwlwifi`
`20260410-1~bpo13+1`, source package `firmware-nonfree`, owns the files.
An optional debug request for `iwl-debug-yoyo.bin` failed; normal firmware
loading nevertheless completed, so that missing debug file is not part of the
successful operational payload.

## Primary-source provenance

The installed sizes and SHA-256 values reproduce exactly from official
`linux-firmware` tag `20260410`:

- annotated tag object `4585dd5a5f0cee08990d754701d8866d9e9266e6`;
- dereferenced release commit
  `dc85ccedc9c973682fbcf4d628ca61174bcc3120`;
- the selected `-89.ucode` and PNVM bytes were last updated by upstream commit
  `dafb7e8506a72e4436a72ac191d85415e48685b5`;
- authoritative records are the tagged
  [WHENCE](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/WHENCE?h=20260410)
  and
  [LICENCE.iwlwifi_firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/LICENCE.iwlwifi_firmware?h=20260410).

The tagged WHENCE record names the `-89.ucode` path and link but labels its
version `86.735b75a4.0`. The running kernel reports `89.735b75a4.0` for the
same exact bytes. The path, size, digest, and runtime report are retained
without silently normalizing this upstream metadata discrepancy.

## License boundary

The local package copyright file maps `intel/iwlwifi/*` to the same
`LICENCE.iwlwifi_firmware` terms retained upstream. Those terms permit
redistribution and use in unmodified binary form provided that the copyright
notice and disclaimer are reproduced. They prohibit endorsement using Intel
or supplier names and prohibit reverse engineering, decompilation, or
disassembly. The limited patent grant applies to the firmware alone or in
combination with an operating system licensed under an OSI-approved license.

The zedBSD base uses the OSI-approved zlib license. Therefore the documentary
terms are clear for a separately selected, unmodified optional firmware
package which installs the complete Intel notice and does not represent the
blob as zedBSD source. This is an engineering redistribution reading, not a
legal opinion. The license boundary is cleared for p037.

No Intel firmware bytes belong in the base source or default image. P038 owns
the default-off `userland/firmware/intelax211/` package for the frozen
`-89.ucode` and PNVM bytes. It must verify every byte, install the applicable
license and manifest below the optional-package boundary, perform no ordinary-
build or kernel runtime network fetch, and treat every update as a separately
reviewed revision, digest, and hardware regression.

## Companion topology and later execution method

Observed PCI facts alone show a singleton IOMMU group, FLR, and MSI-X, but
Intel's primary product material identifies AX211 as CNVio2: the solution is
split between integrated CNVi IP in the processor/PCH and a companion RF
module. A singleton IOMMU group therefore does not establish a self-contained
PCIe device that a generic QEMU machine can reproduce faithfully.

The resulting implementation assumption is that an exact-device checkpoint
would require a bounded direct zedBSD boot, not isolated PCI passthrough. This
is an inference from the observed Root Complex Integrated Endpoint and Intel's
documented CNVio2 topology; q061 did not detach, assign, or boot the device.

## Commands and evidence classes

The read-only command classes were:

- targeted PCI/sysfs identity, resource, IRQ, MSI, IOMMU, driver, module, and
  topology reads;
- target-only privileged `lspci` and a remotely filtered kernel journal which
  retained no machine or network identity;
- installed-module metadata inspection without loading or changing a module;
- `readlink`, `stat`, `sha256sum`, Debian package ownership/version,
  changelog, and local copyright inspection;
- official tag resolution and tagged WHENCE, license, and firmware-byte hash
  verification.

Observed-machine facts, official-primary-source facts, and the direct-boot
inference are separated above. No production, test-source, public-UAPI, or
firmware-package implementation was performed.

## Result and handoff

The exact authorized machine is AX211/CNVio2, not AX201. The user accepted the
observed device as the corrected Intel target. PCI `8086:51f0`, subsystem
`8086:4090`, revision `01`, the CNVio2/direct-boot boundary, and the frozen
firmware/license facts above are therefore authoritative for p038. They must
not be broadened to other Intel identities.

P037 and q061 are complete. The next Intel Phase is the retargeted standalone
AX211 p038 normal path. P039 remains unstarted until that exact-device normal
path completes.
