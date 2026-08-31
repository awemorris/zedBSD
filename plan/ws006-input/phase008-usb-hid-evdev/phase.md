# WS006 Phase 008: USB HID evdev producers and hotplug

Last updated: 2026-08-31

WSID: `ws006`

Phase ID: `p008`

Combined ID: `ws006-p008`

Status: planned; the Report-Protocol and stale-fd namespace policies are
resolved, but WS004 legacy-HCD and general reset/STALL prerequisites remain

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

## Resolved policy and pre-Queue prerequisites (2026-08-31)

The project owner resolved the two observable policy choices as follows:

1. **Report Protocol is mandatory.** A boot-subclass keyboard or mouse must
   successfully parse its report descriptor and complete a checked
   `SET_PROTOCOL(REPORT)` before publication. A malformed or unsupported
   Report Protocol descriptor is an attach failure; it must not silently fall
   back to Boot Protocol. The p007 boot-layout parser remains useful test and
   compatibility machinery, but is not the p008 runtime fallback.
2. **Do not reuse an `eventN` while an old fd survives.** Detach unpublishes
   the pathname immediately, while every already-open fd remains attached to
   the terminal old-generation object and may drain its queued events before
   EOF/HUP. Its number remains reserved until the final old fd closes and that
   object is destroyed; only then may a later device reuse the number. A
   workload which retains many stale fds may therefore exhaust the bounded
   event registry, and attach must fail cleanly rather than alias generations.
The initial HCD runtime scope was resolved by the user on 2026-08-31: USB HID
must also work correctly on USB 1.1 and therefore may not ship as xHCI-only.
Before p008 enters a Queue, WS004 must add a focused UHCI/EHCI Phase which
removes the controller-global single-active-request starvation and supplies
the required legacy hotplug lifecycle. Merely compiling p008 for i386 is not
runtime support.

The same audit found that device reset and endpoint-STALL recovery cannot meet
the current completion wording through the existing general USB contract:
`drv_usb_device_reset()` returns `ENOTSUP`, and no checked endpoint reset/toggle
contract is published. A separate WS004 Phase must provide that contract
before p008 enters a Queue.

No source was changed by this audit. Once the WS004 prerequisites are complete,
the remaining implementation path is concrete: early built-in HID registration,
pending activation after input-core readiness, generation-safe input/devfs
publication, an always-on worker-owned interrupt URB per interface, and q35
xHCI keyboard/mouse/tablet plus concurrent USB-root fixtures.

## Frozen driver boundary

- Match USB HID interfaces by class/subclass/protocol and validated HID,
  report, and endpoint descriptors.  Do not bind by QEMU VID:PID, port number,
  interface number, or fixed configuration index.
- Claim only the matched interface.  Require one usable interrupt-IN endpoint;
  validate packet size and interval before submitting an URB.  A sibling HID,
  Storage, NCM, or unsupported interface remains independently bindable.
- Fetch the report descriptor through a bounded control request and parse it
  with p007 before registering an event device. Boot-subclass interfaces then
  complete checked `SET_PROTOCOL(REPORT)` before publication; malformed or
  unsupported report descriptors do not silently become Boot Protocol.
- Keyboard state is the difference between complete accepted reports.  Emit
  press and release transitions plus one `SYN_REPORT`; duplicate usages and HID
  rollover/error usages do not fabricate transitions.  Disconnect clears held
  keys exactly once.  Initial v1 does not expose LED/output reports or
  `EVIOCSREP`.
- Relative mouse reports emit signed X/Y/wheel deltas and button transitions.
  Absolute tablet reports register and update the descriptor's validated
  X/Y ranges.  Each interface owns a separate event device and separate state.
- Register with `BUS_USB`, truthful vendor/product/version, bounded descriptor
  strings, and a topology-derived physical path. Event numbers remain dynamic,
  but a detached number stays reserved while any old-generation fd exists and
  becomes reusable only after final object destruction. Consumers must
  discover devices by capability.
- Extend the append-only character-device registry and mount-time devfs
  snapshot into a checked dynamic lifecycle. Unregister removes the pathname
  from new lookup/enumeration immediately, while every old inode/file retains
  an immutable reference to its old cdev/input generation through final close.
  Re-registering the same name must create a distinct generation and may occur
  only after the input-layer stale-fd reservation releases that event number.
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

1. Add checked dynamic cdev/devfs unregister and generation-aware registration,
   then add input-device reference/finalization ownership so terminal devices
   are freed and their event-number reservation is released only after the
   last old fd closes. Cover lookup, open, directory-enumeration, unmount/reset,
   failed registration, and same-name re-registration races.
2. Add one USB HID class-driver registration and build/menu integration for the
   supported platforms, using the existing general USB driver registry.
3. Implement transactional probe: interface match, HID/report descriptor
   retrieval, p007 parse, endpoint validation, private state allocation,
   `input_device` registration, and initial interrupt submission.  Every
   failure unwinds in reverse without a visible half-bound event device.
4. Implement keyboard and pointer report-to-event state transitions, including
   multiple simultaneous keys/buttons, relative and absolute axes, report IDs,
   rollover rejection, short/error completion, and exact synchronization.
5. Route keyboard events through the p006 internal console subscriber while
   preserving independent evdev readers and per-source state.
6. Implement close, reopen, reset, endpoint-STALL recovery, unplug/reinsert,
   forced removal, and terminal shutdown using the WS004 recovery contract,
   USB callback drain, and input-device terminal publication. Preserve the
   old-generation event number until its final fd closes.
7. Extend production-source cdev/devfs/input and fake-HCD fixtures with dynamic
   pathname removal, an old fd spanning detach, failed lookup after unpublish,
   blocked event-number reuse, final-close destruction, same-number/new-
   generation re-registration, plus multiple HID interfaces,
   composite HID plus Storage, failed descriptor/control/endpoint stages,
   completion/rearm races, held-key/button detach, callback blockage, and
   stale-fd reservation, final-close event-slot reuse, and bounded slot
   exhaustion.
8. Add an IN-T41 runner using `qemu-system-x86_64`, q35, xHCI, production
   `usb-kbd`, `usb-mouse`, and `usb-tablet` models.  Verify capability-only
   discovery, keyboard console input, evdev records, relative/absolute motion,
   hotplug/reinsert, and concurrent xHCI USB-root storage.
9. After automatic gates pass, request one bounded physical USB
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
- A production-source dynamic-cdev/devfs fixture proves immediate namespace
  removal, old-fd isolation, final-close release, and generation-safe
  re-registration without stale operation/data aliases.
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
- Detach removes the event pathname immediately; stale fds drain only their old
  generation, block number reuse until final close, and cannot observe a later
  same-number device.
- One supported physical USB keyboard and pointer acceptance is recorded.  If
  unavailable, the automatic software milestone is recorded without falsely
  completing the WS hardware condition.

## Reconsideration boundary

Stop and mark the Phase `uncleared` if a supported device requires a public
evdev ABI expansion, an unbounded report layout, output reports before input
can operate, or a change to the general USB ownership contract.  Extract the
owning change rather than adding a VID:PID success path or weakening teardown.
