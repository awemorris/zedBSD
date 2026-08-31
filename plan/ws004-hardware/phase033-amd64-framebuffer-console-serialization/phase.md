# WS004 Phase 033: amd64 framebuffer console serialization

Last updated: 2026-08-31

Phase ID: `ws004-p033`

Work item: `HW-29`

Primary test: `HW-T27`

Status: complete (`q047`)

Parent: [WS004 hardware expansion](../ws.md)

Tests: [WS004 test index](../tests/README.md)

## Objective

Make the amd64 PC/AT console output path safe when task, IRQ, and SMP logging
overlap.  Serialize every shared cursor, cell, and framebuffer mutation with
one early-boot-safe lock, keep ordinary implementation helpers non-recursive,
and reject every out-of-range cell or framebuffer write.

This Phase owns the console defect exposed by `ws004-p031` QEMU hotplug stress.
It does not change USB ownership, HID input policy, the PS/2 repeat producer,
the public console API, or another architecture's console.

## Observed defect and diagnosis

The `build/q047-p031-debug14` UHCI run completed two HID detach cycles, then
failed during the third add while USB and repeated keyboard output were
concurrent.  Its fault RIP `0xffffffff80210443` maps to the framebuffer color/
cell path.  A separate audit found no unmatched checkpoint URB callback,
cancel, drain, worker, buffer, or device lifetime at that boundary.

The amd64 console had no output serialization.  `newline()` temporarily made
`cursor_row == HAL_CONS_ROWS` before scrolling it back into range.  A second
CPU or interrupt could observe that transient value and pass it to
`write_cell()`, indexing one row beyond `framebuffer_cells`.  That array is
adjacent to framebuffer state, so the write could corrupt the framebuffer
pointer and produce the observed fault.  The long keyboard repeat and USB
diagnostics enlarged the race window; they are not accepted as the memory-
safety root cause.

## Frozen implementation boundary

- Use one leaf HAL output lock for cursor, mode, cell shadow, framebuffer
  pointer, cursor drawing, clear, scroll, suspend, resume, and initialization
  state.
- Disable local maskable interrupts before lock acquisition and restore the
  caller's prior interrupt state only when its matching acquisition returns.
- Identify the physical CPU with CPUID rather than scheduler or GS-based
  per-CPU state, because `pcat_cons_init()` runs before per-CPU selection.
- Store owner identity and recursion depth in one atomic word.  Public output
  routines normally call only non-recursive `_locked` helpers.  Same-CPU
  recursion remains solely so a terminal fault/NMI raised while that CPU owns
  the console can report instead of self-deadlocking.
- Keep cross-CPU ownership exclusive.  The panic path prints before sending
  the terminal NMI broadcast; an NMI on the current owner may recurse and all
  other owners wait until the current bounded cell operation releases.
- Reject an invalid row or column in the cell writer.  Before drawing, require
  non-null framebuffer state and prove width, height, stride, byte extent, and
  final pixel position are inside the handed-off framebuffer.
- Validate the same framebuffer geometry during console initialization.  An
  invalid optional framebuffer falls back to the existing VGA console path.
- Add no public header, UAPI, scheduler lock, allocation, or sleepable wait.

## HW-T27 verification

The focused host runner is:

```sh
TMPDIR="$PWD/build/q047-tmp" \
  plan/ws004-hardware/tests/run-amd64-console-output-host-test.sh
```

It links the production `cons.c`, drives four synthetic CPUs concurrently,
exercises same-CPU fault-style recursion at the former transient final row,
checks invalid positioned writes, and surrounds the framebuffer with canaries.
The ordinary and ASan/UBSan variants must pass.  The existing WS006 input
ownership/resynchronization runner must also pass because output locking must
not change the independent input queue.

The production amd64 console object must compile with the q047 legacy-HCD
configuration and `-Wall -Wextra -Werror`.  Final evidence is one fresh run of
the maintained HW-T25 QEMU matrix using an image which contains this fix.  Its
standalone UHCI and paired EHCI/UHCI cells must finish their existing hotplug,
storage, login, and reboot contract with no amd64 fault, invalid framebuffer
access, corrupted console state, or console-lock stall.  HW-T27 shares that
single final run; it does not add another repetition campaign.

## Completion evidence

- HW-T27 ordinary host run: pass.
- HW-T27 ASan/UBSan host run: pass.
- Existing WS006 input ownership/resynchronization suite: pass.
- Configured amd64 production `cons.o` build and scoped diff check: pass.
- Fresh forced-canonical HW-T25 QEMU evidence:
  `build/q047-legacy-hcd-final4/results.tsv` records `pass` for standalone
  UHCI and paired EHCI plus three UHCI companions under QEMU 10.0.11.
  Each cell completed 11 hotplug cycles (generation 12), preserved the
  throttled Storage payload, and reached checked reboot with no `amd64 fault`,
  panic, console corruption, or console-lock stall.  The immutable source
  image SHA-256 before and after the run is
  `9ae50624558cec3d7b6d83d6bd2ff1862c47042dcddbbbb5f8368822ef1e741a`.

## Completion conditions

- One lock protects all amd64 PC/AT output state without depending on
  initialized scheduler or per-CPU GS state.
- Ordinary helpers do not reacquire the public lock, while terminal same-CPU
  fault/NMI output cannot self-deadlock.
- Invalid cursor/cell values and inconsistent framebuffer geometry cannot
  write outside the cell shadow or handed-off pixel extent.
- HW-T27 ordinary and sanitizer gates, the WS006 input regression, configured
  production compilation, and the fresh shared HW-T25 QEMU matrix all pass.

Completion fixes only this amd64 framebuffer-console defect.  It does not by
itself complete `ws004-p031`, USB HID, or the unrelated repeated-key diagnosis.
