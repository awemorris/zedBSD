# evdev compatibility profile

Status: experimental UAPI. The event core, capability/current-state queries,
console-keyboard adapter, PC/AT PS/2 mouse, PC-98 bus mouse, and USB HID Report
Protocol keyboard/mouse/tablet producers are implemented. The q048 automatic
matrix covers USB HID on xHCI and paired EHCI/UHCI with concurrent USB-root
storage, and the user confirmed physical USB keyboard/mouse operation on
2026-09-05.

zedBSD reserves `/dev/input/eventN` for an independently implemented event
interface whose initial keyboard and pointer subset is source-oriented toward
Linux and FreeBSD evdev. The authoritative public header is
[`<zedbsd/input.h>`](../../include/uapi/zedbsd/input.h). The compatibility
include paths [`<linux/input.h>`](../../libc/include/linux/input.h) and
[`<dev/evdev/input.h>`](../../libc/include/dev/evdev/input.h) include that same
header; they do not provide a second ABI.

## ABI and event stream

`struct input_event` contains a native `struct timeval`, 16-bit type and code,
and signed 32-bit value. The frozen zedBSD layouts are 24 bytes for x86-64 LP64
and 20 bytes for i386 ILP32. Events are timestamped from the kernel monotonic
tick count at publication time; they are not wall-clock timestamps. These
claims are fixed by the [public header](../../include/uapi/zedbsd/input.h), the
[timestamp implementation](../../src/drivers/input-device.c), the
[monotonic-clock owner](../../src/kern/clock.c), and the
[dual-ABI layout fixture](../../plan/ws006-input/tests/evdev-layout-test.c).

Reads return only whole records. A buffer smaller than one record fails with
`EINVAL`; a nonblocking empty read fails with `EAGAIN`. `poll()` reports
readability when a complete record exists. A newly opened reader starts at the
current producer position and does not replay older records. Production
logical updates end with `EV_SYN/SYN_REPORT`. `EV_KEY` values are release `0`,
press `1`, and repeat `2`; relative axes carry deltas, while absolute axes
carry current values described by `EVIOCGABS`. The read/poll path is in the
[input-device implementation](../../src/drivers/input-device.c), and the
independent/late-reader behavior is fixed by the
[bounded-queue implementation](../../src/drivers/input-queue.c) and
[IN-T10 fixture](../../plan/ws006-input/tests/input-queue-test.c).

Each open file has an independent cursor over a bounded 256-record device
queue. If that reader falls behind, its next read starts with
`EV_SYN/SYN_DROPPED`; the application discards through the next `SYN_REPORT`
boundary and then queries current key/axis state before trusting later
packets. Device
detach first queues releases for every kernel-known held
`EV_KEY` code (including pointer buttons), followed by `SYN_REPORT`, then wakes
readers. Already queued records drain before `read()` returns end-of-file;
`poll()` reports `POLLHUP` and may report readability at the same time while
records remain. These are production behaviors in
[`input-device.c`](../../src/drivers/input-device.c) and
[`input-queue.c`](../../src/drivers/input-queue.c), exercised by the
[input-device ownership fixture](../../plan/ws006-input/tests/input-device-ownership-test.c).

An `eventN` number is an allocation result, not a stable device identity.
Programs enumerate the nodes, query identity and capabilities, and select a
device by those properties. Detach unpublishes the pathname immediately, but
the number stays reserved while any fd still refers to that terminal device
generation. Those stale fds drain only old queued records before EOF/HUP and
cannot observe a later generation. After the final old fd closes, a new device
may reuse the released number. Bounded registry exhaustion fails attachment
instead of aliasing generations. The registration behavior is owned by
[`input-device.c`](../../src/drivers/input-device.c), the dynamic namespace
lifecycle by [`devfs.c`](../../src/kern/devfs.c), and capability-only
enumeration by the
[IN-T12 probe](../../plan/ws006-input/tests/evdev-capability-probe.c).

## Initial ioctl and policy subset

