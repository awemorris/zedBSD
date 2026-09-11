# Input spans without temporary SYS mappings

2026-09-10, q294. Within p028 standing authorization; no HAL declarations.
Implementation not started. Both current experimental defaults remain off.

## Evidence and relevant owners

- `src/kern/uaccess.c:uaccess_view_prepare` acquires the input lease, allocates
  a contiguous virtual owner, maps16 physical pages read-only and pins its VA.
  Private pinned pages already expose stable kernel addresses. The input lease
  must remain: lifetime pins alone do not freeze concurrent input modifications.
- `src/kern/file.c:file_io_transfer_impl` owns growth clipping, append retries,
  resize/content gates, credentials/set-id clearing, write epoch, backend result,
  VM publication and position. Preserve this single implementation.
- Delayed writes use `vm_object_content_prepare_delayed`; a successfully prepared
  writeback ticket skips the immediate backend and publishes caller bytes into
  cached pages. Ineligible/failed optional preparation goes through the existing
  contiguous backend path. Extending writes do not automatically qualify.
- `src/kern/vm.c:vm_object_content_finish` intersects committed range with every
  captured page in object->pages. This linked list is NOT a sorted stream. The
  output sequential cursor cannot be reused without adding a correct arbitrary
  logical-offset operation or changing traversal. Do not assume page order.
- Backend write/pwrite/pwrite_internal require one contiguous input pointer.
  Repeating those operations per page would regress batching and transaction
  semantics. Cache writeback batching remains a separate owner.

## Borrowed input representation

Introduce a const-address source-span descriptor, max16 pages, with validated
snapshot metadata and total length. Retain owner-backed residency and input
content lease through file completion. No ownership transfer, allocation or
faulting in source copy. Arbitrary offset/range gather must validate range before
copy, support uneven spans, reject overflow, and not use the output write API
with const discarded. Immutable prepared metadata may carry cumulative offsets
for lookup; keep the representation bounded and its contract explicit.

VM content commit accepts either existing contiguous bytes or a validated source.
Share transaction checks, generation/dirty/ticket accounting and cleanup. For
unordered pages, gather from copy_start-write_start into the intersected cache
page. No later validation/allocation failure is permitted after accepting backend
bytes. Prepare the source before any visible file mutation. Preserve committed
short prefix and untouched suffix exactly.

## File transaction and fallback

Add source-aware entry sharing file_io_transfer_impl, rather than a parallel
writer. Pass a caller-owned contiguous fallback buffer/capacity along with the
validated source. Acquire scratch before file locks; pool borrow is nonblocking
but the current fixture explicitly forbids borrow while held. Do not lazily
allocate inside content commit or under inode/object locks.

If prepared delayed content accepts the write, publish directly from source
spans without copying through scratch. If immediate backend is required, gather
this transaction's clipped logical range into that buffer before mutation and
call the backend ONCE with the full bounded chunk. Publish the returned prefix
from the same immutable source/snapshot. Keep credentials, set-id, epochs,
append positioning, RLIMIT_FSIZE and resize abort ordering in the shared path.

A source-aware operation must never return ENOTSUP after modifying metadata or
accepting bytes and invite syscall replay. Decide fallback inside the same
transaction, or reject unsupported operations before entering it. Invalid
source metadata is rejected before any mutation. Buffer capacity is a syscall
chunk bound: with normal pool64KiB keep64KiB backend batches; with exhausted pool
retain existing512-byte fallback. Counter reporting must distinguish direct
cache publication from gather-to-staging bytes; do not call both zero-copy.

## uaccess/syscall transition

New input owner validates private pinned const addresses and takes existing
vmspace_user_input_lease_acquire without SYS mapping. Parent view_active excludes
conflicting leases; release follows file_io_complete and precedes parent unpin.
No writable user-alias shortcut. Scalar WRITE/PWRITE use this owner and prepare
fallback storage before file_io_begin. Ordinary unaligned/nonprivate/unsupported
paths remain. Remove old mapped input uaccess API only after both syscall paths
and all actual-owner fixtures migrate; common VM maps remain for DMA/scratch.

## Finite follow-up queues

1. Const source preparation/range gather plus actual VM content-commit adapter.
   Test unordered/reversed source and cache pages, nonzero file offset, partial
   page/short commit, abort, dirty-credit completion and invalid metadata.
2. Shared file transaction source entry and inside-transaction fallback. Test
   prepared delayed write, ineligible/resize/backend path, append and growth
   clipping, set-id failure, backend short/error, one64KiB backend call, failed
   preparation cleanup. Confirm no fallback replay after visible mutation.
3. Mapping-free input owner and WRITE/PWRITE wiring; preserve input COW and
   scratch-before-lock ordering. Remove superseded mapped-input API/fixtures.
   Check512-byte pool exhaustion and copy/direct counters meaningfully.
4. Native off/on input correctness and CPU comparison, source immutability,
   post-release user writes, fork/COW, append/RLIMIT, forced normal restoration.
   No default adoption without demonstrated benefit and required acceptance.

Output q291 lacks CPU benefit; input feasibility does not clear output or p028.
q287 intermittent login/exec ENOSPC remains recorded independently.
