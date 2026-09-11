# q130 atomic publication results

Date: 2026-09-09
Status: completed

## Implementation

- Shared RENAME_NOREPLACE flag and explicit syscall 163 / libc renameat2.
  The VFS checks destination lookup under the existing mount namespace mutex,
  including same-inode aliases, consumes the flag and invokes drivers with
  their original zero flag. Unknown flags fail; ordinary rename still replaces.
- mv -n/--no-clobber skips conflicts; --update=none-fail reports failure.
  -T/--no-target-directory prevents directory reinterpretation. No cross-mount
  copy/delete fallback or private installer command was added.
- FAT directory fsync uses the existing mount metadata flush and disk barrier.
- New ordinary UNIX sync command under userland/base, selected by default for
  newly generated configs and added to the three explicit CI configs. File
  operands report open/fsync/close failures; whole-system sync reports this
  libc's mount-sync errno. User config.mk was preserved.
- Reference: docs/reference/atomic-publication.md. Noct helper was not added.

## Evidence

- temp/q130-publication-vfs-final: current mount/inode/namei/cache code,
  2,048 checks in each ordinary and ASan/UBSan run, including 100 concurrent
  publishers, same inode, unknown flags, wrong mount, busy source and EIO.
- /tmp/zedbsd-q130-commands-host.log: production mv/sync ordinary + sanitizer,
  exact-path/no-clobber, symlink conflicts, open/flush/close failure ordering.
- temp/q130-fat-fsync4: current full FAT source, ordinary + ASan/UBSan,
  dirty-sector failure retains retry state, disk barrier failure propagates,
  retry succeeds. Unused disk reads are fail-fast boundary mocks in a separate
  translation unit; no private driver fragments or copied implementation.
- temp/q130-publication-final/result.json: PASS publication. Three filesystems
  (NVMe FAT, USB-root overlay, native UFS on second USB disk), each with 20
  competing thread publishers, two competing real mv processes, no-replace
  conflicts, old rename, directory fsync. Native UFS hard-link alias and FAT
  case alias included. Actual mv conflict exit status, ordinary replacement,
  per-file/global sync and missing-file failure pass. Three persisted files and
  the replaced command output survive reboot. GPT/FAT labels/sentinel and the
  production image hash are unchanged.
- Explicit make -j16 amd64: /tmp/zedbsd-q130-amd64-sync.log; fixture final:
  /tmp/zedbsd-q130-publication-build-final.log. PCAT:
  /tmp/zedbsd-q130-pcat.log. PC98: /tmp/zedbsd-q130-pc98.log. All passed.
- git diff --check passed. No aggregate make check or commits.

## Failures used to refine implementation and fixtures

- The original rename wrapper passed its destination pointer in the shared
  mode/option field. Forwarding that field as new flags broke ordinary mv.
  q130-publication-qemu3 caught EINVAL; the wrapper now supplies mode only for
  mkdir and zero for ordinary rename. Final runtime covers both ABIs.
- Initial overlay hard-link test failed EOPNOTSUPP (zedBSD errno 21). Overlay
  lacks a link operation. Native UFS now supplies that test; overlay still
  undergoes full publication and persistence testing. A commentary initially
  misnamed errno 21 EISDIR and was corrected after inspecting errno.h.
- A second NVMe controller was rejected by the existing single-controller
  driver profile. The final fixture uses a second USB disk for native UFS.
- sync initially was absent from the explicit CI program list; ordinary
  packaging and all three CI selections were corrected before final runtime.
- Full-source FAT host compile exposed GCC inference warnings from an always-
  failing inlined read mock in unused BPB parsing. Moving that mock to a
  separate translation unit removed the fixture artifact without suppressing
  warnings or changing production parsing.

## Remaining boundaries

p015 does not freeze source content across separate commands or implement the
installer. p016 supplies boot provenance; p017 resolves staging and command
ownership. BUG001 IDE flush failure and BUG010 USB halt failure remain separate
open work; this USB-root acceptance does not claim either fixed.
