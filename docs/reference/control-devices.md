# Console, graphics and system controls

Status: current x86 implementation; zedBSD-specific ABI

Use target headers, not host ioctl numbers or structures. [ABI selection](compatibility-profile.md)
affects pointer-bearing records. Kernel handlers return errors through libc's
`ioctl` convention (`-1` and `errno`). Invalid user addresses fail rather than
being treated as kernel pointers. Unsupported requests generally return
`EOPNOTSUPP`; callers must not assume every device uses Linux's error mapping.

## Text console and terminals

[console.h](../../include/uapi/zedbsd/console.h),
[console.c](../../src/drivers/generic/console.c) and
[tty.c](../../src/kern/tty.c) define `/dev/console`. Ordinary reads, writes,
polling and termios use the TTY path. Text display controls use ioctl group
`c`:

| Number / suffix (`ZEDBSD_CONSOLE_`) | Record / operation |
| --- | --- |
| 1 `GET_SIZE` | `console_size`: rows and columns |
| 2 `CLEAR` | Clear display |
| 3 `CLEAR_ROW` | `console_row`: one row |
| 4 `CLEAR_TO_EOL` | `console_position`: row and column |
| 5 `GET_CURSOR`, 6 `SET_CURSOR` | `console_cursor`: row, column, visibility |
| 7 `SHOW_CURSOR` | `console_cursor`: visibility control |
| 8 `WRITE_AT` | `console_write_at`: position, attribute, user address and byte length |
| 13 `ISATTY` | Console-specific probe retained in the ABI |

Rows, columns and display fields are 32-bit quantities; `WRITE_AT.address`
uses `uapi_ptr_t`. The driver bounds coordinates and limits a write-at request
to 512 bytes before copying text. Numbers 9–12 and 14–15 are retired/reserved;
they are not the input-event interface. Use [evdev](evdev.md) instead.

Application terminal detection should use `isatty`, and identification should
use `ttyname_r`. Current libc uses termios for the former and device identity
for the latter; a PTY is a terminal but is not `/dev/console`. Normal example:
`tty` inside zterm reports `/dev/pts/N`. A regular file descriptor is not a
terminal, and an invalid descriptor must remain distinguishable from it.
[q147](../../plan/history/queue-q147.md) tests these cases and actual graphical input.

## Graphical ownership and drawing

[graphics.h](../../include/uapi/zedbsd/graphics.h) defines group `g`.
The [PC/AT driver](../../src/drivers/platform/pcat/graphics/pcat-graphics.c)
serves the maintained PC/AT x86 path; the
[PC-98 driver](../../src/drivers/platform/pc98/graphics/pc98-graphics.c)
has its own backend. Query capabilities instead of assuming equal modes.

Opening `/dev/graphics` acquires one exclusive open-file-description owner.
An unavailable backend returns `ENODEV`; a second independent open returns
`EBUSY`. Duplicated descriptors share ownership. Final close leaves graphics
mode and resumes the HAL console, including process cleanup. There is no
public LEAVE ioctl, framebuffer `mmap` endpoint or native GPU takeover in this
interface. Non-owner ioctl access fails with `EBADF`.

| Number / suffix (`ZEDBSD_GRAPHICS_`) | Purpose |
| --- | --- |
| 1 `GET_CAPS` | Discover supported drawing operations |
| 2 `ENTER` | Request preferred mode; read the actual selected mode |
| 3 `GET_MODE` | Read active mode |
| 4 `FILL_RECT`, 5 `DRAW_LINE`, 6 `PATTERN_FILL` | Primitive drawing |
| 7 `BLIT`, 8 `BLIT_PATTERN` | Copy user pixel data with explicit format and stride |
| 9 `FLUSH` | Submit bounded damaged rectangles |
| 10 `GET_GLYPH` | Obtain the supported glyph representation |
| 11 `GET_MODES` | Enumerate available modes through a capacity/count record |

