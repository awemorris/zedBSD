# p044 results

Completed q177, 2026-09-09.

Added backing_claim_finalize_file, sharing one implementation with the existing
range finalizer. The file must match the retained canonical claim. Optional
metadata is counted and canonicalized into the same allocated ownership array
as data; count overflow, fill changes, foreign ranges and provider errors refuse
publication. Existing self-overlap checks now cover data/metadata and duplicate
metadata. Logical I/O maps remain unchanged. Format, swap and loop callers all
use the file-aware finalizer. Preparing exclusion survives every failed attempt.

Production claim fixture: 512 layout oracle plus 12 metadata protection/failure
scenarios PASS under ASan/UBSan and leak checking, terminal exit 0:
/tmp/zedbsd-q177-claims-final.log. Cases cover normal metadata writes exclusion,
data/metadata collision, duplicate metadata, fewer/more entries, zero-to-nonzero
fill, provider errors, foreign range, wrong inode, allocation failure, overflow,
explicit retry, release and disk-reference balance. Earlier ordinary 10-case
version also passed: /tmp/zedbsd-q177-claims.log.
Format reservation: 655 checks in each normal/sanitizer run, terminal 0,
/tmp/zedbsd-q177-reservation.log. Swap source/manager: terminal 0,
/tmp/zedbsd-q177-swap.log. Three disk-image builds exit 0:
/tmp/zedbsd-q177-{amd64,pcat,pc98}.log.

Actual FAT provider and loop boot: temp/q177-view1, terminal 0,
/tmp/zedbsd-q177-view1.log. USB boot, two read-only UFS source view lifecycles,
write refusal, live-root isolation, cleanup and source screen cancellation
pass. Source rootfs.img, disposable target and production image hashes remain
unchanged (../temp/q177-view1/result.json). Actual source screenshot inspected.
No build/test process remains running.

UFS canonical identity and symmetric snapshot exclusion remain before UFS swap
admission. Enabling UFS claims must also preserve loop's optional FAT-only map
optimization; p007 records this discovered integration dependency. Native
installation and graphical frontend remain required and unfinished.
