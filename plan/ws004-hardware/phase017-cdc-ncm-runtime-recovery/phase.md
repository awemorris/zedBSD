# WS004 Phase 017: CDC NCM runtime recovery and completion accounting

Last updated: 2026-08-29

Phase ID: `ws004-p017`

Status: pending; not queued

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Recover CDC NCM receive service after a dropped, malformed, or sequence-skipped
NTB, and make an accepted transmit's later terminal HCD error observable in
network statistics without weakening the strict wire validator or the completed
ownership contract.

## Dependencies

- `ws004-p013`: strict transactional NTH16/NDP16 wire validation.
- `ws004-p014`: integrated `ueN` driver and persistent-URB lifecycle.

## Audit handoff

The final p014 review found no open P0/P1 issue, but identified two nonblocking
runtime-policy gaps:

1. The wire parser advances `expected_sequence` only after a completely valid
   NTB. That is correct for its transactional interface, but the driver rearms
   after a malformed NTB or transfer-status error without entering a resync
   state. If the function advances its sequence, later valid NTBs can then be
   rejected indefinitely as `EILSEQ`.
2. `net_device_transmit()` counts a successfully submitted packet, but a later
   terminal bulk-OUT error currently only releases `tx_busy`; no asynchronous
   error is reflected in network statistics.

These are recovery and accounting limitations, not buffer-bounds, callback,
DMA, UAF, or detach-ownership defects in p014.

## Planned design work

1. Keep `drv_usb_cdc_ncm_parse_ntb16()` strict and transactional. Add an
   adapter-local resynchronization policy rather than silently making malformed
   wire input acceptable.
2. After a transfer loss, malformed NTB, or sequence gap, use only a minimally
   valid NTH16 header to select a candidate sequence. Deliver no frame until the
   complete existing validator accepts the NTB.
3. Define whether a valid current NTB that merely skipped sequence numbers is
   accepted on a checked second parse, or whether it is dropped and only the
   next valid NTB resumes delivery. Record the selected policy and wraparound.
4. Define the `net_device` meaning of accepted-versus-completed TX statistics,
   then publish terminal bulk-OUT errors without double-consuming a packet or
   racing device removal.
5. Add production-source fixtures for malformed then valid input, transport
   loss then valid input, forward sequence gaps, wraparound, terminal TX error,
   close/detach during recovery, and reconnect reset.

## Completion conditions

- One bad or lost NTB cannot leave RX permanently rejecting subsequent valid
  traffic.
- Recovery never delivers an incompletely validated datagram and remains
  bounded under repeated hostile input.
- A submitted TX that later fails has the documented statistics outcome
  exactly once.
- Existing p013/p014, USB binding, xHCI, net-device, build, and QEMU regression
  gates pass.

## Reconsideration boundary

Do not queue implementation until the sequence-gap acceptance rule and the
meaning of asynchronous TX statistics are explicit. If correct accounting
requires changing the general `net_device` contract, plan that shared change
before modifying the NCM driver or its public headers.
