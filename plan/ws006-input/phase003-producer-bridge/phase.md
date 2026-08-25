# ws006-p003: existing input producer bridge

WSID: `ws006`  
Phase ID: `p003`  
Combined ID: `ws006-p003`  
Status: complete milestone  
Parent WS: [WS006](../ws.md)

## Objective

Publish events from the existing HAL console keyboard and generic relative
mouse producers through `/dev/input/eventN` without removing the legacy console
text and `/dev/mouse` paths.

## Work packages

- [x] Register the console keyboard and generic mouse with the input core.
- [x] Translate the current HAL key representation to the published evdev key
  subset and publish press/release plus `SYN_REPORT`.
- [x] Publish mouse relative axes, button transitions, and `SYN_REPORT`.
- [x] Share PS/2 backend activation between legacy and evdev opens.
- [x] Prove event-node registration and unchanged boot/login behavior in amd64
  QEMU.
- [x] Record the translated-HAL-key limitation for the later console broker.

## Completion conditions

- Keyboard and mouse event nodes register in a production amd64 boot.
- Focused key translation and input queue tests pass.
- Opening either `/dev/mouse` or the mouse event node activates the same bounded
  backend lifecycle, and the final close stops it.
- Console text input remains available at login.

## Known boundary

The current HAL reports translated key values rather than physical scan
positions. Shifted ASCII is normalized to a stable evdev code, but left/right
modifier identity and non-US physical layout identity cannot be recovered.
`ws006-p004` must move console translation behind the input core before this
can become a physical-key-complete producer path.

## Result

Focused keymap/queue tests pass. amd64 QEMU registers the production keyboard
as `event0` and relative mouse as `event1`, then reaches `login:`. The legacy
console and mouse paths remain installed. A guest event-record reader and
physical-key producer conversion belong to the next consumer/broker Phase;
registration alone is not claimed as complete end-user evdev migration.
