# WS017 Phase 004: amd64 UEFI Xzed LFB acceptance

Last updated: 2026-08-27

WSID: `ws017`

Phase ID: `p004`

Combined ID: `ws017-p004`

Status: Planned; Queue-ready after `ws017-p001`--`p003`

Parent: [WS017](../ws.md)

Tests: [WS017 test index](../tests/README.md)

## Objective

Demonstrate that the production amd64 UEFI image launches the existing Xzed
desktop through the new mapped-LFB path and that the old device-independent
path remains a usable fallback.

## Acceptance procedure

Use `qemu-system-x86_64` with the repository-pinned OVMF files and a disposable
copy of `build/amd64/hdd-image.img`.

1. Boot through UEFI to login, run the literal `startx`, and require Xzed,
   zwm, zshell, and zterm to reach a stable desktop.
2. Capture the exact LFB query and Xzed startup marker proving
   `ZEDBSD_GRAPHICS_GET_LFB_INFO` succeeded, a mapping was created, and the
   selected width/height/bpp/stride/mask layout matches the firmware handoff.
3. Exercise full redraw, a bounded window move/expose, text update, and pointer
   movement. Capture a screenshot or deterministic framebuffer sample proving
   visible output and cursor updates.
4. End the session, verify the final mapping/owner lease is released and the
   console resumes, then start Xzed again successfully.
5. Run one forced-unavailable or forced-mmap-failure test configuration and
   require the `ioctl-blit` marker plus a visibly usable desktop.

The harness records command line, QEMU/OVMF versions and hashes, image hash,
serial/debug log, markers, screenshots or samples, and result table under
WS017 `temp/`. It bounds every wait and scans for panic, fault, VM/device-map
errors, and ownership leaks.

## Completion conditions

- LFB-T011 reaches the desktop and redraw/input cases through `lfb-mmap` under
  amd64 UEFI;
- direct layout evidence agrees with the accepted GOP framebuffer and visible
  colors/cursor are correct;
- session exit and second start prove deterministic mapping release;
- LFB-T012 reaches the same usable desktop through `ioctl-blit` fallback;
- affected host regressions, `make -j16`, and `git diff --check` pass; and
- no PC-98 Cirrus implementation or acceptance claim is included.

## Reconsideration boundary

If QEMU succeeds but target hardware later shows a cache/coherency defect,
record this Phase as the software milestone and extract a physical cache-policy
Phase. Do not add write combining or GPU modesetting as an unplanned repair.
