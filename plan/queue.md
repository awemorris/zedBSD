# Queue: Intel AX211 standalone normal path

Last updated: 2026-09-02

QID: `q062`

Queue status: in progress

Queue finished: **No**

Authorization: the user authorized continuous Queue execution and accepted the
q061 exact Intel AX211/CNVio2 target. Q060 completed p030's automatic
RTL8822BU dependency, and q061 completed p037's identity, firmware, provenance,
license, package, and direct-boot boundaries.

Timebox: none. Execute only finite `ws004-p038`. Do not broaden the exact
device identity, change the public WLAN UAPI without an explicit decision,
create an Intel/Realtek hardware framework, claim generic QEMU passthrough, or
begin p039 in this Queue.

Parent: [master plan](master.md)

Previous Queue: [q061](queue-q061.md)

## Purpose

Implement the exact q061 Intel Wi-Fi 6E AX211/CNVio2 function independently
behind the existing WLAN/net-device contracts. Prove exact attach, pinned
firmware/PNVM start, bounded 2.4-GHz scan, WPA2-Personal/CCMP authorization,
DHCP, gateway/public ping, bounded nonempty HTTP fetch, disconnect, and
administrative down, ending with one bounded direct zedBSD boot on the exact
machine.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p038` | [standalone Intel AX211 normal path](ws004-hardware/phase038-intel-ax211-standalone-driver/phase.md) | in-progress (`q062`) | Exact AX211/CNVio2 attach and pinned firmware/PNVM pass automatic gates and one direct exact-device scan/WPA2/CCMP/DHCP/ping/fetch/disconnect/down checkpoint without prior Intel/RTL commonization |

## Accepted decisions

- Bind only PCI `8086:51f0`, subsystem `8086:4090`, revision `01`, with the
  q061-recorded AX211/CNVio2 transport relationship. Reject AX201, AX210,
  neighboring AX211 identities, and broad Intel matches.
- Use only `intel/iwlwifi/iwlwifi-so-a0-gf-a0-89.ucode` and
  `intel/iwlwifi/iwlwifi-so-a0-gf-a0.pnvm` at the exact q061 sizes and SHA-256
  values from official `linux-firmware` tag `20260410`. Preserve the WHENCE
  `86.735b75a4.0` versus runtime `89.735b75a4.0` discrepancy visibly.
- Implement a default-off `userland/firmware/intelax211/` package. Firmware
  bytes and the complete Intel notice remain outside the base source/default
  image; ordinary builds and the kernel perform no firmware network fetch.
- Keep firmware transport, CNVi/CRF interaction, DMA rings, interrupts,
  descriptors, NVM/calibration, reset, and hardware keys AX211-private. Reuse
  only existing device-independent PCI/DMA/interrupt, WLAN, WPA2/CCMP, and
  net-device contracts.
- Keep the first capability profile to station-mode 2.4 GHz, 20 MHz,
  WPA2-Personal/CCMP and one useful IP path. Do not claim 5/6 GHz, wide
  channels, HE performance, WPA3, roaming, AP/monitor, suspend, or throughput.
- Use runtime-only credentials. Retain no real-machine or real-network
  credential, SSID, BSSID, MAC address, lease, hostname, account name, or host
  address in plans, fixtures, logs, or screenshots. Clearly synthetic protocol
  vectors remain permitted in automatic fixtures.
- The singleton IOMMU group does not reproduce CNVio2 platform topology in a
  generic guest. The exact-device acceptance method is direct zedBSD boot, not
  PCI passthrough.

## Implementation checkpoints

1. Add the selected-only `intelax211` firmware package with frozen file sizes,
   digests, official snapshot, license/manifest installation, offline reuse,
   corruption rejection, and absence from the default image.
2. Implement exact PCI/subsystem/revision and CNVio2 transport validation,
   transactional BAR0/MSI-X/DMA/net-device ownership, checked reverse unwind,
   bounded firmware/PNVM/NVM parsing and upload, finite command/event handling,
   and staging/secret erasure.
3. Keep the AX211 transport private while adapting its finite scan and
   authentication/association/key/TX/RX callbacks to the unchanged p027/p029
   WLAN, WPA2/CCMP, controlled-port, and Ethernet contracts.
4. Pass exact/neighboring identity, allocation/unwind, DMA/ring wrap,
   interrupt, malformed firmware/PNVM/NVM, timeout, stale/duplicate frame,
   authorization-order, hardware-key failure, TX/RX bound, and terminal-down
   automatic fixtures, including sanitizer and compiler-analyzer variants.
5. Pass the retained p027/p029 WLAN/security/L2 suites through an AX211-private
   fake transport, configured amd64, ordinary repository build,
   `git diff --check`, and IDE/xHCI USB-root plus RTL8822BU regressions.
6. Only after automatic gates pass, produce one direct-boot candidate and run
   the exact-device checkpoint: attach, pinned firmware/PNVM, bounded scan,
   WPA2/CCMP authorization, DHCP, gateway/public ping, bounded nonempty fetch,
   disconnect, and administrative down with all network identity redacted.

## Completion definition

Q062 completes when p038 passes the automatic package/identity/ownership/
firmware/transport/scan/security/L2/build/regression gates and one exact AX211/
CNVio2 direct-boot normal path reaches useful IP communication, then
disconnects and goes down cleanly. The public WLAN UAPI remains unchanged, the
AX211 implementation remains independent of RTL internals, and exhaustive
recovery/rekey/suspend/race/throughput/repeatability claims remain outside this
Queue.

## Execution result

In progress. The automatic implementation and focused gates are complete:
the default-off firmware package, the automatically selected source-notice
package, strict exact-device
attach, checked PCI/MSI-X/DMA lifetime, API89 firmware/PNVM/NVM boot, runtime
radio command sequence, passive scan, association/key/TX/RX paths, common
WLAN/WPA2/CCMP/L2 adaptation, fatal recovery, and detach/down behavior pass
their declared ordinary, sanitizer, analyzer, amd64, and i386 fixtures. The
configured amd64 kernel links with AX211 selected, and the production common
integration fixture reaches an authorized protected Ethernet exchange using
only synthetic protocol data. The public WLAN UAPI remains unchanged and no
Intel/Realtek hardware layer was introduced.

The exact-device checkpoint has not run. Q062 and p038 therefore remain
`in-progress`: one direct zedBSD boot on the exact AX211/CNVio2 machine must
still establish physical firmware/PNVM execution, RF scan and WPA2/CCMP
association, DHCP, gateway/public ping, a bounded nonempty fetch, disconnect,
and administrative down. Automatic fixtures do not claim those physical
firmware, RF, or useful-network results.
