# ws024-p001 results

Status: completed, Queue q100, 2026-09-07.

Frozen [format/migration contract](../format-contract.md) and
[24-item acceptance matrix](../tests/acceptance.md). Inventoried the current
production/build and maintained fixture consumers under `../temp/p001/`.

Decisions: preserve the existing 64-bit UFS2 wire codec under one public UFS owner;
reject UFS1 without mutation; preserve optional journal/snapshot tails and add an
explicit producer profile; preserve ordinary write-through defaults until WS025
p021; migrate all normal Noct/C/Python/platform producers and root/blkid consumers.

The source audit exposed an unchecked decoded inode-size cast and a loop
narrowing-before-limit check. P002 includes checked conversions. The existing
ILP32 off_t ABI and loop 2 GiB limits are explicit and are not silently represented
as full 64-bit user API support. Disk addresses/codecs remain shared 64-bit logic.

P002/p003 must switch driver and generated images together before runtime. Preserve
opaque generated-image backups; retained user images require export/import with
the appropriate old/new systems, never an in-place reformat. No production code,
image conversion, physical test or implementation acceptance was performed by q100.
