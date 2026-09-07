# ws025-p010 execution record

Status: completed, Queue q097. Physical acceptance remains user-accepted;
no agent physical runtime is claimed.

Implemented the immediate begin/allocate/commit/abort adapter in both current
UFS drivers, preserving existing allocation, quota, publication and rollback
ordering. Indirect pointer lookup now reads a 512-byte stack window through the
common cache instead of allocating a full filesystem block for each lookup.
CG working copies retain bounded common-cache references and content generations;
explicit validity rejects initial group zero. Disk lifecycle admission protects
read/hit paths; pinned identities prevent eviction/replacement aliasing. A
wrapped buffer generation permanently disables reuse until eviction. CG writes,
switches, errors and teardown release views. Optional view resource failure
falls back to an ordinary read; physical I/O errors are propagated.

Evidence, under `../temp/p010/`:

- amd64 supported build: PASS (`build-amd64.log`).
- Real buffer implementation, ordinary and ASan/UBSan: PASS (`buffer-view-host/`).
  Includes pinned invalidation refusal, mutation, generation wrap, failed I/O,
  allocation failure fallback, existing run/concurrency/reentry oracles.
- UFS1/UFS2 data-run regressions, ordinary and ASan/UBSan: PASS (`ufs-run-host/`).
- Existing metadata audit: PASS (`metadata-audit.log`), 6010/6595 allocation and
  truncate checks per variant plus shared-block exclusion/error-unlock checks.
- New UFS ownership boundary fixture: PASS (`metadata-host-2/`), initial CG zero,
  hit, generation invalidation, CG switching, read/write errors and adapter
  regression. This fixture models the disk view boundary; it does not substitute
  for the separate real-buffer test. Its first run had an incorrect test baseline
  (fixture setup itself reads an inode block); corrected to record setup count.

- Disk lifecycle regression: PASS (`storage-foundation-final.log`), 20383 checks
  per ordinary/sanitized variant and amd64/i386 ABI. Includes view admission during
  reload and parent removal, distinct disk identity, and balanced cache users.
- pcat and pc98 supported builds: PASS (`build-pcat.log`, `build-pc98.log`).
- Native QEMU UEFI/xHCI, 16 GiB, controlled 4 KiB aligned backing: PASS
  (`native-16g-aligned/`), 202 samples with fsync and readback oracle. Both modes
  p50/p95/p99 = 10/20/20 ms. Mode 0 overwrites the same 64 KiB four times; mode 1
  writes four distinct ranges. This is a warm overwrite regression, not evidence
  for new allocation batching. CG hit/rollback evidence is the focused fixture.
- `git diff --check`: PASS. Final production/fixture hashes in
  `final-source.sha256.json`. No commit made.

Allocation commit remains immediate for this phase; p011 owns deferred batching.
The bounded CG working copy is mount-owned under the existing mount mutex; its
common-cache pins are charged to the cache cap. Global reclaim integration is p016.
