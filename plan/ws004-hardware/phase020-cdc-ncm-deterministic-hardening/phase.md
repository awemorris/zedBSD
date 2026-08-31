# WS004 Phase 020: CDC NCM deterministic receive and open hardening

Last updated: 2026-09-01

Phase ID: `ws004-p020`

Status: complete (`q029` automatic software scope)

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Apply the now-approved deterministic subset of CDC NCM runtime hardening before
the next physical RTL8156 data-path check:

1. accept the first completely valid NTB regardless of its sequence value;
2. deliver every completely valid later NTB, including a sequence mismatch,
   and resynchronize the next expected value to the received sequence plus one;
3. make every consumed completion count against bounded network-poll work even
   when it delivers no frame; and
4. program the Ethernet packet filter during each open, after the data
   interface's active alternate has been selected and before traffic URBs are
   submitted.

Malformed NTBs remain rejected transactionally and cannot alter sequence
state. This Phase does not claim that any one static finding is already the
physical freeze's proven cause.

## Dependencies

- `ws004-p013`: strict transactional NTH16/NDP16 wire validation.
- `ws004-p014`: integrated `ueN` driver and persistent-URB lifecycle.
- `ws004-p015`: active-alternate, endpoint-admission, and callback-drain
  transaction.
- `ws004-p018`: physical RTL8156 configuration selection, bind, and `ue0`
  publication.

## Accepted policy

### Sequence state

- Sequence continuity is diagnostic state, not a reason to reject an otherwise
  valid NTB.
- With no accepted predecessor, any 16-bit wire sequence is valid. After the
  complete existing validator accepts it, deliver its datagrams and set
  `expected_sequence = wire_sequence + 1`, with ordinary 16-bit wraparound.
- With an accepted predecessor, an exact expected value and a mismatch are both
  delivered if the complete NTB is valid. A mismatch increments bounded
  diagnostic/statistical evidence and commits
  `expected_sequence = wire_sequence + 1`.
- Signature, header length, block length, NDP chain, bounds, alignment,
  termination, overlap, CRC mode, datagram count, and frame-size failures remain
  malformed input. Such input delivers no datagram and leaves both the
  initialized/expected state and the last accepted sequence unchanged.
- Validation and state commit remain transactional. A minimally valid header is
  not sufficient to change sequence state.

### Completion budget

- A network-poll invocation has a finite work budget. Consuming a terminal RX,
  notification, or TX completion charges at least one work unit regardless of
  transfer status, parse result, carrier change, or delivered-frame count.
- Delivery of already queued frames and bounded rearm/retry work also cannot
  bypass the poll boundary. Remaining ready work is rescheduled rather than
  drained in an unbounded zero-delivery loop.
- Packet and error statistics retain their existing meanings; the poll return
  value may represent consumed work rather than only successfully delivered
  packets where required by the existing `net_device` contract.
- Completion callbacks remain short publication callbacks. This Phase does not
  move xHCI event or DMA-reclaim work out of hard-interrupt context.

### Open ordering

- Attach completes descriptor/profile negotiation, resource creation, and the
  final active-data-alternate transition, but does not rely on a packet filter
  programmed before that transition.
- Every successful `ncm_open()` programs the supported directed,
  all-multicast, and broadcast filter after confirming the active alternate and
  before notification or bulk-IN submission. Reopen therefore restores state a
  function may lose across `SET_INTERFACE` or close.
- A filter control failure fails open transactionally: no traffic URB remains
  admitted and the device does not report an opened data path.

## Planned implementation

1. Refactor the NTH16 receive transaction so complete structural validation can
   return the wire sequence independently of continuity policy. Commit sequence
   state and invoke delivery only after all current p013 checks pass.
2. Add explicit uninitialized/accepted sequence state or an equivalent
   representation; do not overload sequence zero as a sentinel.
3. Make the NCM poll path account for every consumed ready completion and
   bounded queue/rearm unit. Preserve deferred parsing/delivery and all
   callback, close, detach, and quarantine barriers.
