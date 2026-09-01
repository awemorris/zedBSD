# WS004 Phase 017: CDC NCM runtime recovery and completion accounting

Last updated: 2026-09-01

Phase ID: `ws004-p017`

Status: Complete (`q054`)

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
now complete after q053 closed the shared asynchronous-TX statistics decision
and q054 implemented it. Later recovery work outside p020's explicit boundary
remains separate.

## Planned design work

1. Retain p020's fully validated sequence acceptance/resynchronization and
   completion-budget contracts as dependencies rather than reopening them.
2. Define the `net_device` meaning of accepted-versus-completed TX statistics,
   then publish terminal bulk-OUT errors without double-consuming a packet or
   racing device removal.
3. Add production-source fixtures for every accepted terminal TX error,
   exactly-once callback publication, close/detach cancellation, a genuine
   completion immediately before close, and close/open reuse.
4. Retain transport-loss or quarantine recovery beyond p020 as future work
   unless new physical evidence identifies one exact missing rule.

## Completion conditions

- The completed p020 receive-policy and bounded-work regressions remain
  passing.
- A submitted TX that later fails has the documented statistics outcome
  exactly once.
- Existing p013/p014, USB binding, xHCI, net-device, build, and QEMU regression
  gates pass.

## Reconsideration boundary

The asynchronous TX-statistics meaning is now explicit. Return to human design
only if implementation would require changing the accepted counter meanings,
publishing a public UAPI, or adding a recovery rule without failure evidence.
Do not fold p020, notification reassembly, xHCI IRQ redesign, or ECM into p017
merely because they concern the same device.

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

The recommended compatible policy was accepted by the user on 2026-09-01:
retain accepted/submitted `tx_packets`/`tx_bytes`, increment `tx_errors`
exactly once for genuine later `STALL`, `TIMEOUT`, `DISCONNECTED`, or
`IO_ERROR`, leave `tx_dropped` unchanged, and exclude administrative
`CANCELLED`. Q029 supplies no residual physical recovery failure beyond the
already completed p020 rules. ECM remains outside p017 and would require a
separate consumer Phase after the common contract is fixed.

## q054 implementation boundary

The general network layer supplies one locked asynchronous-TX-error helper.
It changes only `tx_errors`; accepted packet and byte counters remain owned by
`net_device_transmit()`, and drops retain their existing synchronous rejection
meaning. CDC NCM classifies the terminal status in the one USB completion
callback and calls the helper only for the four accepted genuine failures.
The USB core's single terminal-claim callback contract supplies exactly-once
publication even when HCD completion races administrative cancellation.

This callback-side accounting deliberately precedes worker polling. A close,
detach, or shutdown may discard scheduled poll work after joining/cancelling
URBs, but it cannot discard a genuine error already published by the terminal
callback. An orderly cancellation follows the same callback path and is
explicitly ignored. A failed drain retains the existing graph and does not
invent a terminal result.

## q054 implementation result (2026-09-01)

The common network-device layer now exposes a locked internal
`net_device_tx_error()` helper. CDC NCM calls it from the one terminal TX URB
callback only for `STALL`, `TIMEOUT`, `DISCONNECTED`, and `IO_ERROR`.
Successful driver acceptance remains the `tx_packets`/`tx_bytes` boundary,
later terminal failure changes only `tx_errors`, and administrative
`CANCELLED` changes no failure counter. The callback does not clear `tx_busy`:
poll or the checked stop/drain path first observes HCD ownership release and
then makes the persistent URB reusable.

The common hotplug fixture passes accepted-counter, asynchronous-error,
unchanged-drop, null-helper, and removal-join cases. The production-source NCM
fixture passes ordinary and ASan/UBSan runs at 2,013 checks each plus its
analyzer gate. It covers every genuine status, completion-before-poll,
completion-before-close, no double account after a second poll or drain retry,
orderly close/detach cancellation, close/open URB reuse, and twelve fresh
detach/reconnect generations with zero inherited statistics.

The retained NCM wire, USB binding, concurrent-URB, USB recovery, and shutdown
regressions pass. Configured amd64 and i386 production objects compile, the
ordinary repository `make -j16` passes, and a fresh private amd64/xHCI
configured image builds successfully. Its SHA-256 is
`0c794540d535c9a83006428683a16db4d4ffc949b457819401ce00938a7d187c`.
A disposable 4-GiB, four-CPU OVMF q35 guest booted that image solely through
xHCI USB Storage, mounted the overlay root, activated `swap0`, started init,
and reached the exact `login:` prompt in 13 seconds.

CDC ECM consumption of the helper, coherent locked `SIOCGIFSTATS` snapshots
on i386, and conversion of dp8390's private accounting are separate future
boundaries. They neither change nor block this NCM Phase. An autonomous NCM TX
timeout policy is also separate: the present asynchronous submit path does not
itself manufacture `TIMEOUT`, but correctly classifies one if the USB layer
publishes it.
