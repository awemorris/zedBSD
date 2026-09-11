# ws019-p050: PC98 graphical FAT installation

Status: completed / cleared q186 (q185 was uncleared: BUG-020)
Timebox: 180 active minutes
Authorization: user 2026-09-10; autonomous implementation and QEMU smoke test
Parent: [WS019](../ws.md)

## Scope

Run /sbin/zedinst-graphic on PC98 with two IDE HDDs. Disk 0 is an
installer source; disk 1 has an existing PC98 FAT partition and working PC98
boot entry. Select FAT coexistence only, copy the PC98 kernel/loader/config
and root/data/swap image payloads using the shared Noct progress machinery,
then boot the installed disk without the source. Preserve unrelated FAT files.
No GPT, native UFS partition provisioning or arbitrary partition editor.
A blank destination requiring partitioning is outside this coexistence scope.

## Implementation order

1. Enable canonical Noct i386 zedBSD builds and installer packaging; retain
   managed dependency patches and amd64 behavior. Use the zedBSD BeUI adapter,
   not the distinct DOS backend. Inspect i386 ABI/JIT limits before choosing
   interpreter/build settings. Build using make -j16 and CI PC98 config.
2. Verify PC98 Cirrus/CoreGraph 640x480 RGB24 and keyboard operation with the
   actual public frontend. Keep source admission and rootfs.img mount check.
3. Add platform-specific source/destination identity, PC98 disklabel/FAT
   admission and BIOS artifact/config selection, retaining shared copy,
   progress, transaction and UI code. Hide dedicated/native mode on PC98.
   Never overwrite a whole disk or inject GPT to satisfy the fixture.
4. Use disposable copies with two IDE disks and the existing PC98 emulator.
   Perform one normal graphical FAT install; save actual screenshots, verify
   payload/source preservation, then boot disk 1 alone and check root access.

## Acceptance and limits

One successful normal path is sufficient. No expansion into exhaustive fault
injection. Record concrete build/runtime blockers and resume conditions if the
timebox cannot complete the port; do not substitute amd64 or DOS execution for
PC98 zedBSD evidence. Physical PC-9821V13 boot failure remains WS025-p032.

## User platform contract

Detect PC98 from `uname -a` in Noct. Place vmunix, rootfs.img, data.img
and swapfile in the FAT partition, not an EFI partition. Require partition
size <= 4 GiB and every payload file <= 2 GiB before copying; evaluate all
sizes in 64-bit integers. PC98 BIOS loader/config files accompany the payload.
# ws019-p050: PC98 graphical FAT installation

Status: completed / cleared q186 (q185 was uncleared: BUG-020)
Timebox: 180 active minutes
Authorization: user 2026-09-10; autonomous implementation and QEMU smoke test
Parent: [WS019](../ws.md)

## Scope

Run /sbin/zedinst-graphic on PC98 with two IDE HDDs. Disk 0 is an
installer source; disk 1 has an existing PC98 FAT partition and working PC98
boot entry. Select FAT coexistence only, copy the PC98 kernel/loader/config
and root/data/swap image payloads using the shared Noct progress machinery,
then boot the installed disk without the source. Preserve unrelated FAT files.
No GPT, native UFS partition provisioning or arbitrary partition editor.
A blank destination requiring partitioning is outside this coexistence scope.

## Implementation order

1. Enable canonical Noct i386 zedBSD builds and installer packaging; retain
   managed dependency patches and amd64 behavior. Use the zedBSD BeUI adapter,
   not the distinct DOS backend. Inspect i386 ABI/JIT limits before choosing
   interpreter/build settings. Build using make -j16 and CI PC98 config.
2. Verify PC98 Cirrus/CoreGraph 640x480 RGB24 and keyboard operation with the
   actual public frontend. Keep source admission and rootfs.img mount check.
3. Add platform-specific source/destination identity, PC98 disklabel/FAT
   admission and BIOS artifact/config selection, retaining shared copy,
   progress, transaction and UI code. Hide dedicated/native mode on PC98.
   Never overwrite a whole disk or inject GPT to satisfy the fixture.
4. Use disposable copies with two IDE disks and the existing PC98 emulator.
   Perform one normal graphical FAT install; save actual screenshots, verify
   payload/source preservation, then boot disk 1 alone and check root access.

## Acceptance and limits

One successful normal path is sufficient. No expansion into exhaustive fault
injection. Record concrete build/runtime blockers and resume conditions if the
timebox cannot complete the port; do not substitute amd64 or DOS execution for
PC98 zedBSD evidence. Physical PC-9821V13 boot failure remains WS025-p032.

## User platform contract

Detect PC98 from `uname -a` in Noct. Place vmunix, rootfs.img, data.img
and swapfile in the FAT partition, not an EFI partition. Require partition
size <= 4 GiB and every payload file <= 2 GiB before copying; evaluate all
sizes in 64-bit integers. PC98 BIOS loader/config files accompany the payload.

## q186 focused repair

Keep the outer file_io cache pin and content leases. After a coherent read
returns ENOMEM without bytes, retire bounded clean pages only if that cache
pin is the sole operation/reference besides the cache itself; exclude mappings,
transitions, dirty/busy/held/pinned pages and registry waiters. Retry once only
if bytes were actually released. Host tests cover credit-demanding backend
reads, other-owner exclusion and unchanged identity; repeat q185-memory1 before
full installation. q185 is archived uncleared; this is a finite repair cycle.

The q186 runtime trace found a second active owner (flags CACHE_REFERENCE,
mappings 0, active 2, refs 3). Track unpublished prefetch operations separately:
they retain only private frames and publish under the object lock, so clean
resident-page retirement may coexist with them. Other readers/faults/mappings
still exclude targeted reclaim. The additional counter is protected by the
registry lock, incremented with prefetch admission and decremented before its
operation/reference release. Host coverage keeps a prefetch pending throughout
a read larger than the cache budget and verifies balanced abort ownership.

## Final user acceptance clarification (2026-09-10)

Clear this phase once the installed target disk alone boots and reaches a
logged-in shell. No additional near-normal or abnormal installer tests. The
current normal-path install and source-free boot are sufficient; unrelated
PC98 physical boot and general concurrency cases do not expand this gate.

Acceptance: `temp/q186-install1/result.json` reports PASS. The target-only
boot reaches root login, uname reports pc98/i386, the private overlay uses
rootfs.img/data.img and swap0 has 16383 slots. Source is not attached. This
satisfies the user's final completion criterion; no additional installer
near-normal/abnormal acceptance is required. See [results](results.md).
