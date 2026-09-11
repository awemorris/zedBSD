# p040 results

Completed q173.

Added filesystem_type.file_extents and common checked file_backing_extents
inline dispatch. FAT registers its existing provider; format, swap and loop
consumers no longer directly call drv_fat_file_extents. FAT-only admission and
canonical identity remain deliberately unchanged pending the UFS provider.
No new public ABI and no claim ownership change.

Validation completed:
- Format reservation: 605 checks in ordinary and ASan/UBSan runs, terminal 0;
  /tmp/zedbsd-q173-reservation.log.
- Swap source/manager full fixture: PASS terminal 0;
  /tmp/zedbsd-q173-swap3.log.
- Independent file-extent-dispatch-test.c: missing file/inode/provider, invalid
  type, null callback, successful callback and exact error propagation pass.
- amd64 / PCAT / PC98 disk-image builds terminal 0;
  /tmp/zedbsd-q173-{amd64,pcat,pc98}.log.

Fixture maintenance required by current refactored tree: removed nonexistent
split swap source files from the runner; renamed its local io_context to avoid
the production type; restored physical disk pin accounting (claim + raw source,
both released) in the old assertions. Existing tests were retained, not skipped.
The first two swap runs terminated at compile error and stale assertion,
respectively; see /tmp/zedbsd-q173-swap.log and -swap2.log. Neither was a product
extent-dispatch defect. Loop fault injection now intercepts the common entry
and delegates normally through it, preserving its deliberate malformed extent.
The focused C89 compile attempt hit pre-existing plain inline definitions in
shared kernel headers; compiling the new test with the established C11 host
flags passes. New helper/test declarations remain ANSI-style.


Final QEMU: temp/q173-view1, /tmp/zedbsd-q173-view1.log, terminal exit 0.
USB boot exercised the actual FAT extent provider through loop's new common
call. Two read-only UFS source mounts/unmounts passed; writes were refused,
overlay-only changes were absent, original root remained usable. Actual source
screen was captured and inspected, and Escape cancelled. Production image,
source rootfs.img and disposable target hashes remained unchanged, as recorded
in ../temp/q173-view1/result.json. No running test/build process remains.

UFS swap remains unimplemented: provider, canonical identity, snapshot exclusion,
format/swap admission and mutation tests are next. This phase does not claim
native swap or installer completion.
