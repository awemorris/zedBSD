# WS007 shared test cases

Parent: [WS007](../ws.md)

| Case ID | Area | Required observation |
| --- | --- | --- |
| GFX-T00 | X11 packaging | The intended image contains `/bin/startx` and launches Xzed, zwm, zshell, and zterm repeatably |
| GFX-T01 | X11 input | Relative/absolute pointer state reaches screen coordinates correctly through evdev; keyboard/session shutdown pass |
| GFX-T02 | PC-98 Xzed bus mouse | PC-98 slave PIC activity keeps master IRQ7 cascade open, concurrent slave masks coexist, and five QEMU `mouse_move 20 10` operations move the production Xzed cursor exactly `+100,+50` through evdev |
| GFX-T10 | GPU UAPI | Version negotiation, feature queries, object lifetime, invalid sizes/handles, isolation, cleanup, and 32/64 ABI pass |
| GFX-T11 | Display takeover | VBE/GOP fallback, GPU takeover, mapping ownership, failure recovery, and panic/console output pass |
| GFX-T20 | i915 | Modeset, scanout, submission, fences, hang/reset, and teardown pass on the exact Latitude GPU |
| GFX-T30 | Vulkan | Declared feature profile and applicable conformance/rendering cases pass; unsupported compute reports honestly |
| GFX-T31 | GLES2 | GLES 2.0 API/rendering suite passes on Vulkan within documented limits |
| GFX-T40 | Wayland | Compositor/session, surfaces, input, clients, shutdown, fallback, and recovery pass |

Software/model results and target i915 results remain separate evidence.

## X11 focused commands

The internal relative-pointer/clamp state is checked independently of the
current PS/2 transport:

```sh
cc -std=c11 -I. -Wall -Wextra -Werror \
  plan/ws007-graphics/tests/xzed-pointer-test.c \
  -o /tmp/ws007-xzed-pointer-test
/tmp/ws007-xzed-pointer-test
```

GFX-T00 and the device-path part of GFX-T01 use the bounded QEMU matrix recorded
by their Phase documents. QEMU monitor mouse deltas are sent as small repeated
movements because a single delta outside the PS/2 packet range sets hardware
overflow bits and is intentionally discarded by the current driver.

## GFX-T02 PC-98 PIC cascade and production Xzed mouse

Compile and run the focused fixture directly against the production PC-98 PIC
implementation:

```sh
cc -std=c11 -Wall -Wextra -Werror \
  -Iinclude -Iinclude/uapi -Isrc -Isrc/hal/i386 -Ilibc/include \
  plan/ws007-graphics/tests/pc98-pic-cascade-test.c \
  -o /tmp/ws007-pc98-pic-cascade-test
/tmp/ws007-pc98-pic-cascade-test
```

After `make -j16` has produced the normal PC-98 image, run the end-to-end gate
with a new evidence directory:

```sh
build/NoctLang/build-static/noct --path=tools/build \
  plan/ws007-graphics/tests/pc98-xzed-mouse-qemu.noct \
  . plan/ws007-graphics/temp/gfx-t02-fresh
```

The runner starts the maintained qemu-pc98 binary with a disposable copy of
`build/pc98/hdd-image.img`, logs in, launches Xzed, captures PPM images before
and after five `mouse_move 20 10` operations, and invokes
`xzed-cursor-ppm-test.c` as the visual oracle. Acceptance requires exactly one
standard Xzed cursor in each capture and an exact `+100,+50` hotspot delta.
Both readiness and movement use bounded visual polling rather than a fixed
Xzed startup delay.
The runner deliberately has no monitor operation which reads or writes PIC
ports. It also verifies that the source disk hash is unchanged and rejects a
fatal guest log.

The 2026-08-31 q039 execution passed with `(320,240) -> (420,290)`. q043
repeated that exact result on final image SHA-256 `b62c958f...` and emulator
SHA-256 `9400ec81...`. That emulator exposes only `-display none` and rejects
`-vnc`, so an interactive host-pointer/focus/grab cell remains external rather
than being represented by the deterministic monitor-injection gate.
