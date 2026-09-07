# Exec cache snapshot ownership

Status: implemented and verified in q118; see results.md. The paragraphs below retain the staged design rationale.

The existing loader holds an exclusive file_content_lease across header, program
headers and PT_LOAD reads, then copies into anonymous pages. This remains the
fallback for mutable or stacked identities. It must not be replaced by an ordinary
lazy MAP_PRIVATE mapping: that would read later file generations after exec.

First build a bounded immutable-input lease without changing writable-file
semantics. Initially eligible inputs are direct regular files whose content inode
is the visible inode, on a read-only mount over a read-only disk, with no format
or backing claim on the description. Hold the file/mount and a content read gate;
retain a canonical cache owner when available. Read the cache through its coherent
API without holding inode I/O mutexes across misses; optional cache refusal uses
an internal serialized pread under the same content gate. This must not fall back
to an ordinary read path that waits on its own long content gate. Content leases
for other files keep the existing exclusive revoke/writeback/copy protocol.

A read-only flag by itself will not authorize lazy snapshot sharing: VM publication
also needs an owning lifetime that prevents content replacement and retains the
captured pages or another proven immutable backing contract. Subsequent work adds
an explicit private snapshot region with cache-page ownership, COW on writes or
permission changes, split/fork/unmap references, and reclaim accounting. Full pages
may share; leading/trailing partial pages, BSS and writable data must keep private
contents and exact zero/protection semantics. There is no second unbounded shadow
cache, no blanket ETXTBSY for writable binaries, and no shared writable cache text.

The first queue verifies input lease cache hits, overlapping immutable leases,
EOF/invalid access, unsupported/mutable fallback and cleanup on refusal. It does
not claim text sharing until the VM/ELF mapping path and physical-frame identity
tests are implemented. Full EXEC01–EXEC05/CACHE01–CACHE09, interpreter/fork/COW,
failed load and pressure/native acceptance remain p022 completion requirements.


## q118 private snapshot mapping design

Create an explicit reference-counted owner for one page-aligned full-file range.
It retains a second immutable input lease with its canonical cache-operation pin,
and an eagerly faulted/pinned array of its full cache pages. Charge the owner and
pointer-array metadata to the shared cache budget; frames keep their existing
canonical FILE_DATA accounting. Partial preparation unwinds only acquired pins.
Reclaim cannot evict those captured frames, and later faults never splice a newer
backend generation into an executing image. This is bounded by admission/budget,
not an unbounded shadow content cache.

Snapshot regions carry a separate owner pointer, not a MAP_SHARED object mapping
reference. The loader's original cache-operation pin is still active during partial
rollback, so a final vm_object_put could otherwise wait for the caller's own lease.
The snapshot's cache/page pins already own object lifetime independently of reverse
mappings. Region teardown removes its reverse mappings before dropping the owner;
final owner teardown releases page pins and then its input lease. Normal shared
mapping references and their existing final-put protocol remain separate.

Add a private-cache region distinction, separate from MAP_SHARED. Initial cache
PTEs are read-only/COW even if protection later permits writes. A write fault must
copy into normal private backing before success, including kernel uaccess/pins;
it cannot return a writable object-page pin after merely installing a read-only
PTE. Keep existing PTE shootdown, BUSY placeholder and region-hold contracts.
Snapshot-private regions acquire commit when made writable. fork shares the owner
and still clones already-private COW pages; it must not take the existing early
MAP_SHARED shortcut and discard those private modifications. Split/unmap/failure
paths balance owner and object references. Protection rollback retains COW.

For ELF, keep the original anonymous segment mapping and copy fallback. On eligible
nonwritable segments, share only the page-aligned interior fully covered by file
bytes; copy leading/trailing edges and keep BSS anonymous zero. The original
whole-segment rollback range still removes all resulting pieces. Prepare the owner
before removing any anonymous interior; optional admission refusal leaves the
copy path available. A mapping failure after removal aborts the unpublished exec
vmspace normally. Writable segments keep private snapshots and existing fork/COW.

Verify actual physical-frame identity across two loads, private writes via fault
and uaccess, mprotect, fork after COW, splitting/unmap and owner accounting before
claiming sharing. Then complete ELF header/interpreter/partial/BSS/failed-load and
pressure/native EXEC/CACHE acceptance. No blanket writable-file ETXTBSY change.
