# Queue: RTL8156 CDC NCM association and physical-enumeration follow-up

Last updated: 2026-08-29

QID: `q028`

Queue status: finished

Queue finished: **Yes**

Authorization: the user approved implementation on 2026-08-29 after the first
Latitude RTL8156 insertion enumerated `0bda:8156` but did not publish a network
device.

Timebox: no fixed wall-clock limit; complete the finite automatic gates and
prepare one final real-hardware `ue0` acceptance action.

Parent: [master plan](master.md)

Previous Queue: [q027](queue-q027.md)

## Purpose

Correct the standards-based CDC NCM association rule exposed by the real
RTL8156 descriptor layout. A CDC Union descriptor is the authoritative
control/data association; an Interface Association Descriptor is optional
corroboration rather than a prerequisite. Preserve driver-aware
multi-configuration selection so an NCM configuration outranks unsupported
vendor and ECM configurations without a VID:PID configuration-number quirk.

Add concise USB binding diagnostics so a successful USB enumeration that does
not create a class device identifies the selected configuration, relevant
interface tuple, and probe outcome instead of stopping at device class `00`.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p018` | [Phase](ws004-hardware/phase018-rtl8156-ncm-association/phase.md) | completed | Union-associated, IAD-less NCM wins a three-configuration RTL8156-shaped fixture, binding diagnostics are actionable, automatic/build/QEMU gates pass, and Latitude publishes `ue0` |

## Dependency order

```text
ws004-p010 USB configuration/function model
          +
ws004-p014 CDC NCM software driver
          |
          v
ws004-p018 RTL8156 association and diagnostics
```

## Frozen product boundary

- Keep the generic CDC NCM class match. Do not hardcode RTL8156 configuration
  numbers or relabel its vendor-specific configuration as NCM.
- Require exactly one valid CDC Union association between the NCM control
  interface and CDC data interface.
- Do not require an IAD. If a relevant IAD is present, require it to corroborate
  the same two-interface NCM function; contradictory or ambiguous metadata is
  rejected.
- Preserve strict Header, Ethernet, NCM functional-descriptor, alternate,
  endpoint, MAC-string, HCD capability, and lifecycle validation from p014.
- Diagnostics are concise and bounded. They expose selection/binding facts and
  errors; they do not dump arbitrary descriptor payloads or add a debug-only
  success path.
- This Queue proves enumeration and `ue0` publication. DHCP, IP transfer,
  sustained traffic, link recovery, and repeated physical acceptance remain
  WS005 networking evidence.

## Execution rules

- Preserve unrelated work, including the existing root `AGENTS.md` move; do
  not stage or rewrite it as part of q028.
- Do not inspect or modify `.internal/`, `userland/noct/NoctLang`, or
  `/home/awe/NoctLang`.
- Use `make -j16`, focused WS004 fixtures, and `qemu-system-x86_64`; do not use
  `make check`.
- Keep ordinary USB Storage selection, boot, disconnect, and reclaim behavior
  working while changing function matching and diagnostics.
- Complete automatic work before requesting one real-machine action. State
  that action's purpose and exact image path; do not request repeated boots at
  intermediate steps.
- Do not commit from the documentation-only planning subtask. The executing
  root task owns the final Phase commit and push under the user's standing
  authorization.

## Execution result

The generic association fix and binding diagnostics are implemented. Focused
ordinary, sanitizer, analyzer, USB binding, NCM wire, xHCI, net hotplug, and
USB Storage gates pass. Both amd64 and configured i386/PC/AT builds pass, and a
disposable amd64 q35/qemu-xhci USB-root boot reaches `login:` without a tracked
failure marker. The final Latitude insertion selected configuration 2, bound
the NCM control/data function, and published `ue0`. q028 is complete.

The same acceptance exposed the next, separate WS005 boundary: DHCP reports an
error and static addressing does not yet yield packet transfer, with a possible
first-inbound-activity stop. This is carried by
[`ws005-p001`](ws005-networking/phase001-usb-ncm-physical-datapath/phase.md);
q028 does not claim carrier, DHCP, or data transfer. Before that physical Phase
is queued, [`ws004-p019`](ws004-hardware/phase019-cdc-ecm-qemu-baseline/phase.md)
provides an independent CDC ECM implementation and QEMU common-path control.

## Completion definition

q028 is finished when `ws004-p018` is either `completed` or honestly
`uncleared`, its automatic evidence is synchronized to P/W/M/Q, and the final
single-boot Latitude acceptance either observes `ue0` or records the exact new
descriptor/probe boundary needed for a successor Phase.
