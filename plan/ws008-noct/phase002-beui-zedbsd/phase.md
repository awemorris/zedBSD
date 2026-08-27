# WS008 Phase 002: canonical BeUI zedBSD graphics and evdev backend

Last updated: 2026-08-28

WSID: `ws008`

Phase ID: `p002`

Combined ID: `ws008-p002`

Status: Complete (`q020`)

Parent: [WS008](../ws.md)

Tests: [WS008 test index](../tests/README.md)

## Objective

Move the existing zedBSD BeUI adaptation into canonical Noct, retain
`/dev/graphics` rendering, replace legacy `/dev/console` event/key-state input
with capability-discovered `/dev/input/eventN`, and delete the downstream BeUI
duplicate after parity is demonstrated.

## Dependencies

- `ws008-p001` supplies the canonical zedBSD CMake target and installed CLI.
- `ws006-p001`--`ws006-p004` supply the public evdev profile, input core,
  producers, and console broker software milestone.
- USB HID, Xzed evdev migration, LFB mmap acceleration, and removal of the
  legacy console UAPI are not dependencies.

## Fixed backend contract

- The source resides beside canonical Noct's generic BeUI core and SDL2
  backend and is selected by the `zedbsd` preset. SDL2 remains independently
  buildable and its tests continue to pass.
- Rendering uses public `<zedbsd/graphics.h>` ioctls on `/dev/graphics`:
  enter/get mode, fill, line, pattern, supported blits, glyph, flush, and clean
  close. Unsupported capabilities fail or fall back according to the BeUI HAL
  contract; the backend does not infer LFB access.
- Input scans `/dev/input/eventN`, queries identity/capability bits, and binds
  keyboard and relative/absolute pointer roles by capability. It never assumes
  event0 is the keyboard or event1 is the mouse.
- `EV_KEY` press/release/repeat maintains the BeUI key/button state; `EV_REL`
  and `EV_ABS` update bounded pointer state; changes become visible on
  `SYN_REPORT`. `SYN_DROPPED` causes supported state resynchronization or a
  visible backend reset, never silent stale state.
- Multiple event records and partial reads are handled correctly. Unknown
  types/codes are ignored safely. Device EOF/removal closes that source and
  allows bounded rediscovery without spinning.
- BeUI no longer invokes `ZEDBSD_CONSOLE_POLL_EVENT`,
  `ZEDBSD_CONSOLE_READ_EVENT`, `ZEDBSD_CONSOLE_KEY_STATE`, or
  `ZEDBSD_CONSOLE_DRAIN_INPUT`. Ordinary tty/text-console functionality is not
  removed by this Phase.
- The canonical backend includes zedBSD headers from the target sysroot. It
  does not vendor a copy of UAPI structures or numeric constants.

## Work packages

- [x] Isolate the reusable `/dev/graphics` logic from downstream
      `userland/packages/lang/noct/runtime/platform.c` and implement the
      canonical zedBSD BeUI display HAL.
- [x] Implement bounded event-node discovery, capability selection, keyboard
      state, relative/absolute pointer state, button state, synchronization,
      detach, and cleanup in the canonical backend.
- [x] Add `NOCT_ENABLE_API_BEUI_ZEDBSD` (or an equivalently explicit target
      selection) with invalid-option checks and enable it in the `zedbsd`
      preset.
- [x] Add host tests around backend translation/state with mocked public UAPI
      operations and retain canonical SDL2 BeUI tests.
- [x] Add an amd64 QEMU BeUI probe which performs visible drawing and reports
      injected keyboard and pointer observations through a non-graphical
      evidence channel.
- [x] Remove the downstream BeUI implementation only after the canonical
      backend passes the same graphics operations; retain unrelated package
      target/terminal adapters until their owning work explicitly removes them.
- [x] Run a source audit proving the zedBSD Noct/BeUI path has no legacy
      console event/key-state calls, followed by `make -j16`.
- [x] Mirror the official source change into the integration checkout and
      record the path parity manifest without committing or pushing.

## Acceptance

