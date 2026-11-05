/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Renderer-owned virtual ranges, serialized with native jobs by the caller. */
#ifndef KERN_DRIVERS_GPU_BCM2711_V3D_MEMORY_H
#define KERN_DRIVERS_GPU_BCM2711_V3D_MEMORY_H

#include <stdbool.h>
#include "drivers/gpu/bcm2711/buffer.h"

struct bcm2711_v3d;
struct bcm2711_native_snapshot;

/* Each mapping owns its allocation independently of any resource descriptor. */
struct bcm2711_v3d_view {
	struct bcm2711_v3d_view *next;
	struct bcm2711_buffer *buffer;
	uint32_t address;
	uint32_t references;
	uint64_t bytes;
	bool quarantined;
	/* A zero-reference batch retirement keeps physical storage until the final flush. */
	bool pending_release;
	/* The last reference ended inside the executor's window: still mapped, unmapped once it closes. */
	bool delayed;
	/* Links an idle, still mapped private upload view in the space's reuse cache. */
	struct bcm2711_v3d_view *cache_next;
	/* An idle cached view's last owning session mark, or zero when any session's bytes may be in it. */
	uint64_t owner;
};

/* The controller mutex serializes every edit with all submitted native jobs. */
struct bcm2711_v3d_space {
	struct bcm2711_v3d *native;
	struct bcm2711_v3d_view *views;
	/* A serialized no-DMA interval accumulates PTE changes before one publication flush. */
	bool deferred;
	uint32_t pending_address;
	uint64_t pending_end;
	/*
	 * Idle private upload views stay mapped for reuse by later passes, which
	 * avoids allocating, mapping and flushing the same sizes every frame.
	 * Single pages and larger runs keep separate lists; each holds one view
	 * reference.  Recovery drains both before reclaiming translations.
	 */
	struct bcm2711_v3d_view *cache_pages;
	struct bcm2711_v3d_view *cache_runs;
	uint64_t cache_bytes;
	/* The renderer sets the cache's capacity when it registers; zero keeps no idle view. */
	uint64_t cache_limit;
	/* Converted textures of never-mapped images, reused while their source generation is unchanged. */
	struct bcm2711_native_snapshot *snapshots;
	uint64_t snapshot_bytes;
	uint64_t snapshot_limit;
	uint64_t snapshot_serial;
	/* Whole and in-place partial conversions made by snapshot lookups, which diagnostics tell apart from reuses. */
	uint64_t snapshot_fills;
	uint64_t snapshot_updates;
	/*
	 * The owner mark of the session whose job step is building native
	 * work, set by that step around the creation only (zero otherwise).
	 * Only the executor builds draws, so no other storage user sees it.
	 */
	uint64_t builder;
};

/* No DMA or overlapping edits may occur between begin and checked end. */
int bcm2711_v3d_memory_begin(struct bcm2711_v3d_space *space);
int bcm2711_v3d_memory_end(struct bcm2711_v3d_space *space);

int bcm2711_v3d_memory_map(struct bcm2711_v3d_space *space, struct bcm2711_buffer *buffer, struct bcm2711_v3d_view **result);
void bcm2711_v3d_memory_retain(struct bcm2711_v3d_view *view);
int bcm2711_v3d_memory_release(struct bcm2711_v3d_space *space, struct bcm2711_v3d_view *view);
int bcm2711_v3d_memory_recover(struct bcm2711_v3d_space *space);

/* Unmaps and frees the views whose last reference ended inside the window; the caller holds the mutex after it closed. */
int bcm2711_v3d_memory_release_delayed(struct bcm2711_v3d_space *space);

/* Records one write of the box of the image at offset and advances the run's generation; a NULL box covers everything. */
void bcm2711_v3d_memory_written(struct bcm2711_buffer *buffer, uint64_t offset, const uint32_t *box);

/* Reports in box the union of a run's writes of the image at offset after since; false when they are not all known. */
bool bcm2711_v3d_memory_changes(struct bcm2711_buffer *buffer, uint64_t offset, uint64_t since, uint32_t *box);

/* Reports a run's writes of the image at offset after since as at most capacity boxes; false when they are not all known. */
bool bcm2711_v3d_memory_changes_boxes(struct bcm2711_buffer *buffer, uint64_t offset, uint64_t since, uint32_t (*boxes)[4], uint32_t capacity, uint32_t *count);

#endif
