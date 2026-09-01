# WS004 Phase 039: evidence-driven WLAN commonization review

Last updated: 2026-09-01

Phase ID: `ws004-p039`

Status: planned; follows the `ws004-p030` automatic milestone and completed
`ws004-p038`; not queued

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Review the independently working RTL8822BU and Intel AX201 implementations
side by side, identify only common behavior proven by both implementations,
and move a worthwhile stable subset into the existing generic WLAN framework.
This is late abstraction, not a mandate to eliminate duplication. A reviewed
conclusion that no additional extraction is justified is a valid Phase result.

Both drivers must already pass their useful normal paths before this review.
Do not create a proposed common layer as a prerequisite for the Intel driver.

## Frozen design principles

- The stable public WLAN UAPI and its semantic contract are the primary
  module boundary. Do not change them for naming symmetry or implementation
  convenience.
- Each driver remains independently replaceable and may retain entirely
  different firmware, transport, DMA, interrupt, ring, descriptor, reset,
  calibration, regulatory, and hardware-security structures.
- Code duplication is acceptable. Extract code only when both production
  implementations demonstrate the same ownership, lifetime, error, ordering,
  and performance contract—not merely similar syntax.
- Prefer shared conformance tests over shared implementation when they enforce
  the public interface without restricting driver internals.
- Do not introduce a common RTL88 layer, a generic Intel layer, or a combined
  Intel/Realtek hardware framework in this Phase.

## Dependencies

- `ws004-p030`: RTL8822BU normal behavior and the automatic lifecycle matrix
  are complete under its declared profile; p008 retains final physical
  repeatability ownership.
- `ws004-p038`: the independent Intel AX201 scan/WPA2/CCMP/useful-IP path is
  complete and its duplicated/private boundaries are visible.
- Passing focused and physical normal-path evidence for both exact devices.
- The project-wide interface-based modularity and late-abstraction preference
  in the master plan.

## Review procedure

1. Build a symbol/file/ownership inventory for both drivers and the existing
   p027/p029 common core. Classify every candidate as:
   - already correctly common;
   - same public behavior but intentionally independent implementation;
   - genuinely identical internal mechanism and extraction candidate; or
   - device-specific and prohibited from generic code.
2. For each extraction candidate, compare allocation owner, locking context,
   callback lifetime, cancellation, error mapping, timeout, byte order, DMA
   assumptions, secret handling, and teardown. Any mismatch keeps it private.
3. State the benefit in concrete terms: one invariant, parser, state
   transition, or test oracle maintained once. Line-count reduction alone is
   not sufficient.
4. Move at most one coherent boundary at a time beneath the existing public
   UAPI. Keep driver-local adapters so Intel and Realtek state layouts remain
   free to diverge.
5. Run both drivers' complete focused suites after each extraction. Revert the
   extraction if it adds cross-driver conditionals, exposes private hardware
   state, weakens error reporting, or makes one implementation depend on an
   unused feature of the other.

## Public-interface policy

Ordinary refactoring must not modify the public WLAN ioctl structures,
operation meanings, device names, status/error contract, or userland command
surface. Internal driver-operation tables may gain a private adapter only when
both implementations need the same semantic and all existing consumers are
updated atomically.

If the comparison exposes a genuinely missing public capability, document the
exact consumer, operation, lifetime, compatibility effect, and why a private
adapter is insufficient. Mark p039 `uncleared` and request a human interface
decision before editing the public header. Do not bundle that decision into a
mechanical cleanup.

## Verification contract (`HW-T39`)

- Preserve a before/after map of every moved symbol and its ownership.
- Run all p027/p029 generic WLAN, WPA2 codec/engine, CCMP L2, RTL8822B core/
  security/USB, and Intel AX201 transport/security fixtures.
- Prove unchanged ABI layout and ioctl values with compile-time and binary
  conformance checks.
- Run configured amd64 builds, ordinary repository build, and retained
  IDE/xHCI USB-root regressions.
- On the final candidate, rerun one bounded normal-path smoke for each exact
  device using runtime-only credentials: scan, authorized WPA2/CCMP, useful IP
  transfer, disconnect, and down. These are regression checks, not expanded
  throughput or final repeatability campaigns.
- Retain no credential or network identity in evidence.

## Completion conditions

- The review inventory states why each candidate is extracted or remains
  duplicated/device-specific.
- Every extraction is backed by identical demonstrated contracts in both
  drivers and leaves their hardware implementations independently replaceable.
- The public WLAN UAPI is unchanged unless a separately approved significant
  interface decision was made.
- Both complete focused suites and both bounded normal-path smoke checks remain
  passing after the final refactor.
- If no extraction meets the threshold, the documented no-change review and
  passing comparison gates complete the Phase; creating a framework is not a
  completion requirement.

## Reconsideration boundary

Stop an extraction if it requires hardware-family conditionals in generic
code, makes one driver own another's lifetime, changes public UAPI without an
approved decision, or cannot preserve both passing normal paths. Keep the
duplicated implementations and record the rejected abstraction rather than
forcing superficial reuse.