- [x] `NOCT-T010`: host backend tests cover capability discovery, key
  press/release/repeat, relative and absolute pointer updates, buttons,
  `SYN_REPORT`, `SYN_DROPPED`, partial/multiple records, unknown codes, detach,
  and descriptor cleanup.
- [x] `NOCT-T011`: in `qemu-system-x86_64`, BeUI enters `/dev/graphics`, draws a
  deterministic pattern/image/text sample, flushes, and closes without kernel
  or process error.
- [x] `NOCT-T012`: the same guest run injects keyboard and pointer activity; the
  probe reports the expected BeUI key/button/coordinate transitions while
  ordinary console input remains usable after BeUI exits.
- [x] `NOCT-T013`: a source/object audit finds no legacy continuous-event,
  key-state, or drain-input console ioctl in the installed Noct/BeUI path.
- [x] The canonical SDL2 BeUI focused tests and `make -j16` pass; `make check` is
  not used.

## Completion conditions

- [x] Canonical Noct owns the only zedBSD BeUI backend implementation.
- [x] Graphics and input evidence passes on amd64 QEMU using only public
  `/dev/graphics` and `/dev/input/eventN` UAPIs.
- [x] Dynamic event numbering, synchronization loss, detach, and cleanup have
  deterministic bounded behavior.
- [x] No downstream Noct BeUI code depends on the console event/key-state API.

## q020 completion result

[`q020-p002-beui-evidence.md`](../tests/q020-p002-beui-evidence.md) records the
completed acceptance run. In summary:

- the sanitized host state/wiring corpus passed, as did the generic BeUI core,
  PC-98 GDC, PC-98 Cirrus, and SDL2 dummy-driver regressions;
- one amd64 `qemu-system-x86_64` run produced the graphics markers
  `BEGIN`/`DRAWN`/`CLOSED`, observed injected Shift, relative motion, and left
  button state, and returned to a working text console;
- the deterministic 1024x768 screenshot checker found the expected background,
  pattern, line/glyph, and four BMP colors;
- the legacy-console source/object audit, canonical/package/staged artifact
  identity, 15-path canonical/integration parity check, and `make -j16` passed.

The raw QEMU directory
`plan/ws008-noct/temp/q020-p002-beui.59sAEO/` is ignored, disposable evidence;
the checked-in runner, fixtures, and completion record are the durable
reproduction contract.

## Failure and resume rules

An additive key-code translation or backend-local capability fallback is in
scope. If the public evdev profile cannot express a required state, return the
finding to WS006 rather than inventing a private ioctl. If `/dev/graphics`
requires an LFB mmap change for correctness rather than performance, mark this
Phase `uncleared` and hand it to the dedicated LFB workstream.

Resume from the first failing NOCT-T010--T013 case with its event transcript,
capability dump, and guest serial log.

## q019 stop result

The pre-implementation source/UAPI audit found that the public ABI can express
the required behavior, but its kernel implementation cannot yet supply it:

- `include/uapi/zedbsd/input.h` declares `EVIOCGBIT`, `EVIOCGKEY`, and
  `EVIOCGABS`;
- `src/kern/input-device.c` implements version, identity, device-string, and
  grab requests only, returning `EOPNOTSUPP` for the required queries;
- `struct input_device_info` and the registered keyboard/mouse devices carry
  no capability bitsets, current key state, or absolute-axis descriptors.

Consequently a canonical backend cannot distinguish a keyboard, relative
pointer, and absolute pointer by capability or scale an absolute axis. Event
number, name, or product-ID inference was rejected because it would violate
the fixed contract. The allowed visible reset could handle `SYN_DROPPED`
without `EVIOCGKEY`, but it cannot replace `EVIOCGBIT` or `EVIOCGABS`.

The `/dev/graphics` audit found the required mode, drawing, glyph, flush, and
close operations already sufficient; no LFB mapping is needed for correctness.
No partial canonical backend or new downstream duplicate was added. Resume
after [`ws006-p005`](../../ws006-input/phase005-evdev-capability-state/phase.md)
implements and verifies the frozen evdev capability/state contract. Then run
`NOCT-T010` through `NOCT-T013` as originally specified.
