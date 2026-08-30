# WS006 Phase 007: USB HID descriptor and report core

Last updated: 2026-08-30

WSID: `ws006`

Phase ID: `p007`

Combined ID: `ws006-p007`

Status: planned; not queued

Parent: [WS006](../ws.md)

Tests: [WS006 test index](../tests/README.md)

## Objective

Implement a bounded, device-independent USB HID descriptor and input-report
decoder for the keyboard and pointer subset needed by zedBSD.  Treat every
descriptor and report as untrusted device input and prove the parser without
claiming that a real USB interface is bound or that an evdev node is published.

The result is a reusable parser/decoder boundary consumed by
`ws006-p008`.  USB transport, interface lifecycle, event-device publication,
and console integration remain in that later Phase.

## Dependencies

- `ws006-p001`: the public evdev keyboard, button, relative, absolute, and
  synchronization subset is frozen.
- WS004 `p010`, `p011`, and `p015`: retained USB descriptors, checked endpoint
  ownership, concurrent URBs, interface claims, and callback drain are
  complete.

`ws006-p006` may execute in parallel with this parser Phase, but both must be
complete before `ws006-p008` integrates a live USB producer.

## Supported v1 profile

- HID Usage Tables pages: Generic Desktop, Keyboard/Keypad, and Button.
- Keyboard arrays and modifier variables required by ordinary boot/report
  keyboards.
- Relative X/Y/wheel and pointer buttons required by an ordinary mouse.
- Absolute X/Y plus pointer buttons required by the QEMU tablet acceptance
  device.
- Report IDs, signed logical ranges, bit fields that cross byte boundaries,
  constant padding, Variable versus Array input items, and independent reports
  in one descriptor.
- Fixed-format USB boot-keyboard and boot-mouse decoders as explicit profiles,
  not as guesses applied to a malformed report descriptor.

Consumer control, LEDs/output/feature reports, gamepads, touch collections,
multitouch slots, force feedback, vendor pages, and raw HID access are outside
v1.  An unsupported but well-formed item produces a stable unsupported result;
malformed input produces a stable invalid-data result.  Neither is interpreted
as a different supported device.

## Bounded parser contract

- The complete report descriptor is copied into kernel-owned bounded storage
  before parsing.  Initial limits are 4096 descriptor bytes, 16 nested
  collections/global-stack entries, 32 declared report IDs, 256 decoded input
  fields, and 8192 bits per input report.  All multiplication and bit-offset
  arithmetic is checked before allocation or access.
- Short items validate their encoded payload size before reading.  Long items
  are skipped only when their declared extent is fully present; they do not
  alter supported parser state.
- Global `PUSH`/`POP`, collection nesting, local usage ranges, report size,
  report count, logical minimum/maximum, and report-ID state are transactional.
  Underflow, overflow, reversed ranges, zero/duplicate invalid IDs, impossible
  field widths, and truncated items reject the descriptor without publishing a
  partial layout.
- Sparse report IDs remain sparse identities.  Report ID zero means the
  no-prefix form and cannot be mixed ambiguously with explicitly numbered
  reports.
- Report decoding verifies the exact selected layout and minimum bit length
  before extraction.  Signed extension and logical-range validation are
  defined independently of host endianness and alignment.
- The decoder emits a bounded normalized snapshot/delta representation.  It
  does not call `input_device_emit()`, maintain carrier-like device lifetime,
  or retain a pointer into a USB transfer buffer.
- No Linux, BSD, or other external HID implementation is incorporated into the
  base system.  Public specifications and independently authored fixtures are
  behavioral references only.

## Detailed procedure

1. Define private HID item, collection, field, report-layout, and decoded-input
   structures with checked construction and complete destruction paths.
2. Implement short/long item iteration, global/local state, collections,
   usage/range assignment, report-ID layouts, and transactional finalization.
3. Map only the declared v1 usages into existing zedBSD evdev codes and axis
   metadata.  Preserve logical minimum/maximum for absolute-axis registration.
4. Implement checked bit extraction, sign extension, keyboard-array duplicate
   handling, error-rollover recognition, variable buttons/modifiers, relative
   deltas, and absolute values.
5. Add explicit boot-keyboard and boot-mouse layout decoders behind the same
   normalized output interface.
6. Build an independently authored IN-T40 corpus covering valid QEMU-shaped
   keyboard/mouse/tablet descriptors; report IDs; byte-crossing fields;
   signed/absolute ranges; duplicate keys; rollover; unknown usages; and every
   truncation, nesting, count, width, offset, and arithmetic boundary.
7. Run ordinary, ASan/UBSan, and compiler-analyzer host fixtures plus existing
   evdev layout/capability tests and configured x86 builds.  Do not use
   `make check` or `.internal/` material.

## Completion conditions

- The production parser converts every supported corpus descriptor into the
  exact bounded layout and rejects malformed input transactionally.
- Keyboard, relative mouse, and absolute tablet reports decode into the exact
  normalized events and axis metadata expected by the published evdev subset.
- Boot-protocol layouts and report-protocol layouts are independently tested;
  no malformed report silently falls back to boot interpretation.
- Descriptor/report lengths, counts, nesting, IDs, bit arithmetic, and signed
  extraction pass ordinary, sanitizer, and analyzer evidence without an
  out-of-bounds access or unbounded allocation.
- The Phase changes no public input UAPI and makes no live USB-HID or hardware
  completion claim.

## Reconsideration boundary

Stop and return to planning if the first accepted keyboard/mouse requires an
unsupported usage page, report size above the frozen bound, public event codes
outside the current profile, or output/feature-report machinery as a
prerequisite for input.  Do not weaken validation or add a vendor-specific
parser branch to force a corpus pass.
