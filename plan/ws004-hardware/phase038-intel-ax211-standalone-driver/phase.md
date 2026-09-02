# WS004 Phase 038: standalone Intel Wi-Fi 6E AX211 normal path

Last updated: 2026-09-02

Phase ID: `ws004-p038`

Status: Uncleared (`q062`); automatic implementation/gates complete, exact
direct-boot checkpoint deferred by the user

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

Intake evidence:
[HW-T37 q061 read-only record](../tests/q061-intel-wlan-intake-evidence.md)

## Objective

Implement the exact q061 Intel Wi-Fi 6E AX211/CNVio2 target as an independent
native zedBSD driver and establish one bounded useful normal path in this
order:

```text
exact attach + pinned firmware/PNVM
  -> 2.4-GHz/20-MHz scan
  -> WPA2-Personal/CCMP authorization
  -> DHCP
  -> gateway ping + public ping + bounded HTTP fetch
  -> disconnect + administrative down
```

The purpose is first communication, not exhaustive hardening. Essential input
bounds, finite waits, checked DMA/interrupt ownership, secret redaction, and
fail-closed authorization are required from the start. Exhaustive recovery,
rekey, suspend, race, fault-injection, throughput, and repeatability work
remains a later Phase extracted after this exact normal path works.

## Frozen exact-device boundary

P038 binds only this q061 tuple:

| Field | Frozen value |
| --- | --- |
| PCI vendor/device | `8086:51f0` |
| Subsystem vendor/device | `8086:4090` |
| PCI revision | `01` |
| Device | Intel Wi-Fi 6E AX211 160 MHz, Garfield Peak |
| Platform interface | Alder Lake-P PCH CNVi WiFi, CNVio2 Root Complex Integrated Endpoint |
| Firmware | `intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode` |
| Firmware size/SHA-256 | `1736748`; `c569c4b0ffe2054a1cedd5affccff2da8515325eeb23f788c7abe9463d1a1514` |
| PNVM | `intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm` |
| PNVM size/SHA-256 | `55176`; `efa9726d4a9d44b83fc9a14cedcf306a4e439e9de919802eb9e92df4ec032b2a` |
| Upstream snapshot | official `linux-firmware` tag `20260410`, commit `dc85ccedc9c973682fbcf4d628ca61174bcc3120` |
| Runtime firmware report | `89.735b75a4.0` |
| Execution method | bounded direct zedBSD boot on the exact machine |

Do not match AX201, AX210, another AX211 subsystem/revision, a broad Intel
vendor range, or a neighboring `so-a0` firmware family. Tagged WHENCE reports
`86.735b75a4.0` for the exact `-89` bytes while the host runtime reports
`89.735b75a4.0`; retain that discrepancy in diagnostics and tests rather than
using it to substitute a different blob.

## Dependencies

- `ws004-p030`: its automatic RTL8822BU lifecycle implementation is complete;
  WS005 p008's shared physical closure is not a prerequisite here.
- `ws004-p037`: q061 completed the exact AX211/CNVio2 identity, topology,
  selected firmware/PNVM, immutable official provenance, clear license, and
  optional-package boundary.
- `ws004-p003`, `p012`, and the existing PCI/MSI-X, DMA, net-device, shutdown,
  and checked lifetime contracts.
- `ws004-p027` and `p029`: the existing generic WLAN UAPI/common station,
  WPA2-Personal/CCMP engine, controlled port, and Ethernet conversion contract.
- One bounded direct zedBSD boot on the q061 machine. Generic QEMU PCI
  passthrough is excluded because the Root Complex Integrated Endpoint depends
  on platform CNVi and its companion RF module even though its observed IOMMU
  group is a singleton.

P006/p007 WS005 orchestration is not a dependency. The development checkpoint
may use the already working primitive commands and `dhcpc`, keeping this Phase
focused on the Intel hardware path.

## No-premature-commonization rule

Implement AX211 behind the existing stable WLAN and `net_device` contracts in
Intel-owned source. It may duplicate concepts or small routines found in the
RTL8822BU implementation. Do not first introduce a shared Intel/Realtek
firmware loader, command transport, DMA ring, descriptor codec, interrupt
engine, calibration layer, register API, or chip-family framework.

