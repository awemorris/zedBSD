# WS019-p019: read-only pristine image verification

Date: 2026-09-09
Status: completed (q148)
Parent: [WS019](../ws.md)
Timebox: 90 active minutes

Evidence: [results](results.md).

## Purpose and design

Close p004's canonical-comparison prerequisite without a second full image or
a Noct ioctl. Existing commands gain `mkfs -t ufs [--profile=journal-snapshot]
--verify-pristine FILE` and `mkswap --verify-pristine FILE`. UFS options may
appear in either order before the filename. This checks exact initial bytes,
not whether a used filesystem or swap remains valid.

Keep formatting reservations unchanged. Read-only validation performs lstat,
geometry validation, O_RDONLY|O_NOFOLLOW|O_NONBLOCK open, fstat identity, full
content verification, final fstat/lstat identity/size checks and close. It never
reserves, writes, truncates, fsyncs or activates. Success is an observation, not
exclusion against concurrent writers; p004 must revalidate publication.

UFS compares generator-derived deterministic extents, recording exact byte
intervals after comparison. A sorted bounded interval list coalesces adjacency
and rejects overlap/out-of-medium arithmetic. Capacity is four intervals per
cylinder group plus sixteen fixed regions, not proportional to image bytes.
Groups contain boot, superblock, bitmap and inode regions; journals/directories
and the optional tail fit the fixed allowance. Scan every remaining gap and
trailing byte for zero. Swap checks its canonical header and all remaining slot
bytes. Partial-block and tail coverage must not be inferred from allocation maps.

## Gates

1. Independently host-generated canonical UFS/swap images pass unchanged.
2. Corrupt metadata, journal, free area, reserved gap, final byte and feature
   tail fail; minimum, multi-CG and maximum supported geometry are covered.
3. Short/error reads, interval overlap/bounds, descriptor/name replacement,
   symlink/special files, close failure and first-error retention are checked.
4. Strict ordinary and ASan/UBSan host tests; existing formatter reservation
   regressions; explicit amd64/pcat/pc98 `make -j16`.
5. Disposable QEMU runs real commands on pristine and corrupted files, proving
   unchanged content and preserved formatting behavior.

If timebox closes before native acceptance, keep this phase uncleared with
exact evidence and resume conditions. p004 transaction and p005 installed boot
remain separate; this phase alone does not complete them.
