<!-- awesome-plan project=zedbsd record=ws193-p009 -->
# WS193 p009: RPi4 Graphical login boot option

Status: cleared（source/option/image criteria、実機desktopは未確認）
Disposition: normal
Parent: [WS193](../ws.md)
Queue: [focused queue](../../ws048/codex-usb-session-queue.md)

Authority: 2026-10-11 current user explicitly requests USB and sessiond startup option; bootloader logo/kernel quiet animation deferred. Source base main fbcb2b543, independent codex/rpi4-usb-session.
Scope: generate stable cmdline.txt from ZEDBSD_GRAPHICAL_LOGIN, package on FAT through both RPi4 image writers, validate exact payload with existing image checker. Firmware /chosen/bootargs → HAL boot.command-line → kern.boot.login → existing greeter service/sessiond is preserved. No new login policy or HAL API.
Criteria: option y/n/y updates command line and invalidates image as needed; unchanged option retains timestamp; FAT readback exact; existing rootfs contains sessiond/greeter and required service/config; target kernel/sessiond build and changed-source full-standard review. Physical desktop acceptance belongs to user, record it separately.
Applicable rules: AGENTS.md, plan/guardrail.md, full C coding standard for changed C paths; Python existing conventions, focused syntax/diff and image checks. No LLVM rebuild, QEMU or push. Shared projection/GitHub pending Q1.

## Build environment follow-up

Current-config full image validation uses a fresh owned build, current main sysroot copied and its completed stamp held with -o; shared LLVM/source and host Noct are read-only links with source/build verification stamps held. Target Noct is built in this dedicated worktree under the earlier user's express approval「専用worktreeでビルドしてよい」; no Noct/toolchain source or rules changes. Current validated archive/firmware cache copied from main, existing package hash/license validation retained. The target Noct CMake output copied with the source is moved aside in owned build before configuration, preventing writes to its old main path. No prior test image/application artifacts are inputs.

## Terminal outcome / 2026-10-11

rpi4-usb-session-20261011-i02 cleared for the scoped menuconfig boot-option contract: exact login parameter generated and packed, unchanged setting keeps image mtime, y→n→y rebuilds image and FAT readback matches each requested value without rebuilding rootfs. Full current-config make -j16 and check-disk-image pass; target sessiond/Wayland/passkey, greeter service/account and login config present. Final changed-source full-standard review complete; [evidence](../../ws048/tests/rpi4-usb-session-20261011.md). Physical greeter/desktop not tested and WS193 whole acceptance is not claimed. Specific source commit integration is pending user approval. No logo/quiet animation added, no push, shared projections/publication pending Q1.
