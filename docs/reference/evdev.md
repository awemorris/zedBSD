# evdev compatibility profile

Status: experimental UAPI; kernel device implementation planned

zedBSD reserves `/dev/input/eventN` for an independently implemented event
interface whose initial keyboard and pointer subset is source-oriented toward
Linux and FreeBSD evdev. The authoritative header is
[`<zedbsd/input.h>`](../../include/uapi/zedbsd/input.h). Compatibility include
paths `<linux/input.h>` and `<dev/evdev/input.h>` expose that same subset.

## ABI and event stream

`struct input_event` contains a native `struct timeval`, 16-bit type and code,
and signed 32-bit value. The frozen zedBSD layouts are 24 bytes for x86-64 LP64
and 20 bytes for i386 ILP32. Timestamps use `CLOCK_MONOTONIC` and represent the
producer observation time. They are not wall-clock timestamps.

Reads return only whole records. A buffer smaller than one record fails with
`EINVAL`; a nonblocking empty read fails with `EAGAIN`. `poll()` reports
readability when a complete record exists. Each logical update ends with
`EV_SYN/SYN_REPORT`. `EV_KEY` values are release `0`, press `1`, and repeat `2`.
Relative axes carry deltas; absolute axes carry the current value described by
`EVIOCGABS`.

Each open file has an independent bounded queue. On overflow, the affected
reader receives `EV_SYN/SYN_DROPPED` and must query current key/axis state
before trusting later packets. Detach drains already queued records, then
returns end-of-file and a hangup poll indication.

Device numbers are dynamic and not stable identities. Programs enumerate
`/dev/input/eventN`, query name/physical/unique identifiers and capabilities,
and select a device by those properties.

## Initial ioctl and policy subset

The reserved subset is `EVIOCGVERSION`, `EVIOCGID`, repeat get/set, name,
physical path, unique ID, properties, capability bits, current key/LED state,
absolute-axis information, and `EVIOCGRAB`. Unsupported requests return
`ENOTTY`. A grab excludes delivery to other evdev file readers but does not
disable the kernel console text path. Writing/injecting events is not supported.
Device-node permissions are an installation policy; the kernel does not infer
trust from an event-device number.

The header currently declares only the constants needed for ordinary PC
keyboards, relative mice, wheels, absolute pointers, and basic multitouch
position/tracking. It is deliberately not the complete Linux event-code
catalog.

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

The Linux UAPI and event semantics are maintained in the [Linux input
header](https://github.com/torvalds/linux/blob/master/include/uapi/linux/input.h)
and [input documentation](https://github.com/torvalds/linux/blob/master/Documentation/input/input.rst).
FreeBSD maintains its corresponding [evdev
header](https://github.com/freebsd/freebsd-src/blob/main/sys/dev/evdev/input.h).
These are compatibility references; no source from either implementation is
incorporated into the zedBSD base system.

## Evidence and implementation state

The [IN-T00 layout test](../../plan/ws006-input/tests/evdev-layout-test.c)
compiles under both supported x86 ABI modes and freezes sizes, offsets, and core
numeric constants. No `/dev/input/eventN` kernel device exists yet; read,
queue, ioctl, and lifecycle behavior remains acceptance scope for `ws006-p002`.
