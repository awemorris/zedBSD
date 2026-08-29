# Queue: CDC NCM deterministic hardening and one physical data-path check

Last updated: 2026-08-29

QID: `q029`

Queue status: in-progress

Queue finished: **No**

Authorization: the user authorized the deterministic CDC NCM fixes, safe
DHCP/diagnostic corrections, candidate-image preparation, and one physical
Latitude/RTL8156 acceptance check on 2026-08-29. CDC ECM is not authorized in
this Queue.

Timebox: no fixed wall-clock limit; complete the finite automatic gates, build
one candidate image, and request exactly one combined physical action.

Parent: [master plan](master.md)

Previous Queue: [q028](queue-q028.md)

## Purpose

Remove four deterministic defects or hazards before asking the user to repeat
physical testing: strict sequence continuity that can reject valid NCM traffic,
zero-delivery completion work that can evade the network poll budget, packet
filter programming before the active data alternate, and DHCP/diagnostic paths
that can hide the actual first failure. Then produce one candidate image and
use one bounded hardware check to decide whether the physical NCM path is
working or whether the independent ECM/QEMU control Phase should be next.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p020` | [Phase](ws004-hardware/phase020-cdc-ncm-deterministic-hardening/phase.md) | completed | Fully valid NTBs accept/resynchronize any sequence, malformed NTBs preserve state, completion work is budgeted, and packet-filter programming occurs on open after the active alternate |
| 2 | `ws005-p001` | [Phase](ws005-networking/phase001-usb-ncm-physical-datapath/phase.md) | in-progress | The safe automatic DHCP/diagnostic slice and candidate image are complete; one combined physical check must now either prove first carrier/static/DHCP traffic or record the exact boundary that sends the next Queue to p019 |

## Dependency order

```text
ws004-p018 physical bind and ue0 publication
                  |
                  v
ws004-p020 deterministic NCM hardening
                  |
                  v
ws005-p001 automatic DHCP/diagnostics + candidate image
                  |
                  v
one physical RTL8156 acceptance
          | pass                 | fail with retained evidence
          v                      v
  continue WS005          ws004-p019 ECM/QEMU next Queue
```

## Frozen product boundary

- The first fully valid NTH16/NDP16 NTB is accepted at any sequence. Every
  completely valid later mismatch is delivered and resynchronizes the expected
  value to the wire sequence plus one. Malformed input delivers nothing and
  changes no sequence state.
- Every consumed NCM completion is bounded by poll work accounting even when it
  produces no packet. This Queue does not redesign the xHCI IRQ path.
- Program the supported Ethernet packet filter during open, after the active
  data alternate and before notification/RX submission.
- Correct the deterministic static-to-DHCP transition so DISCOVER uses IPv4
  source zero while preserving transactional rollback of the prior static
  configuration. Make at least `ENETDOWN` and `ETIMEDOUT` diagnostics distinct
  rather than both `Unknown error`.
- The q029 diagnostic slice is bounded and stage-oriented for DHCP carrier,
  offer, ACK, configuration, route, resolver, and rollback failures. Broader
  NCM/xHCI open, TX, RX, validate, and delivery counters remain in p001 and are
  not claimed by this candidate image.
- Build exactly one candidate image after all automatic gates. Ask for one
  combined physical action using that image; do not request intermediate boots.
- CDC ECM, notification reassembly, xHCI IRQ redesign, asynchronous TX
  statistics, and vendor-specific Realtek initialization are outside q029.

## Optional QEMU USB passthrough

A libusb-backed `qemu-system-x86_64` USB-host passthrough cell may be used as
additional evidence only if `0bda:8156` is visible to the host and can be
claimed safely without disrupting required host networking. It must use a
disposable guest image and the same bounded stage markers. Passthrough
availability or success is not a completion blocker, does not replace the one
Latitude acceptance, and does not authorize a libusb/QEMU infrastructure
project. No host USB device was visible while q029's automatic gates ran, so
this optional cell was unavailable and was not treated as a blocker.

## Automatic execution evidence

`ws004-p020` is complete. The NCM wire fixture passed ordinary and ASan/UBSan
modes. The integrated production-source driver fixture passed ordinary,
ASan/UBSan, and analyzer modes with 1537 checks. The removable-network-device,
USB binding (971 checks), xHCI/USB function model (1404 checks), and shutdown
regressions passed. The default amd64 `make -j16` build and the focused changed-
object i386 compile also passed.

The automatic q029 slice of `ws005-p001` now waits for carrier within the same
total DHCP deadline, clears the IPv4 source before DISCOVER, restores the
interface and any pre-existing default route transactionally on failure, and
enforces the deadline even under a stream of irrelevant or invalid packets.
Offer, ACK, interface-configuration, route, and resolver failures retain their
stage; resolver replacement preserves the old file and original error across
write, sync, close, and rename failures; `ENETDOWN` and `ETIMEDOUT` have
distinct strings; and the real DHCP builder is covered for zero `ciaddr`. Its
ordinary, sanitizer, and analyzer fixtures passed.

The candidate is
[`build/amd64/hdd-image.img`](../build/amd64/hdd-image.img), 135266304 bytes,
SHA-256
`267315c0c002def1400c8f9fb2d97c1166b69af9ee557550cde4079b8033c719`.
A disposable copy booted as QEMU xHCI USB Storage to `login:` within the
30-second cell without a USB Storage error. These automatic results do not
claim physical RTL8156 carrier or packet transfer, nor do they claim the
broader kernel stage-counter/trace work retained by p001.

## Execution rules

- Preserve unrelated work, including the existing root `AGENTS.md` move; do
  not stage or rewrite it as part of q029.
- Do not inspect or modify `.internal/`, `userland/noct/NoctLang`, or
  `/home/awe/NoctLang`.
- Use `make -j16`, focused WS004/WS005 fixtures, and
  `qemu-system-x86_64`; do not use `make check`.
- Keep ordinary USB Storage, NCM bind/detach/reconnect, direct
  `/sbin/ifconfig`, `networkd`, and DHCP rollback behavior working.
- Use disposable image copies for runtime and passthrough tests. Retain the
  exact candidate-image path/hash and the bounded automatic evidence before
  requesting the single hardware action.
- Keep p001 and q029 in progress until the one physical acceptance has been
  returned. Automatic evidence alone does not satisfy the physical data-path
  completion conditions.

## Physical decision rule

The one acceptance action checks link/carrier, a fixed-address peer ARP/ping,
DHCP from a clean or transactionally cleared address state, and one post-lease
ping while retaining the available bounded stage/error output. If it passes,
synchronize the achieved WS005 milestone. If it fails, record the first failed stage and
relevant counters, mark the q029 item honestly `uncleared` if its completion
conditions are not met, and make `ws004-p019` CDC ECM/QEMU the next Queue item.
Do not implement ECM inside q029.

## Completion definition

q029 is finished when p020 and the authorized p001 slice are each `completed`
or honestly `uncleared`, all declared automatic gates and the candidate image
are recorded, exactly one combined physical acceptance has been processed,
and P/W/M/Q state identifies either the proven physical milestone or p019 as
the next controlled action. Optional USB passthrough is not required.
