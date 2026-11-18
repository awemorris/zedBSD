/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The global GTT: the GT window, the display window and the entry encoding.
 *
 * The GGTT is a flat table of 64-bit entries in the upper half of BAR0.  The
 * driver owns two fixed windows of it:
 *
 *   GT window        the top I915_GT_GGTT_PAGES pages, for the GT's own
 *                    objects (rings, context images, status pages, scratch)
 *   display window   directly below it, for scanout buffers, claimed
 *                    explicitly and tracked by its own bitmap, so growing
 *                    the display side never moves a ring, a context image
 *                    or a status page
 *
 * Every entry outside an allocation holds the scratch encoding.  A GGTT
 * entry and a PPGTT leaf encode a page differently and must never share an
 * encoder: bit 1 is I915_GT_GEN12_GGTT_PTE_LM in the GGTT but I915_GT_GEN8_PAGE_RW in a
 * PPGTT.  For system memory:
 *
 *   GGTT entry   = DMA address | PRESENT      (LM clear)
 *   PPGTT leaf   = DMA address | PRESENT | RW
 *
 * Every encode first checks the whole [address, address + length) against
 * the DMA mask without overflow and requires page alignment; it never masks
 * high bits off to force an address into range and never truncates to 32
 * bits.
 */

#ifndef DRIVERS_GPU_I915_GGTT_H
#define DRIVERS_GPU_I915_GGTT_H

#include "dma.h"
#include "memory.h"

#include <stdint.h>

/* The present bit of a GGTT entry. */
#define I915_GEN8_PAGE_PRESENT		(((uint64_t)1) << 0)

/* The local-memory bit of a GGTT entry; clear for system memory. */
#define I915_GEN12_GGTT_PTE_LM		(((uint64_t)1) << 1)

/* The page one GGTT entry maps. */
#define I915_PAGE_SIZE			((uint64_t)0x1000)

int drv_i915_dma_in_range(uint64_t dma_addr, uint64_t length, uint64_t mask);
int drv_i915_ggtt_pte_encode(i915_dma_addr_t dma, uint64_t length, uint64_t mask, uint64_t *pte_out);

int drv_i915_gt_ggtt_bind(struct i915_gt_mem *gm, struct i915_gt_object *o);
void drv_i915_gt_ggtt_unbind(struct i915_gt_mem *gm, struct i915_gt_object *o);
void drv_i915_gt_ggtt_flush(struct i915_gt_mem *gm);
uint64_t drv_i915_gt_ggtt_read_pte(const struct i915_gt_mem *gm, unsigned index);

int drv_i915_gt_display_window_init(struct i915_gt_mem *gm, unsigned pages);
int drv_i915_gt_display_bind(struct i915_gt_mem *gm, struct i915_gt_object *o, unsigned align_pages, unsigned guard_pages);
void drv_i915_gt_display_unbind(struct i915_gt_mem *gm, struct i915_gt_object *o);
int drv_i915_gt_display_bind_foreign(struct i915_gt_mem *gm, uint64_t phys, unsigned pages, unsigned *ggtt_page_out);
void drv_i915_gt_display_unbind_foreign(struct i915_gt_mem *gm, unsigned ggtt_page, unsigned pages);
unsigned drv_i915_gt_ggtt_restore(struct i915_gt_mem *gm);

int drv_i915_gt_init_scratch(struct i915_gt_mem *gm, struct i915_gt_object **out);

#endif