The operational subset is `EVIOCGVERSION`, `EVIOCGID`, `EVIOCGNAME`,
`EVIOCGPHYS`, `EVIOCGUNIQ`, `EVIOCGBIT`, `EVIOCGKEY`, `EVIOCGABS`, and
`EVIOCGRAB`. `EVIOCGABS` succeeds only for an axis registered by that device;
the current built-in relative pointers register no absolute axes. Repeat
get/set, properties, and current LED state remain reserved in the header but
are not operational. Unsupported requests return `ENOTTY` without changing
caller memory. The definitions and dispatcher are respectively the
[public UAPI](../../include/uapi/zedbsd/input.h) and
[production ioctl path](../../src/drivers/input-device.c); registration and
state validation are covered by the
[IN-T11 fixture](../../plan/ws006-input/tests/input-capability-test.c).

Capability and key-state buffers use the caller architecture's native
`unsigned long[]` representation, matching the Linux/FreeBSD source convention:
code `n` is bit `n % (sizeof(unsigned long) * 8)` in word
`n / (sizeof(unsigned long) * 8)`. The natural bitmap size is rounded up to a
whole native word. A zero encoded length is a successful no-op, a shorter
length receives the corresponding byte prefix, and bytes beyond the natural
size in an oversized query are zero-filled. All dynamic-length requests require
the exact output-direction encoding. The bitmap representation and copy rules
are implemented in
[`input-capability.h`](../../include/kern/input-capability.h) and
[`input-capability.c`](../../src/drivers/input-capability.c), and frozen by the
[capability fixture](../../plan/ws006-input/tests/input-capability-test.c).

A grab excludes delivery to other evdev file readers but does not disable the
kernel console path. On release, non-grabbing readers resume at the current
queue position rather than receiving records produced during the grab.
Writing/injecting events is not supported. Device-node permissions are an
installation policy; the kernel does not infer trust from an event-device
number. The grab and open-mode behavior is in
[`input-device.c`](../../src/drivers/input-device.c), and its interaction with
independent readers is covered by the
[input-device ownership fixture](../../plan/ws006-input/tests/input-device-ownership-test.c).

The header currently declares only the constants needed for ordinary PC
keyboards, relative mice, wheels, absolute pointers, and basic multitouch
position/tracking. It is deliberately not the complete Linux event-code
catalog. Declared constants do not imply that a live producer currently exists.

## Source ownership and console delivery

Each registered producer owns one `input_device`: identity, capabilities,
current key/button/axis state, reader cursors, grab, and detach state are not
shared with unrelated producers. The registration and publication contract is
declared by [`input-device.h`](../../include/kern/input-device.h) and implemented
by [`input-device.c`](../../src/drivers/input-device.c). Built-in mouse button
state is backend-local in the
[PC/AT PS/2 driver](../../src/drivers/hid/ps2-mouse.c) and
[PC-98 bus-mouse driver](../../src/drivers/hid/pc98-busmouse.c). The q044
[ownership runner](../../plan/ws006-input/tests/run-input-ownership-host-test.sh)
exercises two independent keyboard and pointer states, overlapping modifiers
and buttons, registration races, and detach while held.

The production [`usb-hid.c`](../../src/drivers/usb-hid.c) matches validated HID
interfaces rather than device IDs, fetches and parses each Report descriptor,
requires checked `SET_PROTOCOL(REPORT)` for boot-subclass devices, and publishes
truthful `BUS_USB` identity and capabilities. A malformed or unsupported Report
Protocol interface fails attachment; there is no silent Boot-Protocol runtime
fallback. Each interface owns one interrupt-IN URB and one independent event
device. Keyboard events also reach the console subscriber, while evdev readers
remain independent and `EVIOCGRAB` still does not disable console text input.

The HAL advertises whether a console source has physical release/repeat
information or only characters through `struct hal_cons_input_info` returned
by [`hal_cons_get_input_info()`](../../include/hal/hal.h). PC/AT, PC-98, and X68000
keyboard adapters preserve physical press, release, and repeat events. A
character-only arm64 or sparcv9 console is registered as a momentary source:
each character that maps to a public key code publishes press, synthetic
release, and `SYN_REPORT` in one bounded report, so `EVIOCGKEY` cannot retain a
fabricated held key. Truthful platform declarations are in the
[PC/AT](../../src/hal/amd64/bsp-pcat/cons.c),
[PC-98](../../src/hal/i386/bsp-pc98/cons.c),
[X68000](../../src/hal/m68k/bsp-x68k/keyboard.c),
[arm64](../../src/hal/arm64/bsp-rpi4/cons.c), and
[sparcv9](../../src/hal/sparcv9/bsp-sun4u/console.c) HAL sources. The q044
runner retains the physical x86/m68k paths and generic momentary behavior;
arm64/sparcv9 declarations are source-audited because their cross compilers
were unavailable. This reference does not claim fresh hardware or QEMU
acceptance for every architecture.

