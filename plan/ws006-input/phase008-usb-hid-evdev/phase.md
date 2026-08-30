# WS006 Phase 008: USB HID evdev producers and hotplug

Last updated: 2026-08-30

WSID: `ws006`

Phase ID: `p008`

Combined ID: `ws006-p008`

Status: planned; depends on `ws006-p006` and `ws006-p007`; not queued

Parent: [WS006](../ws.md)

Tests: [WS006 test index](../tests/README.md)

## Objective

Bind standards-compliant USB HID keyboard and pointer interfaces through the
general USB function framework, publish truthful dynamic evdev devices, feed
USB keyboard events into the existing console broker, and retire every URB and
device reference safely across close, unplug, reset, and shutdown.

This Phase completes the initial USB HID producer implementation.  It does not
remove the legacy `/dev/console` event UAPI; that deletion occurs only in
`ws006-p009` after all consumers have migrated.

## Dependencies

- `ws006-p006`: truthful per-source state, internal console subscription, and
  multi-source detach ownership.
- `ws006-p007`: bounded HID descriptor/report and boot-layout decoding.
- WS004 `p010`, `p011`, and `p015`: generic USB function matching,
  configuration/alternate selection, concurrent interrupt URBs, checked
  endpoint admission, cancellation, and callback drain.
- WS004 xHCI USB-root milestone: QEMU can exercise HID and Mass Storage on one
  controller without replacing the production boot path.

## Frozen driver boundary

- Match USB HID interfaces by class/subclass/protocol and validated HID,
  report, and endpoint descriptors.  Do not bind by QEMU VID:PID, port number,
  interface number, or fixed configuration index.
- Claim only the matched interface.  Require one usable interrupt-IN endpoint;
  validate packet size and interval before submitting an URB.  A sibling HID,
  Storage, NCM, or unsupported interface remains independently bindable.
- Fetch the report descriptor through a bounded control request and parse it
  with p007 before registering an event device.  Boot-interface support uses
  an explicit checked `SET_PROTOCOL` choice and the matching fixed layout;
  malformed report protocol does not silently become boot protocol.
- Keyboard state is the difference between complete accepted reports.  Emit
  press and release transitions plus one `SYN_REPORT`; duplicate usages and HID
  rollover/error usages do not fabricate transitions.  Disconnect clears held
  keys exactly once.  Initial v1 does not expose LED/output reports or
  `EVIOCSREP`.
- Relative mouse reports emit signed X/Y/wheel deltas and button transitions.
  Absolute tablet reports register and update the descriptor's validated
  X/Y ranges.  Each interface owns a separate event device and separate state.
- Register with `BUS_USB`, truthful vendor/product/version, bounded descriptor
  strings, and a topology-derived physical path.  Event numbers remain dynamic
  and may be reused only after complete removal; consumers must discover by
  capability.
- One admitted interrupt URB owns its transfer buffer until checked completion
  or cancellation and callback drain.  Completion performs bounded publication
  and rearm work; malformed/short reports increment a bounded diagnostic/error
  path without losing terminal ownership or spinning.
- USB keyboards enter the console through p006's internal input subscriber.
  The console never opens the HID event node and an evdev grab does not disable
  ordinary console text input.

## Scope exclusions

- `/dev/hidraw`, user event injection, output/feature reports, keyboard LEDs,
  force feedback, gamepads, consumer/media keys, touch/multitouch, and wireless
  HID transports.
- Stable `eventN` numbering or names as an application selection policy.
- Vendor-specific quirks not supported by a separately approved exact-device
  Phase.
- Removal of legacy console event/key-state interfaces.

## Detailed procedure

1. Add one USB HID class-driver registration and build/menu integration for the
   supported platforms, using the existing general USB driver registry.
2. Implement transactional probe: interface match, HID/report descriptor
   retrieval, p007 parse, endpoint validation, private state allocation,
   `input_device` registration, and initial interrupt submission.  Every
   failure unwinds in reverse without a visible half-bound event device.
3. Implement keyboard and pointer report-to-event state transitions, including
   multiple simultaneous keys/buttons, relative and absolute axes, report IDs,
   rollover rejection, short/error completion, and exact synchronization.
4. Route keyboard events through the p006 internal console subscriber while
   preserving independent evdev readers and per-source state.
5. Implement close, reopen, reset, unplug/reinsert, forced removal, and terminal
   shutdown using USB callback drain and input-device terminal publication.
6. Extend production-source fake-HCD fixtures with multiple HID interfaces,
   composite HID plus Storage, failed descriptor/control/endpoint stages,
   completion/rearm races, held-key/button detach, callback blockage, and
   repeated event-slot reuse.
7. Add an IN-T41 runner using `qemu-system-x86_64`, q35, xHCI, production
   `usb-kbd`, `usb-mouse`, and `usb-tablet` models.  Verify capability-only
   discovery, keyboard console input, evdev records, relative/absolute motion,
   hotplug/reinsert, and concurrent xHCI USB-root storage.
8. After automatic gates pass, request one bounded physical USB
   keyboard/mouse observation on supported hardware.  Intermediate development
   is not blocked on repeated human boots; final WS acceptance records the
   exact frozen image and devices.

## Verification gates

- IN-T40 remains passing against the production parser.
- IN-T41 covers keyboard, mouse, tablet, composite binding, hotplug, detach,
  event delivery, and console coexistence under QEMU xHCI.
- Existing USB function/binding/concurrent-URB/storage and evdev
  layout/queue/capability/keymap tests pass in their declared ordinary and
  sanitizer modes.
- Default `make -j16`, configured amd64 and i386 builds, a disposable amd64
  xHCI USB-root boot, and `git diff --check` pass.  `make check` and
  `.internal/` are not used.
- IN-T42 records one physical supported-hardware keyboard/mouse hotplug and
  input observation, or the Phase remains honestly partial at the automatic
  milestone.

## Completion conditions

- Standards-based USB keyboard, relative mouse, and absolute tablet interfaces
  publish truthful capability-discovered evdev devices under QEMU.
- USB keyboard input operates both the console and independent evdev readers;
  multiple keyboards and pointers retain independent state.
- Malformed descriptors/reports and every injected attach/rearm/cancel error
  fail safely without a stale node, URB, callback, key/button state, or DMA
  owner.
- Hot-unplug/reinsert and shutdown are bounded and pass with concurrent USB
  Storage.
- One supported physical USB keyboard and pointer acceptance is recorded.  If
  unavailable, the automatic software milestone is recorded without falsely
  completing the WS hardware condition.

## Reconsideration boundary

Stop and mark the Phase `uncleared` if a supported device requires a public
evdev ABI expansion, an unbounded report layout, output reports before input
can operate, or a change to the general USB ownership contract.  Extract the
owning change rather than adding a VID:PID success path or weakening teardown.
