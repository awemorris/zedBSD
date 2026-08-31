# WS006 shared test cases

Parent: [WS006](../ws.md)

| Case ID | Area | Required observation |
| --- | --- | --- |
| IN-T00 | UAPI | `input_event`, constants, ioctl layouts, timestamps, and 32/64-bit ABI match the published compatibility profile |
| IN-T10 | Input core | Multiple readers, queue overflow/resync, nonblocking read, poll, grab/permissions, and detach wakeup pass |
| IN-T11 | Capability/state core | Registration validation, capability bitmaps, query lengths, current key/button state, and ABS metadata/state pass |
| IN-T12 | Capability discovery | An amd64 guest discovers production keyboard and relative-pointer roles without event numbers or names |
| IN-T20 | Console bridge | Console text input and evdev readers coexist with modifiers, repeat, and virtual-terminal behavior |
| IN-T30 | Consumer migration | Xzed and BeUI keyboard/relative/absolute pointer cases pass without console event ioctls |
| IN-T40 | USB HID parser | Valid and malformed report descriptors, bit bounds, boot/report protocols, and unknown usages pass |
| IN-T41 | QEMU USB HID | xHCI and paired EHCI/UHCI keyboard, mouse/tablet, hotplug, disconnect, event delivery, and concurrent USB-root I/O pass |
| IN-T42 | Physical USB HID | Target laptop keyboard/mouse devices identify and operate through evdev across reconnect |
| IN-T50 | Legacy removal | No in-tree consumer uses console continuous-event/key-state UAPI and console regressions pass after deletion |

The q044 ownership, console, producer, and physical-HAL fixtures are run with:

```sh
sh plan/ws006-input/tests/run-input-ownership-host-test.sh
```

This runner covers ordinary and ASan/UBSan input-device/subscriber lifecycle,
two-source state, atomic overflow resynchronization, console drain/detach, and
amd64/i386 PC/AT, PC-98, and X68000 producer behavior. The p007 IN-T40 corpus
links `src/drivers/hid/hid-report.c` directly with
`plan/ws006-input/tests/hid-report-test.c`; its strict, ASan/UBSan, and GCC
analyzer modes each pass 791 checks in q044.

Executable paths are added when each Phase is extracted.

IN-T00 uses `evdev-layout-test.c`. Compile it for both zedBSD x86 ABIs without
linking:

```sh
cc -m64 -nostdinc -Ilibc/include -Iinclude/uapi -Iinclude \
  -DZEDBSD_USER_ABI_LP64 -std=c11 -Wall -Wextra -Werror -fsyntax-only \
  plan/ws006-input/tests/evdev-layout-test.c
cc -m32 -nostdinc -Ilibc/include -Iinclude/uapi -Iinclude \
  -std=c11 -Wall -Wextra -Werror -fsyntax-only \
  plan/ws006-input/tests/evdev-layout-test.c
```

IN-T10 begins with the implementation-shared bounded queue model:

```sh
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Iinclude/uapi -Iinclude \
  -Wall -Wextra -Werror src/drivers/input-queue.c \
  plan/ws006-input/tests/input-queue-test.c -o /tmp/ws006-input-queue
/tmp/ws006-input-queue
make -j16 build/amd64/vmunix
```

IN-T11 links the production capability/state helper directly. The strict run
freezes registration validation, bit ordering and boundary bits, zero-filled
capability storage, key press/release/repeat state, and absolute-axis metadata
and value updates. The sanitizer run checks the same query boundaries for
out-of-bounds and undefined behavior:

```sh
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Iinclude/uapi -Iinclude \
  -Wall -Wextra -Werror src/drivers/input-capability.c \
  plan/ws006-input/tests/input-capability-test.c \
  -o /tmp/ws006-input-capability
/tmp/ws006-input-capability
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Iinclude/uapi -Iinclude \
  -Wall -Wextra -Werror -g -fno-omit-frame-pointer \
  -fsanitize=address,undefined src/drivers/input-capability.c \
  plan/ws006-input/tests/input-capability-test.c \
  -o /tmp/ws006-input-capability-sanitize
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/ws006-input-capability-sanitize
```

IN-T20 begins with the producer key normalization contract:

```sh
cc -std=c11 -DHAL_ARCH_AMD64 -Iinclude/uapi -Iinclude -Wall -Wextra \
  -Werror src/drivers/input-keymap.c \
  plan/ws006-input/tests/input-keymap-test.c -o /tmp/ws006-input-keymap
/tmp/ws006-input-keymap
```

