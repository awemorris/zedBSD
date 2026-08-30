# WS007 Phase 004: PC-98 Xzed mouse exact reproduction

Last updated: 2026-08-31

Phase ID: `ws007-p004`

Status: Uncleared (`q043`); every locally constructible cell passes, while
the reported interactive-GUI failure remains unreproduced

Parent: [WS007](../ws.md)

Tests: [WS007 test index](../tests/README.md), extending `GFX-T02`

## Objective

Reconcile the user's current report that PC-98 Xzed mouse input is unusable in
QEMU with the same-day managed production-path result which moves the cursor
exactly `(320,240) -> (420,290)`. Freeze every relevant executable, image,
command-line, GUI, and input-injection condition; reproduce the failing case;
then localize it before changing code.

This is not authorization for a speculative second PIC, evdev, bus-mouse, or
Xzed repair. If the failure cannot be reproduced from a precisely identified
environment, retain it as `uncleared` with the passing evidence and request
only the missing reproducer facts.

## Established boundary

- q039 p003 repaired the demonstrated slave-PIC cascade defect. Its focused
  mask fixture and production qemu-pc98 path passed.
- A fresh audit of the current `build/pc98/hdd-image.img` again reached Xzed
  with `/dev/input/event1`; five monitor `mouse_move 20 10` operations moved
  the cursor exactly `+100,+50` without a monitor-side PIC-port write.
- The user independently reports that the PC-98 mouse is not usable in Xzed
  and that the regression is reproducible in QEMU.
- Those facts do not prove that either observation is wrong. Image revision,
  qemu-pc98 binary, machine flags, display backend, grab/focus state, host
  pointer events, or guest input-node selection may distinguish the paths.

## Fixed reproduction record

Every run records these values before interpretation:

- repository commit and dirty-state summary;
- SHA-256 and size of `build/pc98/hdd-image.img` and its disposable run copy;
- SHA-256/version of `build/qemu-pc98/build/qemu-system-i386`;
- selected PC-98 configuration and installed Xzed binary hash when available;
- complete QEMU argv, display backend, monitor transport, and host GUI/window
  system;
- whether the QEMU window was focused/grabbed and whether movement came from
  monitor `mouse_move`, the host pointer, or both;
- the guest `/dev/input/eventN` inventory and the node actually opened by
  Xzed.

The maintained baseline argv is:

```text
build/qemu-pc98/build/qemu-system-i386
  -M pc9821,pegc=off,coregraph=on
  -cpu 486 -m 64M -smp 1
  -drive if=ide,bus=0,unit=0,format=raw,file=<disposable-image>
  -display none -serial none
  -debugcon file:<debug-log> -monitor stdio -no-reboot
```

The deterministic oracle logs in as root, launches `Xzed`, captures the
standard cursor at `(320,240)`, injects five `mouse_move 20 10` commands, and
requires one cursor at `(420,290)`. A second run changes only `-display none`
to the project's supported interactive GUI backend and repeats monitor
injection before testing focused/grabbed host-pointer movement. Each changed
condition is a separate recorded cell; do not change image and QEMU flags at
the same time.

## Diagnostic plan

1. Re-run the headless managed baseline on a disposable image and save its
   before/after screen captures, guest debug log, and cursor-oracle result.
2. Repeat with the interactive GUI backend while retaining the same machine,
   CPU, memory, SMP, drive, and guest sequence. Test monitor injection first,
   then focused/grabbed host-pointer input.
3. If a cell fails, localize the first missing transition in order:
   PC-98 bus-mouse sample/IRQ13, master IRQ7 cascade delivery, evdev record
   publication, Xzed event-node selection/read, then cursor state/redraw.
   Capture counters or bounded debug markers at those boundaries; do not add a
   new public UAPI merely for diagnostics.
4. If one exact failing cell identifies a defect wholly within the p003
   producer/cascade or Xzed evdev path, apply only the demonstrated repair and
   retain a failing-before/passing-after fixture. If it identifies a distinct
   QEMU GUI/backend or wider input-model issue, extract that implementation as
   a new Phase rather than expanding this diagnostic Phase silently.
5. Re-run the headless and interactive cells after any bounded repair, plus
   the PIC lifecycle, input/HID ownership, Xzed coordinate/clamp, immutable
   source-image, and fatal-log gates.
6. If every controlled cell passes, mark p004 `uncleared`, record the exact
   pass matrix, and request the user's complete QEMU command, image SHA-256,
   qemu-pc98 binary/version, display backend, and observed pointer behavior.
   Do not claim the user-reported defect is fixed.