The console is a bounded kernel-internal subscriber to the same publication
stream; it does not open an event node or assume an event number. Translation,
modifier, active-key, resynchronization, and detach state are maintained per
source. Subscriber callbacks are required to be bounded and nonblocking, and
unsubscribe joins already admitted callbacks. A user-space `EVIOCGRAB` affects
only evdev readers, not this console subscriber. The contract and registry live
in [`input-device.h`](../../include/kern/input-device.h) and
[`input-subscriber.c`](../../src/drivers/input-subscriber.c); the console owner
is [`console.c`](../../src/drivers/fs/console.c), with focused evidence in the
[subscriber fixture](../../plan/ws006-input/tests/input-ownership-test.c) and
[console fixture](../../plan/ws006-input/tests/console-input-ownership-test.c).

Stable `jis-*` position symbols let PC-98 and X68000 retain physical identity
when modifiers change between make, repeat, and break. A console-only symbol
which has no code in the frozen public subset remains internal to the console
subscriber and does not fabricate an evdev capability or event. The mapping is
owned by [`input-keymap.c`](../../src/drivers/input-keymap.c) and exercised by
the [PC-98](../../plan/ws006-input/tests/pc98-keyboard-ownership-test.c) and
[X68000](../../plan/ws006-input/tests/x68k-keyboard-ownership-test.c) fixtures.

## Resynchronization

There are two ways to observe `SYN_DROPPED`:

- A slow evdev reader can overrun its independent cursor. The queue emits
  `SYN_DROPPED` only to that reader and then resumes from the oldest retained
  record.
- A physical HAL scan ring can overflow before publication. Its private
  begin/snapshot/end protocol stages authoritative held-key state. Completion
  atomically replaces the device's `EVIOCGKEY` state, emits `SYN_DROPPED`
  followed by `SYN_REPORT` to evdev, and updates the console's per-source state
  without replaying snapshot keys as tty text.

In both cases, user space treats state before `SYN_DROPPED` as unreliable and
queries current state before interpreting subsequent packets. The first path
is implemented by [`input-queue.c`](../../src/drivers/input-queue.c); the
transactional producer path is implemented by
[`input-device.c`](../../src/drivers/input-device.c) and the
[console subscriber](../../src/drivers/fs/console.c). The
[HAL resync fixture](../../plan/ws006-input/tests/hal-input-resync-test.c) and
[input-device ownership fixture](../../plan/ws006-input/tests/input-device-ownership-test.c)
cover the source snapshot and atomic state transition.

## Compatibility differences

| Property | Linux | FreeBSD | zedBSD initial profile |
| --- | --- | --- | --- |
| Include path | `<linux/input.h>` | `<dev/evdev/input.h>` | Both wrappers plus authoritative `<zedbsd/input.h>` |
| LP64 event layout | Native timeval; 24 bytes on x86-64 | Native timeval; 24 bytes on x86-64 | 24 bytes |
| i386 event layout | Linux compat/time-mode dependent; commonly 16 bytes | Native FreeBSD timeval layout | 20 bytes because zedBSD `time_t` is 64-bit |
| ioctl encoding | Linux `_IOC` ABI | FreeBSD ioctl ABI | zedBSD ioctl ABI; source names match but numbers are not binary-compatible |
| Timestamp selection | Clock selection facilities exist | Native evdev behavior | Fixed `CLOCK_MONOTONIC` initially |
| Grab effect | Linux evdev exclusive delivery | FreeBSD evdev-compatible behavior | Other evdev readers excluded; console broker remains active |
| Code catalog | Broad Linux catalog | Tracks Linux codes closely | Frozen keyboard/pointer subset only |
| Event injection | Separate uinput facility | Separate uinput facility | Not provided |
| Hotplug node generation | Dynamic event nodes | Dynamic event nodes | Immediate unpublish; stale generation reserved through final fd close, then number reuse permitted |

