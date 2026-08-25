# WS006: input and evdev

Last updated: 2026-08-25

WSID: `ws006`

Status: planned; no Phase started

Parent: [master plan](../master.md)

Last verified Phase: none

Resume point: extract IN-00 and freeze the evdev compatibility profile before
implementing or publishing an ABI.

Shared tests: [WS006 test index](tests/README.md)

## Phase registry

No Phase has started. The intended sequence is ABI, input core, existing
producer bridge, consumer migration, legacy-console removal, and USB HID.

## Goals

- Provide a documented `/dev/input/eventN` evdev interface for keyboard and
  pointer events.
- Make existing console producers and USB HID devices use one kernel input
  core.
- Migrate consumers before removing the legacy console event/key-state API.

## WS completion conditions

WS006 is complete when the published evdev profile and ABI tests pass, console
input and multiple evdev readers coexist correctly, USB HID keyboard/mouse work
in QEMU and on supported hardware, all in-tree consumers are migrated, and the
obsolete console event/key-state interfaces are removed without regression.

## 1. Objective

Introduce `/dev/input/eventN` devices with an explicitly selected
Linux/FreeBSD-compatible evdev profile, migrate event consumers away from
zedBSD-specific `/dev/console` event/key-state interfaces, and support USB HID
keyboards and mice. The console continues to receive normal character input
through a kernel-internal input broker.

## 2. Migration rule

Legacy `/dev/console` input interfaces are removed only after equivalent evdev
producers and consumers are verified:

1. freeze the evdev UAPI and event semantics;
2. implement the input core and expose existing console keyboard/mouse producers
   as `/dev/input/eventN`;
3. feed console character processing from the same internal input core;
4. migrate Xzed and the Noct/BeUI zedBSD backend to evdev;
5. implement USB HID producers and verify hotplug;
6. delete continuous event acquisition and key-state ioctls from
   `/dev/console`, then remove dead compatibility code.

The console driver should not open its own `/dev/input/eventN` device. Producers
publish into a kernel input core, which fans out to evdev readers and to the
console's character/key translation path.

## 3. Work items

| ID | Status | Deliverable | Dependencies | Acceptance gate |
| --- | --- | --- | --- | --- |
| IN-00 | Planned | evdev compatibility profile and public UAPI | Existing console UAPI audit | Header/layout tests and a documented Linux/FreeBSD difference table |
| IN-01 | Planned | Kernel input core, registration, event fan-out, buffering, poll/read, and lifecycle | IN-00, VFS/device primitives | Multiple-reader, overflow, nonblocking, poll, disconnect, and permission tests |
| IN-02 | Planned | Existing console input producers also register evdev devices | IN-01 | Keyboard/mouse events appear under `/dev/input/` without breaking console text input |
| IN-03 | Planned | Console consumes the internal input stream | IN-01/02 | Console editing, modifiers, repeat, virtual-terminal behavior, and evdev readers coexist |
| IN-04 | Planned | Xzed evdev migration | IN-02, GFX X11 repair | Keyboard and absolute/relative mouse behavior pass in QEMU and hardware |
| IN-05 | Planned | Noct/BeUI evdev migration | IN-02, NOCT upstream/backend work | BeUI event behavior passes without console event ioctls |
| IN-06 | Planned | Remove console continuous-event and key-state UAPI | IN-03–05 | No in-tree consumer remains; compatibility audit and regression tests pass |
| IN-10 | Planned | USB HID descriptor/report core | HW-01 xHCI, USB core | Descriptor parser corpus, malformed reports, boot/report protocol tests |
| IN-11 | Planned | USB HID keyboard and mouse evdev devices | IN-01, IN-10 | QEMU USB keyboard/tablet/mouse and physical hotplug tests pass |

## 4. UAPI design gate

“Compatible with Linux/FreeBSD” is not sufficient as a binary contract. IN-00
must publish the selected definitions and differences, including:

- `struct input_event` field types, timestamp clock, alignment, and 32/64-bit
  behavior;
- event type/code/value constants required by keyboard and mouse consumers;
- device identity/capability/name queries and the supported `EVIOC*` subset;
- relative, absolute, synchronization, repeat, and device-removal semantics;
- grab/exclusive access policy, permissions, and event injection policy;
- stable numbering versus dynamic `/dev/input/eventN` discovery.

If exact source compatibility requires aliases while binary layouts differ,
that fact is documented rather than called transparent ABI compatibility.

## 5. Buffering and failure behavior

Each reader needs an independent bounded queue or cursor. Slow readers may not
stall input producers or the console. Overflow emits the selected synchronization
loss indication and requires consumers to resynchronize according to the
published profile. Detach wakes blocked readers and returns a stable error/end
condition.

USB HID parsing treats report descriptors as untrusted device input: all
lengths, counts, usages, and bit ranges are bounded before access.