4. Move packet-filter programming to the open transaction after the active
   alternate and before either persistent input URB. Cover first open, close and
   reopen, filter failure, and detach races.
5. Extend production-source fixtures for arbitrary initial sequences, exact
   continuity, forward/backward mismatch, wraparound, malformed-before-valid,
   malformed-between-valid, repeated invalid completions, simultaneous ready
   completions, budget exhaustion/reschedule, and packet-filter ordering.

## Explicit exclusions

- notification header/payload reassembly or RTL8156 split-notification policy;
- duplicate-notification suppression beyond evidence already available;
- xHCI IRQ/completion architecture redesign or DMA allocation redesign;
- asynchronous terminal TX-statistics semantics from `ws004-p017`;
- CDC ECM, RNDIS, MBIM, vendor-specific Realtek initialization, or a shared
  USB-Ethernet backend.

## Verification gates

- The focused NCM wire fixture passes ordinary and ASan/UBSan modes; the
  integrated-driver fixture also passes analyzer mode.
- A first fully valid NTB with zero, nonzero, and wrap-boundary sequence values
  is delivered; every fully valid mismatch is delivered and sets the next
  expectation from the wire value.
- Every malformed fixture leaves sequence state byte-for-byte unchanged and
  delivers zero datagrams; a following valid NTB is accepted immediately.
- A sustained stream of transfer failures or malformed NTBs consumes bounded
  poll work and yields/reschedules without a zero-delivery tight loop.
- Packet-filter control ordering is active alternate, filter, notification URB,
  RX URB on every successful open; each injected failure retains exact
  ownership and carrier state.
- Existing p013/p014, USB binding, xHCI concurrency, removable `net_device`, USB
  Storage, and shutdown regressions remain passing.
- The default amd64 build passes with `make -j16`, and the changed production
  objects pass a focused i386 compile; no aggregate `make check` target or
  `.internal/` material is used.
- `git diff --check` passes.

## Execution result

Completed in q029 on 2026-08-29:

- the production NTH16/NDP16 path accepts the first structurally valid NTB at
  any sequence, accepts and resynchronizes later valid mismatches, and commits
  no sequence state for malformed input;
- terminal notification, RX, and TX completions consume bounded poll work,
  ready classes are serviced fairly, rearm ownership is claimed atomically,
  and remaining work is rescheduled;
- the packet filter is programmed on every open after the active alternate and
  before persistent traffic URBs, with transactional failure and reopen
  coverage;
- the wire fixture passed ordinary and ASan/UBSan modes; the integrated driver
  fixture passed ordinary, ASan/UBSan, and analyzer modes with 1537 checks;
- removable network-device/hotplug, USB binding (971 checks), xHCI/USB function
  model (1404 checks), and shutdown regressions passed; and
- the default amd64 `make -j16` build, focused i386 changed-object compile, and
  `git diff --check` passed.

This is an automatic software result only. It does not claim physical RTL8156
carrier or packet transfer and does not add the broader kernel stage-counter or
trace work retained by `ws005-p001`.

## Completion conditions

- The accepted sequence, malformed-state, completion-budget, and packet-filter
  policies are implemented in production code and locked by focused fixtures.
- No invalid NTB can advance sequence state or deliver a partial frame.
- No terminal completion can trigger an unbudgeted NCM poll/rearm loop merely
  because it delivered zero frames.
- All declared automatic regression/build gates have retained evidence. This
  software Phase does not itself claim physical RTL8156 packet transfer.

## Relationship to `ws004-p017`

`ws004-p017` is deferred pending the shared asynchronous-TX statistics
decision. This Phase extracts only the deterministic policy the user has now
approved: valid-sequence acceptance/resynchronization and bounded completion
work. p017 retains asynchronous TX completion accounting and any later
recovery topics not explicitly frozen here.

## Reconsideration boundary

Stop and return the exact evidence rather than expanding scope if the change
requires a public USB/network UAPI redesign, a general xHCI IRQ architecture
change, notification reassembly, ECM, vendor initialization, or a new meaning
for asynchronous TX statistics. A physical failure after this Phase is a
WS005 discriminator, not permission to add those excluded changes.
