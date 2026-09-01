# Queue: Intel WLAN read-only identity and firmware intake

Last updated: 2026-09-02

QID: `q061`

Queue status: finished

Queue finished: **Yes**

Authorization: the user authorized continuous Queue execution and selected the
WS004 hardware sequence. Q060 completed the RTL8822BU p030 automatic lifecycle
milestone, satisfying p037's ordering dependency without consuming p030's
later shared WS005 p008 physical closure.

Timebox: none. Execute only the finite read-only `ws004-p037` intake. Do not
detach a host driver, change radio state, start a scan, upload firmware,
implement the Intel driver, or begin p038/p039 in this Queue.

Parent: [master plan](master.md)

Previous Queue: [q060](queue-q060.md)

## Purpose

Test the Intel Wi-Fi 6 AX201 hypothesis against the authorized machine and
freeze the corrected exact PCI/subsystem/revision/topology facts, selected host
firmware, immutable provenance, digest, license, optional-package boundary,
safe later execution method, and negative binding identities before any zedBSD
Intel register access or firmware upload.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p037` | [Intel identity and firmware boundary](ws004-hardware/phase037-intel-ax201-intake-firmware/phase.md) | complete (`q061`) | Read-only evidence corrected the target to exact AX211/CNVio2 `8086:51f0`, subsystem `8086:4090`, revision `01`, and froze firmware, provenance, license, package, and direct-boot boundaries |

## Accepted decisions

- Treat `Intel Wi-Fi 6 AX201` as a product-family hypothesis until exact PCI,
  subsystem, revision, companion-topology, and selected-driver evidence agrees.
- The test-machine inventory is read-only. Do not unbind, reload, replace, or
  reconfigure the host driver or firmware; do not change networking or radio
  state.
- Retain only the minimum redacted hardware and firmware facts required by the
  Phase. Do not retain unrelated machine, network, or credential identity.
- Freeze firmware bytes, acquisition source, digest, license, packaging, and
  update rules before p038 uses them. Do not infer redistribution permission
  from availability alone.
- Reuse the stable public WLAN contract, not RTL8822BU hardware internals, and
  do not create an Intel/Realtek common hardware layer.
- Do not begin p038 driver implementation or p039 commonization review in q061.

## Implementation checkpoints

1. Collect the exact read-only PCI identity, subsystem identity, revision,
   topology, active host driver, and selected firmware report from the
   authorized machine.
2. Resolve the selected firmware files to exact immutable bytes and record
   primary-source provenance, digests, license terms, and update boundaries.
3. Define exact positive and neighboring negative binding identities and the
   optional zedBSD package/install boundary without adding production code.
4. Freeze a safe later p038 execution method that does not mutate the current
   host during this intake.
5. Reconcile the retained evidence with HW-T37 and the WS004/master ledgers;
   stop at any exact-target or redistribution decision rather than guessing.

## Completion definition

Q061 completes when p037 has a redacted, read-only, exact-machine record and a
primary-source-backed firmware/provenance/license/package decision sufficient
to start a separately queued p038 without guessing device identity or bytes.
Completion makes no driver, scan, association, data-path, or physical zedBSD
radio claim and does not alter p030/WS005 p008's pending shared physical and
five-run closure.

## Execution result

Complete. The authorized read-only inventory rejected the AX201 hypothesis and
identified Intel Wi-Fi 6E AX211/CNVio2 at PCI `8086:51f0`, subsystem
`8086:4090`, revision `01`. HW-T37 freezes its Root Complex Integrated
Endpoint topology, BAR/IRQ/MSI-X/IOMMU/FLR facts, `iwlwifi`/`iwlmvm` binding,
selected `iwlwifi-so-a0-gf-a0-89.ucode`, family PNVM, exact sizes and SHA-256
values, official `linux-firmware` `20260410` provenance, WHENCE/runtime-version
discrepancy, and clear Intel unmodified-binary redistribution boundary.

The user accepted AX211 as the corrected target. P038 is retargeted to an
independent AX211 driver using the exact binding tuple, frozen firmware, and a
bounded direct zedBSD boot rather than generic PCI passthrough. P039 remains
unstarted until p038 works. No host state, source, public WLAN UAPI, firmware
package, or driver implementation changed in q061.

Next Queue: [q062](queue.md)
