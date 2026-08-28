# WS018 Phase 007: Xzed evdev-only consumer

Last updated: 2026-08-29

WSID: `ws018`

Phase ID: `p007`

Combined ID: `ws018-p007`

Status: Complete (`q026`)

Parent: [WS018](../ws.md)

Shared tests: [WS018 test index](../tests/README.md)

## Objective

Move every Xzed keyboard and pointer input path to capability-discovered
`/dev/input/eventX` devices before the kernel removes the legacy mouse and
console continuous-event interfaces.  Xzed must not assume event numbering,
device names, or a platform-specific producer, and it must not retain a hidden
`/dev/mouse` or console-event fallback.

This is the consumer-side deletion gate for `ws018-p008`.  The old kernel
interfaces remain present during this Phase only so a failed consumer migration
does not simultaneously destroy its rollback path.

## Dependencies

- `ws018-p001` has established the canonical `src/drivers/` paths used by the
  fixtures and source audit.
- WS006 Phases 002--005 are complete: dynamic event nodes, independent reader
  queues, capability discovery, key/button state, relative axes, and absolute
  metadata are available.
- WS007 provides the current Xzed startup and pointer-coordinate acceptance
  baseline.

`ws018-p008` depends on this Phase.  This Phase does not depend on the later
mouse-source relocation and must run while both old and new kernel interfaces
can still be compared.

## Current implementation boundary

Xzed currently opens `/dev/console`, switches it to the zedBSD continuous-event
mode for keyboard input, and opens `/dev/mouse` for relative motion/buttons.
The kernel already exposes a keyboard and relative pointer through evdev, but
event numbers are dynamic.  WS006's capability probe demonstrates the intended
discovery rule: enumerate decimal `eventN` directory entries and derive roles
from `EVIOCGBIT`, not `EVIOCGNAME`, `EVIOCGPHYS`, or a fixed path.

## Fixed design

- Enumerate `/dev/input` with `opendir()`/`readdir()`.  Accept only names that
  exactly match `event` followed by one or more decimal digits, and safely join
  them below `/dev/input`.
- Open candidates read-only, nonblocking, and close-on-exec.  Classify each
  descriptor solely from `EVIOCGBIT` and, for absolute devices, `EVIOCGABS`.
- Keep every usable keyboard and pointer descriptor rather than selecting
  `event0`, the first name, or a human-readable identity.  A future USB device
  or hot-plug rescan must not be made impossible by this representation.
- Keyboard role requires the declared keyboard capability subset already
  frozen by WS006.  Translate `EV_KEY` press (`1`), release (`0`), and repeat
  (`2`) to X events and maintain X modifier state from actual modifier-key
  transitions.  `SYN_REPORT` terminates an input frame; unknown codes are
  ignored without corrupting state.
- Pointer roles may be relative (`REL_X`, `REL_Y`, mouse buttons) or absolute
  (`ABS_X`, `ABS_Y` plus pointer buttons).  Relative deltas retain the current
  signed, clamped coordinate behavior.  Absolute values are scaled from the
  queried axis range to the current screen bounds with overflow-safe arithmetic.
- Accumulate motion/button changes within an evdev frame and publish them on
  `SYN_REPORT`, preserving button-edge ordering and the existing X pointer-grab
  semantics.  A `SYN_DROPPED` frame triggers state resynchronization through
  `EVIOCGKEY` and, where available, current absolute-axis values; it must not be
  interpreted as ordinary motion.  Resynchronize physical keys, modifiers,
  pointer buttons, and axes.  Keep the last CapsLock toggle observed through
  ordinary events: do not add `EVIOCGLED`, and do not infer a new toggle while
  recovering a dropped frame.
- Do not request an evdev exclusive grab merely to reproduce the old path.
  Kernel console brokering remains independent and Xzed's X-level pointer grab
  remains an X server concern.
