# WS004 Phase 010: USB function and alternate-setting model

Last updated: 2026-08-29

Phase ID: `ws004-p010`

Status: completed (`q027`)

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Represent and operate the USB configuration/function structure required by
CDC networking: multiple configurations, every alternate setting and its
endpoints, IAD and class-specific interface descriptors, string descriptors,
and explicit ownership of a control interface's sibling data interface.

## Frozen contract

- Preserve configuration, interface-number, alternate-setting, endpoint, IAD,
  and class-specific descriptor bytes after enumeration with strict bounds.
- Provide checked configuration and alternate-setting selection. Hardware and
  software endpoint state must roll back or remain quarantined on failure.
- A class driver may locate a sibling interface by number and claim/release it
  exclusively. Ambiguous adjacency is never treated as a function contract.
- String retrieval discovers LANGID and decodes bounded UTF-16LE into UTF-8 or
  a documented ASCII subset; malformed strings fail visibly.
- Existing one-configuration, alternate-zero USB storage continues to work.

The public USB header may grow only by the cohesive internal-driver API needed
for these operations. Descriptor parsing details remain implementation-owned.

## Planned work

1. Replace the alternate-count placeholder with retained host-interface
   records containing descriptors, endpoints, and class-specific extras.
2. Retain and validate every advertised configuration and IAD needed for class
   matching rather than discarding the selected raw descriptor immediately.
3. Implement configuration/interface lookup, exclusive sibling claim/release,
   and active-setting accessors.
4. Implement checked `SET_CONFIGURATION` and `SET_INTERFACE`, endpoint
   disable/enable ordering, and rollback/quarantine on HCD failure.
5. Implement bounded USB string-descriptor retrieval and conversion.
6. Add malformed, multi-configuration, IAD/Union, alternate-zero/one,
   endpoint, claim-contention, rollback, and string fixtures.

## Completion conditions

- A CDC-shaped fixture retains a control interface and data alt 0/alt 1 bulk
  endpoints and can claim the data interface through explicit descriptors.
- Checked alternate selection sends the correct request and publishes the new
  endpoint set only after HCD success; failure leaves a safe old or quarantined
  state.
- Competing claims and malformed/ambiguous descriptors are rejected.
- MAC-style string retrieval and malformed string cases pass.
- Existing USB storage fixtures, relevant configured x86 builds, and
  `git diff --check` pass without `make check` or `.internal/`.

## Reconsideration boundary

Stop and record `uncleared` if safe alternate rollback requires a new HCD
transaction API whose ownership cannot be specified locally. Do not publish
partially enabled endpoints as an active interface.

## Actual result

- The USB core now retains every bounded configuration, logical interface,
  alternate setting, endpoint, IAD, class-specific extra descriptor, and raw
  configuration byte sequence.  Inactive configurations remain retained but
  are never attached.
- Enumeration selects the configuration with the highest registered-driver
  match score across alternate-zero interfaces.  Descriptor order is the
  deterministic tie breaker and the first configuration remains the fallback
  when no driver supports the device.
- Configuration and alternate selection use checked endpoint transitions.
  Partial endpoint and later-interface failures compensate only the operations
  completed by that transaction; failed compensation or ambiguous control I/O
  quarantines the device instead of publishing a partial endpoint set.
- The HCD endpoint-disable callback is checked and returns `int` consistently
  in xHCI, EHCI, and UHCI.  xHCI publishes disabled state and releases endpoint
  ring ownership only after its command succeeds.
- Function drivers can locate, claim, and release sibling interfaces.  Claims
  are released after attach failure and successful detach, retained after a
  failed detach, and handled safely during disconnect.
- USB strings discover the first LANGID and convert bounded UTF-16LE, including
  valid surrogate pairs, into NUL-terminated UTF-8.  Malformed and truncated
  descriptors fail visibly.
- No new HCD transaction API was required, so the reconsideration boundary was
  not reached.  Existing USB-storage behavior and the URB flag contract were
  preserved.

## Evidence

- [`usb-function-model-test.c`](../tests/usb-function-model-test.c): 1280
  checks pass, including multi-configuration preference, alternate/configuration
  rollback, strict endpoint state, claim lifetime, malformed descriptors,
  bounded nth-allocation failure with balanced cleanup, hot-unplug, and terminal
  shutdown ordering.
- The same production-source fixture passes ASan/UBSan with leak detection
  disabled because LeakSanitizer is unavailable under the host ptrace policy;
  explicit allocation/free balance remains active.  GCC `-fanalyzer` also
  passes.
- Existing `xhci-model-test.c`, `usb-storage-scsi-test.c`, and
  `usb-urb-publication-test.c` pass.
- `make -j16` passes for the default amd64 image, and `make -j16
  ZEDBSD_CONFIG=plan/ws004-hardware/tests/config-pcat-xhci.mk` passes for the
  configured i386 PC/AT xHCI image.
- Scoped production and new-fixture whitespace checks pass.  `make check` and
  `.internal/` were not used.