Guest event-node read/poll/ioctl evidence is added with IN-02, because IN-01
does not install a fake production input device merely to make `/dev/input`
appear populated.

The production keyboard/console coexistence observation for `ws006-p004` is
recorded in [qemu-evdev-evidence.md](qemu-evdev-evidence.md).

IN-T12 uses a test-only amd64 user program and disk image.  The guest program
enumerates every decimal `eventN` entry in `/dev/input`, queries only
`EVIOCGBIT`, prints the complete event/key/relative-axis code sets, and derives
keyboard and relative-pointer roles from those sets.  The runner rejects fixed
event-number literals and name/identity ioctls in the probe source:

```sh
plan/ws006-input/tests/qemu-evdev-capability.sh
```

The runner builds with `make -j16`, creates a writable QEMU image copy under
the untracked WS temp directory, requires exactly one production keyboard and
one relative pointer, scans the guest log for fatal errors, and verifies that
`config.mk` was not modified.  The production rootfs does not gain a permanent
test command; [evdev-capability-qemu.mk](evdev-capability-qemu.mk) adds the
probe only to the dedicated IN-T12 image. The final q020 transcript and hashes
are retained in
[qemu-evdev-capability-evidence.md](qemu-evdev-capability-evidence.md).

IN-T41 uses a private production image and a test-only guest probe:

```sh
plan/ws006-input/tests/qemu-usb-hid-acceptance.sh \
  build/q048-p008-usb-hid-acceptance
```

The default matrix boots a fresh image copy for each of two q35 topologies.
The xHCI cell places USB Storage root and HID on one controller. The paired
legacy cell places Storage on ICH9 EHCI and USB 1.1 HID on its companion UHCI.
Both disable i8042, so login, shell control, and the explicit console marker
must travel through the production USB keyboard.

The guest discovers keyboard, relative-pointer, and absolute-pointer roles
from `EVIOCGBIT` capabilities and `BUS_USB`, never from a fixed event number or
device name. It checks exact keyboard, relative, and absolute records. The
hotplug sequence keeps the first pointer fd open across detach, requires EOF or
HUP, proves that the stale generation reserves its `eventN`, closes the old
fd, and then proves that a later generation may reuse the released number.
Each cell also overlaps a 64 MiB read from the USB root disk with pointer event
delivery before attaching the tablet.

`USB_HID_QEMU_CELLS=xhci` or `paired` selects one topology for focused reruns.
All build products, temporary compiler files, writable media, OVMF variables,
logs, hashes, and tabular results remain below the named output directory (or
the untracked WS temp directory when no output is supplied). The runner uses
timeouts, rejects QMP command failures and fatal guest diagnostics, preserves
`config.mk`, and never installs the guest probe in an ordinary production
image. IN-T42 remains a separate, single bounded physical observation after
the automatic milestone.

The final q048 IN-T41 evidence is split across two immutable private builds:

- `build/q048-p008-xhci-usbonly3/results.tsv` records every xHCI gate as
  `pass`; its source image SHA-256 is
  `457ca9583d814e61df71ee86e7e28aecc50356da0f0b4d8a085761368ca38733`.
- `build/q048-p008-paired-uhci-baseline1/results.tsv` records every paired
  EHCI/UHCI gate as `pass`; its source image SHA-256 is
  `40fa1a6149c4123c73b6ff789587192058130622605dff79108eea1821161e96`.

Both logs contain capability-only keyboard/relative/absolute selection, exact
records, USB-only console control, stale-fd isolation and number reuse, plus
the overlapping 64 MiB USB-root read. The final q048 regression pass also
cleared the legacy-HCD, USB-recovery, xHCI concurrent-URB, dynamic lifecycle,
IN-T30--IN-T35, and USB HID 92-check/sanitizer/hot-unplug gates, followed by
default PC-98, amd64, and i386 full builds and disk-image generation. The
pre-existing undefined `NOCT_NM` still causes part of the i386 undefined-symbol
scan to be skipped. The replacement host `nm -u` scan of `build/pcat/vmunix`,
all top-level `build/pcat/*.ELF`, and every `build/pcat/bin` file found zero
undefined-symbol lines; its transcript is
`build/q048-regression-tmp/pcat-bin-undefined-symbols.txt`. The Makefile defect
is tracked as `BUG-007`. Physical IN-T42 was not run in q048.
