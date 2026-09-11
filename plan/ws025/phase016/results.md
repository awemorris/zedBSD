# WS025 p016 execution results

2026-09-07; q106 completed. Intermediate checkpoints below are retained; the final verdict is authoritative.

The first accounting adapter is `src/kern/cache-memory.c` with kernel/UAPI headers.
It separates pending and resident ownership by category, preserves mandatory
ownership above the soft target, enforces a physical-free floor for optional
admission, and gates shrink against a pending target before committing policy.
Its clean-reclaim adapter calls only explicit clean callbacks outside the
accounting lock. It is not yet wired into normal kernel build/boot or allocations.

`temp/p016-accounting-2.log` passes 80,052 checks per ordinary/ASan/UBSan variant:
8 MiB through 64 TiB policy arithmetic, reservation/cancellation, exact category
sums during four concurrent allocators, overflow, free-floor refusal, mandatory
DMA-like ownership above target, EBUSY rollback, and a successful pending-gated
clean shrink. Reclaim callbacks in this adapter test are controlled host models;
this is not evidence of integrated VM/buffer reclaim yet. The initial compile
failure (`p016-accounting-first.log`) was a fixture attempting to define an
already-macro HAL compiler barrier; that redundant fixture definition was removed.

Remaining: actual file-page metadata/index and growth, VM clean reclaim, block
reservation accounting, mandatory pool/DMA accounting and worker reserve,
boot/sysctl/build integration, real subsystem host faults and native pressure gates.

## q106 integration checkpoint

The preceding adapter-only description is superseded by this checkpoint. The
phase remains in progress; worker reserve and final native/build gates remain.

- Block data/header slabs now reserve/commit/cancel/release actual physical bytes
  against the shared manager. Automatic RAM/16 sizing no longer stops at 16 MiB;
  explicit component limits remain. Admission pressure invokes clean-only reclaim.
  `temp/p016-buffer-adapter-2/` passes the maintained transfer/fault/concurrency
  suite. `temp/p016-buffer-integrated/` passes real block-manager accounting,
  no-I/O dirty refusal, pin refusal and successful clean shrink in both variants.
- I/O pool and coherent DMA backing are mandatory categories. Borrowing does not
  charge storage again. Failed DMA physical retirement preserves the descriptor,
  allocation-list membership and accounting so callers can retry; destruction
  refuses outstanding ownership. `temp/p016-device-integrated-2/` passes real
  pool/DMA accounting with low/high extent constraints, page rounding, mandatory
  over-target use, failed retirement/retry, and selected pool allocation failures,
  ordinary and ASan/UBSan. The first fixture compile lacked errno.h; fixed.
- File pages use an intrusive AVL index; rotations preserve descriptor identity.
  Fault/batch publication, resize orphan/abort and invalidation maintain the index.
  `temp/p016-page-index.log` passes 7,127,222 node checks per variant with ordered,
  reverse and shuffled insertion, removal, and repeated reinsertion. Existing
  cache race/resize/lifetime checks also pass after index integration.
- Descriptors now occupy physical page slabs, with a constant-time available-slab
  list and retirement after the last descriptor. Charge all slab capacity and
  data frames, including private preparations, mapped/anonymous mandatory frames,
  and orphaned/pinned storage. Optional read retention no longer has a 16-page
  ceiling; the 16-page transfer vector and 32-cache-object limit remain.
- Clean reclaim detaches only idle unmapped/unpinned pages without filesystem I/O
  or object close. VM pressure tries it before private-page swap/writeback. Empty
  object close remains at existing explicit lifecycle checkpoints.
- `temp/p016-shared-file-3.log` passes 415,750 / 408,956 checks with the real
  manager, slab/data physical-allocation fault injection, concurrent readers,
  writers, mappings and eviction. A 256 KiB working set remains warm; mapped
  ownership refuses a zero target, subsequent clean shrink retires data and every
  empty slab, and zero-target reads fall back successfully. Independent HAL live
  bytes agree with shared resident accounting and return to zero. First two
  integration attempts fixed fixture API names and missing host IRQ services.
- Normal boot initializes policy before block-cache construction. Platforms link
  the accounting owner. sysctl exposes vfs.cache_memory.stats and the root-only
  transactional vfs.cache_memory.target_bytes control. Early uninitialized target
  calls now reject before touching the control mutex.
