# WS006 Phase 006: input truthfulness and multi-source ownership

Last updated: 2026-08-31

WSID: `ws006`

Phase ID: `p006`

Combined ID: `ws006-p006`

Status: complete automatic/source milestone (`q044`); QEMU acceptance is
deferred by the known Noct post-link checker incompatibility

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

## Producer inventory and result

| Producer | Source truth | Result |
| --- | --- | --- |
| amd64 and i386 PC/AT keyboard | Physical scan make, break, repeat, and lock state | Preserved as stable string symbols; a bounded overflow snapshot repairs state without replaying tty text. |
| PC-98 keyboard | Physical make, break, typematic repeat, Caps, and Kana | Converted to shift-independent `jis-*` position symbols. Caps/Kana toggle only on first make. ASCII key-state queries retain explicit aliases. |
| X68000 keyboard | Physical make, break, repeat, Caps, and keypad positions | Converted to shift-independent `jis-*`/`jis-kp-*` symbols. Release retains the original physical position after modifier changes. |
| Raspberry Pi 4 UART console | Characters only; no physical break or repeat | Declared as text-only and registered as a momentary source; every accepted press and synthetic release is one report. |
| sun4u UART console | Characters only; no physical break or repeat | Declared as text-only and registered as a momentary source; every accepted press and synthetic release is one report. |
| PC/AT PS/2 and PC-98 bus mouse | Backend-local buttons and relative axes | Each registered device keeps independent capability/current state; two-pointer overlap and held-button detach are covered by the input-core fixture. |

The input core now has a fixed eight-slot kernel-internal subscriber registry.
Publication invokes no callback while holding an input-device state lock, and
subscriber removal is the join point for every admitted bounded callback.  The
console subscribes at this boundary and keeps translation, modifier, active
key, resynchronization, and detach state per source.  It no longer opens or
assumes an evdev node for keyboard delivery.

Normal removal first publishes terminal releases for held evdev and
console-only keys.  Producer `open`/`close` admission is retired and joined
before unregister returns; callback pairs must be both present or both absent.
Concurrent unregister callers join the single remover through transferred
producer closes and terminal `DETACH` publication, so every caller returns
only after callbacks have ended and each producer close has run exactly once.
Failed character-device publication rolls back its reserved slot, while
successful node-number reuse remains deliberately outside this Phase in
`ws006-p008`.

Physical HAL overflow uses an internal-only begin/snapshot/end stream.  Begin
records the authoritative Caps/Kana lock state, held modifiers precede other
held-key snapshots, and snapshots update staging state without producing tty
characters or ordinary evdev presses.  End atomically replaces live
`EVIOCGKEY` state and then publishes `SYN_DROPPED` plus `SYN_REPORT`.  A second
overflow restarts the complete snapshot.  The PC/AT rings have 259 entries
(256 scans, two markers, and one sentinel); PC-98 and X68000 have 131 entries
(128 scans, two markers, and one sentinel), so every complete snapshot fits.

Stable JIS position names avoid ambiguous reverse mapping when Shift changes
between press, repeat, and release.  Known console-only positions such as
`jis-yen`, `jis-ro`, `jis-kp-*`, and `kana` remain subscriber-visible but do
not fabricate public evdev capabilities or codes.  The public evdev structures,
constants, and ioctl encodings are unchanged.

## q044 evidence

- `plan/ws006-input/tests/run-input-ownership-host-test.sh`: PASS for the
  subscriber race, production input-device lifecycle and registration races,
  concurrent same-device unregister with a blocked producer close,
  production console broker, two keyboards, two pointers, overlapping state,
  detach-held cleanup, momentary reports, queue overflow, atomic resync
  staging, console drain ownership, PC/AT (amd64 and i386), PC-98, and X68000
  physical HAL overflow paths.  The applicable cells pass both ordinary and
  ASan/UBSan builds.
- IN-T00 passes for LP64 and ILP32; IN-T10, IN-T11 ordinary plus ASan/UBSan,
  and IN-T20 pass with the production helpers.
- The affected amd64 and i386 PC/AT objects compile with their configured
  freestanding `-Werror` flags.  The configured PC-98 build compiles all
  affected objects and links the kernel.  Full amd64, i386 PC/AT, and PC-98
  kernel links also succeed before the existing host Noct rejects
  `--path=tools/build` in the post-link checker.
- arm64, sparcv9, and m68k configured object gates could not be rerun because
  their cross compilers are absent on this host.  Their character-only and
  X68000 behavior is covered by the focused host fixtures where possible.
- `git diff --check` passes.  `make check` and `.internal/` were not used.

The amd64 QEMU coexistence boot and IN-T12 rebuild were not rerun: the known
Noct post-link failure deletes the newly linked kernel before an image can be
assembled.  This is an external build-tool gate rather than an input ownership
failure, so the automatic/source milestone is complete but runtime acceptance
is not claimed here.

The legacy PC-98/X68000 scalar `hal_cons_getc()` compatibility path maps named
JIS positions to unmodified base characters and consumes resync records
silently.  It has no in-tree caller and is not the console broker path; exact
historical Shift/Caps/Kana character transformation is therefore not claimed
for that unused scalar fallback.

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

No reconsideration boundary was crossed.  Live USB HID binding and dynamic
event-node teardown/reuse remain in `ws006-p008`; legacy console UAPI removal
remains in `ws006-p009`.
