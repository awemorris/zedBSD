# WS006 Phase 002: input core and event device

Last updated: 2026-08-25

Phase ID: `ws006-p002`

Status: complete implementation milestone

Parent: [WS006](../ws.md)

Tests: [WS006 test index](../tests/README.md)

## Objective

Implement IN-01 against the evdev profile frozen in `ws006-p001`, without
moving existing console/mouse producers or consumers in the same Phase.

## Scope

- opaque input-device registration, event publication, and detach lifecycle;
- dynamic `eventN` nodes under `/dev/input/`;
- independent reader cursors over a bounded event journal;
- `SYN_DROPPED` overflow resynchronization;
- blocking/nonblocking read, poll, detach wakeup, and exclusive evdev grab;
- version, identity, and device-string ioctls;
- read-only device access and restricted event-node mode.

Producer bridging, console input brokering, Xzed/BeUI migration, USB HID, event
injection, and legacy removal are outside this Phase.

## Work packages

- [x] Add the bounded queue and independent-reader model.
- [x] Add registration, publication, detach, read, poll, and grab paths.
- [x] Add `/dev/input/eventN` devfs discovery and permissions.
- [x] Wire the core into every kernel target.
- [x] Add overflow and multiple-reader host evidence.
- [x] Pass focused ABI/queue tests and the amd64 kernel build.
- [x] Record remaining runtime and producer-integration evidence.

## Completion conditions

The queue and ABI tests pass, the configured amd64 kernel builds, and the
device lifecycle is present without claiming producer/consumer migration.
Guest read/poll/ioctl coverage may be handed to the first producer-bridge
Phase because this Phase deliberately creates no synthetic production device.

## Evidence and result

Both IN-T00 ABI compile commands pass. The implementation-shared queue fixture
passes independent-reader, late-reader, overflow/`SYN_DROPPED`, and detach
cases. `make -j16 build/amd64/vmunix` passes the kernel validator, the full
image rebuild succeeds, and that image reaches `login:` under
`qemu-system-x86_64`.

No producer is connected in this Phase, so no `eventN` node is fabricated.
Guest read/poll/ioctl, producer timestamps, grab behavior under real input, and
disconnect observation remain acceptance work for IN-02. Detached cdev slots
remain tombstones because the current cdev registry has no safe unregister;
hotplug slot reuse is also handed forward.

## Resume point

Extract `ws006-p003` for IN-02: bridge existing keyboard and mouse producers
to registered evdev devices while preserving console input.
