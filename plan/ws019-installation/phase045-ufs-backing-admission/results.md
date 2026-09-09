# q178 verification

Status: completed q178, 2026-09-10

Final native acceptance: `temp/q178-swap7`, `/tmp/zedbsd-q178-swap7.log`, exit 0.
Actual public mkswap/swapon/swapoff, alias duplicate refusal, truncate/unlink/
rename protection, snapshot mutual exclusion and both release directions pass.
Page-aligned sparse backing fails extent admission with EIO. Production image,
source rootfs and outside-partition bytes remain unchanged. This verifies
admission/lifecycle, not paging stress or source-free native boot (p007).

Final builds passed: amd64 `/tmp/zedbsd-q178-amd64-truncate.log`, pcat
`/tmp/zedbsd-q178-pcat-write.log`, pc98 `/tmp/zedbsd-q178-pc98-write.log`.
UFS provider normal/sanitized: 161 checks each, `/tmp/zedbsd-q178-ufs-write2.log`.
Common claim/canonical identity/snapshot-order tests passed
`/tmp/zedbsd-q178-claims.log`; format reservation 655 checks each
`/tmp/zedbsd-q178-reservation.log`; swap manager `/tmp/zedbsd-q178-swap.log`.
The truncate early-result correction is exercised by the final native command
regression requiring normal EBUSY error reporting rather than SIGXFSZ.

Implementation: filesystem identity capability; UFS data/metadata claim
admission; snapshot CREATE volume guard; generic format/swap admission;
non-FAT loop optional cache-map handling; trusted UFS journal/home writes;
initialized truncate outcome on early claim refusal. Keep physical UFS volume
requirement: file-backed virtual UFS disks are not accepted as canonical swap.

## Investigation evidence

First native QEMU attempt (`temp/q178-swap1`, `/tmp/zedbsd-q178-swap1.log`)
failed at the initial mkswap. The fixture created a hardlink before formatting;
`format_file_run` intentionally rejects st_nlink != 1 with EBUSY before kernel
admission. Native formatting, mounting and full file allocation succeeded.
Production image, source rootfs and bytes outside the test partition were
unchanged. The runner terminated with AssertionError.

Correct fixture ordering: format first, create alias before activation, remove
alias after swapoff before snapshot tests so their formatter refusal tests the
snapshot rather than the link-count condition. Product admission is unchanged.

Second attempt (`temp/q178-swap2`, `/tmp/zedbsd-q178-swap2.log`) also failed
initial mkswap with EBUSY after removing the initial hardlink. All protected
hashes still matched. dd reported 32 short input records and only 16384 bytes;
short character-device reads are permitted and count counts records. The next
fixture uses conv=sync to allocate the intended 2 MiB, and explicit file sync
before reserving backing extents. Dirty VM caches intentionally refuse claims;
the installer must durably allocate its swapfile before formatting/activation.

Third attempt (`temp/q178-swap3`) proved a 2097152-byte, single-link file and
successful explicit sync, but mkswap still returned EBUSY. Dirty cache was not
established as the cause. Next diagnostic uses the private probe to distinguish
reservation, owner write and synchronization, without changing kernel policy.

Fourth attempt (`temp/q178-swap4`) isolated the real failure: reservation
succeeds, owner pwrite returns EBUSY and subsequent fsync reports that failure.
The UFS journal/home writers use raw disk_write_context, unlike FAT's trusted
filesystem write path. Raw writes intentionally cannot borrow an inode claim.
Route both mounted UFS writers through disk_write_filesystem_context, which
inherits the retained inode mutation owner on the same volume while excluding
foreign claimed extents. No raw disk admission policy is relaxed.

Fifth attempt (`temp/q178-swap5`) passes owner write/fsync, public mkswap/swapon
and alias duplicate refusal. Truncate is refused, but exits with SIGXFSZ (156)
instead of reporting EBUSY: inode_truncate_transaction returned at the backing
guard before initializing its result, and the syscall read uninitialized
limit_exceeded. Initialize the public result before guard admission, preserving
actual_size and clearing the limit flag on early refusal. The native regression
requires normal error exit 1 plus EBUSY text, not merely a nonzero exit.

UFS provider tests after the write change pass 161 checks in both modes; the
first host link required updating its memory-disk adapter for the new filesystem
write entry point. amd64 write-path build passed.

Sixth attempt (`temp/q178-swap6`) passes the lifecycle, including normal truncate
EBUSY, alias unlink/rename refusal, snapshot mutual exclusion and recovery.
It exits 0 with all protected hashes unchanged. Final fixture correction adds
conv=sync to the sparse-file tail as well: its length must be page-aligned so
the test reaches extent admission rather than just swap-header geometry.
The owner-write probe now asserts each successful syscall result explicitly.
