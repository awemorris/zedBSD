# WS004 Phase 019: independent CDC ECM and QEMU network baseline

Last updated: 2026-08-29

Phase ID: `ws004-p019`

Status: planned; ready for Queue

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md) and
[WS005 test index](../../ws005-networking/tests/README.md)

## Objective

Implement a self-contained USB CDC ECM Ethernet driver and use QEMU's standard
`usb-net` function to prove the complete common path from xHCI and USB binding
through `net_device`, ARP/IPv4/UDP, `networkd`, and `dhcpc`. This creates an
automatic reference path before more physical RTL8156 NCM debugging is asked
of the user.

This Phase does not replace CDC NCM and does not extract a speculative common
USB-Ethernet backend. ECM and NCM keep separate implementations behind the
already stable USB and `net_device` interfaces. Common code is considered only
after both implementations have demonstrated substantial stable duplication.

## Why ECM is the control path

The installed QEMU 10.0.11 exposes `-device usb-net`. QEMU's production device
model presents RNDIS as configuration 2 and standards-based CDC Ethernet as
configuration 1. The CDC configuration contains a `02/06/00` control
interface, Header/Union/Ethernet functional descriptors, an interrupt-IN
notification endpoint, and a CDC data interface whose alternate 1 has bulk-IN
and bulk-OUT endpoints. It transports ordinary Ethernet frames rather than NCM
NTBs.

The generic driver-aware configuration selector must therefore choose the ECM
configuration because an ECM driver gives it a positive class match; no QEMU
VID:PID or configuration-number quirk is permitted. The authoritative QEMU
model is `hw/usb/dev-network.c`:

<https://gitlab.com/qemu-project/qemu/-/blob/master/hw/usb/dev-network.c>

## Dependencies

- `ws004-p010`: retained multi-configuration/function model.
- `ws004-p011`: concurrent xHCI interrupt/bulk URBs.
- `ws004-p012`: removable `net_device` and deferred-poll lifetime.
- `ws004-p015`: checked interface claims, alternate transitions, endpoint
  admission, and callback drain.
- `ws004-p009`: checked UHCI/EHCI ownership baseline for any existing
  zero-packet transfer-contract work outside xHCI.
- `ws002-p020`: `net` -> `networkd` -> `dhcpc` control-plane baseline.

The physical NCM Phase `ws005-p001` depends on this Phase's automatic result;
this Phase does not depend on physical RTL8156 access.

## Frozen driver boundary

- Match only a CDC Communication interface with class/subclass/protocol
  `02/06/00` and an explicitly Union-associated CDC Data interface with
  `0a/00/00` on the selected data alternate.
- Require and validate the CDC Header, Union, and Ethernet functional
  descriptors, the 12-hex-digit MAC string, interrupt-IN notification
  endpoint, and one data alternate with exactly the required bulk directions.
  An IAD is optional corroboration, not an association prerequisite; QEMU's
  ECM configuration has a valid Union and no IAD.
- Claim the associated data interface transactionally. Alternate 0 remains the
  inactive state; select the discovered bulk alternate for operation.
- Program `SET_ETHERNET_PACKET_FILTER` after the data alternate is selected and
  whenever reopening the function requires the filter to be restored.
- Send and receive one raw Ethernet frame per bulk transfer. Do not reuse NCM
  NTB framing, sequence state, or negotiation logic.
- Interpret `NETWORK_CONNECTION` and speed notifications with bounded state.
  Accept a notification index naming the verified control interface or its
  Union-associated data interface, debounce repeated identical carrier state,
  and keep notification rearm bounded. Completion callbacks only publish work
  and the network worker handles frame delivery.
- Set `DRV_USB_URB_ZERO_PACKET` for a positive bulk-OUT frame whose length is
  an exact endpoint-packet multiple. Make every enabled HCD used by ECM honor
  that existing flag, with xHCI as the runtime acceptance target; do not hide
  an unsupported flag behind an ECM-local assumption.
- Use the existing `ueN`, packet ownership, carrier, detach, callback-drain,
  and terminal-shutdown contracts. No ECM-only USB-core lifecycle API is
  added.

## Planned implementation

1. Add an independent `usb-cdc-ecm` driver, public registration declaration,
   menu option, default configuration, amd64/i386 build integration, and
   platform registration before host-controller probing.
2. Add a production-source fake-HCD fixture covering descriptor match and
   rejection, QEMU-shaped RNDIS-first/ECM-second configuration selection,
   Union-only association, optional matching/contradictory IAD, data protocol
   zero, alternate selection, filter ordering, MAC/segment-size parsing,
   data-interface-indexed and repeated notification records, raw-frame RX/TX,
   an exact-64-byte TX requiring a terminating zero packet, error unwind,
   close, detach, reconnect, and concurrent USB Storage ownership.
