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
