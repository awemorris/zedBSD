# p043 results

Completed q176, 2026-09-09.

Added optional filesystem file_metadata_extents and a checked common dispatcher.
An absent optional provider contributes no file-exclusive metadata; unsupported
file backing still fails. The UFS provider reports allocated indirect blocks
in 512-byte sectors, bounded by EOF and at most three traversal levels, without
allocating a tree or modifying storage. A shared admission/locking helper now
serves data and metadata iteration. Logical data maps are unchanged.

Current complete production UFS host fixture: 157 checks, normal and ASan/UBSan
with leak checking PASS, terminal exit 0. /tmp/zedbsd-q176-ufs-final.log.
Tests include existing data mappings plus exact single/double/triple owned
metadata order and sector scaling, direct-only EOF, unused invalid pointers
past EOF, required invalid pointers, callback/read failures, snapshot refusal,
and lock release. Earlier 154-check run also passed before the additional
metadata I/O failure case: /tmp/zedbsd-q176-ufs.log.
Common dispatch fixture passes optional/unsupported providers and exact error
propagation (file-extent-dispatch-test.c; /tmp/zedbsd-q176-dispatch).

amd64, PCAT and PC98 disk-image builds all completed with exit 0:
/tmp/zedbsd-q176-amd64.log, /tmp/zedbsd-q176-pcat.log,
/tmp/zedbsd-q176-pc98.log. No test/build process remains running.

Next: combine data and owned metadata in common claim finalization and migrate
format/swap/loop consumers before admitting UFS canonical inode claims. Then
implement symmetric snapshot exclusion and actual UFS format/swap acceptance.
This phase exposes metadata; it does not yet add it to published claims or enable
UFS swap. Full native installation and the graphic frontend remain required.