- Remove all Xzed use of `struct mouse_event`, `<zedbsd/mouse.h>`,
  `console_input_event`, `ZEDBSD_CONSOLE_*` input-mode ioctls, `/dev/mouse`, and
  `/dev/console` as an event source.  There is no compatibility fallback.
- Failure to discover at least one keyboard and one pointer is a visible Xzed
  startup error.  Do not silently launch a desktop with an unusable input path.

## Detailed procedure

1. Freeze a focused Xzed input-consumer boundary that owns event-node
   enumeration, capability classification, per-descriptor read framing, and
   key/pointer state.  Keep it userland-local; do not add a kernel convenience
   API or identify devices by names.
2. Replace the fixed `console`/`mouse` members in the Xzed server with a bounded
   collection of classified evdev descriptors and their per-device state.
   Reject descriptor-count or path-length overflow cleanly.
3. Port keyboard translation to evdev `KEY_*` codes.  Preserve the existing X
   keycode contract, modifier masks, focus selection, timestamps, and repeat
   behavior, including left/right modifier inputs.
4. Port relative-pointer processing to `EV_REL`/`EV_KEY` frames and preserve
   coordinate clamping, motion compression, focus raising, button numbering,
   and X pointer grabs.
5. Add absolute-pointer processing using `EVIOCGABS` ranges.  Zero-width or
   malformed ranges are rejected as that device's role rather than divided by
   zero or accepted with guessed coordinates.
6. Integrate every input descriptor into the existing poll loop.  Handle
   partial reads, whole-`struct input_event` validation, `EAGAIN`, `EINTR`,
   `POLLHUP`, and device disappearance without indexing stale storage.
7. Remove console-mode save/restore and all legacy mouse code from Xzed.  Close
   every event descriptor on normal exit and initialization failure.
8. Add WS-owned focused fixtures during the authorized implementation Queue:
   capability classification independent of numbering/name; keyboard/modifier
   translation; relative frame aggregation; absolute scaling/bounds; and
   `SYN_DROPPED` recovery.
9. Build and run the focused tests, then use a disposable amd64 image and
   `qemu-system-x86_64` to launch Xzed, inject keyboard and PS/2 pointer input,
   exercise buttons/motion, and terminate the session normally.

## Verification contract

The implementation Queue must record at least:

```sh
rg -n '/dev/mouse|zedbsd/mouse|ZEDBSD_CONSOLE_(GET|SET)_INPUT_MODE|\
console_input_event' userland/X11/xzed
make -j16
git diff --check
```

The `rg` command must return no live Xzed source reference.  Focused host tests
must prove classification is unchanged when event numbers and identity strings
are permuted.  Runtime evidence must show that Xzed starts through its normal
`startx` path, receives keyboard press/release/repeat and relative pointer
motion/button events from production evdev nodes, and exits without leaving a
console input mode altered.  Absolute-pointer behavior may use a focused
synthetic stream until a production absolute producer exists, but it is part of
the completed consumer implementation rather than an unimplemented branch.

Do not use `make check` or repository `.internal/` material.  QEMU images used
for runtime mutation are disposable copies under the WS temporary area.

## Completion conditions

- Xzed discovers keyboard and relative/absolute pointer roles by capability,
  with no fixed `eventN`, name, physical-path, or platform assumption.
- Keyboard, modifier, repeat, relative motion, absolute scaling, button, frame,
  and resynchronization fixtures pass.
- The normal amd64 Xzed desktop is operable through production evdev keyboard
  and relative-pointer nodes under QEMU.
- Xzed contains no `/dev/mouse` or console continuous-event implementation or
  fallback, while the kernel legacy paths are intentionally still present for
  the next Phase's deletion gate.
- KA-T060, `make -j16`, and `git diff --check` pass with recorded evidence.

## Reconsideration conditions

