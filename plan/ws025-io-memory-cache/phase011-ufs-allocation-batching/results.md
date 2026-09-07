# WS025 p011 execution results

2026-09-07, q103 in progress; not yet complete.

The first production implementation batches full new blocks within one CG and
one direct/indirect leaf, at most 64 KiB and 16 blocks. A private dinode/leaf
image stays unpublished until allocation and data flush. Shared dinode serialization
has a lock-held helper. Abort confirms old pointer/size persistence before CG reuse;
failed rollback stops writes and retains quota charges. Missing indirect ancestors
and partial blocks retain the zero-initializing immediate path, then later eligible
leaf entries use batching. No second UFS driver or cross-operation deferral exists.

- `temp/p011-baseline-host.log`: before batching, 32 KiB fresh content required
  25 reads, 41 writes, no internal flush and 8 content write requests.
- `temp/p011-batch-splits`: ordinary and ASan/UBSan each pass 7,935 checks.
  Same direct workload: 3 reads, 4 writes, 2 ordered flushes, 1 content request.
  Existing single/double/triple leaves: 5/7/9 reads respectively, 5 writes,
  2 ordered flushes, 1 content request. No native speedup claim yet.
- Coverage includes before/after-error write mutation, every flush boundary,
  failed rollback, no reusable block referenced by volatile/durable metadata,
  preserved initialized bytes, unchanged public size after failure, quota prefix,
  CG split, ENOSPC prefix, zero-filled partial block and new indirect root.
- WS024 `temp/ws025-p011-first`: existing metadata 6,595/view 6,635/run and expanded
  consistency 114 checks retain ordinary/sanitizer contracts after implementation.

Pending: build results, QEMU mounted-feature and storage persistence, native baseline
counter comparison, remaining allocation-memory/width boundary checks, final review.

## q103 completion

- `temp/p011-batch-final`: 7,973 checks per ordinary/sanitizer variant, including
  bounded working-memory refusal with safe immediate fallback, negative offsets,
  active off_t overflow and unsigned range overflow before storage access.
- amd64 (`p011-amd64-build.log`, `p011-amd64-final-build.log`), PC/AT
  (`p011-pcat-build.log`) and PC-98 (`p011-pc98-build.log`) make -j16 gates pass.
- WS024 `temp/ws025-p011-features/results.json`: mounted xattrs/quota/snapshot,
  remount and reboot pass with the actual new allocator.
- WS018 `temp/ws025-p011-final/results.json`: storage **50/50 PASS**, including
  fresh xHCI two-boot persistence; WiFi 30 ordinary/sanitizer passes.
- `temp/p011-baseline` and `p011-baseline-repeat`: both 202-sample aligned 16 GiB
  xHCI baseline runs pass readback, sample oracles and source-image hashes.
  Fresh 256 KiB creation reduces UFS writes 176→40, content writes 32→6,
  transferred UFS write bytes 1,441,792→540,672. Allocation scopes 33→7 while
  allocated blocks remain 33 (32 data, one indirect). Ordered publication raises
  loop flushes 5→15 and total driver flushes 9→29; this is deliberate durability
  ordering, not cross-operation write-back.
- Existing overwrite request counts remain unchanged. First run mode 0 is
  10/20/20 ms; mode 1 is 30/40/40 ms. One justified repeat returns both modes to
  10/20/20 ms. Retain both runs as timing variability; do not claim a latency
  speedup from batching or discard the slower result.
- Compiler dependencies include `ufs-allocation.inc`; `git diff --check` passes.

The finite acceptance scope is complete. A crash before pointer publication can
leave an allocated but unreachable extent; allocation recovery/checking remains
necessary. The immediate path still creates missing indirect ancestors with zeroed
blocks and handles partial blocks; subsequent full blocks batch within existing
leaves at all three depths. This is bounded synchronous operation batching, with
cross-operation cache/write-back and global memory admission owned by later phases.
