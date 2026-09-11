# WS007 Phase 001: X11 launch and startx repair

Last updated: 2026-08-25

Phase ID: `ws007-p001`

Status: complete

Parent: [WS007](../ws.md)

Tests: [WS007 test index](../tests/README.md)

## Objective

Verify the installed X11 session path in the production amd64 image and repair
the failure that prevents `/bin/startx` from being invoked by name.

## Baseline and diagnosis

The image already contained executable `/bin/startx`, Xzed, zwm, zshell,
zterm, and session configuration. In QEMU, `/bin/sh /bin/startx` launched the
desktop, but `startx` returned `sh: startx: not found`.

The shell's PATH search accepted only files identified as ELF. Its later
execution path already knew how to run a non-ELF executable through `/bin/sh`,
making that branch unreachable for scripts found through PATH.

## Work packages

- [x] Confirm the Make-controlled image manifest installs `/bin/startx` mode
  0755 and all four X11 programs.
- [x] Reproduce the command-name failure in the production image.
- [x] Make shell PATH lookup select executable regular files, not ELF only.
- [x] Preserve ELF execution and route executable scripts through the existing
  shell-script path.
- [x] Reject non-executable/non-regular direct script execution.
- [x] Build the full configured system.
- [x] Boot the rebuilt image and launch `startx` by name.

## Acceptance evidence

QEMU 10.0.11 ran `build/amd64/hdd-image.img` with `-machine pc -m 512 -smp 4`.
After passwordless root login, the literal command `startx` launched Xzed; the
captured display showed zwm, zshell, and a zterm session. This also proves that
the session script and its installed mode are usable through normal PATH
resolution.

## Resume point

GFX-00 is closed. Broader session exit/restart behavior belongs to GFX-02;
mouse investigation is recorded separately in `ws007-p002`.