Mark this Phase `uncleared` and request human review if correct X keyboard
semantics require changing the published evdev UAPI, if dynamic directory
discovery cannot be implemented with the existing libc/devfs contracts, or if
absolute coordinates cannot be recovered from the existing `EVIOCGABS`
contract.  Do not resolve such a finding by restoring `/dev/mouse`, hard-coding
an event number/name, or continuing to consume console continuous events.

## Execution result

Completed on 2026-08-29 without reaching a reconsideration condition.

- Xzed now owns a userland-local evdev consumer split between the production
  POSIX/ioctl adapter and a testable discovery/framing/state machine.  It
  enumerates only exact decimal `eventN` entries, classifies roles only from
  `EVIOCGBIT` plus valid `EVIOCGABS` ranges, retains up to eight useful
  descriptors, and requires at least one keyboard and one pointer.  It never
  requests an identity string or exclusive grab and has no fixed event number.
- Key processing preserves the existing converted ASCII-plus-8 and navigation
  `0xe0`--`0xe9` X keycodes.  Press, release, repeat, left/right Shift,
  Control, and Alt are tracked per device; CapsLock toggles on fresh ordinary
  presses only.  Drop recovery uses `EVIOCGKEY` for physical state while
  retaining the last observed CapsLock toggle and makes no `EVIOCGLED` query.
- Relative and absolute pointer records are accumulated through `SYN_REPORT`.
  Relative sums and final coordinates clamp safely, absolute scaling uses
  positive `uint64_t` arithmetic across the full signed 32-bit axis range,
  button edges remain ordered, and Xzed's focus, raise, motion compression,
  and pointer-grab behavior remain at the X layer.  Arbitrarily split byte
  reads are carried per descriptor; `EINTR`, `EAGAIN`, EOF, `POLLHUP`, malformed
  terminal records, descriptor compaction, and required-role disappearance
  have bounded handling.
- KA-T060's production-linked host runner passed both its ordinary `-Werror`
  build and ASan/UBSan build.  It covers permuted event numbers, rejected
  non-event names, multiple keyboards/pointers, ASCII/navigation/repeat/
  modifier/Caps behavior, REL aggregation and ordered buttons, full-range ABS
  scaling including an `INT_MAX` screen extent, `SYN_DROPPED` key/button/axis
  recovery, Caps retention, byte-split reads with `EAGAIN`, drain-before-remove
  HUP, and incomplete-record EOF.  Its source audit found no legacy input path,
  fixed event number, identity ioctl, or evdev grab.
- A fresh configured `make -j16` passed, as did the focused Xzed production
  link/user-ELF check, the required legacy-path `rg` audit with no matches, and
  `git diff --check`.
- The amd64 runtime used a disposable image below
  `plan/ws018-kernel-architecture/temp/q026-p007-runtime.UBVbt3`.  Literal
  `startx` reached the normal zwm/zshell/zterm session and Xzed reported
  `/dev/input/event0 keyboard` plus `/dev/input/event1 relative-pointer` from
  capability discovery.  Injected PS/2 input entered zterm through Xzed:
  ordinary keys plus Shift produced `p007key`, repeated make codes produced
  `aaa`, relative movement changed the captured cursor position, and a left
  press/release restored zterm focus before the typed `kill 12` terminated
  Xzed by SIGTERM.  The original console shell then returned its prompt.  The
  evidence log SHA-256 is
  `beabd5b4ccc82d426d94cb2cdc114d40bc62aa1e9c2d4ada1911e7d3f5a95dac`;
  stable before/after REL screenshots have SHA-256
  `04ccc6c05e610cc1db6a5678cac08fdc13a7f70379c325afd1d5ec76810d8f2b`
  and `fbc25af6058ae868458e84ec45d0dbdaa68e1dee2b432af79327743972105e0c`.
  Xzed made no console-mode ioctl, and normal exit required no mode restore.

The production kernel still intentionally contains the legacy console-event
and `/dev/mouse` producers.  Their relocation and deletion remain the ordered
scope of `ws018-p008`; this Phase changed only the Xzed consumer.
