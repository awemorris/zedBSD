# Direct output without a temporary contiguous mapping

2026-09-10, q282 source-backed implementation design. Within standing p028
scope; no new HAL interface or change to the pending trap proposal.

## Problem and evidence

q281 output is correct but CPU 1.23 s versus 0.83 s copyout for 128 MiB.
Current uaccess_output_view_acquire builds a contiguous kernel view because
file_io_transfer_prefix and vm_object_read_coherent_prefix accept one pointer.
Pinned private pages already carry stable kernel addresses in memory.vaddr.
q279 counted eight SYS map runs plus one SYS unmap per 64 KiB output; later
q280 restores user aliases but still creates/retires that temporary kernel view.
This motivates removing the pointer-shape requirement, not removing the lease.

Source owners: src/kern/uaccess.c view prepare/release; src/kern/file.c
file_io_transfer_impl; src/kern/vm.c object_read_coherent and
object_cache_read_missing. The cache has two confirmed destination-copy sites:
resident-page memcpy and scratch confirmed-prefix memcpy. The raw backend
fallback into the caller's buffer must remain forbidden for strict output.

## Design

Represent a bounded borrowed destination as kernel-address spans with lengths.
The destination owns no pages, mappings, pins or allocation; it is valid only
while its caller retains the page pins and exclusive content lease. Use a
single-span adapter for existing contiguous callers and a bounded stack span
array for direct output (current 16-page/64 KiB limit). Do not enlarge transfer
limits to manufacture a favorable benchmark.

Validate all spans, count, pointers, summed capacity and overflow before any
copy. Copy a confirmed source prefix into spans using logical offsets; no
allocation, locking, faulting or callback in the copy routine. Its callers must
supply resident stable kernel addresses. Copy cannot fail after validation;
malformed descriptors are rejected before output publication. Never interpret
this as a user iovec API or DMA address vector.

Keep one file transaction and one coherent VM read over the complete requested
length. Adapt the VM read's destination-copy sites, including bounded miss fill,
to the destination representation. Cache miss discovery, one bounded backend
request, content/media generation checks, scratch ownership, prefix count and
EOF rules stay unchanged. Do not call file_io_transfer_prefix once per span:
that would regress backend batch size and duplicate transaction accounting.

Add an internal strict vector-output entry at file/VM layers which delegates
to the common algorithms. Validate destination capacity before any transfer;
retain existing pinned read_object/content lease prerequisites. Zero confirmed
bytes with ENOTSUP/ENOMEM can use the existing staging fallback; a positive
prefix must never be replayed. Offset/atime/statistics/completion remain under
the existing file_io owner. Keep raw backend access restricted to its bounded
scratch buffer for cache misses; hostile suffix writes never touch user spans.

uaccess acquires the same writable pins and output content lease but exposes
stable page addresses directly to the destination. It marks captured private
owners dirty as today and holds ownership through file_io_complete. No temporary
SYS alias is created. Release the lease (including q280 best-effort user alias
restoration) only after the last destination access. Pins then release normally.
Input mapping remains separate until an equivalent write-source design exists.

## Finite execution steps

1. Add destination representation/copy helpers with single-span adapter and
   boundary tests: zero length, uneven spans, first offset, multiple boundaries,
   overflow and invalid descriptors before write. Keep helper independent of VM
   and user process types. Integrate only when its consumers are ready.
2. Adapt actual VM coherent prefix read and miss copy sites; test warm pages,
   full 64 KiB miss backend call count, hostile short/error fill, prefix spanning
   pages, scratch refusal, cache/media generation changes. Preserve contiguous
   caller behavior by running its existing actual-VM gates.
3. Wire file transaction and uaccess/syscall output. Verify same ownership,
   completion error, fallback and offsets with actual-function fixtures. Expose
   no unused successful stubs. Remove superseded output temporary-map path once
   the replacement passes; common mapping remains for input/DMA/scratch.
4. Run controlled output off/on native EOF/COW/data/CPU oracle, record every
   sample and verify zero temporary output SYS maps using bounded instrumentation.
   Restore ordinary artifact. Enable only with actual benefit and remaining
   acceptance criteria met; no predetermined performance claim.

Each step becomes a finite queue after inspecting its exact current consumers.
Do not replace data-plane code with a parallel cache implementation. This is a
change to copy destination representation, not filesystem format, block I/O
scheduling, DMA or HAL policy. Full objective/p028 remain unfinished.
