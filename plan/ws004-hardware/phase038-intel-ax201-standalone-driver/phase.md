# WS004 Phase 038: standalone Intel Wi-Fi 6 AX201 normal path

Last updated: 2026-09-01

Phase ID: `ws004-p038`

Status: planned; follows the `ws004-p030` automatic milestone and completed
`ws004-p037`; not queued

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Implement the exact p037 Intel Wi-Fi 6 AX201 target as an independent native
zedBSD driver and establish one bounded useful normal path in this order:

```text
exact attach + pinned firmware
  -> 2.4-GHz/20-MHz scan
  -> WPA2-Personal/CCMP authorization
  -> DHCP
  -> gateway ping + public ping + bounded HTTP fetch
  -> disconnect + administrative down
```

The purpose is first communication, not exhaustive hardening. Essential input
bounds, finite waits, checked DMA/interrupt ownership, secret redaction, and
fail-closed authorization are required from the start. Exhaustive recovery,
rekey, suspend, hotplug, race, fault-injection, throughput, and repeatability
work remains a later Phase extracted after this normal path works.

## Dependencies

- `ws004-p030`: its automatic RTL8822BU lifecycle implementation is complete
  before the second hardware implementation begins; p008's shared physical
  closure is not a prerequisite here.
- `ws004-p037`: exact AX201 identity, topology, selected firmware bytes,
  immutable acquisition source, license, and package boundary are complete.
- `ws004-p003`, `p012`, and the existing PCI/MSI, DMA, net-device, shutdown,
  and checked lifetime contracts.
- `ws004-p027` and `p029`: the existing generic WLAN UAPI/common station,
  WPA2-Personal/CCMP engine, controlled port, and Ethernet conversion contract.
- A test-machine execution path selected from p037 evidence: safely isolated
  QEMU assignment when the complete required function/topology can be assigned
  without losing host control, otherwise one bounded direct zedBSD boot.

P006/p007 WS005 orchestration is not a dependency. The development checkpoint
may use the already working primitive commands and `dhcpc`, keeping this Phase
focused on the Intel hardware path.

## No-premature-commonization rule

Implement AX201 behind the existing stable WLAN and `net_device` contracts in
Intel-owned source. It may duplicate concepts or small routines found in the
RTL8822BU implementation. Do not first introduce a shared Intel/Realtek
firmware loader, command transport, DMA ring, descriptor codec, interrupt
engine, calibration layer, register API, or chip-family framework.

Reuse only facilities which already have a device-independent public contract,
such as PCI/DMA/interrupt primitives, the p027 WLAN operations, p029 WPA2/CCMP
state, and the net-device boundary. If an internal adapter can express the
Intel behavior through those interfaces, prefer the adapter over a public UAPI
change. If correct operation truly requires a new public semantic, stop and
mark p038 `uncleared` with the exact missing operation and affected consumers;
do not casually extend the UAPI or place Intel-specific fields in it.

The Intel driver must remain replaceable as one implementation. Its private
module boundaries, state machine, firmware commands, and data structures may
differ completely from Realtek.

## Initial capability boundary

- Exact p037 AX201-family identity only; no broad Intel vendor/family match.
- Infrastructure station mode, 2.4-GHz channels permitted by the frozen world/
  board policy, 20-MHz width, and the legacy/HT subset actually required for
  the controlled normal path.
- WPA2-Personal PSK with CCMP-128 through the existing common security engine.
- One ordinary Ethernet MTU and one bounded TX/RX queue profile sufficient for
  DHCP, ICMP, ARP, DNS, and the HTTP oracle.
- No 5 GHz, 40/80/160 MHz, 802.11ax/HE performance claim, WPA3/SAE, 802.1X,
  roaming, AP/monitor mode, aggregation tuning, power-save optimization,
  Bluetooth coexistence optimization, suspend/resume, or throughput target.

Unsupported capabilities remain explicit. The marketed AX201 feature set is
not an initial completion contract.

## Ordered implementation

### 1. Exact attach and firmware package

- Match only p037's exact function and subsystem/revision policy.
- Acquire PCI BAR, DMA, interrupt, companion/platform, and net-device ownership
  transactionally with exact reverse unwind.
