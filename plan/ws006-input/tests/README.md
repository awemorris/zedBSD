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
| IN-T41 | QEMU USB HID | xHCI keyboard, mouse/tablet, hotplug, disconnect, and event delivery pass |
| IN-T42 | Physical USB HID | Target laptop keyboard/mouse devices identify and operate through evdev across reconnect |
| IN-T50 | Legacy removal | No in-tree consumer uses console continuous-event/key-state UAPI and console regressions pass after deletion |

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
