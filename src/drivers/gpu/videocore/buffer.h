/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Contiguous allocation ownership shared by the display and render devices. */
#ifndef KERN_DRIVERS_GPU_BCM2711_BUFFER_H
#define KERN_DRIVERS_GPU_BCM2711_BUFFER_H

#include <stdbool.h>
#include <stdint.h>
#include <kern/lock.h>
#include <kern/pmem.h>

/*
 * The number of recent writes a run remembers for incremental copies: a
 * frame's pass records one for each tile box it changed, so
 * a frame drawn in a few passes still fits.
 */
#define BCM2711_BUFFER_WRITES 16U

/* One remembered write: the run's generation after it, and the pixel box of the image at offset it covered. */
struct bcm2711_buffer_write {
	uint64_t generation;
	uint64_t offset;
	uint32_t box[4];
};

/* Independent references keep one CPU mapping and physical run alive. */
struct bcm2711_buffer {
	struct spinlock guard;
	uint64_t references;
	uint64_t bytes;
	struct kern_pmem memory;
	void *address;

	/* An uncached run is never read or written through its cached direct-map alias. */
	bool uncached;

	/*
	 * generation advances after every device or kernel write of the run;
	 * host_mapped means some process may write it through a CPU mapping at
	 * any time.  Converted texture snapshots are valid only for a run that
	 * was never host mapped and only while generation is unchanged.
	 */
	uint64_t generation;
	bool host_mapped;

	/*
	 * identity never repeats among runs.  writes keeps the boxes of the last
	 * writes in generation order.  A whole-image copy into this run records
	 * its source's identity, offset and generation and this run's generation
	 * after the copy, so the next copy from the same source can copy only
	 * what the source changed since.
	 */
	uint64_t identity;
	struct bcm2711_buffer_write writes[BCM2711_BUFFER_WRITES];
	uint32_t write_next;
	uint64_t synced_identity;
	uint64_t synced_offset;
	uint64_t synced_target_offset;
	uint64_t synced_source_generation;
	uint64_t synced_generation;

	/*
	 * The display resources that hold the run (under guard).  The executor
	 * runs first the executions that use a displayed run.
	 */
	uint32_t displayed;

	/*
	 * The Vulkan allocation declarations bound to the run, and the budget
	 * charged for it, under the render controller's mutex.  The first
	 * declaration charges the run; later ones (imports) add nothing; the
	 * charge returns when the last one goes.
	 */
	uint32_t vulkan_holders;
	uint64_t vulkan_charge;
};

int bcm2711_buffer_create(uint64_t bytes, uint64_t limit, size_t alignment, struct bcm2711_buffer **result);
int bcm2711_buffer_create_uncached(uint64_t bytes, uint64_t limit, size_t alignment, struct bcm2711_buffer **result);
void bcm2711_buffer_retain(struct bcm2711_buffer *buffer);

void bcm2711_buffer_release(struct bcm2711_buffer *buffer);
void bcm2711_buffer_release_stats(uint64_t *count, uint64_t *bytes, uint64_t *ticks);
void bcm2711_buffer_zeroed(uint64_t bytes, uint64_t ticks);
void bcm2711_buffer_kept(uint64_t bytes);
void bcm2711_buffer_create_stats(uint64_t stats[9]);

#endif