The Linux UAPI and event semantics are maintained in the [Linux input
header](https://github.com/torvalds/linux/blob/master/include/uapi/linux/input.h)
and [input documentation](https://github.com/torvalds/linux/blob/master/Documentation/input/input.rst).
FreeBSD maintains its corresponding [evdev
header](https://github.com/freebsd/freebsd-src/blob/main/sys/dev/evdev/input.h).
These are compatibility references; no source from either implementation is
incorporated into the zedBSD base system.

## Evidence and remaining boundaries

| Current claim | Production owner | Executable evidence |
| --- | --- | --- |
| Public layout, constants, and wrapper identity | [`input.h`](../../include/uapi/zedbsd/input.h), [Linux wrapper](../../libc/include/linux/input.h), [FreeBSD wrapper](../../libc/include/dev/evdev/input.h) | [IN-T00](../../plan/ws006-input/tests/evdev-layout-test.c) in LP64 and ILP32 modes |
| Independent queues, read/poll/grab, overflow, and detach | [`input-device.c`](../../src/drivers/input-device.c), [`input-queue.c`](../../src/drivers/input-queue.c) | [IN-T10 queue fixture](../../plan/ws006-input/tests/input-queue-test.c), [ownership/lifecycle fixture](../../plan/ws006-input/tests/input-device-ownership-test.c) |
| Capability registration and current key/ABS state | [`input-capability.c`](../../src/drivers/input-capability.c) | [IN-T11](../../plan/ws006-input/tests/input-capability-test.c) and [IN-T12 probe](../../plan/ws006-input/tests/evdev-capability-probe.c) |
| Per-source physical/momentary input, console subscription, resync, and detach | [`input-device.c`](../../src/drivers/input-device.c), [`input-subscriber.c`](../../src/drivers/input-subscriber.c), [`console.c`](../../src/drivers/fs/console.c) | [q044 ownership runner](../../plan/ws006-input/tests/run-input-ownership-host-test.sh) and [`ws006-p006`](../../plan/ws006-input/phase006-input-truthfulness-ownership/phase.md) |
| HID descriptor/report parsing | [`hid-report.c`](../../src/drivers/hid/hid-report.c) | [IN-T40 fixture](../../plan/ws006-input/tests/hid-report-test.c) and [`ws006-p007`](../../plan/ws006-input/phase007-usb-hid-parser/phase.md) |
| USB HID Report-Protocol producers, hotplug, and generation-safe nodes | [`usb-hid.c`](../../src/drivers/usb-hid.c), [`input-device.c`](../../src/drivers/input-device.c), [`devfs.c`](../../src/kern/devfs.c) | [IN-T41/IN-T42 definitions](../../plan/ws006-input/tests/README.md), [`ws006-p008` result](../../plan/ws006-input/phase008-usb-hid-evdev/phase.md) |
| Xzed evdev-only consumer | [Xzed input owner](../../userland/X11/xzed/input-posix.c) | [`ws018-p007`](../../plan/ws018-kernel-architecture/phase007-xzed-evdev-consumer/phase.md) and its [host runner](../../plan/ws018-kernel-architecture/tests/run-xzed-input-host-test.sh) |
| Noct 2.0.1 BeUI evdev consumer | [zedBSD BeUI backend](../../userland/base/noct/noct/src/api/api-beui-zedbsd.c) | [q063 Noct evidence](../../plan/ws008-noct/tests/q063-noct-2.0.1-evidence.md) |

Registration requires `EV_SYN/SYN_REPORT`; malformed declarations and
undeclared producer events are rejected, keeping advertised capabilities and
the delivered public stream consistent. The q044 ownership gates pass, and
q048 adds Report-Protocol keyboard, relative mouse, and absolute tablet
production fixtures plus generation-safe cdev/devfs lifecycle checks. Its QEMU
matrix passes with xHCI and paired EHCI/UHCI, including hotplug, stale-fd
isolation, event-number reuse, console coexistence, and concurrent USB-root
I/O. The user's 2026-09-05 physical USB HID confirmation completes IN-T42.

Xzed and the selected Noct 2.0.1 BeUI backend both discover
`/dev/input/eventN` by capabilities and consume evdev without the console event
interface. `/dev/mouse` is absent. The legacy `/dev/console` continuous-event,
input-mode, and key-state ioctls remain implemented and are deprecated; their
planned removal is `ws006-p009`, not current behavior. Ordinary character/TTY
console input remains supported before and after that planned cleanup. Those
boundaries are tracked by the [WS006 plan](../../plan/ws006-input/ws.md).
