# ws025-p012 execution record

Status: completed, Queue q098. User-authorized autonomous execution.
Physical gate remains user-accepted, not an agent runtime result.

Implemented four mount-owned clean 512-byte slots alongside the existing single
mutable sector. Flush/mirror/rollback ordering is unchanged. Clean hits move into
the mutable window before use; external invalidation clears all retained slots.
The extra payload budget is fixed at 2048 bytes per mount, plus slot descriptors;
no per-operation heap allocation is introduced. P016 owns global budget/reclaim.

Successful operations retain a sequential cursor only after whole-chain
validation. Matching mount chain generation, first cluster and exact next offset
are mandatory. FAT link updates invalidate proofs before modification, including
failed writes and rollback. Generation saturation disables reuse permanently for
that mount. A read's first access now validates the complete chain before retaining
a proof; sequential reads amortize that initial walk. Seek revalidates. Existing
loop mappings still use their retained extent/claim path.

New read-only I/O statistics events count clean-sector hits/misses, FAT link
steps, generation invalidations and cursor hits (ABI version 6, appended IDs).

Evidence under `../temp/p012/` :

- `fat-vfs.log`: native FAT12/16/32 host fixture, ordinary and ASan/UBSan,
  441782 checks each, including mirrors and 1024-byte logical sectors: PASS.
- `fat-write-cursor.log`, `fat-write-cursor-sanitize.log`: 237441 checks each,
  fragmented/unaligned/sparse handoffs, every in-loop write failure and retry: PASS.
- `fat-write-cost.log`: 1660787 checks, distant corruption and read faults before
  mutation, traversal bounds: PASS.
- `cache-host/`: ordinary and ASan/UBSan, sequential count reduction, backward
  seek, another open's overwrite, shared truncate/free/growth and correct bytes: PASS.
- `private-host/`: ordinary and ASan/UBSan, bounded clean retention, mutable alias
  update, read/write failure, full invalidation, generation saturation: PASS.
- `fat-loop/`: production FAT+loop+buffer cache stories, ordinary and ASan/UBSan:
  PASS, including map/claim lifetime, extent failure and flush retry.
- First amd64 build and `git diff --check`: PASS.

- Final amd64/pcat/pc98 supported builds: PASS (`build-*-final.log` or
  `build-pcat.log`/`build-pc98.log`).
- QEMU UEFI/xHCI, 16 GiB, disposable 4 KiB aligned backing: PASS
  (`native-16g-aligned/`). 202 write/fsync/readback samples, both modes
  p50/p95/p99 = 10/20/20 ms; source image hash unchanged. Warm loop I/O retains
  its existing extent route; direct FAT reuse is established by host counters.
- Final hashes: `final-source.sha256.json`; final diff whitespace check passed.

P012 completes clean retention and sequential validation reuse. P013 owns dirty
metadata batching; no additional dirty sector survives an operation in this phase.
