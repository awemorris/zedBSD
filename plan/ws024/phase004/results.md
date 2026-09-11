# WS024 p004 results

2026-09-07; q102 in progress. These are executed results, with remaining work
listed below. No physical runtime was performed by the agent; WS025's physical
gate is user-accepted under the explicit instruction to proceed.

## Consolidated acceptance

- `temp/p004-focused`: unified metadata 6,595, view 6,635, run and consistency
  gates pass ordinary and ASan/UBSan; real LP64/ILP32 width gates pass. WS025
  observer, run and view gates pass under their corresponding WS025 temp paths.
- `temp/p004-identity-first.log`, `p004-endian-unified.log`: filesystem identity,
  selectors and both-endian fixtures pass with one production owner.
- `temp/p004-directory-2.log`: VFS dispatch 13, UFS sync 18 and namespace
  rollback 83 checks pass, including sanitizer mutation coverage and overlay
  directory fsync. `p004-socket-2.log` and `p004-socket-sanitize.log`: 78 pathname
  socket ownership/rollback checks pass in each variant.
- `temp/p004-images.log`: maintained large-directory and bounded root/data
  fixture checks pass. The fixture now reads the 64-bit dinode size at byte 16.
- `temp/p004-features-2/results.json`: mounted features, remount and reboot pass,
  including malformed/oversized xattrs, user/group quota hard-limit refusal,
  accounting reclamation on unlink, persistent limits, snapshot old content,
  and live-mount busy refusal. The previous p004-features attempt stopped at a
  wrong test expectation: generic xattr namespace validation returns EOPNOTSUPP
  for `user.`. Production semantics were retained; the oracle was corrected.
- `temp/p004-consistency-expanded-2.log` and `p004-consistency-sanitize.log`:
  114 checks each pass, adding all five journal flush failure boundaries,
  sticky poisoning on failed recovery, full snapshot store, and all three
  preserve flush failures with ownership unpublished until a successful retry.
- `temp/p004-platform/result.json`: actual ARM64/RPi4/SPARC producer/check paths
  pass. Inputs are synthetic ABI headers and firmware placeholders; this is
  packaging/structure evidence, not executable kernel or physical boot evidence.
  Fixed stale common-checker paths and missing RPi4 data/swap arguments.
- WS018 `temp/ws024-p004-final/results.json`: **50/50 storage scenarios pass**,
  including fresh xHCI two-boot acceptance; WiFi's 30 scenarios pass ordinary
  and sanitizer variants. Generated source images remain protected. Earlier
  retired-source run passed its functional host cells but stopped while hashing
  deleted tracked paths. The runner now hashes existing tracked/untracked source
  files and records deleted source paths separately.
- `temp/p004-pcat-build.log`, `p004-pc98-build.log`: configured make -j16 pass
  after old-owner removal. amd64 also passed `p004-amd64-build.log`; final normal
  amd64 restoration is in progress before the legacy-media mount checks.

## Retirement

26 superseded source/header/producer files were backed up with SHA-256 before
removal: `temp/p004-retired-source/sha256.json`. This preserves the prior local
uncommitted UFS changes as well as the retired codecs. The 42 pre-transition
images remain separately backed up in `temp/pre-transition/images.json`.

The production owner is `src/drivers/fs/ufs/`; old public type spellings remain
negative acceptance cases, and published old I/O statistic IDs remain ABI history.
Active historical fixture entrypoints now link/include unified source. Internal
mount/inode layout fixtures share `ufs-private.h` rather than stale struct copies.

## Remaining completion evidence

Finish final amd64 restoration, legacy UFS2 read-only mount/marker preservation
and UFS1 no-mutation rejection, final source/symbol/dependency inventory, then
synchronize the completed U01–U24 matrix into P/W/M and resume WS025 p011/p013.

## q102 completion

All selected remaining gates passed. Final amd64 restoration passes
`temp/p004-amd64-final-build.log`; the migrated credential guest compiles/links
and passes its ELF/undefined-symbol checks (`p004-credential-build.log`). Its
entire historical standalone QEMU matrix was not rerun; the selected namespace
fault, feature and storage runtime matrix above is the acceptance scope.

`temp/p004-legacy-ufs2/results.json` passes read-only mounting of an image made
by the backed-up pre-transition UFS2 producer: the original `zedBSD ufs2 root v1`
marker remains readable, and the entire image hash is unchanged. The ordinary
root recognizer requires the exact new marker; this is an explicit transition.
`temp/p004-legacy-ufs1/results.json` passes real mount refusal with the entire
image hash unchanged. Inputs/provenance are in `temp/p004-legacy-inputs/inputs.json`;
the historical producers are backup-only and are not restored to production.

`temp/p004-final-inventory.json` passes existing tracked/nonignored production
source and active fixture path checks, and all three supported kernel symbol
tables contain the single unified filesystem type/identify owner with no old
ufs1_/ufs2_ symbols. Published old statistic enum IDs are the sole allowed
production historical names. An initial broader filesystem walk also found an
ignored editor backup (`cred.c~`); it was preserved, not mistaken for build input.
`git diff --check` passes.

| Matrix | Executed evidence |
| --- | --- |
| U01–U03 | p002 super decoder and p003 ordinary/feature target/C/Python comparison, sparse producer boundaries; feature runtime |
| U04–U05 | p003 real old mkfs spelling refusals; p004 legacy mount/refusal hash-preserved runtime; exact new root marker contract |
| U06–U08 | p002/p004 actual LP64/ILP32 sparse mappings, inode width/super arithmetic and original-width loop gates |
| U09–U11 | consolidated metadata/run/view fixtures; directory and socket rollback; mounted open-inode identity and storage native runtime |
| U12–U13 | expanded feature guest: xattr rejection/persistence, both quota classes limits/accounting/reclaim/remount/reboot |
| U14–U15 | expanded 114-check consistency ordinary/sanitizer; mounted snapshot content, deletion and busy-unmount |
| U16–U17 | p003 formatter fault/descriptor transaction, combined target mkfs/native-to-overlay/two-reboot acceptance; p004 storage native |
| U18–U20 | final amd64/pcat/pc98 builds; p003 native startup and p004 ordinary xHCI two-boot acceptance |
| U21–U22 | platform packaging result, source/path and three kernel symbol inventories; active guest compile |
| U23–U24 | p004 WS025 observer/run/view and full storage gates; p003 unified aligned 202-sample baseline unchanged at 10/20/20 ms |

WS024 p004 is completed. No optional real hardware, cross-BSD interoperability,
ILP32 file ABI widening, or asynchronous/write-back improvement is claimed.
WS025 p011/p013 continue on the sole unified owner.
