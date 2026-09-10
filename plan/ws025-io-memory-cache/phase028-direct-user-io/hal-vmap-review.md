# HAL vmap duplication review

Date: 2026-09-10
Status: confirmed architectural concern; replacement not implemented

User review: should hal_vmap_* instead use HAL_SPACE_SYS with existing page APIs?

## Findings

HAL_SPACE_SYS is NULL (include/hal/hal.h). Current amd64 hal_space_map rejects NULL
and enforces valid_user_range; prot/query/unmap likewise reject system space.
i386 map has the same restriction. System switching and TLB flush do support it.
Therefore replacing call sites alone does not work today. This implementation
restriction does not justify a second public HAL mapping subsystem.

space-vmap.inc separately writes/clears PTEs via walk_leaf, detaches tables and
shoots down HAL_SPACE_SYS translations. It also owns fixed VA slots, frame
allocation/free policy, mapping lifetime pins and borrowed/owned state. These
higher-level responsibilities should be common kernel VM ownership layered over
HAL page operations. Physical page pin/content leases remain separate and needed.
The feature is useful; its current HAL boundary duplicates mapping machinery.

## Proposed correction

- Support HAL_SPACE_SYS in the existing map/prot/unmap/query family, with explicit
  kernel VA range validation, supervisor PTEs, shared-root ownership, one consistent
  table lock domain, SMP shootdown and rollback/table retirement semantics.
  Removing the NULL check alone is insufficient: user leaf flags and ranges must
  not be reused blindly; preexisting image/direct/MMIO windows must remain intact.
- Put VA reservation, owned/borrowed vectors and lifetime policy in common kernel
  VM code, using HAL physical allocation and page operations. Preserve the frame
  and user content ownership contracts already established.
- Migrate scratch (kern/io.c), DMA vectors (drivers/generic/dma.c) and uaccess
  together. Retire hal_vmap_* and its specialized page lookup after all consumers
  use the shared kernel mapping path. Consider range/vector batching on the common
  page operation path rather than restoring an independent HAL subsystem.

This is a design finding, not a completed refactor or a claim of current corruption.
Native output flag-on test q244-on1 passed; no flag-off timing comparison yet.
Default amd64 image restored successfully. Input/output flags remain default off.
Further direct-I/O rollout is held while this architectural correction is planned.
