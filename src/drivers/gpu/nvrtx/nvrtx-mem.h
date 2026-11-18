/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The range allocator of the nvrtx driver, which touches no hardware.
 *
 * One allocator hands out runs of the VRAM the driver may use (the buffer
 * objects, page tables and surfaces, from vram-window on); a second one
 * hands out runs of the CPU's window onto the VRAM (BAR1).  Both keep the
 * free runs in order, take a new run from the highest free run that holds
 * it (away from the low VRAM the firmware's picture lies in), and merge a
 * freed run with its neighbours.  The caller serializes the calls.
 */

#ifndef DRIVERS_GPU_NVRTX_MEM_H
#define DRIVERS_GPU_NVRTX_MEM_H

#include <stdint.h>

/* How many separate free runs one allocator keeps. */
#define NVRTX_HEAP_EXTENTS_MAX		256U

/*
 * One free run of a range: its first address and its length.
 */
struct nvrtx_extent {
	uint64_t base;
	uint64_t size;
};

/*
 * One range the driver hands runs of out.
 *
 * The free runs are sorted by address and never touch one another (a freed
 * run is merged into its neighbours), so count is the number of holes.  The
 * range itself is [base, top).  free_bytes is the sum of the free runs.
 */
struct nvrtx_heap {
	uint64_t base;
	uint64_t top;
	struct nvrtx_extent free[NVRTX_HEAP_EXTENTS_MAX];
	uint32_t count;
	uint64_t free_bytes;
};

void nvrtx_heap_init(struct nvrtx_heap *heap, uint64_t base, uint64_t top);
int nvrtx_heap_alloc(struct nvrtx_heap *heap, uint64_t size, uint64_t align, uint64_t *address);
int nvrtx_heap_reserve(struct nvrtx_heap *heap, uint64_t address, uint64_t size);
int nvrtx_heap_free(struct nvrtx_heap *heap, uint64_t address, uint64_t size);

#endif