- `temp/p016-amd64-first.log`: amd64 supported build passes. This is an interim
  build; native execution and worker reserve are still pending at this checkpoint.

No delayed writeback has been enabled. No physical runtime was performed by the
agent; the user's explicit physical-gate acceptance remains in effect.

Further checkpoint: `p016-device-worker/` adds the production dedicated 64 KiB
payload + 4 KiB control reserve. Missing allocation is explicit and retryable;
ordinary pool users cannot consume this storage. Four host borrowers verify
exclusive nonblocking ownership; actual allocator-rounded bytes remain charged
while idle or borrowed. No writeback worker is started. The ordinary and sanitizer
variants pass, including the unchanged DMA/pool cases.

`p016-shared-file-final.log` passes 415,841 / 409,025 checks after bounded optional
admission reclaim was added. It may retire other idle clean ownership once before
retrying credits, without reclaiming the currently active read. Runtime checkpoints
`p016-cache-native-first`, `p016-cache-budget-512`, `p016-cache-budget-256` pass USB
root cache behavior; the latter two exercise real 256 KiB growth, mapped dirty
refusal, subsequent shrink, and reserved worker bytes. Final source validation
with sysctl CLI coverage and the remaining RAM/build/regression gates is pending.

The sysctl CLI now formats category accounting. Its setter previously printed
"Success" but exited 1 even when the kernel accepted a value; the new control
exercised that existing defect, so the successful setter now returns 0.

The first final 16 GiB run passed the cache/budget guest and sysctl category read,
but the host keyboard driver lacked '=' when typing the CLI setter; it raised
KeyError before Enter, so that run is not counted as the complete final gate.
The maintained QEMU keyboard map now includes the equals key; a fresh image run
is used for the complete guest and CLI check.

Final native cache/control cells `p016-cache-final-16384-2` and
`p016-cache-final-256` pass. Managed bytes are respectively 17,173,229,568 and
261,795,840; each reserves 69,632 worker bytes and demonstrates a clean reduction
from 1,712,128 to 1,581,056 resident bytes with content preserved. Warm UFS content
and USB read counts remain zero. CLI category display and successful target
assignment exit status are tested in addition to direct sysctl API checks.

`p016-final-gates/results.json` records successful accounting (80,053 checks per
variant), integrated block tests, pcat, pc98 and amd64 restore builds. The first
full storage gate stopped at a sanitizer link: the old storage-fat host fixture
had no hal_fatal service for newly checked buffer retirement. Its diagnostic stub
was added; production was not weakened. Full rerun is recorded separately under
`WS018/temp/ws025-p016-final-2` / `temp/p016-storage-final-2.log`.


## Final verdict

p016 is complete. `temp/p016-memory-matrix/results.json` passes all 12 BIOS/UEFI
cells at 256 MiB, 1/2/4/8/16 GiB. Each reports a valid boot range map, managed RAM
over 75% of configured RAM, and equal managed/mapped coverage; no 1 GiB reporting
cap returns. p005's forced high-PFN/COW/boot-owner retirement evidence remains
applicable because those HAL ownership paths are unchanged. The new file-cache
native cells independently cover low/high RAM, USB root and real content checks.

`WS018/temp/ws025-p016-final-2/results.json`: **50/50 PASS, none unrun**, with both
Wi-Fi30 host variants and the native USB two-boot regression. Three supported x86
builds passed. Final accounting, indexed metadata mutation, file/cache concurrency,
physical allocation failures, dirty/pin refusal, pool/DMA retirement and exclusive
worker reserve evidence is listed above. `temp/p016-final-source.json` records
production/fixture hashes; `git diff --check` passes.

CACHE05 keeps the existing lifecycle/claim gates; actual media replacement remains
p025. CACHE06 and p016 portions of CACHE07/CACHE09 now have real clean reclaim,
shared credits, rollback and native pressure-control evidence. CACHE08's delayed
per-device dirty quota/fairness and dirty-worker scheduling remain explicitly p018:
this phase provides independent preallocated worker/device resources but does not
claim a running delayed scheduler. MEM10–MEM14 preserve earlier allocator evidence;
MEM15 adds the complete current boot matrix; MEM16 combines current USB/SMP gates
with the user's physical acceptance, not an agent physical run. The 32-cache-object
bound remains intentional; clean page count is demand/budget controlled.

Next: p017 error observer and dirty/drain contracts while maintaining write-through.
No commit was created. No aggregate make check or .internal access was used.
