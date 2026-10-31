/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private uploaded storage retains its native VA until both DMA and translation retirement are proved. */
#ifndef KERN_DRIVERS_GPU_BCM2711_NATIVE_STORAGE_H
#define KERN_DRIVERS_GPU_BCM2711_NATIVE_STORAGE_H

#include "drivers/gpu/bcm2711/v3d-memory.h"
#include "drivers/gpu/bcm2711/compiler/shader.h"

/* The registered renderer keeps up to this many bytes of idle upload views mapped for reuse. */
#define BCM2711_NATIVE_CACHE_BYTES (96ULL * 1024U * 1024U)

/* One unpublished or prepared job owns this complete cached allocation through its independent native mapping reference. */
struct bcm2711_native_storage {
	struct bcm2711_v3d_space *space;
	struct bcm2711_v3d_view *view;
	uint64_t bytes;
	/*
	 * The session mark of an owned storage (zero for the other kinds), and
	 * whether it was given again without zeroing, still holding that
	 * session's earlier bytes.
	 */
	uint64_t owner;
	bool stale;
	/* Diagnostic wall times belong to this owner and never control its lifetime. */
	uint64_t allocation_ticks;
	uint64_t mapping_ticks;
	uint64_t clean_ticks;
};

/* The registered renderer keeps up to this many bytes of converted texture snapshots. */
#define BCM2711_NATIVE_SNAPSHOT_BYTES (128ULL * 1024U * 1024U)

/*
 * One converted UIF texture of one raster image interval, owned by the space.
 *
 * users counts the draws of the current pass that sample it; a snapshot with
 * users can be neither rewritten nor evicted.  The buffer reference keeps the
 * source's generation and flags readable for the snapshot's lifetime.
 */
struct bcm2711_native_snapshot {
	struct bcm2711_native_snapshot *next;
	struct bcm2711_buffer *buffer;
	uint64_t offset;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t texel_bytes;
	/* One, or the levels of a mip tree (which the texture formatting unit converts whole). */
	uint32_t levels;
	uint64_t generation;
	/*
	 * A 16-bit copy of 32-bit indices (texel_bytes zero, width the
	 * indices) whose source at generation held an index too large for it.
	 */
	bool unfit;
	/* Pixels converted at generation; false while the texture formatting unit has yet to fill them. */
	bool valid;
	struct bcm2711_native_storage *pixels;
	uint32_t users;
	uint64_t used;
	/* The session whose earlier bytes the pixels may still hold outside what the conversion wrote, or zero. */
	uint64_t owner;
};

/* The controller mutex serializes every operation with native execution and recovery. */
int bcm2711_native_storage_create(struct bcm2711_v3d_space *space, uint64_t bytes, struct bcm2711_native_storage **storage);

/*
 * How a reused (cached, still mapped) view is given again:
 * zeroed, or as it is, for a caller that writes every byte the device may
 * read before the device reads it (the zeroing is the caller's to skip, by
 * kind; a new allocation is always zeroed).  Either way the caller cleans
 * the whole storage before the device sees it.
 */
#define BCM2711_NATIVE_STORAGE_ZEROED	0U
#define BCM2711_NATIVE_STORAGE_WRITTEN	1U

/*
 * Owned by the building session (space->builder): a cached view is given
 * again as it is only when that same session left it, so bytes the device
 * may read but the use does not write can only be the session's own.
 * Anything else is zeroed.
 */
#define BCM2711_NATIVE_STORAGE_OWNED	2U
int bcm2711_native_storage_create_as(struct bcm2711_v3d_space *space, uint64_t bytes, unsigned fill, struct bcm2711_native_storage **storage);
int bcm2711_native_storage_clean(struct bcm2711_native_storage *storage);
int bcm2711_native_storage_clean_range(struct bcm2711_native_storage *storage, uint64_t offset, uint64_t bytes);
int bcm2711_native_program_upload(struct bcm2711_v3d_space *space, const struct bcm2711_shader_binary *binary, struct bcm2711_native_storage **storage);

/* retired=false preserves the whole owner; true consumes and nulls it even if final translation flush fails into the space quarantine. */
int bcm2711_native_storage_release(struct bcm2711_v3d_space *space, struct bcm2711_native_storage **storage, bool retired);

/* Unmaps every cached idle upload view and every unused snapshot; recovery calls it before reclaiming translations. */
int bcm2711_native_storage_drain(struct bcm2711_v3d_space *space);

/*
 * ENOTSUP means the source may change without a generation step, and the caller converts a private copy.
 * With unit set, a whole conversion is left to the caller's unit job, which reports it with _filled;
 * the CPU still converts a small changed box of the raster in place.
 */
int bcm2711_native_snapshot_get(struct bcm2711_v3d_space *space, struct bcm2711_buffer *buffer, uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch, const void *raster, size_t raster_bytes, bool unit, struct bcm2711_native_snapshot **result);
int bcm2711_native_snapshot_get_texel(struct bcm2711_v3d_space *space, struct bcm2711_buffer *buffer, uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch, uint32_t texel_bytes, const void *raster, size_t raster_bytes, bool unit, struct bcm2711_native_snapshot **result);
int bcm2711_native_snapshot_get_levels(struct bcm2711_v3d_space *space, struct bcm2711_buffer *buffer, uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch, uint32_t texel_bytes, const void *raster, size_t raster_bytes, bool unit, uint32_t levels, uint32_t tree_bytes, struct bcm2711_native_snapshot **result);

/*
 * Finds or makes the 16-bit copy of count 32-bit indices at offset in a
 * buffer, kept like a texture snapshot until the buffer's generation moves.
 * ENOTSUP leaves the caller reading the 32-bit indices: a host-mapped
 * source, an index of 0xffff or more, or a copy every draw still uses.
 */
int bcm2711_native_snapshot_get_indices(struct bcm2711_v3d_space *space, struct bcm2711_buffer *buffer, uint64_t offset, uint32_t count, const void *indices, size_t indices_bytes, struct bcm2711_native_snapshot **result);
void bcm2711_native_snapshot_put(struct bcm2711_native_snapshot *snapshot);
void bcm2711_native_snapshot_filled(struct bcm2711_native_snapshot *snapshot);

#endif
