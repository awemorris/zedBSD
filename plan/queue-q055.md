# Queue: first WLAN common foundation

Last updated: 2026-09-01

QID: `q055`

Queue status: completed

Queue finished: **Yes**

Authorization: the user supplied the remaining exact-device identity facts,
approved `userland/packages/wifi-firmware/` as an optional GitHub-fetched
firmware package, and explicitly directed WLAN to take priority and this Queue
to start.

Timebox: none. Close the finite p026 decision record, then implement the one
hardware-independent p027 WLAN UAPI/common-core Phase and its automatic
regressions. No physical radio action is required.

Parent: [master plan](master.md)

Previous Queue: [q054](queue-q054.md)

## Purpose

Freeze the exact first Archer identity and separate firmware-package boundary,
then add one versioned, pointer-free, device-independent WLAN ioctl and station
state layer. Prove its cache, state, generation, credential-erasure, and
hot-unplug behavior with a deterministic fake radio before a Realtek driver is
allowed to consume it.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p026` | [Phase](ws004-hardware/phase026-archer-t3u-nano-identity-firmware/phase.md) | completed | Japan label, absent revision marking, exact retained descriptor authority, and optional hash-verified GitHub firmware-package boundary are synchronized |
| 2 | `ws004-p027` | [Phase](ws004-hardware/phase027-wlan-uapi-common-core/phase.md) | completed | Versioned WLAN ioctl ABI, active-ioctl lifetime gate, persistent common station state, bounded cache/generations, and deterministic fake radio pass every automatic gate |

## Accepted decisions

- The physical target is the exact retained `2357:012e`, `bcdDevice=2.10`,
  `ff/ff/ff`, five-endpoint descriptor. The label says only `Archer T3U Nano`,
  the region is Japan, and no hardware revision is printed; no V1.0 claim or
  cross-unit alias is inferred.
- Kernel and base system contain no Realtek blob. A separately selected
  `userland/packages/wifi-firmware/` recipe may fetch only the immutable
  `endlessm/linux-firmware` GitHub revision frozen by p026, must verify the
  frozen firmware and license hashes, and installs the file separately.
- P027 is bus/chip independent. It contains no RTL8822BU registers, firmware
  loader, WPA implementation, user command, DHCP, or physical-radio claim.

## Boundaries

- Preserve every wired-network interface and ioctl behavior.
- Use the AF_INET socket ioctl route, exact encoded sizes, fixed-width pointer-
  free records, central privilege classification, and kernel-local buffers.
- Extend the generic `net_device` teardown barrier so close/gone/shutdown join
  admitted ioctls before driver data can be released.
- Keep all time/cache/state bounds and fake-radio ownership defined by p027.
- Do not implement p028 hardware/firmware loading or any WS005 command in q055.
- Do not use `.internal/` or aggregate `make check`.

## Automatic gates

1. Pass `HW-T30` against production common-core sources in ordinary,
   ASan/UBSan, and analyzer modes, including malformed/cache/state/deadline/
   cancellation/stale-generation/secret-erasure cases.
2. Prove every public ABI size/offset on configured amd64 and i386; wrong
   version/size/direction/reserved fields must fail before device dispatch.
3. Extend the production net-device/INET fixtures for WLAN capability,
   privilege, active-ioctl admission/join, removal races, and wired regressions.
4. Run `make -j16`, forced configured amd64/i386 consumers, and disposable
   amd64 IDE plus q35 xHCI USB-root boots to exact `login:`.
5. Require `git diff --check`; record exact results in P/W/M/Q documents.

## Completion definition

Q055 completes when p026 is synchronized as complete and p027's generic ABI,
core, fake device, lifecycle gate, and automatic regressions pass without a
new product decision. A physical Archer observation is deliberately deferred
to the later shared hardware acceptance; ECM helper adoption, Latitude xHCI/
NVMe checks, and physical USB HID remain phase-recorded and non-blocking.

## Execution result

Q055 completed both finite entries without physical-radio work. P026 records
the Japan-market label, absence of a printed revision, exact retained
`2357:012e` descriptor authority, and the separately selected hash-pinned
GitHub firmware-package boundary.

P027 adds six versioned, pointer-free, exact-size WLAN ioctls; strict INET
copy/privilege dispatch; an admitted-ioctl teardown gate; a persistent common
station with bounded scan snapshots, generations, total deadlines, normalized
beacon/RSN parsing, connect states, credential scrubbing, and checked
detach/shutdown; plus a level-triggered worker predicate which closes a lost-
wakeup interval. The station retains a live net-device reference and freezes a
driver contract for serialized attach/removal, bounded radio callbacks, and a
single production clock domain. Duplicate, stopping, and capacity attach
failures preserve carrier and reference ownership.

`HW-T30` passes ordinary, ASan/UBSan, GCC analyzer, and amd64/i386 ABI modes.
The strict INET WLAN authorization runner passes twice, the net-device/ARP/INET
hotplug runner passes, and `git diff --check` passes. Ordinary PC-98
`make -j16` and forced amd64/i386 builds pass; the i386 gate caught and removed
one accidental compiler-atomic runtime dependency. The final amd64 image is
`b0409dad5d4dd3574cb4b4e9381ade59a7308e72cc6641b21cf7924fbad8f43f`.
Disposable four-CPU, 4-GiB OVMF q35 boots reached exact `login:` through both
explicit IDE and xHCI USB-only storage with no fatal/storage marker. P028 now
uses the required LIVE-publication then removal-serialized station-attach
ordering.