Reuse only facilities which already have a device-independent public contract,
such as PCI/DMA/interrupt primitives, the p027 WLAN operations, p029 WPA2/CCMP
state, and the net-device boundary. If an internal adapter can express AX211
behavior through those interfaces, prefer the adapter over a public UAPI
change. If correct operation truly requires a new public semantic, stop and
mark p038 `uncleared` with the exact missing operation and affected consumers.

The Intel driver must remain replaceable as one implementation. Its private
module boundaries, state machine, firmware commands, and data structures may
differ completely from Realtek.

## Initial capability boundary

- Exact q061 AX211 identity only: `8086:51f0`, subsystem `8086:4090`, revision
  `01`, with the recorded CNVio2 platform relationship.
- Infrastructure station mode, 2.4-GHz channels permitted by the frozen world/
  board policy, 20-MHz width, and the legacy/HT subset actually required for
  the controlled normal path.
- WPA2-Personal PSK with CCMP-128 through the existing common security engine.
- One ordinary Ethernet MTU and one bounded TX/RX queue profile sufficient for
  DHCP, ICMP, ARP, DNS, and the HTTP oracle.
- No 5 GHz or 6 GHz, 40/80/160 MHz, 802.11ax/HE performance claim, WPA3/SAE,
  802.1X, roaming, AP/monitor mode, aggregation tuning, power-save
  optimization, Bluetooth coexistence optimization, suspend/resume, or
  throughput target.

Unsupported marketed capabilities remain explicit and do not enter the first
normal-path completion claim.

## Ordered implementation

### 1. Exact attach and optional firmware package

- Match only the frozen PCI/subsystem/revision tuple and confirm the expected
  CNVio2/So/GF transport identifiers before register or DMA access.
- Acquire BAR0, MSI-X, DMA, platform-companion, and net-device ownership
  transactionally with exact reverse unwind. The observed BAR/IRQ/IOMMU/FLR
  values are evidence, not hard-coded runtime addresses or vector numbers.
- Implement default-off `userland/firmware/intelax211/`. Acquire only the
  frozen `-89.ucode`, PNVM, and `LICENCE.iwlwifi_firmware` from the immutable
  official snapshot, verify both sizes and SHA-256 values, install below
  `/lib/firmware/`, and install the license and package manifest.
- Keep every blob outside the base-license claim and default image. An ordinary
  build and the kernel perform no firmware network fetch. Missing, wrong-
  digest, or incompatible bytes fail visibly with carrier down.
- Automatically install the complete OpenBSD ISC and Intel BSD-3-Clause
  source-derivation notices whenever the kernel AX211 driver is selected. This
  binary-distribution obligation is independent of the optional firmware
  package and its separate firmware license.
- Parse and upload only the pinned firmware/PNVM formats with checked
  arithmetic, bounded commands, explicit ready/error states, and complete
  staging scrub. Do not reverse engineer, decompile, or disassemble the blob.

### 2. AX211-private CNVio2 transport and radio start

- Keep firmware command/event rings, TX/RX descriptors, DMA ownership,
  interrupts, NVM/calibration, channel programming, CNVi/CRF interaction, and
  device reset inside the AX211 implementation.
- Use finite controller/firmware deadlines and reject unknown firmware, PNVM,
  NVM, RF, or companion layouts. Never guess board calibration or report
  carrier after partial initialization.
- Publish one stable `wlanN` only after exact identity and basic object lifetime
  are valid; keep it administratively down until open succeeds.

### 3. Scan normal path

- Map Intel receive/event formats into the existing bounded WLAN scan callbacks
  without changing their public representation.
- Complete one finite 2.4-GHz scan, publish truthful BSS/security/channel/RSSI
  fields, and stop cleanly. A scan result never implies authorization.

### 4. WPA2/CCMP and Ethernet normal path

- Supply the existing common authentication, association, EAPOL, hardware-key,
  encrypted TX/RX, and completion callbacks from AX211-private operations.
- Preserve p029 replay, MIC, key-install, controlled-port, and secret-lifetime
  rules. Carrier rises only after the common engine authorizes the link.
- Convert one bounded authorized Ethernet stream through the existing common
  L2 boundary; no Intel-specific packet format escapes the driver.

### 5. Useful IP checkpoint

- Produce one clearly identified direct-boot image for the exact q061 machine.
  Scan, connect using runtime-only credentials, obtain DHCP, ping the gateway
  and one public address, fetch a bounded nonempty HTTP object, disconnect, and
  bring the interface down.
