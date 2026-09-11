# WS006 Phase 007: USB HID descriptor and report core

Last updated: 2026-08-31

WSID: `ws006`

Phase ID: `p007`

Combined ID: `ws006-p007`

Status: complete (`q044` software/parser milestone); live USB binding remains
`ws006-p008`

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
- Explicit Usage items and Usage Min/Max ranges are retained as a bounded
  ordered sequence.  Variable fields consume the flattened sequence in the
  descriptor's declaration order, including when an explicit Usage appears
  before or after a range.
- Global `PUSH`/`POP`, collection nesting, local usage ranges, report size,
  report count, logical minimum/maximum, and report-ID state are transactional.
  Underflow, overflow, reversed ranges, zero/duplicate invalid IDs, impossible
  field widths, and truncated items reject the descriptor without publishing a
  partial layout.
- A supported field is accepted only when its complete Logical Minimum/Maximum
  range is representable by its Report Size: unsigned ranges use
  `[0, 2^n - 1]` and signed ranges use
  `[-2^(n - 1), 2^(n - 1) - 1]`.  The 32-bit boundary is evaluated without a
  native-width shift.
- Sparse report IDs remain sparse identities.  A later `REPORT_ID(n)` item may
  validly select an existing identity again and does not consume another one
  of the 32 slots.  Report ID zero is invalid as an item; the implicit
  no-prefix form cannot be mixed ambiguously with explicitly numbered reports.
- Report decoding verifies the exact selected layout and minimum bit length
  before extraction.  Signed extension and logical-range validation are
  defined independently of host endianness and alignment.
- Keyboard Array capabilities and accepted decode bounds are the intersection
  of the descriptor's Usage and nonnegative Logical ranges.  ErrorRollOver,
  POSTFail, and ErrorUndefined share one invalid-keyboard-state flag only when
  their values are inside that intersection.
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
   handling, recognition of ErrorRollOver/POSTFail/ErrorUndefined as one
   invalid-keyboard-state flag, variable buttons/modifiers, relative deltas,
   and absolute values.
5. Add explicit boot-keyboard and boot-mouse layout decoders behind the same
   normalized output interface.
6. Build an independently authored IN-T40 corpus covering valid QEMU-shaped
   keyboard/mouse/tablet descriptors; report IDs; byte-crossing fields;
   signed/absolute ranges; duplicate keys; all three keyboard error usages;
   unknown usages; and every
   truncation, nesting, count, width, offset, and arithmetic boundary.
7. Run ordinary, ASan/UBSan, and compiler-analyzer host fixtures plus existing
   evdev layout/capability tests and configured x86 builds.  Do not use
   `make check` or `.internal/` material.

## q044 execution result

The production implementation is
[`hid-report.c`](../../../src/drivers/hid/hid-report.c) behind the private
opaque interface
[`hid-report.h`](../../../include/drivers/hid/hid-report.h).  The complete
descriptor is copied before parsing; layouts and parser work have fixed bounds
and complete allocation-failure cleanup.  Decoding performs no allocation and
returns caller-owned normalized values: held-key snapshots, relative deltas,
and absolute coordinates.  Boot keyboard and boot mouse are explicit layout
constructors and are never automatic fallbacks for a rejected descriptor.

The IN-T40 fixture is
[`hid-report-test.c`](../tests/hid-report-test.c).  It independently exercises
ordinary report keyboards, relative mice, an absolute tablet, fixed boot
profiles, Output-item coexistence, sparse and reselected IDs, 12- and 32-bit
signed extraction, cross-byte fields, global-stack restoration, long items,
owned descriptor storage, duplicate arrays, ErrorRollOver/POSTFail/
ErrorUndefined handling, a Usage A..Z array narrowed by Logical 4..4 to the
sole KEY_A capability and accepted value, unknown usages, transactional
failures, both declaration orders for mixed explicit Usage and Usage ranges,
allocation failures, and the exact descriptor,
report-ID, expanded-field, and report-bit bounds.
Every non-Constant Input item validates that its unsigned or signed Logical
range is representable by the declared field width before usage filtering.
This includes unsupported/vendor fields, so an impossible field followed by a
supported one rejects the complete descriptor before any layout or capability
is published.

Final focused evidence:

```text
strict C11 host fixture                 791 checks PASS
ASan + UBSan host fixture               791 checks PASS
GCC -fanalyzer fixture                  791 checks PASS
evdev LP64/ILP32 syntax/layout          PASS / PASS
input capability ordinary/sanitized     PASS / PASS
PC-98 i386 production object            PASS (-Werror)
PC/AT i386 production object            PASS (-Werror)
PC/AT amd64 production object           PASS (-Werror)
six platform production manifests       hid-report retained
git diff --check                        PASS
```

LeakSanitizer process inspection is unavailable in the execution sandbox, so
the sanitizer run used `ASAN_OPTIONS=detect_leaks=0`.  The same fixture counts
every `kern_calloc()`/`kern_free()` lifetime, injects failure at both allocation
sites, and finishes every ordinary, sanitizer, and analyzer run with zero live
allocations.  Neither `make check` nor `.internal/` material was used.

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

All completion conditions above are satisfied for the device-independent
software boundary.  USB interface binding, URBs, devfs publication, detach,
and QEMU/physical device claims remain exclusively in `ws006-p008`.

## Reconsideration boundary

Stop and return to planning if the first accepted keyboard/mouse requires an
unsupported usage page, report size above the frozen bound, public event codes
outside the current profile, or output/feature-report machinery as a
prerequisite for input.  Do not weaken validation or add a vendor-specific
parser branch to force a corpus pass.
