# q020 ws008-p002 BeUI completion evidence

Date: 2026-08-28

Phase: [`ws008-p002`](../phase002-beui-zedbsd/phase.md)

Result: PASS

## Acceptance summary

| Test | Result | Evidence |
| --- | --- | --- |
| `NOCT-T010` | PASS | Sanitized host state engine and wiring corpus |
| `NOCT-T011` | PASS | amd64 QEMU drawing markers and checked 1024x768 PPM |
| `NOCT-T012` | PASS | QEMU Shift, relative-motion, left-button, and post-close console markers |
| `NOCT-T013` | PASS | Canonical source, linked-object, symbol, and artifact audit |

The canonical/integration changed-path parity check passed for all 15 paths.
The generic BeUI core, PC-98 GDC, PC-98 Cirrus, and SDL2 dummy-driver host
regressions also passed. The final project build gate was `make -j16`, which
passed; `make check` was not used.

## Host evidence

`run-beui-zedbsd.sh` compiled the production evdev state engine with
`-Wall -Wextra -Werror`, ASan, and UBSan. The corpus passed capability-derived
keyboard/relative-pointer/absolute-pointer classification in varied source
order, press/repeat/release and button state, relative integration, absolute
scaling, `SYN_REPORT`, visible `SYN_DROPPED` reset and resynchronization,
partial and multiple records, unknown records, detach, and cleanup. Its wiring
audit also found no legacy console event/key-state/drain-input dependency in
the canonical backend.

The separate canonical host suite passed the generic core and the retained
PC-98 GDC and Cirrus backends. The SDL2 suite passed with its dummy video and
audio drivers, proving that selecting the new zedBSD backend did not regress
the independently selectable desktop backend.

## QEMU graphics and input evidence

The reusable runner was:

```sh
plan/ws008-noct/tests/qemu-beui-zedbsd.sh
```

It used QEMU 10.0.11, an amd64 PC/AT private configuration, a disposable copy
of `build/amd64/hdd-image.img`, and this guest command:

```sh
/usr/bin/noct -j0 /usr/share/noct/beui-zedbsd.noct
```

The non-graphical guest channel reported the complete successful sequence:

```text
NOCT-T011-BEGIN
NOCT-T011-DRAWN
NOCT-T012-SHIFT-OK
NOCT-T012-MOTION-OK
NOCT-T012-LEFT-OK
NOCT-T012-INPUT-OK
NOCT-T011-CLOSED
NOCT-T012-CONSOLE-OK
```

The PPM checker independently validated the visible result:

```text
background 0x102030 783385
pattern 0x406080 1536
line/glyph 0xffffff 1507
BMP red 0xff0000 1
BMP green 0x00ff00 1
BMP blue 0x0000ff 1
BMP yellow 0xffff00 1
NOCT-T011-PIXELS-OK 1024x768
```

The canonical CMake, packaged, and staged Noct artifacts were byte-identical;
their SHA-256 was
`69b02ab00441e12868348a97ba7a9802455cfe92921656990b36efdc7a3ea086`.
The tested production image SHA-256 was
`be16534a98677cd839c067d8f67059af7a8327d534f3500fe04e56e4579fc51d`,
and the captured PPM SHA-256 was
`beab546dd85f0d64c94b5e1333380f001d1df83c430f0d841eb1b592cb20a4bf`.

## Evidence lifetime

The successful raw run was recorded temporarily at
`plan/ws008-noct/temp/q020-p002-beui.59sAEO/`. That directory is ignored and
disposable: its disk image, logs, generated checker, and screenshot are not a
permanent repository artifact. The checked-in runner, fixtures, pixel checker,
this result summary, and the hashes and markers above form the durable
reproduction record.
