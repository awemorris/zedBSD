# ws018-p018 results

Date: 2026-09-06. Result: completed for the selected I/O path.

Regular read/write/pread/pwrite/readv/writev use a bounded 4096-byte heap buffer,
allocated before the file/VM transaction. Allocation pressure falls back to
the old 512-byte stack buffer. Non-regular descriptors retain their prior
buffer size and stream completion; PIPE_BUF vector coalescing remains intact.

Loop extent collection retains logical offsets and verifies exact coverage.
The finalized claim and map have separate lifetimes; every attach error and
detach clears/frees the borrowed map. The registry-full allocation leak found
during review was fixed and its failure path runs under ASan.
Loop continues through ordinary file I/O and VM content transactions. FAT uses
the immutable map beneath those transactions, inheriting the owned inode claim
through disk_write_filesystem into parent-cache writeback. This avoids bypassing
existing VM coherence or incorrectly authorizing a wider cache-line raw write.
The FAT lock drains dirty slot contents before invalidation and mapped I/O;
unbind itself does not discard unrelated dirty state.

Existing full-block UFS overwrite skips the redundant data pre-read.
Partial writes still read surrounding bytes. Allocation zeroing and complete FAT
write-chain validation remain in place.

S19–S33 and S37–S43 pass their applicable production fixtures/native checks;
the full FAT12/16/32 regression passes 441782 checks in ordinary and sanitized
runs. Existing UFS1/UFS2 metadata failure-ordering and concurrency gates pass.
Native mmap/ordinary I/O, vector/pipe behavior, copy-up and Wi-Fi store operations
pass on the actual FAT/loop/UFS/overlay stack.

[Acceptance and evidence](../phase019/results.md).
General FAT cursor/batching, copy-up/dir iteration and full metadata rewrites
remain explicit [follow-ups](../../old/fs-report-followups.md).

