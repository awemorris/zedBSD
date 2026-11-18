/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The migrate context: an address space and a pinned context on the first
 * copy engine, for copies the GPU does.
 *
 *   address space     a kernel PPGTT; per copy engine instance i at
 *                     base = i << 32, two 8 MiB windows (source and
 *                     destination), then a PTE window right after them with
 *                     one PTE per page of the windows; page tables for the
 *                     whole range; and every page table of the windows
 *                     mapped itself, uncached, into the PTE window, so a
 *                     blit can rewrite the windows' PTEs
 *   pinned context    a 512 KiB ring, the timeline in the engine's own
 *                     status page at I915_GT_HWS_MIGRATE, built and pinned
 *                     as any context
 *
 * The GT start does not check the result: without a migrate context the
 * move path falls back to a CPU copy.  With one copy engine and no 64K
 * pages the layout is [0, 8M) source, [8M, 16M) destination and
 * [16M, 16M + 32K) PTEs.  Nothing is submitted here.
 *
 * The 512 KiB ring is larger than a DMA vector allows, so the memory layer
 * makes it one coherent allocation, and the page tables are created on demand
 * from the object pool instead of being stashed in advance under ww locks.
 */

#ifndef DRIVERS_GPU_I915_MIGRATE_H
#define DRIVERS_GPU_I915_MIGRATE_H

#include <stdint.h>

#include "context.h"
#include "ppgtt.h"

struct i915_gt_engines;
struct i915_gt_mem;

/* The copy chunk (CHUNK_SZ = SZ_8M) and the migrate ring size (SZ_512K). */
#define I915_MIGRATE_CHUNK_SZ		(8U << 20)
#define I915_MIGRATE_RING_BYTES		(512U << 10)

/*
 * The migrate address space and context.
 *
 * The device owns one; it lives from drv_i915_migrate_init() to
 * drv_i915_migrate_fini().
 */
struct i915_gt_migrate {
	/* The migrate address space and the pinned context on it. */
	struct i915_gt_ppgtt vm;
	struct i915_gt_context ce;

	/* Nonzero once a copy engine was found, and which engine it is. */
	int has_engine;
	unsigned engine_idx;

	/* The pinned timeline: the engine's status page at I915_GT_HWS_MIGRATE. */
	uint32_t hwsp_ggtt;
	volatile uint32_t *hwsp_cpu;
	uint32_t tl_seqno;

	/* The two windows' size, where the PTE window starts, and how many page tables it maps. */
	uint64_t window_bytes;
	uint64_t pte_window;
	unsigned exposed_pts;

	/* The first failure (a positive errno) and the step it came from. */
	int err;
	const char *err_where;

	/* Nonzero once the context is ready. */
	int inited;
};

int drv_i915_migrate_init(struct i915_gt_migrate *m, struct i915_gt_engines *es, struct i915_gt_mem *gm);
void drv_i915_migrate_fini(struct i915_gt_migrate *m, struct i915_gt_mem *gm);

#endif
