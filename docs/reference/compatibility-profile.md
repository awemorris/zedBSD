# C interfaces and compatibility profile

Status: current declarations; conformance work remains active

zedBSD supplies a target libc and its own syscall/device ABI. A familiar
function name or a feature-test macro is not evidence of complete POSIX/SUS
conformance, Linux binary compatibility or BSD disk compatibility. The
[WS001 ledger](../../plan/ws001/ws.md) owns outstanding behavior and
acceptance; individual implementation tests establish the supported subset.

## Feature selection

The source of truth is [features.h](../../libc/include/features.h).
Its current declarations include:

| Declaration | Current value / selection |
| --- | --- |
| `_POSIX_VERSION` | `202405L` |
| `_POSIX2_VERSION` | `200809L` |
| `_XOPEN_VERSION` | `700` |
| `__ZEDBSD_POSIX_2024_VISIBLE` | 1 when `_POSIX_C_SOURCE >= 202405L` or `_XOPEN_SOURCE >= 800`; otherwise 0 |
| `_POSIX_THREADS`, `_POSIX_TIMERS`, `_POSIX_MONOTONIC_CLOCK` | `200809L` |
| `_POSIX_DEVICE_CONTROL` | `202405L` |
| `_POSIX_THREAD_SAFE_FUNCTIONS`, priority inheritance/protection, asynchronous/prioritized I/O, typed memory objects | `-1` |

These are literal implementation declarations, not an independent certification
of the corresponding standards. In particular, the selector threshold 800
and reported X/Open version 700 must not be collapsed into a claim of full
SUS Issue 8 support. Legacy declarations are visible by default and under the
older selectors as specified by `__ZEDBSD_LEGACY_VISIBLE`. Inspect the header
for the exact combined-selector expression before relying on mixed selectors.

Use a consistent selector before including any public header. Header visibility,
link availability and correct runtime behavior are separate checks. Unsupported
operations must be handled through the individual call's documented return
and `errno`; do not infer success from `_POSIX_VERSION` alone.

## ABI boundary

[uapi/types.h](../../include/uapi/types.h) selects `uapi_ptr_t` as
64 bits for `ZEDBSD_USER_ABI_LP64`, otherwise 32 bits. Structures containing
that type and ioctl encodings must be compiled for the intended ABI. Do not
copy host Linux structures, assume all UAPI records have one architecture-neutral
size, or send a host process's pointer to a guest kernel.

The maintained x86 builds cover amd64 LP64 and i386 ILP32 (PC/AT and PC-98).
[TLS](tls.md) documents the accepted x86 thread-pointer and executable TLS
boundary. [evdev](evdev.md) describes a bounded compatibility profile, while
[console/graphics/system](control-devices.md) are zedBSD-specific controls.
These documents do not extend their tested ABI claims to every non-x86 port.

## Checking an application

Build against the [project sysroot](../howto/build-from-source.md), check the
actual required declarations and symbols, then exercise failure paths on the
target. For example, `isatty` checks termios support; `ttyname_r` must identify
the actual terminal and return an error for a nonterminal. It must not label
every graphical PTY `/dev/console`. The real Xzed/zterm PTY and error cases
passed [q147](../../plan/history/queue-q147.md).

Use [atomic publication](atomic-publication.md) and
[image formatters](image-formatters.md) for their specific extensions and
restrictions. zedBSD's single UFS formatter profile is not a general claim
that arbitrary FreeBSD or NetBSD filesystems can be mounted or rewritten.
