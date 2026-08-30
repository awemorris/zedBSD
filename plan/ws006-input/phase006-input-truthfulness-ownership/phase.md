# WS006 Phase 006: input truthfulness and multi-source ownership

Last updated: 2026-08-30

WSID: `ws006`

Phase ID: `p006`

Combined ID: `ws006-p006`

Status: planned; not queued

Parent: [WS006](../ws.md)

Tests: [WS006 test index](../tests/README.md)

## Objective

Remove the producer-truthfulness and ownership limitations retained by
`ws006-p004` and `ws006-p005` before USB HID adds more simultaneous input
sources.  Each physical or logical producer must have independent identity,
capabilities, pressed-button/key state, lifetime, and detach cleanup, while the
console consumes keyboard input through a kernel-internal subscriber rather
than by opening an evdev node.

This is an internal input-core and producer-boundary Phase.  It does not add a
new public event code, ioctl, stable event number, or USB HID driver.

## Dependencies

- `ws006-p001` through `ws006-p005`: published evdev profile, independent
  reader queues, production producers, console broker, and capability/current
  state queries.
- `ws018-p007`: Xzed's completed capability-based evdev model is retained as a
  multi-device consumer regression, but this Phase does not modify Xzed.

## Entry findings

- PC/AT preserves make, break, and repeat identity.  PC-98 and X68000 still use
  less complete platform translation, while arm64 and sparcv9 serial-style
  consoles can report only logical character presses.
- Character-only producers currently share the broad console keyboard
  capability declaration.  A press without any possible release can leave
  `EVIOCGKEY` claiming a permanently held key.
- The present console worker owns the only HAL event read and directly emits to
  one keyboard evdev device.  That path cannot also consume a later USB
  keyboard without introducing a second, inconsistent console path.
- Existing mouse backends keep backend-local button state, but the common
  contract does not yet state how multiple pointers, detach with held buttons,
  and per-device state coexist.

## Frozen ownership contract

- One registered `input_device` represents one source identity.  Capabilities,
  current key/button/axis state, readers, grab ownership, and detach state are
  owned by that device and are never stored in a process-wide aggregate shared
  by unrelated producers.
- An event publication is delivered both to that device's evdev readers and to
  explicitly registered kernel-internal subscribers.  The console keyboard
  broker is such a subscriber; it does not open `/dev/input/eventN`, assume an
  event number, or bypass the input core for a USB keyboard.
- Internal subscribers are bounded and nonblocking.  Producer interrupt paths
  cannot sleep on console or reader work.  Subscription removal joins any
  admitted callback before device-owned storage is released.
- Keyboard translation state is per source.  A modifier held on one keyboard
  does not become a fabricated press on another.  The console may derive its
  effective modifier state from the union of live source states, but removal
  of one source clears only that source's contribution.
- A producer with genuine make/break/repeat information publishes those events
  unchanged.  A character-only producer is explicitly a logical momentary-key
  source: one accepted logical character publishes a press followed by a
  synthetic release in the same bounded report transaction.  It advertises
  only the keys its translation table can emit and never claims physical
  repeat or held-key state.
- A pointer source owns its own button state and axes.  Motion from multiple
  sources may be consumed together, but button transitions and
  `EVIOCGKEY` remain attributable to the originating event device.  There is no
  global `last_buttons` shared between devices.
- Normal detach publishes or internally applies releases for every known held
  key/button before the device reaches its terminal reader state.  Forced
  removal still clears kernel-maintained state exactly once and wakes readers;
  it must not call a retired producer.
- Unknown key symbols/usages remain safely ignored.  This Phase does not widen
  the public evdev catalog merely to make a producer appear more capable.

## Detailed procedure

1. Inventory every current console keyboard and pointer producer, its
   make/break/repeat ability, declared capabilities, source identity, and
   shutdown path.  Record character-only sources explicitly.
2. Add a bounded internal input-subscriber contract below the public evdev
   interface.  Prove registration, publication, callback admission, removal,
   and detach ordering without exposing a new user ABI.
3. Move console key translation onto the internal subscriber.  Keep one HAL
   reader per platform, but make that reader only a producer adapter into the
   input core.  Preserve ordinary tty character input and console editing.
4. Give each keyboard source independent translation and modifier state.  Add
   the logical momentary press/release adapter for sources that cannot report
   physical releases, and replace the shared broad capability declaration with
   truthful per-platform declarations.
5. Audit PS/2 and PC-98 pointer state for per-device ownership.  Add common
   detach-state cleanup needed before a USB pointer can coexist with either
   backend; do not reintroduce a generic mouse backend.
6. Extend focused input-core/keymap tests with two keyboards and two pointers,
   overlapping modifiers/buttons, detach while held, subscriber removal racing
   publication, character-only momentary events, and queue overflow.
7. Run the existing IN-T00, IN-T10, IN-T11, IN-T12, and IN-T20 gates, supported
   x86 builds, `make -j16`, and an amd64 QEMU console-plus-evdev coexistence
   boot.  Do not use `make check` or `.internal/` material.

## Completion conditions

- Every registered production source reports capabilities and current state
  that its hardware/adapter can truthfully maintain.
- Character-only input cannot leave a key permanently set, and physical
  make/break sources retain exact press/release/repeat semantics.
- Two keyboard and two pointer sources coexist without cross-device modifier,
  button, grab, queue, or detach-state corruption.
- The console receives keyboard input through the internal input-core
  subscriber while independent evdev readers receive the same source events.
- Detach and subscriber removal have bounded, race-tested terminal ownership.
- Existing evdev ABI, PC/AT console, Xzed, and supported x86 build/QEMU gates do
  not regress.

## Reconsideration boundary

Stop and mark the Phase `uncleared` if truthful behavior requires changing the
published `struct input_event` or ioctl encodings, if a platform needs a new
public key-code policy, or if the console cannot be an internal subscriber
without a wider tty redesign.  Do not solve a character-only source by
inventing unobserved physical state.
