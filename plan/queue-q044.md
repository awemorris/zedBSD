# Queue: bounded HID decoding and truthful input-source ownership

Last updated: 2026-08-31

QID: `q044`

Queue status: finished

Queue finished: **Yes**

Authorization: after the PC-98 corrections, the user directed continuous
execution of the remaining workstreams until a human decision is required.
Both selected WS006 Phases are fully designed, independent implementation
bodies and are prerequisites of the later live USB HID Phase.

Timebox: none. Process p007 then p006 to `completed` or `uncleared`,
synchronize P/W/M, commit locally, and immediately select the next executable
Queue.

Parent: [master plan](master.md)

Previous Queue: [q043](queue-q043.md)

## Purpose

Add the bounded device-independent HID descriptor/report core, then make all
current keyboard and pointer sources truthful and independently owned before
live USB HID devices are attached to evdev and the console.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws006-p007` | [Phase](ws006-input/phase007-usb-hid-parser/phase.md) | completed | The bounded parser, including unsupported-field width validation, passes strict, ASan/UBSan, and analyzer fixtures at 791 checks without public UAPI change |
| 2 | `ws006-p006` | [Phase](ws006-input/phase006-input-truthfulness-ownership/phase.md) | completed | Per-source state, bounded console subscription, transactional overflow resync, and callback/register/unregister ownership pass the focused automatic/source milestone |

## Dependency and ordering decisions

- p007 is independent of current producer ownership and executes first because
  it has no guest-image or Noct dependency.
- p006 consumes no HID parser internals, but its per-source and console
  subscriber result is the second prerequisite of p008.
- p008 is not silently folded into q044. It is selected by the next Queue only
  after both prerequisites have honest final states and its dynamic device
  publication boundary has been re-audited.

## Fixed boundaries

- Public evdev structures, ioctls, and event-code policy do not change.
- HID descriptor/report bytes are untrusted. Frozen size, nesting, report-ID,
  field-count, bit-width, arithmetic, and ownership bounds fail closed.
- The parser emits normalized snapshots only; it does not publish evdev,
  submit URBs, or guess a boot profile after report-protocol failure.
- One input device means one source identity and one state owner. Character-
  only sources are momentary; physical make/break/repeat remains physical.
- The console is a bounded kernel-internal subscriber, never a consumer that
  opens a guessed `/dev/input/eventN` path.
- Dynamic cdev/devfs removal and event-number reuse belong to p008. p006 may
  retain terminal resident device objects but may not claim live USB hotplug.
- Do not modify Noct, use `.internal/`, run aggregate `make check`, or add a
  Python dependency. Use Phase-owned focused fixtures, `make -j16` where the
  known Noct gate permits, configured object builds, sanitizers/analyzer, and
  `git diff --check`.

## Completion definition

q044 is finished when both rows are processed with exact evidence. Full
success makes p008 dependency-ready. A public ABI need, unsupported HID
profile, unbounded parser requirement, or required tty redesign ends the
owning Phase as `uncleared` at its documented reconsideration boundary rather
than expanding this Queue.

## Result

- `ws006-p007` is complete for its device-independent parser boundary. Every
  non-constant Input validates its Logical range against Report Size before
  usage filtering; strict, ASan/UBSan, and GCC analyzer runs each pass 791
  checks.
- `ws006-p006` completes its automatic/source boundary. The production input
  core and console subscriber, two-keyboard/two-pointer ownership, callback
  retirement, concurrent unregister join, and all four physical HAL resync
  fixtures pass ordinary and sanitizer gates.
- LP64/ILP32 evdev layout and existing queue, capability, and keymap gates pass.
  Affected amd64/i386 PC/AT objects and the PC-98 kernel link with configured
  `-Werror` settings.
- The full amd64, i386 PC/AT, and PC-98 builds reach successful kernel link and
  then stop at the already recorded WS008 `MB-008` host-Noct
  `Unknown option --path=tools/build` checker failure. Consequently q044 does
  not claim a fresh image/QEMU run; that external gate does not reopen either
  completed source boundary.
- Public evdev UAPI is unchanged. `git diff --check` passes, and neither
  `make check` nor `.internal/` was used.

Both implementation prerequisites are now dependency-ready for
`ws006-p008`. Live USB HID binding, dynamic event-node removal/reuse, QEMU USB
keyboard/mouse/tablet operation, and physical observation remain p008.
