# ws025-p023 results

Status: completed in q119, 2026-09-08 JST. All owner/integration/final checks below are terminal.

Implemented bounded amd64 owned vmap: 128 descriptors, at most 128 KiB per run,
separate reserve/populate states and borrower pins. Reserve consumes no data
frames/tables. Population owns individual RAM pages and supervisor RW/NX leaves;
rollback and release perform system-wide shootdown before releasing tables/pages.
Existing permanent upper-half roots remain shared by every user space.

Added io_scratch: tries contiguous memory, then optional amd64 vmap for large
pool/worker storage. Keeps an explicit physical descriptor or a vmap owner/pin;
never fabricates a contiguous physical descriptor for a virtual buffer. Pool small
slots and metadata retain contiguous allocation. Readahead/writeback/cache-worker
payload plus control pages use the same owner. Existing drain gates protect every
borrower; release failure retains ownership. Consumer cache accounting remains
single-charged. Readahead trim uses the retained worker size after successful
scratch teardown clears the local descriptor. Other HALs use the weak optional
capability boundary and their established contiguous fallback.

Evidence so far:

- p023-vmap-owner-3.log: real vmap and scratch code with controlled HAL table/frame
  boundaries, ordinary/ASan+UBSan, 13548 checks each. Exhaust reserve slots with no
  allocation, invalid/unsupported input, first 16 frame and leaf failures, high
  discontiguous PFNs, supervisor/NX flags, guards, busy release/pin recovery,
  payload+control allocation, and zero remaining frames after teardown.
- p023-scratch-pool-1: real pool + exec-copy, both variants pass.
- p023-scratch-cache-device-2: real shared accounting + DMA, pool, worker and pool
  allocation faults, both variants pass. Initial worker fixture lacked the newly
  reached HAL page-size boundary; adding the 4096-byte host HAL contract fixed
  its link, with no production behavior relaxation.
- p023-scratch-readahead-2: actual asynchronous worker, 252 checks per variant.
- p023-scratch-writeback-2: actual policy/worker, ordinary 1492 and sanitizer1499
  checks; disabled/error/drain/independent progress/credits retained.
- p023-vmap-native-4/guest.log: actual amd64 8 GiB BIOS USB boot with four CPUs.
  Both real subordinate table allocation fault points roll back exactly; three
  16-page high (>4 GiB), fragmented mappings work through existing and newly
  created user spaces and every AP. Every round restores physical free bytes.
  Forced contiguous refusal routes all 16 large pool slots and 69632-byte worker
  through vmap; login and memory reporting pass. Link-only wrappers are test-only.
- Earlier native-1/2 independently passed owner/scratch cases. Native-3 passed
  table rollback but its exact PFN injector then collided with ordinary space
  allocations advancing the physical allocator cursor. It was stopped on fatal;
  native-4 leaves a 64 KiB gap between forced pages, retains forced high/nonadjacent
  PFN assertions, and passes. No production allocation policy changed for this.
- p023-vmap-source.json records the current production identity.

Next: combined USB journal writeback + readahead with forced vmap scratch,
normal supported builds and native regression; record matrix coverage and scope.
Physical gate remains user-accepted, not agent-measured.


## Final acceptance

- `temp/p023-vmap-writeback/` + log/results.json: real UEFI 8 GiB, four CPU,
  xHCI USB-root + USB journal-snapshot filesystem. Both table-failure rollback
  points and high fragmented mapping/AP probes pass again. Forced contiguous
  refusal yields vmap pool, cache-worker, writeback and readahead scratch.
  Writeback batch/age/fsync/unmount/remount verification passes; sequential and
  random readahead pass. Final readahead jobs/running/demand/errors are zero;
  requested/started=1007616, published/confirmed_useful=942080, discarded=65536.
- `temp/p023-final-{pcat,pc98}-2.log`, `p023-final-amd64-3.log`: supported make -j16
  builds pass. First pcat link found io-scratch missing from its explicit object
  list; both i386 lists now include it (the other HALs use source lists).
- `temp/p023-final-native-normal.log` and WS018
  `temp/ws025-p023-final-normal/`: ordinary, wrapper-free amd64 native USB grouped
  storage and reboot persistence pass. The earlier final-native-2 run exposed
  stale link-only probe code: removing an auxiliary makefile alone did not force
  relink. Rebuilt with `-W src/hal/amd64/space.c`, verified probe wrapper symbols
  absent with nm, and reran. This was a test artifact restoration failure, not
  a production high-memory allocation failure. Future wrapper removal must force
  relink and verify absence rather than relying on unchanged source timestamps.
- `temp/p023-final-source.json`: final source/config identity, including both
  i386 object lists. Earlier production hashes remain unchanged; git diff --check
  passes. No live process, no commit, no aggregate make check. Ordinary amd64 restored.

MEM08/09 and SG01/02 use actual high vector lookup, guards, shared preexisting/new
roots, AP access, failed table population, reuse/shootdown and free accounting.
MEM10/11 reuse proven high RAM/user/COW support from p005 and force >4 GiB vmap
frames here. MEM12/13 and SG03 preserve constrained coherent DMA: the real DMA
accounting fixture passes and generic pointer DMA still accepts registered
coherent allocations only; a vmap VA is not treated as a contiguous physical span.
CACHE06–CACHE09 retain common owner/pressure/drain/error acceptance from p016–p018,
with real worker failure/trim/credit tests and combined native vmap worker operation.

The bounded capability is amd64 CPU scratch; SG DMA is still p024, and unsupported
HALs keep contiguous fallback. Static descriptors and virtual slots do not claim
free RAM; population owns frames and table allocations reported by the allocator.
Physical hardware acceptance remains user-accepted, not agent-measured.