3. Preserve fixed persistent notification/RX/TX URBs and the p011/p012/p015
   ownership barriers. Keep error and retry work bounded; never parse frames or
   emit unbounded logs in hard-interrupt context.
4. Add a Phase-owned QEMU harness rather than extending the GUI-oriented
   `make run` target. Build the production kernel and real network userland
   into a disposable test image with one test-only oneshot service ordered
   after `networkd`. The service executes the real `/sbin/net`, `/sbin/dhcpc`
   path, `/sbin/ifconfig`, and `/bin/ping`, checks exit status, interface flags,
   addresses, and packet counters, and emits one fixed terminal PASS/FAIL
   record to the port `0xe9` console. Host orchestration is a Noct script; the
   QEMU monitor is used only for bounded termination, not keyboard injection.
   Retain the complete QEMU command/version, input hashes, debug console, QEMU
   diagnostics, result record, and a QEMU `filter-dump` pcap. Do not use
   `.internal/` material, Python, or `make check`.
5. First boot the system disk through IDE so only the USB network path is under
   test. Require driver-aware selection of ECM configuration 1 and `ue0`
   publication. Because connection notification is asynchronous, wait for
   `ifconfig ue0` to report `RUNNING` before starting address configuration.
   Use separate fresh-image cells for fixed-address ARP/ICMP and for a bounded
   DHCP lease through `/sbin/net` and `dhcpc`, followed by one ping to the QEMU
   user-network peer. Do not let the known static-to-DHCP address-retention bug
   contaminate the ECM transport result.
6. Use four fresh bounded cells: IDE/static, IDE/DHCP, xHCI USB-storage/static,
   and xHCI USB-storage/DHCP. The isolated IDE cells attach only the network
   function to xHCI. The integrated cells use the proven UEFI USB-storage
   topology and put storage on xHCI port 1 and ECM on port 2, so both functions
   share one controller without starving, corrupting, or stealing completions.
   Use a deterministic restricted QEMU user network (`10.0.2.0/24`, gateway
   `10.0.2.2`, DHCP start `10.0.2.15`, DNS `10.0.2.3`) and a fixed guest MAC.
7. If the QEMU path exposes a localized defect in the common USB, xHCI,
   `net_device`, ARP/IP/UDP, `networkd`, or `dhcpc` path, correct it in its
   owning module and add a focused regression before continuing. Do not alter
   NCM wire behavior merely to make ECM pass.

## Verification gates

- Focused ECM production-source fixture passes ordinary, ASan/UBSan, and
  analyzer modes.
- Existing USB function/binding, concurrent xHCI, removable `net_device`, CDC
  NCM, USB Storage, and shutdown regressions remain passing.
- Focused HCD transfer-model evidence proves that
  `DRV_USB_URB_ZERO_PACKET` emits a terminating zero-length bulk-OUT packet
  exactly when requested for a positive endpoint-packet multiple.
- Default amd64 and configured i386 PC/AT `make -j16` builds pass with ECM
  enabled.
- `HW-T22` and `NET-T42` pass in the IDE control topology and the combined
  xHCI USB-storage plus USB-network topology.
- `git diff --check` passes and the production image/config inputs are proven
  unchanged by the disposable-image harness.

## Completion conditions

- QEMU's RNDIS-first/ECM-second device binds only the standards-based ECM
  configuration without VID:PID or fixed-configuration assumptions.
- The ECM driver publishes `ue0`, carrier transitions are truthful, raw
  Ethernet TX/RX completes, and detach/reconnect has no stale interface, URB,
  callback, packet, or endpoint ownership.
- Static ARP plus a guest-initiated ping with an echo reply, and
  `net dhcp ue0` plus a post-lease ping, succeed through the production kernel
  and userland stack.
- Concurrent xHCI USB Storage and ECM traffic remain operational.
- The resulting automatic evidence gives `ws005-p001` a passing common-path
  control. It does not claim that NCM NTB framing or the physical RTL8156 data
  path works.

## Reconsideration boundary

Mark the Phase `uncleared` and extract a new owner Phase if QEMU's current
device no longer exposes the documented CDC ECM configuration, a fix requires
changing a public USB/network UAPI, or the failure proves to be an unrelated
network-stack redesign rather than a bounded defect. Do not add RNDIS support,
QEMU-specific matching, a shared ECM/NCM backend, or physical-hardware work to
force this Phase complete.
