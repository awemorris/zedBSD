# p039 results

Completed q172, 2026-09-09.

Implemented rootview.noct and installed it with the amd64 installer package.
Workspace mount records now carry explicit fat/ufs type, with the existing FAT
wrapper retained. Shared source acquisition admits the actual backing file,
then acquires an independently owned read-only UFS view; source revalidation
checks the view as well as source provenance and immutable artifacts.

Host Noct (build/NoctLang/build-static/noct): installer-rootview.noct passes 12
scenarios under JIT and -j0. Covers normal admission, missing/writable/incorrect
geometry, parent/offset, duplicate registration, post-mount rename/root change,
wrong mount, uncertain mount completion and unsupported filesystem. Existing
installer-workspace.noct passes all 10 lifecycle cases in both engines.
An initial workspace invocation mistakenly supplied one argument, selecting its
native mode; host mkdir correctly failed outside the workspace. Correct no-arg
host invocations passed; no product change was needed for that fixture usage.

amd64 disk-image build: terminal exit 0, /tmp/zedbsd-q172-amd64.log.
No kernel or other-architecture package sources changed in this phase.

Public installer QEMU: temp/q172-source1, terminal exit 0,
/tmp/zedbsd-q172-source1.log. Live source admission and all shared source mounts
completed, followed by destination preparation and the actual review screen.
Escape cancelled; /run/zedinst no longer existed, and live root-image identity
was unchanged. Target and production image hashes matched. Independently
extracted rootfs.img hash also matched the original:
05064cf7aee66a3e82bc3e700fadcca151c8c2e7468afe3dba47ed91aa92272d.
See ../temp/q172-source1/result.json. Source and review screenshots were captured;
the actual review screenshot was visually inspected.

This completes shared source acquisition, not native installation. Full
transaction/copy/swap/fallback boot acceptance remains p006/p007, and graphic
frontend p029 remains required. User confirmation remains before disk writes.
