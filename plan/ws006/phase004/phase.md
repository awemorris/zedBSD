# ws006-p004: physical-key input broker and guest evidence

Last updated: 2026-08-25

WSID: `ws006`

Phase ID: `p004`

Combined ID: `ws006-p004`

Status: complete PC/AT software milestone; port-specific physical mapping remains

Parent WS: [WS006](../ws.md)

Tests: [WS006 test index](../tests/README.md)

## Objective

Make the kernel input core the single keyboard event broker: native keyboard
producers publish physical key transitions, evdev readers receive compatible
`EV_KEY` records, and the console translates the same stream into terminal
input. Add guest evidence that reads real event records while console login and
editing continue to work.

## Current finding

The current scalar `hal_cons_read_event()` contract contains a translated key
or character plus aggregate Shift/Ctrl/Graph flags. The amd64 and i386 PC/AT
backends discard physical scan identity before the input core sees the event;
PC-98 and X68000 perform equivalent platform translation. Consequently the
existing bridge cannot distinguish left/right modifiers, preserve physical
layout identity, or provide a sound common boundary for later USB HID.

Mapping the translated character back to an evdev code, as `input_key_from_hal`
currently does, is necessarily lossy. Extending that reverse mapping would
make the evidence look broader without satisfying the Phase objective.

## Human decision record

The kernel-facing contract is a fixed-size string keysymbol event:

```c
struct hal_key_event {
    char symbol[16];     /* at most 15 bytes plus NUL */
    uint32_t flags;      /* press, release, or repeat */
};
```

The HAL does not publish a keycode or HID-usage enumeration. Each supported
producer emits stable, lowercase ASCII symbol tokens no longer than 15 bytes;
the structure is zero-filled so termination is unconditional. Architecture-
independent kernel tables convert these tokens to the evdev `KEY_*` subset and
tty symbols. Unknown tokens are ignored for evdev/tty while remaining safe to
log. Serial consoles that provide characters rather than make/break identity
use a one-byte symbol and press flag and explicitly cannot synthesize release.

## Work packages after the gate

1. Freeze the selected string keysymbol type, flags, termination rule, and
   canonical token policy in the public HAL contract.
2. Add a bounded kernel producer queue/callback that does not make the console
   open its own `/dev/input/eventN` node.
3. Convert amd64 PC/AT first, then i386 PC/AT, PC-98, X68000, arm64, and sun4u;
   retain an explicit logical-only fallback for consoles that cannot report a
   physical key.
4. Move modifier state and terminal character/layout translation behind the
   input core. Preserve canonical tty input, repeat, control chords, and VTs.
5. Remove the lossy reverse `input_key_from_hal()` producer path once every
   configured backend uses the selected event.
6. Add a WS-local guest event reader, boot it in a checkpoint image, inject
   QEMU keyboard input, and prove matching press/release/SYN records coexist
   with login and shell editing.
7. Run the focused queue/keymap/ABI fixtures, `make -j16`, PC/AT and PC-98
   kernel links, and amd64 QEMU acceptance. Cross-target limitations remain
   explicit rather than weakening the contract.

## Result

The public HAL event is now the selected `char symbol[16]` plus exactly one of
press, release, or repeat. No public HAL keycode/HID enumeration remains.
amd64 and i386 PC/AT preserve unshifted physical identity, distinguish left
and right Ctrl/Alt/Shift, and detect repeated make codes. The kernel input
worker is the single hardware reader and fans each event to independent evdev
readers and the tty translation path.

Focused keymap, queue, and dual-ABI fixtures pass. `make -j16`, amd64, i386
PC/AT, and PC-98 kernel links pass. In QEMU the normal image reaches login;
the guest reads a real `KEY_C` press plus `SYN_REPORT` from
`/dev/input/event0` while the console also echoes the key and returns to the
shell. See [QEMU evidence](../tests/qemu-evdev-evidence.md).

PC-98 and X68000 cross the new bounded string API through their existing JIS
logical translation. They do not yet preserve the same complete physical-key
identity/repeat model as PC/AT. arm64 and sun4u character-only serial consoles
emit one-byte press events, as permitted by the contract. The missing PC-98
and X68000 physical-token conversion is retained as an explicit port follow-up
rather than misreported as complete hardware acceptance. Cross compilers for
arm64, sparcv9, and m68k were unavailable; their converted sources passed the
available host syntax checks except that m68k's required ILP32 system headers
are unavailable on this host.

## Completion conditions

- One physical transition fans out to independent evdev readers and the tty
  path without reading the hardware twice.
- Left/right modifiers and unshifted physical identity survive on PC/AT; layout
  translation affects console symbols, not evdev key identity.
- Press, release, and repeat semantics are explicit and tested.
- QEMU guest evidence reads real `input_event` records while the same keyboard
  remains usable for login and shell input.
- Existing legacy console event/key-state ioctls remain until IN-04/IN-05
  consumers migrate; their removal is not folded into this Phase.

## Prior stop record

`q005` stopped before implementation because choosing the physical-key
namespace and HAL event shape is an architecture contract, not a mechanical
follow-up. The human decision subsequently selected the fixed string structure,
so the Phase resumed without creating another Queue.
