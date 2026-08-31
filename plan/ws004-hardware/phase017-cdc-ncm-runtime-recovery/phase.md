# WS004 Phase 017: CDC NCM runtime recovery and completion accounting

Last updated: 2026-09-01

Phase ID: `ws004-p017`

Status: deferred; human TX-statistics decision required; not queued

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Complete the CDC NCM recovery and accounting work that remains after the
approved deterministic subset was extracted into `ws004-p020`, especially
making an accepted transmit's later terminal HCD error observable in network
statistics without weakening the strict wire validator or the completed
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

The user has since approved one deterministic receive policy as
[`ws004-p020`](../phase020-cdc-ncm-deterministic-hardening/phase.md): the first
fully valid NTB is accepted at any sequence, every fully valid mismatch is
delivered and resynchronizes the next expectation to the wire sequence plus
one, malformed input changes no sequence state, and completion work is
budgeted. p020 also owns packet-filter programming on open after the active
alternate. Those items no longer wait on this broader Phase, but p017 is
deferred pending the shared asynchronous-TX statistics decision and any later
recovery work outside p020's explicit boundary.

## Planned design work

1. Retain p020's fully validated sequence acceptance/resynchronization and
   completion-budget contracts as dependencies rather than reopening them.
2. Define the `net_device` meaning of accepted-versus-completed TX statistics,
   then publish terminal bulk-OUT errors without double-consuming a packet or
   racing device removal.
3. Decide whether any transport-loss or quarantine recovery behavior beyond
   p020 is required after physical evidence, without adding notification
   reassembly or an xHCI IRQ redesign implicitly.
4. Add production-source fixtures for terminal TX error, close/detach during
   accounting, reconnect reset, and any separately approved residual recovery
   rule.

## Completion conditions

- The completed p020 receive-policy and bounded-work regressions remain
  passing.
- A submitted TX that later fails has the documented statistics outcome
  exactly once.
- Existing p013/p014, USB binding, xHCI, net-device, build, and QEMU regression
  gates pass.

## Reconsideration boundary

Do not queue this remaining Phase until the meaning of asynchronous TX
statistics is explicit and new physical evidence identifies any additional
recovery rule. If correct accounting requires changing the general
`net_device` contract, plan that shared change before modifying the NCM driver
or its public headers. Do not fold p020, notification reassembly, xHCI IRQ
redesign, or ECM into p017 merely because they concern the same device.

## q053 readiness audit (2026-09-01)

No decision-free implementation slice remains. Today
`net_device_transmit()` counts `tx_packets` and `tx_bytes` when a driver accepts
the submission, while a later NCM bulk-OUT terminal error only releases the
busy state. Before p017 enters a Queue, decide all of the following as one
general `net_device` statistics contract:

- whether accepted packet/byte counters remain counted after a later HCD
  failure or move to successful completion;
- whether that terminal failure increments only `tx_errors` or also
  `tx_dropped`; and
- whether administrative `CANCELLED` during close/detach is excluded from
  failure statistics.

The recommended compatible policy is to retain accepted/submitted
`tx_packets`/`tx_bytes`, increment `tx_errors` exactly once for genuine later
`STALL`, `TIMEOUT`, `DISCONNECTED`, or `IO_ERROR`, leave `tx_dropped` unchanged,
and exclude administrative cancellation. This is a recommendation, not an
adopted decision. Q029 supplies no residual physical recovery failure beyond
the already completed p020 rules. ECM remains outside p017 and would require a
separate consumer Phase after the common contract is fixed.
