# Bounded kernel vmap owner

q119 design, 2026-09-08 JST. No heap replacement or DMA capability claim.

The existing amd64 user mapper rejects upper-half addresses. Reuse its table
allocation/detachment primitives through a separate, permanent kernel-table owner
whose root is system_pml4. Serialize this owner independently of user spaces.
p003 already allocated every shared upper-half PDPT before creating user spaces;
only subordinate vmap tables may be reclaimed. PML4 entries remain permanent.
Map rollback/unmap must complete HAL_SPACE_SYS shootdown on every ready CPU before
freeing detached tables or backing frames. Kernel leaves are supervisor RW/NX.

Reserve from the existing vmap window with a fixed descriptor limit and a bounded
128 KiB maximum allocation (64 KiB payload plus worker control pages fit). Each slot has a guard gap.
Reservation alone allocates no data frames/tables. Population allocates individual
owned pages, validates RAM/address width, and installs the complete vector; any
failure unwinds leaves/tables/pages and leaves the reservation reusable.
Descriptor states exclude concurrent populate/release; explicit borrower pins
reject release rather than freeing a referenced VA. The owner frees its pages
only after PTE retirement. This is an owning scratch allocator, not permission
to map somebody else's unpinned cache frames.

Expose a checked page lookup for a caller-held kernel address lifetime. Image and
RAM-direct windows keep their existing checked conversions; vmap lookup walks its
supervisor tables. Holes, MMIO and invalid windows are rejected. Returning one
page's PA never asserts physical continuity of the whole buffer.

amd64 implements the optional HAL capability. Other HALs retain existing bounded
contiguous buffers. Consumers check the optional capability before calls. Initially
pool large slots may use vmap on contiguous allocation failure; small reclaim and
coherent DMA reserves retain their physical allocation contract. Scratch accounting
charges frames exactly once to the existing consumer class, keeping VA reservations
separate from resident bytes. Worker owners use the same abstraction only where
teardown and borrowers can retain its lifetime.

Verify partial page/table allocation failure, admission/overflow, pinned release,
reuse, fragmented and >4 GiB frames, shared roots in preexisting/new processes,
and system shootdown before publishing support. Use actual amd64 mappings in a
link-only QEMU probe in addition to bounded host owner tests. Continue retained
DMA bounce tests and native storage/exec regressions before phase completion.