- Implement the default-off `userland/firmware/intelax201/` entry frozen by
  p037. Verify bytes before upload and keep all firmware/license/package
  metadata outside the base-license claim.
- Parse and upload only the pinned firmware format with checked arithmetic,
  bounded commands, explicit ready/error states, and complete staging scrub.

### 2. Intel-private transport and radio start

- Keep firmware command/event rings, TX/RX descriptors, DMA ownership,
  interrupts, NVM/calibration, channel programming, and device reset inside
  the Intel implementation.
- Use finite controller/firmware deadlines and reject unknown firmware/NVM
  layouts. Never guess board calibration or report carrier after partial
  initialization.
- Publish one stable `wlanN` only after exact identity and basic object
  lifetime are valid; keep it administratively down until open succeeds.

### 3. Scan normal path

- Map the Intel receive/event format into the existing bounded WLAN scan
  callbacks without changing their public representation.
- Complete one finite 2.4-GHz scan, publish truthful BSS/security/channel/RSSI
  fields, and stop cleanly. A scan result never implies authorization.

### 4. WPA2/CCMP and Ethernet normal path

- Supply the existing common authentication, association, EAPOL, CAM/key-slot
  equivalent, encrypted TX/RX, and completion callbacks from Intel-private
  operations.
- Preserve p029 replay, MIC, key-install, controlled-port, and secret-lifetime
  rules. Carrier rises only after the common engine authorizes the link.
- Convert one bounded authorized Ethernet stream through the existing common
  L2 boundary; no Intel-specific packet format escapes the driver.

### 5. Useful IP checkpoint

- On one exact candidate, scan, connect using runtime-only credentials, obtain
  DHCP, ping the gateway and one public address, fetch a bounded nonempty HTTP
  object, disconnect, and bring the interface down.
- Never retain a credential, SSID, BSSID, MAC address, lease, hostname, or
  account name in plans, build logs, screenshots, or committed fixtures.

## Verification contract (`HW-T38`)

Automatic gates first cover:

- exact/neighboring identity, allocation failure, reverse unwind, DMA/ring
  wrap, interrupt claim, finite firmware command/event handling, malformed
  firmware/NVM, and secret/staging erasure;
- deterministic scan events and malformed/stale/duplicate frame rejection;
- the existing p027/p029 fake-authenticator WPA2/CCMP and L2 suites against an
  Intel-private fake transport;
- authorization ordering, key-program failure, TX/RX bounds, terminal down,
  and no traffic before controlled-port authorization;
- configured amd64 build, ordinary repository build, `git diff --check`, and
  IDE/xHCI USB-root regressions proving the new driver does not disturb the
  boot medium or RTL8822BU baseline.

After automatic gates, use p037's safe execution method for one exact-candidate
normal-path checkpoint. PCI assignment is allowed only if the complete needed
topology is isolated and the host management path does not use it. Otherwise
produce one clearly identified direct-boot image and request only that bounded
physical observation. Do not experiment with partial IOMMU groups or lose the
host's control network merely to avoid a checkpoint.

## Completion conditions

- Exact AX201 attach, pinned firmware start, bounded scan, WPA2/CCMP controlled
  port, encrypted Ethernet, and checked down all pass on production code.
- The automatic transport/firmware/scan/security/L2/build/regression gates
  pass without changing the public WLAN UAPI.
- One exact physical or faithful complete-function passthrough run reaches
  DHCP, gateway/public ping, and a bounded nonempty HTTP fetch, then disconnects
  and goes administratively down.
- The Intel implementation remains independent of RTL internal code. Any
  duplicated implementation is intentionally retained for p039 review.
- Exhaustive reconnect/rekey/recovery/hotplug/long-run behavior is explicitly
  unclaimed and recorded for a later hardening Phase.

## Reconsideration boundary

Return to planning if the exact device is not independently assignable and no
direct-boot checkpoint is available, the pinned firmware/license boundary is
uncleared, a required platform companion is absent, firmware/NVM behavior
cannot be bounded, or the existing public WLAN contract cannot express a
required semantic. A public UAPI change or firmware redistribution judgment is
a human decision; do not hide it behind a broad common framework, relaxed
identity match, embedded firmware, plaintext network, or premature fallback to
another Intel device.
