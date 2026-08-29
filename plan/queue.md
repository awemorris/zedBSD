# Queue: general USB transactions and CDC NCM software milestone

Last updated: 2026-08-29

QID: `q027`

Queue status: in-progress

Queue finished: **No**

Authorization: the user approved the original five-Phase Queue on 2026-08-29.
After p014 reached its documented ownership reconsideration boundary, the user
approved rebuilding q027 on 2026-08-29 to insert the general `ws004-p015`
binding/interface transaction before resuming p014, with execution authorized.

Timebox: no fixed wall-clock limit; continue until all six finite items have
been completed or honestly marked `uncleared` at a documented boundary.

Parent: [master plan](master.md)

Previous Queue: [q026](queue-q026.md)

## Purpose

Add a native host-side USB CDC NCM network interface without hiding the USB,
xHCI, network-device, and shutdown contracts that NCM exercises.  Resolve the
discovered alternate/URB conflict as a general Mass Storage, NCM, HID, Audio,
and composite-device USB transaction contract rather than an NCM exception. The first
implementation is an NCM 1.0-compatible NTH16/NDP16, no-CRC, 1500-byte-MTU
profile named `ueN`.  It is deliberately independent rather than built on a
speculative common USB-Ethernet implementation.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p010` | [Phase](ws004-hardware/phase010-usb-function-model/phase.md) | completed | USB configuration, alternate-setting, functional-descriptor, string, interface-claim, and transactional endpoint contracts pass 1280 focused checks plus sanitizer, analyzer, regression, and configured-build gates |
| 2 | `ws004-p011` | [Phase](ws004-hardware/phase011-xhci-concurrent-urbs/phase.md) | completed | per-endpoint xHCI ownership, checked cancellation/drain, bounded reclaim reserve, capability query, and callback-aware URB drain pass focused, analyzer, build, and USB-root QEMU gates |
| 3 | `ws004-p012` | [Phase](ws004-hardware/phase012-net-device-hotplug/phase.md) | completed | carrier, concurrent detach, stale-identity purge, deferred release, and terminal shutdown barriers pass focused and sanitizer gates |
| 4 | `ws004-p013` | [Phase](ws004-hardware/phase013-cdc-ncm-wire/phase.md) | completed | strict bounded negotiation and NTH16/NDP16 encode/decode, including the advertised-maximum no-ZLP exception, pass production-source fixtures |
| 5 | `ws004-p015` | [Phase](ws004-hardware/phase015-usb-binding-transactions/phase.md) | completed | interface-scoped I/O gate, active-endpoint submit, provisional binding, and EP0 serialization pass general USB gates |
| 6 | `ws004-p014` | [Phase](ws004-hardware/phase014-cdc-ncm-driver/phase.md) | pending | production NCM class driver moves to the general transaction contract, binds as `ueN`, transfers, detaches, reconnects, and passes the declared software gate |

## Dependency order

```text
ws004-p010 ----+----> ws004-p015 ----> ws004-p014
               |
ws004-p011 ----+
               |
ws004-p012 ----+
               |
ws004-p013 ----+
```

p010 through p013 are complete. p014's first implementation exposed a general
allocated-URB/alternate ownership gap. p015 now executes against the frozen
USB, xHCI, and network lifetime contracts; p014 resumes only after p015 passes.

## Frozen product boundary

- Host-side CDC NCM 1.0-compatible operation only.
- NTH16 and NDP16 only; CRC, NTH32, MBIM, ECM, RNDIS, vendor-specific Realtek,
  USB-device/gadget role, jumbo frames, and advanced TX aggregation are out of
  scope.
- RX accepts multiple valid Ethernet datagrams per NTB. Initial TX may emit one
  datagram per NTB.
- xHCI is the first concurrent-HCD target. UHCI/EHCI must fail safely rather
  than falsely advertising NCM support.
- The driver is self-contained below stable USB and `net_device` interfaces.
  Common USB-Ethernet code may be extracted only after another implementation
  demonstrates stable commonality.
- Stock QEMU `usb-net` is ECM/RNDIS, not NCM. Host production-source fixtures,
  configured builds, xHCI regression, and ordinary QEMU boot form the software
  completion gate. True NCM interoperability remains WS005 physical/gadget
  acceptance and is not falsely claimed here.

## Execution rules

- Preserve unrelated work, including the existing root `AGENTS.md` move; do
  not stage or rewrite it as part of q027.
- Do not inspect or modify `.internal/`, `userland/noct/NoctLang`, or
  `/home/awe/NoctLang`.
- Use `make -j16`, focused WS004 fixtures, and `qemu-system-x86_64`; do not use
  `make check`.
- Keep USB storage boot and reclaim-safe DMA behavior working while adding
  persistent network URBs.
- Commit each completed Phase as `WIP` and push it. If push is unavailable,
  retain the local commit and continue.

## Completion definition

q027 is finished when every item is `completed` or `uncleared`, actual results
are synchronized to P/W/M/Q, focused fixtures and declared builds pass, and no
result claims physical NCM interoperability without a proven NCM device role.