`GET_CAPS`, `GET_MODES` and `ENTER` are available before entering graphics;
other requests require entered state (`ENXIO`). A normal client opens, queries
capabilities, enters, uses the returned dimensions/stride, draws and closes.
It must handle `EBUSY` by leaving the current owner's display alone, and reject
unsupported operations rather than assuming an accelerated fallback.

The header defines INDEX8, RGB24 and MONO1 formats and MSB-first glyph bits.
Pixel, palette and mode-array addresses use `uapi_ptr_t`; reserved fields must
be initialized as required. PC/AT currently bounds mode capacity to 16,
flush rectangles to 32 and its row buffer to 4096 bytes. These implementation
bounds do not mean arbitrary user strides, multiplication overflows or
out-of-range rectangles are accepted. Returned mode may differ from the
requested preference. The actual Xzed/zterm path passed
[WS006 q147](../../plan/history/queue-q147.md).

`/dev/gpu` remains a [WS014 proposal on manual hold](../../plan/ws014/ws.md).
There is no published current GPU object ABI to enumerate here.

## System administration and observation

[system.h](../../include/uapi/zedbsd/system.h),
[mountinfo.h](../../include/uapi/zedbsd/mountinfo.h) and
[system-device.c](../../src/drivers/generic/system-device.c) define
`/dev/system`, group `s`. The device exposes ioctl operations, not a stream
of textual status. Device-node access alone does not authorize privileged
operations: handlers apply their own checks.

| Number / suffix (`ZEDBSD_SYSTEM_`) | Record / authority |
| --- | --- |
| 1 `GET_INFO` | `system_info`: boot BIOS identifier and device/partition counts |
| 2 `GET_DEVICE` | `system_device_info`: indexed device description |
| 3 `GET_VMSTAT` | `vm_statistics`: 64-bit memory accounting fields |
| 4 `HALT`, 5 `REBOOT` | PID 1 only, otherwise `EPERM`; checked shutdown precedes the platform action |
| 6 `GET_RESOURCES` | Live kernel object counts, not an allocation-history trace |
| 7 `GET_PROCESS` | `process_info`: next process after the supplied PID cursor |
| 8 `GET_FILE_USAGE` | Versioned path/cursor query; own-UID or root inspection |
| 9 `SWAP_ADD`, 10 `SWAP_REMOVE` | Versioned `system_swap_control`; superuser required |
| 11 `GET_SWAP_SOURCE` | Versioned source state and slot counts |
| 12 `GET_MOUNTS` | Versioned mount membership snapshot with bounded records |

Initialize the exact version, structure size and zero reserved fields required
by each header. The swap-control ABI version 1 is unrelated to the on-disk
swap format; the current formatter writes ZEDSWAP2. Runtime swap changes use
backing claims and draining, not a rewrite of immutable boot parameters.
Prefer `/sbin/swapon` and `/sbin/swapoff` to reproducing their ioctl protocol.

Process enumeration returns the next greater PID (`-1` includes PID 0) and
ends with `ENOENT`. For a non-root observer of another UID, command text and
controlling-terminal presence are redacted; this does not hide all process
statistics. File-usage enumeration has its separate permission and cursor
rules. Neither enumeration promises a frozen process population.

The mount-query header is 32 bytes and each entry 544 bytes, at most 64 entries.
`ENOSPC` returns the required count in the header without partial entries.
Retry with bounded capacity and handle concurrent changes; a successful
membership snapshot is not a reservation preventing a mount or rename.

Use the ordinary `halt`/`reboot` administration commands, which coordinate
with init, rather than invoking these ioctls from an arbitrary root process.
Console/graphics/system nodes normally use the devfs non-event-device mode
0666; this does not override the system handler's PID/UID checks or graphics
ownership. See [devfs](../../src/kern/devfs.c) for node policy and
[service shutdown](init-services.md) for orchestration.

Relevant evidence includes [runtime swap](../../plan/ws016/tests/README.md),
[USB-root checked halt q141](../../plan/history/queue-q141.md), and
[resource-baseline q142](../../plan/history/queue-q142.md). Resource counts are live:
an independently retiring boot worker can change the baseline, so immediate
before/after subtraction alone does not prove a leak.
