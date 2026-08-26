# Queue: Latitude xHCI capability/MMIO bring-up

Last updated: 2026-08-26

QID: `q012`

Queue status: finished

Queue finished: **Yes**

Authorization: explicitly approved by the user on 2026-08-26

Timebox: no fixed duration

Parent: [master plan](master.md)

Previous Queue: [q011](queue-q011.md)

## Purpose

Execute only `ws003-p003`. Diagnose and correct the physical Latitude xHCI
capability/MMIO attach failure, retain the QEMU USB-root baseline, and prepare
one precisely identified production image for a single U2 boundary-confirmation
boot on the target.

## Execution registry

| Priority | WS / Phase | Authoritative documents | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws003-p003` | [WS003](ws003-bringup/ws.md), [Phase](ws003-bringup/phase003-latitude-xhci-capability-mmio/phase.md), [evidence](ws003-bringup/tests/latitude-xhci-evidence.md) | uncleared | PCI decode/BAR/capability work passed on QEMU and hardware, but BR-T33 stopped during EP0 device enumeration before USB mass storage appeared |

## Dependencies and uncertainty

- q011 and `ws003-p002` are complete; the Latitude reaches U1 reliably.
- Entering q012, both physical xHCI functions failed the compound capability
  check with `ENODEV` before ownership, IRQ, DMA, or controller start.
- The first implementation branch tested PCI Memory Space enable ordering.
- The low-BAR relocation was a secondary hypothesis. Generic PCI
  resource rebalance or arbitrary high-MMIO redesign is not authorized unless
  bounded diagnostics prove it necessary; such work is extracted to WS004.
- HCIVERSION compatibility, scratchpad composition, xECP walking, legacy
  handoff, and reset/CNR sequencing were corrected only against the xHCI
  contract and focused fixtures.

## Execution contract

- Do not implement unrelated USB HID, VFS, memory-capacity, NVMe, WLAN, or
  graphics work.
- Keep bus mastering separate from pre-DMA capability MMIO.
- Preserve or balance PCI command/BAR/IRQ/controller state on every failure and
  detach path.
- Reject zero/all-one and malformed capability images distinctly; do not hide
  failures with delays, retries, or version wildcards.
- Use focused tests, existing WS004 xHCI controls, `make -j16`, BR-T24
  4/8/16-GiB OVMF USB boots, the legacy-BIOS USB control, and
  `git diff --check`. Do not use `make check`, `.internal/`, or commits.
- One successful physical U2 observation provisionally clears this Phase.
  Repeatability remains the final BR-T30 campaign and does not block
  implementation.

## Execution record

Execution started on 2026-08-26. The capability/MMIO implementation, BR-T25,
the checked HCD teardown fixture BR-T26, existing xHCI/PCI host regressions,
`make -j16`, BR-T24 at 4/8/16 GiB, the legacy-BIOS USB-root control, and
`git diff --check` pass. QEMU confirms
`MEM on, MASTER off` before capability MMIO, `reject=00000000:ok`, controller
attach, `usb-storage: sda`, and `login:`.

The recorded q012 physical candidate is
`build/amd64/hdd-image.img` (135266304 bytes), SHA-256
`4b346ec9d303c557c4b810f2a5b3ea430964c7e6e9a98fc7a572410f2ba667f4`.
BR-T33 was run once on the Latitude with this artifact. Both physical xHCI
functions reported HCIVERSION 1.2, `reject=00000000:ok`, and registered an HCD.
The previous `attach failed at capabilities (13)` boundary is cleared, and the
fallback BAR for `0000:00:14.0` is proven reachable after readback.

The Queue item remains uncleared because USB device enumeration then failed on
EP0 with transfer completion codes 6 and 4, a timeout, and a secondary Stop
Endpoint completion code 19. No `usb-storage: sda` marker was reached. This is
recorded as the bounded handoff to
[ws003-p004](ws003-bringup/phase004-latitude-xhci-device-enumeration/phase.md).
That Phase is not authorized by q012. There is no remaining q012 action and no
additional physical run is requested. The failed-cancel path also has an
orphaned-request/live-DMA teardown risk; this artifact is diagnostic-only and
must not be used for further physical testing.

Resume condition: construct and obtain approval for a new finite Queue which
contains `ws003-p004`; start with its BR-T27 host fixture and Control TRB
correction.