The existing Noct runner should be reused when its host CLI is available. An
equivalent explicitly recorded direct QEMU/monitor run may establish the same
diagnostic boundary while WS008 p010 is blocked; this Phase must not alter the
Noct tree or hide that independent build-tool failure.

## Completion conditions

This diagnostic Phase completes only when all of the following hold:

- the formerly passing and newly reported conditions are represented by exact
  immutable image/QEMU/argv/display/input records;
- at least one failing cell is reproduced and the first broken boundary is
  demonstrated, or the external environment difference is identified
  precisely enough to hand off without guessing;
- any in-scope repair has a failing-before/passing-after regression and both
  managed headless and interactive paths pass afterward;
- focused PIC, evdev/input ownership, Xzed pointer, immutable-image, fatal-log,
  and `git diff --check` gates pass.

If all locally controlled cells pass and the user's exact failing environment
is still unavailable, the correct result is `uncleared`, not `completed` and
not a speculative source change.

## Reconsideration boundary

Stop and create a separate Phase if reproduction requires changing the public
evdev ABI, restoring `/dev/mouse`, changing generic QEMU input semantics,
reopening the independent amd64 p002 report, or modifying a driver outside the
PC-98 bus-mouse/PIC/Xzed path. Stop for user evidence only after the automatic
headless and GUI-controlled matrix has been exhausted.

## q043 result (2026-08-31)

No additional mouse, PIC, evdev, or Xzed source change was justified. The
final p024 production image and maintained qemu-pc98 binary were frozen as:

- image: `build/pc98/hdd-image.img`, 135,266,304 bytes,
  SHA-256 `b62c958face27ed31e74e5725117b0dd2cda57cd3f57c33044f984310d9a804e`;
- emulator: `build/qemu-pc98/build/qemu-system-i386`, 84,180,552 bytes,
  SHA-256 `9400ec81d8ce99e89fafa580a5bf6adfaeb9e8be15a8f0eed427710bfd7e12da`,
  reporting QEMU `11.0.93`;
- installed build artifact `build/pc98/bin/Xzed`, 72,408 bytes,
  SHA-256 `8251ac5e2d5fe7b3bbc4ec218f6a81d38226930bfa60e5a3533f410cf7a455fb`;
- repository base `c8176d02b5d9021bff4e9e24ff935d749267b933`, with only the
  concurrently reviewed p024 and q043 planning edits dirty before the run.

The exact passing invocation was:

```text
build/qemu-pc98/build/qemu-system-i386
  -M pc9821,pegc=off,coregraph=on
  -cpu 486 -m 64M -smp 1
  -drive if=ide,bus=0,unit=0,format=raw,file=<disposable-copy>
  -display none -serial none
  -debugcon file=<debug-log> -monitor stdio -no-reboot
```

It reached login, launched Xzed, found the standard cursor at `(320,240)`,
and five HMP `mouse_move 20 10` operations moved it exactly to `(420,290)`.
The production source image remained byte-identical, and the debug log
contained no `fatal:`, `panic:`, or VFS-initialization failure. A preliminary
run of the preceding image hash `4982d051...` produced the same exact motion;
it is not counted as immutable-source evidence because p024 intentionally
published the final image while that run was active.

Focused gates also pass for the ordinary and UBSan PIC lifecycle fixture, the
ordinary and ASan/UBSan Xzed coordinate/clamp fixture, independent PC/AT and
PC-98 input/HID producer ownership in ordinary and sanitized builds, and the
ordinary and sanitized Xzed evdev consumer/disconnect matrix.

The maintained qemu-pc98 build reports only `none` from `-display help` and
rejects `-vnc` as an invalid option. Therefore no interactive window,
focus/grab, or host-pointer cell exists locally; only deterministic monitor
injection can be exercised. The maintained Noct runner is independently
unavailable because the pinned host tool lacks its required helper/CLI
contract (`zbShellQuote` is unresolved without the already-rejected
`--path`); q043 used the Phase-authorized direct QEMU sequence and did not
modify or work around Noct.

The Phase remains `uncleared`. Resume only with the user's exact failing QEMU
command, image SHA-256, qemu-pc98 binary SHA-256/version, display backend,
focus/grab state, and observed pointer behavior. If those facts identify a
GUI/backend difference, extract it as a new Phase. Do not change the current
passing PC-98 input path speculatively.
