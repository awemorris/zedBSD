# WS004 Phase 014: integrated USB CDC NCM network driver

Last updated: 2026-08-29

Phase ID: `ws004-p014`

Status: pending (`q027`)

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Bind a standards-shaped CDC NCM control/data function, negotiate the approved
profile, and expose a removable Ethernet `net_device` named `ueN` with bounded
notification, RX, TX, close, detach, and reconnect behavior.

## Dependencies

- `ws004-p010`: USB function/alternate-setting ownership.
- `ws004-p011`: concurrent xHCI endpoint URBs.
- `ws004-p012`: removable `net_device` lifetime and carrier state.
- `ws004-p013`: strict NCM wire codec.

## Frozen driver boundary

- Match communication class `0x02`, NCM subclass `0x0d`, protocol `0x00` and
  an explicitly associated CDC data interface; never match vendor-specific or
  merely adjacent interfaces.
- Validate Header, Union, Ethernet, and NCM functional descriptors and obtain a
  strict 12-hex-digit MAC string.
- Negotiate the p013 profile, select the data interface's bulk alternate,
  program the Ethernet packet filter, and handle NETWORK_CONNECTION and speed
  notifications.
- Completion callbacks only publish bounded ready work. NTB parsing and packet
  delivery execute in network worker context.
- TX consumes the existing packet ownership exactly once. RX copies each
  accepted Ethernet frame into an ordinary bounded `packet_buf`.
- Close/detach/shutdown cancel and drain persistent URBs before releasing the
  USB function, DMA, interface claim, or `net_device`.
- No speculative common USB-Ethernet backend is introduced in this Phase.

## Planned work

1. Add config/build registration before xHCI root probing.
2. Implement strict descriptor binding, data-interface claim, MAC retrieval,
   negotiation, alternate selection, and unwind.
3. Add persistent interrupt and bulk-IN requests plus bounded bulk-OUT state.
4. Bridge notifications, RX frames, and TX ownership to `net_device`.
5. Implement close, normal shutdown, disconnect, failed negotiation, and
   reconnect cleanup.
6. Add a production-source fake-HCD/function fixture for bind, control-request
   ordering, data transfer, malformed NTB, link state, timeout/cancel, detach,
   reconnect, and concurrent storage ownership.
7. Pass relevant x86 builds and ordinary amd64 QEMU xHCI boot regression.

## Completion conditions

- A valid NCM function binds as `ue0`, reports its MAC, and transitions carrier
  from notifications.
- TX produces a valid NTB and RX delivers every valid datagram while rejecting
  malformed input without partial unsafe state.
- Negotiation/control/alt-setting failure unwinds every resource in reverse
  ownership order.
- Timeout, close, disconnect, shutdown, and more than eight reconnects leave no
  live callback or leaked registry/interface claim.
- NCM fixture, prerequisite regressions, relevant builds, and QEMU xHCI boot
  pass.
- The result is explicitly a software milestone until a real NCM role or
  `g_ncm` gadget passes WS005 NET-T40.

## Reconsideration boundary

Stop and mark this Phase `uncleared` if production HCD or network ownership
cannot satisfy the frozen close/detach contract, or if available physical
hardware proves not to expose CDC NCM. Do not relabel ECM/RNDIS or a Realtek
vendor protocol as NCM.
