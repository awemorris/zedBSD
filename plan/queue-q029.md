# Queue: CDC NCM deterministic hardening and one physical data-path check

Archive status: closed

Last updated: 2026-08-29

QID: `q029`

Queue status: finished

Queue finished: **Yes**

Authorization: the user authorized the deterministic CDC NCM fixes, safe
DHCP/diagnostic corrections, candidate-image preparation, the first physical
Latitude/RTL8156 check, and evidence-led remote QEMU/libusb debugging with the
same physical adapter on 2026-08-29. CDC ECM and the separately planned native
xHCI periodic-context repair are not authorized in this Queue.

Timebox: no fixed wall-clock limit; finish the finite repair and automatic
gates, then request one final combined Latitude action with the superseding
candidate rather than intermediate boots.

Parent: [master plan](master.md)

Previous Queue: [q028](queue-q028.md)

## Purpose

Remove four deterministic defects or hazards before asking the user to repeat
physical testing: strict sequence continuity that can reject valid NCM traffic,
zero-delivery completion work that can evade the network poll budget, packet
filter programming before the active data alternate, and DHCP/diagnostic paths
that can hide the actual first failure. The first candidate exposed a carrier
timeout; the user-authorized remote discriminator then found one bounded
notification filter defect, produced a superseding candidate, and proved the
physical NCM path through QEMU before the final Latitude-native check.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p020` | [Phase](ws004-hardware/phase020-cdc-ncm-deterministic-hardening/phase.md) | completed | Fully valid NTBs accept/resynchronize any sequence, malformed NTBs preserve state, completion work is budgeted, and packet-filter programming occurs on open after the active alternate |
| 2 | `ws005-p001` | [Phase](ws005-networking/phase001-usb-ncm-physical-datapath/phase.md) | completed | The final Latitude-native image reaches carrier and DHCP and successfully fetches `www.google.com`; automatic, passthrough, static/DHCP, DNS, routing, and external-transfer evidence all pass |

## Dependency order

```text
ws004-p018 physical bind and ue0 publication
                  |
                  v
ws004-p020 deterministic NCM hardening
                  |
                  v
ws005-p001 automatic DHCP/diagnostics + first candidate
                  |
                  v
first Latitude result: carrier timeout
                  |
                  v
real RTL8156 USB capture -> paired-interface notification repair
                  |
                  v
QEMU xHCI + physical RTL8156 carrier/DHCP/ping pass
                  |
                  v
final Latitude-native acceptance: pass
                  |
                  v
q029 and ws005-p001 completed
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
- Accept a CDC connection/speed notification only when its `wIndex` names one
  of the exact control/data interfaces already validated as the bound NCM
  function. This covers the RTL8156 data-interface notification without a
  VID:PID special case or acceptance of unrelated interfaces.
- Correct the deterministic static-to-DHCP transition so DISCOVER uses IPv4
  source zero while preserving transactional rollback of the prior static
  configuration. Make at least `ENETDOWN` and `ETIMEDOUT` diagnostics distinct
  rather than both `Unknown error`.
- The q029 diagnostic slice is bounded and stage-oriented for DHCP carrier,
  offer, ACK, configuration, route, resolver, and rollback failures. Broader
  NCM/xHCI open, TX, RX, validate, and delivery counters remain in p001 and are
  not claimed by this candidate image.
- Retain the original candidate and the single evidence-led superseding image
  with exact hashes. Ask for one final combined Latitude action using the
  superseding image; do not request intermediate boots.
- CDC ECM, notification reassembly, xHCI IRQ redesign, asynchronous TX
  statistics, and vendor-specific Realtek initialization are outside q029.

## QEMU USB passthrough discriminator

A libusb-backed `qemu-system-x86_64` USB-host cell became available on the
user-provided remote host. The host's SSH route used a different PCI Ethernet
device, the RTL8156 was safely claimable, and every run used a disposable IDE
guest image. The pre-repair guest reproduced offline carrier. Its USB capture
then proved successful endpoint `0x83` completions carrying valid connection-up
and 2.5-Gbit/s notifications with data-interface `wIndex=1`. The repaired guest
reached carrier, DHCP, and bidirectional traffic through the same adapter.

This discriminator proves the physical NCM device and zedBSD protocol path but
does not replace the Latitude-native xHCI acceptance or authorize a general
libusb/QEMU infrastructure project.

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

The original automatic candidate was
[`build/amd64/hdd-image.img`](../build/amd64/hdd-image.img), 135266304 bytes,
SHA-256
`267315c0c002def1400c8f9fb2d97c1166b69af9ee557550cde4079b8033c719`.
A disposable copy booted as QEMU xHCI USB Storage to `login:` within the
30-second cell without a USB Storage error.

After the first Latitude check timed out at carrier, the real-device capture
identified the exact notification filter defect. The repaired production
driver fixture passed ordinary and ASan/UBSan modes at 1540 checks plus its
analyzer. NCM wire, USB binding at 971 checks, concurrent xHCI/USB function at
1404 checks, and removable-network-device regressions pass. `make -j16` and a
fresh disposable USB-root login pass. The first notification-repair candidate
remained the same path and size with SHA-256
`982201f8f5f2b00632c3c3e7d9437504fe6dcc110b49419ca455e0b7df0cb7bf`.

Clean DHCP passed with that image. A subsequent static-to-DHCP run proved via
USB PCAP that no DHCP client frame left the guest while the old default route
remained; rollback restored the exact static configuration. Moving the
old-route snapshot/removal before DHCP send is now covered by ordinary,
ASan/UBSan, and analyzer fixtures. The final candidate remains the same path
and size with SHA-256
`34341960f871335f9ff40177664d1d0da017ce1cd3497aff0ad45658adb06e46`.

With the final image and the physical RTL8156 passed through QEMU xHCI, `ue0`
became online, static peer traffic passed, USB PCAP retained the zero-source
DHCP broadcasts and server responses, the dynamic default route was installed,
and two post-lease pings each to the gateway and remote host completed without
loss.

The final Latitude-native acceptance then passed on 2026-08-29. The user
reported that the image worked perfectly and that `fetch www.google.com`
completed successfully. This closes the remaining native xHCI, carrier, DHCP,
DNS, default-route, and external application-transfer checkpoint for q029. The
separately planned `ws004-p021` remains a general
xHCI specification correction rather than a prerequisite or failure response
for this RTL8156 milestone.

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
- The returned final physical acceptance satisfies the p001/q029 hardware
  boundary. Later reconnect, repeated-boot, and long-transfer reliability
  remain their existing follow-up work rather than reopening this Queue.

## Physical decision rule

The final Latitude action passed. Native carrier and DHCP remained operational
through an external hostname fetch, exceeding the minimum peer-ping decision
rule. No failure branch was selected; neither p021 nor ECM was implemented
inside q029.

## Completion definition

q029 is finished when p020 and the authorized p001 slice are each `completed`
or honestly `uncleared`, all declared automatic/remote gates and the current
candidate image are recorded, the final combined Latitude acceptance has been
processed, and P/W/M/Q state identifies either the proven physical milestone
or the exact next controlled action.

Result: **satisfied**. Both Queue items are completed and the physical
milestone is proven.