- Never retain a real-machine or real-network credential, SSID, BSSID, MAC
  address, lease, hostname, account name, or host address in plans, build logs,
  screenshots, or fixtures. Clearly synthetic protocol vectors are permitted
  in automatic tests.

## Verification contract (`HW-T38`)

Automatic gates first cover:

- exact and neighboring PCI/subsystem/revision identities, expected CNVio2
  transport IDs, allocation failure, reverse unwind, DMA/ring wrap, interrupt
  claim, finite firmware command/event handling, malformed firmware/PNVM/NVM,
  and secret/staging erasure;
- reproduction of the frozen two firmware-file sizes/digests and rejection of
  WHENCE/runtime-version confusion, unapproved fallbacks, and floating updates;
- deterministic scan events and malformed/stale/duplicate frame rejection;
- the existing p027/p029 fake-authenticator WPA2/CCMP and L2 suites against an
  AX211-private fake transport;
- authorization ordering, hardware-key failure, TX/RX bounds, terminal down,
  and no traffic before controlled-port authorization;
- configured amd64 build, ordinary repository build, `git diff --check`, and
  IDE/xHCI USB-root regressions proving the new driver does not disturb the
  boot medium or RTL8822BU baseline.

After automatic gates, use only one bounded direct zedBSD boot on the exact
q061 machine. QEMU assignment of the isolated BDF is not an acceptance path:
the recorded IOMMU grouping does not reproduce the required platform CNVi/CRF
topology.

## Completion conditions

- Exact AX211/CNVio2 attach, pinned firmware/PNVM start, bounded scan,
  WPA2/CCMP controlled port, encrypted Ethernet, and checked down pass on
  production code.
- The automatic transport/firmware/scan/security/L2/build/regression gates
  pass without changing the public WLAN UAPI.
- One exact direct-boot run reaches DHCP, gateway/public ping, and a bounded
  nonempty HTTP fetch, then disconnects and goes administratively down.
- The AX211 implementation remains independent of RTL internal code. Any
  duplicated implementation is intentionally retained for p039 review.
- Exhaustive reconnect/rekey/recovery/long-run behavior is explicitly
  unclaimed and recorded for a later hardening Phase.

## Q062 progress

- Complete: default-off exact firmware/PNVM/license/WHENCE package and its
  immutable-cache fixture, plus automatic installation of the complete driver
  source notices whenever the kernel driver is selected.
- Complete: private API89/PNVM parser, exact real-blob inventory, Gen3
  context/descriptor and command/event codecs, ring/staging bounds, and
  ordinary/sanitizer/analyzer/ABI gates.
- Complete as production integration plus automatic evidence: exact PCI
  ownership and down-state publication, checked MSI-X/DMA open-generation
  lifetime, firmware boot/ALIVE/PNVM/NVM, the non-DQA API89 runtime command
  sequence, MCC-constrained passive scan, external-DMA scan command, BSS,
  association, key, TX-ring/TX, and RX paths all pass their focused gates.
- Complete: the production common station opens only after hardware success;
  the synthetic integration fixture traverses scan selection, WPA2/CCMP
  authorization, protected Ethernet TX/RX, disconnect, close, and detach
  without changing the public WLAN UAPI.
- Complete: operation leases, single-poll ownership, generation/sequence
  checks, secret erasure, and fatal recovery cover concurrent close/detach,
  stale completions, ambiguous command failure, and retained-DMA retry without
  use-after-free or premature resource release. The configured amd64 kernel
  links with the driver selected.
- Pending human evidence: one direct boot on the exact q061 AX211/CNVio2
  machine must still establish physical firmware/PNVM execution, RF scan,
  WPA2/CCMP association, DHCP, gateway/public ping, bounded nonempty fetch,
  disconnect, and administrative down. Until that single run succeeds, p038
  remains required and p038 makes no physical firmware/RF claim. Q062 closed
  this item as `uncleared` when the user deferred the physical test so that
  independent work could continue.

## Reconsideration boundary

Return to planning if the exact q061 tuple or CNVio2 companion relationship
does not match, direct boot is unavailable, the frozen firmware/PNVM fails its
digest or format contract, firmware/NVM behavior cannot be bounded, or the
existing public WLAN contract cannot express a required semantic. A public
UAPI change or a new firmware redistribution decision is a human decision; do
not hide it behind a broad Intel match, embedded firmware, plaintext network,
or premature commonization.
