# WS006 Phase 005: evdev capability and current-state queries

Last updated: 2026-08-28

WSID: `ws006`

Phase ID: `p005`

Combined ID: `ws006-p005`

Status: Planned; Queue-ready

Parent: [WS006](../ws.md)

Tests: [WS006 test index](../tests/README.md)

Consumer handoff: [WS008 p002](../../ws008-noct/phase002-beui-zedbsd/phase.md)

## Objective

Complete the already-published evdev capability and current-state kernel
contract needed by consumers that enumerate dynamic `/dev/input/eventN`
devices. This is a dependency repair for Noct/BeUI, not a new UAPI design.

## Entry finding

`<zedbsd/input.h>` and the evdev reference declare capability, key-state, and
absolute-axis queries, but the production input core currently implements only
version, identity, device strings, and grab. Registered devices carry no
capability or absolute-axis metadata. Consequently `EVIOCGBIT`, `EVIOCGKEY`,
and `EVIOCGABS` return `EOPNOTSUPP`, and a consumer cannot identify device
roles without relying on unstable event numbers or display names.

## Fixed contract

- Keep all existing public structures, constants, ioctl encodings, event
  numbering, queue semantics, and producer event values unchanged.
- Registration supplies bounded event-type/code capability declarations and,
  for each supported absolute axis, its `input_absinfo` range metadata.
- `EVIOCGBIT(0, length)` reports supported event types. Per-type
  `EVIOCGBIT(type, length)` reports the registered codes. Copyout truncates to
  the encoded caller length, zero-fills the returned range, and never exposes
  kernel padding.
- `EVIOCGKEY(length)` reports core-maintained current key/button state.
  Release clears a bit, press sets it, and repeat leaves it set.
- `EVIOCGABS(axis)` reports registered bounds and the core-maintained current
  value. An unsupported type, code, or axis returns the documented unsupported
  ioctl/error result without changing caller memory.
- The console keyboard advertises its published `KEY_*` subset. The existing
  mouse advertises `EV_REL` X/Y and its three buttons. No capability is inferred
  from device names, product IDs, or event-node positions.
- Properties, LED/repeat control, USB HID, multitouch policy, event injection,
  hotplug slot reuse, and new event constants are outside this repair unless a
  focused implementation dependency makes a small existing-profile query
  unavoidable. Any remaining declared-but-unimplemented query is documented
  explicitly rather than reported as operational.

## Work packages

- [ ] Add bounded registration metadata and owned device copies for event/code
      capabilities and absolute-axis descriptors.
- [ ] Maintain current key/button and absolute-axis state in the input core as
      events are published.
- [ ] Implement length-safe `EVIOCGBIT`, `EVIOCGKEY`, and `EVIOCGABS` copyout
      paths and align unsupported-request behavior with the published profile.
- [ ] Declare accurate capabilities for the production console keyboard and
      relative mouse without changing their event streams.
- [ ] Add host fixtures for truncation, zero-fill, unknown types/codes,
      press/release/repeat state, absolute value/range updates, and malformed
      registration metadata.
- [ ] Add an amd64 QEMU guest probe that enumerates event nodes by capabilities
      and identifies the production keyboard and relative pointer without
      assuming their numbers or names.
- [ ] Run the existing queue/keymap/ABI fixtures and `make -j16`; do not use
      `make check` or `.internal/`.
- [ ] Update the evdev reference with the exact operational ioctl subset and
      retained residuals.

## Acceptance

- `IN-T11`: strict host fixtures prove registration validation, bit ordering,
  short and oversized query lengths, zero-fill, current key/button state, ABS
  range/value state, and unsupported-query behavior without out-of-bounds
  access or information disclosure.
- `IN-T12`: a disposable amd64 QEMU image reaches normal init/login, and a
  guest probe discovers one keyboard and one relative pointer solely from
  `EVIOCGBIT`; their reported code sets match the production producers.
- Existing `IN-T00`, queue, keymap, console coexistence, and production build
  gates continue to pass.

## Completion conditions

- A source consumer can identify keyboard, relative-pointer, and supported
  absolute-pointer roles through the public capability ioctls.
- Current key/button state and registered ABS ranges/values are queryable after
  normal events and synchronization loss.
- Existing keyboard, mouse, console, and independent-reader behavior does not
  regress.
- The implementation and reference agree about every ioctl claimed operational
  at Phase completion.

## Failure and resume rules

Repair ordinary input-core metadata, bitset, copyout, state-tracking, and
producer declaration defects in this Phase. If completion requires changing a
public structure/ioctl encoding, inventing a stable event-number policy, or
choosing new USB/multitouch semantics, mark the Phase `uncleared` and request
human review. On success, resume `ws008-p002`; no BeUI name/number fallback is
permitted.
