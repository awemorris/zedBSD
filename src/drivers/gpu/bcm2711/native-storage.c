/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native upload owners separate logical callback completion, DMA retirement and final translation flush. */
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/native-state.h"
#include "drivers/gpu/bcm2711/native-storage.h"

/* The underlying contiguous owner accepts at most 256 MiB per physical allocation. */
#define NATIVE_STORAGE_BYTES (256ULL * 1024U * 1024U)

/* An idle view kept for reuse holds at most 16 MiB. */
#define NATIVE_CACHE_VIEW_BYTES (16ULL * 1024U * 1024U)

/* Next to the texture formatting unit, the CPU converts a changed box of at most 256 KiB of pixels in place. */
#define NATIVE_SNAPSHOT_UPDATE_BYTES (256U * 1024U)

static struct bcm2711_v3d_view *cache_take(struct bcm2711_v3d_space *space, uint64_t bytes, uint64_t owner);
static bool cache_put(struct bcm2711_v3d_space *space, struct bcm2711_v3d_view *view);
static int storage_cleanable(const struct bcm2711_native_storage *storage);
static int snapshot_fill(struct bcm2711_native_snapshot *snapshot, const void *raster, size_t raster_bytes);
static int snapshot_update(struct bcm2711_v3d_space *space, struct bcm2711_native_snapshot *snapshot, const void *raster, size_t raster_bytes, bool unit, bool *updated);
static int snapshot_evict(struct bcm2711_v3d_space *space, uint64_t bytes, bool all);
static int snapshot_release(struct bcm2711_v3d_space *space, struct bcm2711_native_snapshot *snapshot);
static int snapshot_scrub(struct bcm2711_native_snapshot *snapshot);

/*
 * Allocates and maps one complete native upload interval before giving it to a prepared job owner.
 *
 * A failed translation publication remains owned by the native space's
 * existing quarantine.  The caller must close native admission after that
 * failure and use checked recovery rather than repeat publication blindly.
 */
int
bcm2711_native_storage_create(
	struct bcm2711_v3d_space *space,
	uint64_t bytes,
	struct bcm2711_native_storage **storage)
{
	/* Zeroed, as every storage was. */
	return bcm2711_native_storage_create_as(space, bytes, BCM2711_NATIVE_STORAGE_ZEROED, storage);
}

/*
 * Allocates and maps one complete native upload interval, a reused view
 * zeroed, or as it is for BCM2711_NATIVE_STORAGE_WRITTEN and for
 * BCM2711_NATIVE_STORAGE_OWNED when the building session left it.
 */
int
bcm2711_native_storage_create_as(
	struct bcm2711_v3d_space *space,
	uint64_t bytes,
	unsigned fill,
	struct bcm2711_native_storage **storage)
{
	struct bcm2711_native_storage *created;
	struct bcm2711_buffer *buffer;
	struct bcm2711_v3d_view *view;
	uint64_t limit;
	uint64_t started;
	uint32_t physical_bits;
	bool keep;
	int error;

	/* Every failure leaves the caller without a partially initialized upload owner. */
	if (storage == NULL)
		return EINVAL;
	*storage = NULL;

	/* Only the actual initialized native address space can supply identified placement constraints. */
	if (space == NULL || space->native == NULL)
		return EINVAL;

	/* Per-allocation bounds also keep complete clean/copy operations representable on this architecture. */
	if (bytes == 0 || bytes > NATIVE_STORAGE_BYTES)
		return EINVAL;
	if (fill != BCM2711_NATIVE_STORAGE_ZEROED &&
	    fill != BCM2711_NATIVE_STORAGE_WRITTEN &&
	    fill != BCM2711_NATIVE_STORAGE_OWNED)
		return EINVAL;

	/* Placement uses actual physical addressing capacity, without a guessed bus alias or framebuffer limit. */
	physical_bits = space->native->hardware.physical_bits;
	if (physical_bits < 30U || physical_bits > 36U)
		return ENOTSUP;
	limit = ((uint64_t)1 << physical_bits) - 1U;

	/* The CPU owner exists before any physical run or native translation can be acquired. */
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;

	/* An owned storage carries the building session's mark into the cache when it is released. */
	if (fill == BCM2711_NATIVE_STORAGE_OWNED)
		created->owner = space->builder;

	/*
	 * An idle cached view of the same size is reused with its mapping:
	 * zeroed again, or as it is for a caller that writes every byte the
	 * device may read, or for an owned use when the same session left it
	 * (counted apart).
	 */
	started = clock_ticks();
	view = cache_take(space, bytes, created->owner);
	if (view != NULL) {
		keep = fill == BCM2711_NATIVE_STORAGE_WRITTEN ||
		    (fill == BCM2711_NATIVE_STORAGE_OWNED && created->owner != 0 && view->owner == created->owner);
		if (!keep)
			kern_memset(view->buffer->address, 0, (size_t)view->bytes);
		view->owner = 0;
		view->buffer->bytes = bytes;
		created->allocation_ticks = clock_ticks() - started;
		if (keep)
			bcm2711_buffer_kept(view->bytes);
		else
			bcm2711_buffer_zeroed(view->bytes, created->allocation_ticks);
		created->stale = keep;
		created->space = space;
		created->view = view;
		created->bytes = bytes;
		*storage = created;

		/* Succeeded: the caller owns the cached VA and its zeroed physical run. */
		return 0;
	}

	/* Cached private storage has no exported user alias and is explicitly cleaned before device reads. */
	started = clock_ticks();
	error = bcm2711_buffer_create(bytes, limit, 4096U, &buffer);
	created->allocation_ticks = clock_ticks() - started;
	if (error != 0) {
		kern_free(created);
		return error;
	}

	/* Mapping acquires its own allocation hold before the temporary source reference is retired. */
	started = clock_ticks();
	error = bcm2711_v3d_memory_map(space, buffer, &view);
	created->mapping_ticks = clock_ticks() - started;
	bcm2711_buffer_release(buffer);
	if (error != 0) {
		kern_free(created);
		return error;
	}

	/* Only a complete owned mapping can become native upload storage. */
	created->space = space;
	created->view = view;
	created->bytes = bytes;
	*storage = created;

	/* Succeeded: the caller owns both the VA reservation and the zeroed physical run independently of public resources. */
	return 0;
}

/*
 * Cleans a complete cached upload allocation, including initialized page padding, before native publication.
 */
int
bcm2711_native_storage_clean(
	struct bcm2711_native_storage *storage)
{
	struct bcm2711_buffer *buffer;
	uint64_t started;
	int error;

	/* Only a live, cached, whole private allocation can be written back. */
	error = storage_cleanable(storage);
	if (error != 0)
		return error;
	buffer = storage->view->buffer;

	/* Cache maintenance completes CPU writes before the caller's later command publication barrier. */
	started = clock_ticks();
	kern_dcache_clean_range(buffer->address, (size_t)buffer->memory.size);
	storage->clean_ticks += clock_ticks() - started;

	/* Succeeded: device reads can consume the immutable upload bytes without a dirty CPU cache alias. */
	return 0;
}

/*
 * Cleans one interval of a cached upload allocation after the CPU rewrote only that interval.
 */
int
bcm2711_native_storage_clean_range(
	struct bcm2711_native_storage *storage,
	uint64_t offset,
	uint64_t bytes)
{
	struct bcm2711_buffer *buffer;
	uint64_t started;
	int error;

	/* Only a live, cached, whole private allocation can be written back. */
	error = storage_cleanable(storage);
	if (error != 0)
		return error;
	buffer = storage->view->buffer;

	/* The interval lies inside the declared upload bytes. */
	if (bytes == 0 || offset > storage->bytes || bytes > storage->bytes - offset)
		return EINVAL;

	/* Cache maintenance completes the interval's CPU writes before the caller's later command publication barrier. */
	started = clock_ticks();
	kern_dcache_clean_range((uint8_t *)buffer->address + offset, (size_t)bytes);
	storage->clean_ticks += clock_ticks() - started;

	/* Succeeded: device reads of the interval see the CPU's writes. */
	return 0;
}

/*
 * Uploads the exact independently compiled instruction words into owned, aligned native code storage.
 */
int
bcm2711_native_program_upload(
	struct bcm2711_v3d_space *space,
	const struct bcm2711_shader_binary *binary,
	struct bcm2711_native_storage **storage)
{
	struct bcm2711_native_storage *created;
	uint8_t *destination;
	uint64_t bytes;
	uint64_t instruction;
	uint32_t index;
	uint32_t byte;
	int error;
	int released;

	/* A failed upload never publishes a half-filled native program to the caller. */
	if (storage == NULL)
		return EINVAL;
	*storage = NULL;

	/* Compiler products retain real immutable instruction arrays through their pipeline owners. */
	if (binary == NULL ||
	    binary->code == NULL ||
	    binary->code_count == 0)
		return EINVAL;

	/* Whole instruction byte counts are measured before allocation or any array access. */
	bytes = (uint64_t)binary->code_count * 8U;
	if (bytes > NATIVE_STORAGE_BYTES)
		return ENOTSUP;

	/* One native mapping owns the complete code stream; every code byte is written below, and the rest is never fetched as code. */
	error = bcm2711_native_storage_create_as(space, bytes, BCM2711_NATIVE_STORAGE_WRITTEN, &created);
	if (error != 0)
		return error;

	/* Numerical QPU words are stored explicitly in little-endian hardware order, independent of the host fixture. */
	destination = created->view->buffer->address;
	for (index = 0; index < binary->code_count; index++) {
		/* Every instruction keeps all eight native bytes without an unaligned CPU word store. */
		instruction = binary->code[index];
		for (byte = 0; byte < 8; byte++)
			destination[(size_t)index * 8U + byte] = (uint8_t)(instruction >> (byte * 8U));
	}

	/* All compiled bytes become visible before code addresses can enter a native record; the padding after them is never fetched as code. */
	error = bcm2711_native_storage_clean(created);
	if (error != 0) {
		released = bcm2711_native_storage_release(space, &created, true);
		if (released != 0)
			return released;
		return error;
	}

	/* Publication transfers the complete independent native code owner to its prepared job. */
	*storage = created;

	/* Succeeded: subsequent pipeline identity withdrawal cannot invalidate this job-owned uploaded instruction interval. */
	return 0;
}

/*
 * Consumes one upload owner only after native DMA retirement, preserving failed translation teardown in the space quarantine.
 * A caller-owned translation batch retains the mapped backing until checked end.
 */
int
bcm2711_native_storage_release(
	struct bcm2711_v3d_space *space,
	struct bcm2711_native_storage **storage,
	bool retired)
{
	struct bcm2711_native_storage *owned;
	bool cached;
	int error;

	/* A missing owner pointer cannot participate in an explicit consumption contract. */
	if (storage == NULL)
		return EINVAL;

	/* Already consumed owners require no further translation or physical retirement. */
	owned = *storage;
	if (owned == NULL)
		return 0;

	/* Logical callback completion alone never supplies proof that DMA stopped using this code or scratch interval. */
	if (!retired)
		return EBUSY;

	/* A malformed retirement context cannot consume an otherwise valid independent native owner. */
	if (space == NULL ||
	    space->native == NULL ||
	    owned->view == NULL)
		return EINVAL;

	/* A different controller cannot withdraw this owner's actual native VA reservation. */
	if (owned->space != space)
		return EINVAL;

	/* An idle view of a reusable size stays mapped for the next pass, marked with the session its bytes may belong to. */
	*storage = NULL;
	owned->view->owner = owned->owner;
	cached = cache_put(space, owned->view);
	if (cached) {
		space->native->hardware.perf.views_cached++;
		kern_free(owned);
		return 0;
	}
	space->native->hardware.perf.views_unmapped++;

	/* The native space owns a failed final unmap/flush; callers must never retry a consumed mapped reference. */
	error = bcm2711_v3d_memory_release(space, owned->view);
	kern_free(owned);
	if (error != 0)
		return error;

	/* Succeeded: the CPU root is consumed; a translation batch still owns backing until its checked end. */
	return 0;
}

/*
 * Unmaps every cached idle upload view.
 *
 * Recovery calls this after the native reset and before reclaiming
 * translations, so no cached reference keeps a VA reservation alive.
 */
int
bcm2711_native_storage_drain(
	struct bcm2711_v3d_space *space)
{
	struct bcm2711_v3d_view *view;
	int first;
	int error;

	/* Unused snapshots go first, so their pixels join the idle views released below. */
	first = snapshot_evict(space, 0, true);

	/* Releases both lists; a failed release stays in the space's quarantine. */
	for (;;) {
		/* Takes the next cached view from either list. */
		view = space->cache_pages;
		if (view != NULL) {
			space->cache_pages = view->cache_next;
		} else {
			view = space->cache_runs;
			if (view == NULL)
				break;
			space->cache_runs = view->cache_next;
		}
		view->cache_next = NULL;
		space->cache_bytes -= view->bytes;

		/* The cache's reference was the last, so this unmaps and frees the view. */
		error = bcm2711_v3d_memory_release(space, view);
		if (first == 0 && error != 0)
			first = error;
	}

	/* Reports the first failed release. */
	if (first != 0)
		return first;

	/* Succeeded: no cached view holds a translation. */
	return 0;
}

/*
 * Finds or makes the converted texture of one raster image interval and counts one more user.
 *
 * A snapshot is reused while its never-mapped source keeps the generation it
 * was converted at.  A stale unused one is converted again in place: only the
 * box the source's write ring names when it can, otherwise the whole image,
 * by the CPU or, when unit is set, by the caller's texture formatting unit job.
 * The caller holds the controller mutex, every earlier pass has retired, and
 * the caller drops its use with bcm2711_native_snapshot_put when its draw retires.
 */
int
bcm2711_native_snapshot_get(
	struct bcm2711_v3d_space *space,
	struct bcm2711_buffer *buffer,
	uint64_t offset,
	uint32_t width,
	uint32_t height,
	uint32_t pitch,
	const void *raster,
	size_t raster_bytes,
	bool unit,
	struct bcm2711_native_snapshot **result)
{
	int error;

	/* An RGBA8 or BGRA8 image has four-byte texels. */
	error = bcm2711_native_snapshot_get_texel(space, buffer, offset, width, height, pitch, 4U, raster, raster_bytes, unit, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller samples the snapshot's pixels until its draw retires. */
	return 0;
}

/*
 * Finds or makes the converted texture of one raster interval of one-, two- or four-byte texels.
 *
 * As bcm2711_native_snapshot_get; texel_bytes is part of the key.  A snapshot
 * of one- or two-byte texels is always converted whole (the plane of a
 * decoded picture changes whole).
 */
int
bcm2711_native_snapshot_get_texel(
	struct bcm2711_v3d_space *space,
	struct bcm2711_buffer *buffer,
	uint64_t offset,
	uint32_t width,
	uint32_t height,
	uint32_t pitch,
	uint32_t texel_bytes,
	const void *raster,
	size_t raster_bytes,
	bool unit,
	struct bcm2711_native_snapshot **result)
{
	int error;

	/* Level 0 alone, in its strict UIF size. */
	error = bcm2711_native_snapshot_get_levels(space, buffer, offset, width, height, pitch, texel_bytes, raster, raster_bytes, unit, 1, 0, result);
	if (error != 0)
		return error;

	/* Succeeded: the caller samples the snapshot's pixels until its draw retires. */
	return 0;
}

/*
 * As bcm2711_native_snapshot_get_texel, for a mip tree of levels levels
 * (more than one) of tree_bytes, which only the texture formatting unit
 * fills (unit), whole, each time its source changed; levels is part of the
 * key.  One level is bcm2711_native_snapshot_get_texel.
 */
int
bcm2711_native_snapshot_get_levels(
	struct bcm2711_v3d_space *space,
	struct bcm2711_buffer *buffer,
	uint64_t offset,
	uint32_t width,
	uint32_t height,
	uint32_t pitch,
	uint32_t texel_bytes,
	const void *raster,
	size_t raster_bytes,
	bool unit,
	uint32_t levels,
	uint32_t tree_bytes,
	struct bcm2711_native_snapshot **result)
{
	struct bcm2711_native_snapshot *snapshot;
	const void *whole;
	uint32_t bytes;
	bool updated;
	bool foreign;
	int error;

	/* A source a process can write through a CPU mapping has no generation to trust. */
	*result = NULL;
	if (space->snapshot_limit == 0 || buffer->host_mapped)
		return ENOTSUP;
	error = bcm2711_native_texture_size_texel(width, height, texel_bytes, &bytes);
	if (error != 0)
		return error;

	/* A mip tree is the unit's alone, and as large as its levels. */
	if (levels == 0)
		levels = 1;
	if (levels > 1U) {
		if (!unit || tree_bytes < bytes)
			return ENOTSUP;
		bytes = tree_bytes;
	}
	space->snapshot_serial++;

	/* A whole conversion left to the unit reads no raster on the CPU. */
	whole = raster;
	if (unit)
		whole = NULL;

	/* Finds the snapshot of exactly this interval and layout. */
	for (snapshot = space->snapshots; snapshot != NULL; snapshot = snapshot->next) {
		/* Every key field must match; a different view of the same run is another snapshot. */
		if (snapshot->buffer == buffer &&
		    snapshot->offset == offset &&
		    snapshot->width == width &&
		    snapshot->height == height &&
		    snapshot->pitch == pitch &&
		    snapshot->texel_bytes == texel_bytes &&
		    snapshot->levels == levels)
			break;
	}

	/*
	 * A known snapshot is converted again only when its source changed and
	 * no draw samples it.  One whose pixels may hold another session's
	 * earlier bytes is zeroed and converted whole before this session
	 * samples it.
	 */
	if (snapshot != NULL) {
		foreign = snapshot->owner != 0 && snapshot->owner != space->builder;
		if (foreign || !snapshot->valid || snapshot->generation != buffer->generation) {
			if (snapshot->users != 0)
				return ENOTSUP;
			if (foreign) {
				error = snapshot_scrub(snapshot);
				if (error != 0)
					return error;
			}

			/* Converted four-byte pixels with a known small change are brought up to date in place. */
			updated = false;
			if (snapshot->valid && texel_bytes == 4U && levels == 1U) {
				error = snapshot_update(space, snapshot, raster, raster_bytes, unit, &updated);
				if (error != 0)
					return error;
			}

			/* Anything else is converted whole. */
			if (!updated) {
				error = snapshot_fill(snapshot, whole, raster_bytes);
				if (error != 0)
					return error;
				space->snapshot_fills++;
			}
		}
		snapshot->users++;
		snapshot->used = space->snapshot_serial;
		*result = snapshot;
		return 0;
	}

	/* Makes room among unused snapshots; a full cache of used ones leaves the private copy. */
	error = snapshot_evict(space, bytes, false);
	if (error != 0)
		return error;

	/*
	 * Creates the new entry with its own pixels before converting; pixels
	 * the building session left are given again unzeroed, and the snapshot
	 * remembers that session until it is scrubbed (a CPU conversion
	 * rewrites the texture's bytes but not the rest of the page).
	 */
	snapshot = kern_calloc(1, sizeof(*snapshot));
	if (snapshot == NULL)
		return ENOMEM;
	error = bcm2711_native_storage_create_as(space, bytes, BCM2711_NATIVE_STORAGE_OWNED, &snapshot->pixels);
	if (error != 0) {
		kern_free(snapshot);
		return error;
	}
	if (snapshot->pixels->stale)
		snapshot->owner = snapshot->pixels->owner;
	snapshot->offset = offset;
	snapshot->width = width;
	snapshot->height = height;
	snapshot->pitch = pitch;
	snapshot->texel_bytes = texel_bytes;
	snapshot->levels = levels;

	/* The first conversion defines the generation the snapshot is valid for. */
	snapshot->buffer = buffer;
	error = snapshot_fill(snapshot, whole, raster_bytes);
	if (error != 0) {
		snapshot->buffer = NULL;
		(void)bcm2711_native_storage_release(space, &snapshot->pixels, true);
		kern_free(snapshot);
		return error;
	}
	space->snapshot_fills++;

	/* The snapshot holds its source run, so the run's flags and generation stay readable. */
	bcm2711_buffer_retain(buffer);
	snapshot->users = 1;
	snapshot->used = space->snapshot_serial;
	snapshot->next = space->snapshots;
	space->snapshots = snapshot;
	space->snapshot_bytes += snapshot->pixels->view->bytes;
	*result = snapshot;

	/* Succeeded: the caller samples the snapshot's pixels until its draw retires. */
	return 0;
}

/*
 * Drops one draw's use of a snapshot after the draw's DMA retired.
 */
void
bcm2711_native_snapshot_put(
	struct bcm2711_native_snapshot *snapshot)
{
	/* A snapshot with no user may be converted again or evicted by a later pass. */
	if (snapshot->users == 0)
		__builtin_trap();
	snapshot->users--;
}

/*
 * Marks a snapshot's pixels as converted once the unit job that writes them completed.
 *
 * A pass that never ran or failed leaves the snapshot invalid, and its next use converts it again.
 */
void
bcm2711_native_snapshot_filled(
	struct bcm2711_native_snapshot *snapshot)
{
	/* The generation recorded when the job was prepared stays the one the pixels match at least. */
	snapshot->valid = true;
}

/* Reports whether an upload owner is a live, cached, whole private allocation the CPU may clean. */
static int
storage_cleanable(
	const struct bcm2711_native_storage *storage)
{
	struct bcm2711_v3d_view *view;
	struct bcm2711_buffer *buffer;

	/* An absent or partially retired owner grants no usable native input. */
	if (storage == NULL || storage->view == NULL)
		return EINVAL;
	view = storage->view;

	/* Quarantined or dead translations cannot be cleaned back into executable ownership. */
	if (view->quarantined || view->references == 0)
		return EIO;

	/* Actual cached backing must cover the entire declared upload interval. */
	buffer = view->buffer;
	if (buffer == NULL || buffer->address == NULL)
		return EINVAL;

	/* This private owner never treats non-cacheable exported storage as a cached alias. */
	if (buffer->uncached)
		return EINVAL;

	/* Padding and logical bytes belong to the same whole mapped allocation. */
	if (storage->bytes == 0 ||
	    storage->bytes > buffer->bytes ||
	    buffer->bytes > buffer->memory.size ||
	    buffer->memory.size != view->bytes)
		return EINVAL;

	/* Succeeded: the owner's whole mapped allocation may be cleaned. */
	return 0;
}

/*
 * Takes one idle cached view of exactly the page-rounded size, or NULL; a
 * larger run the given nonzero owner left is preferred to another of the
 * same size.
 */
static struct bcm2711_v3d_view *
cache_take(
	struct bcm2711_v3d_space *space,
	uint64_t bytes,
	uint64_t owner)
{
	struct bcm2711_v3d_view **position;
	struct bcm2711_v3d_view **first;
	struct bcm2711_v3d_view *view;
	uint64_t rounded;

	/* Single pages come from their own list without a search. */
	rounded = (bytes + 4095U) & ~4095ULL;
	if (rounded == 4096U) {
		view = space->cache_pages;
		if (view == NULL)
			return NULL;
		space->cache_pages = view->cache_next;
		view->cache_next = NULL;
		space->cache_bytes -= view->bytes;
		return view;
	}

	/* Larger runs are matched by exact size, the owner's own first. */
	first = NULL;
	for (position = &space->cache_runs; *position != NULL; position = &(*position)->cache_next) {
		/* Remembers the first run of the same page count, and stops there without an owner or at one the owner left. */
		if ((*position)->bytes != rounded)
			continue;
		if (first == NULL)
			first = position;
		if (owner == 0 || (*position)->owner == owner)
			break;
	}
	if (*position == NULL)
		position = first;
	if (position == NULL)
		return NULL;
	view = *position;
	*position = view->cache_next;
	view->cache_next = NULL;
	space->cache_bytes -= view->bytes;

	/* Succeeded: the caller now owns the cache's reference of the view. */
	return view;
}

/* Keeps one retired private upload view mapped for reuse when it fits the cache. */
static bool
cache_put(
	struct bcm2711_v3d_space *space,
	struct bcm2711_v3d_view *view)
{
	/* Only a sole, healthy, cached-RAM private view of bounded size is kept. */
	if (view->references != 1 ||
	    view->quarantined ||
	    view->pending_release ||
	    view->buffer == NULL ||
	    view->buffer->uncached)
		return false;
	if (view->bytes > NATIVE_CACHE_VIEW_BYTES || space->cache_bytes + view->bytes > space->cache_limit)
		return false;

	/* A faulted engine retires views through recovery instead of keeping them. */
	if (!space->native->hardware.ready || space->native->hardware.faulted)
		return false;

	/*
	 * The cache keeps no dirty line: a view given again
	 * without zeroing must not later write back the bytes of a user that
	 * failed before cleaning over what the device wrote since.
	 */
	kern_dcache_clean_range(view->buffer->address, (size_t)view->buffer->memory.size);

	/* Pushes the view on the list of its size class. */
	if (view->bytes == 4096U) {
		view->cache_next = space->cache_pages;
		space->cache_pages = view;
	} else {
		view->cache_next = space->cache_runs;
		space->cache_runs = view;
	}
	space->cache_bytes += view->bytes;

	/* Succeeded: the cache holds the view's last reference. */
	return true;
}

/*
 * Converts the snapshot's source raster into its UIF pixels and records the source generation.
 *
 * Without a raster the caller's texture formatting unit job writes the
 * pixels when its pass runs; the snapshot stays invalid until
 * bcm2711_native_snapshot_filled reports that job's completion.
 */
static int
snapshot_fill(
	struct bcm2711_native_snapshot *snapshot,
	const void *raster,
	size_t raster_bytes)
{
	int error;

	/* The generation is recorded first; a unit fill reads the source no earlier than this. */
	snapshot->generation = snapshot->buffer->generation;
	snapshot->valid = false;
	if (raster == NULL)
		return 0;

	/* The CPU conversion writes every block, including zero padding; four-byte texels have the faster copy. */
	if (snapshot->texel_bytes == 4U) {
		error = bcm2711_native_texture_copy(
			snapshot->width,
			snapshot->height,
			raster,
			snapshot->pitch,
			raster_bytes,
			snapshot->pixels->view->buffer->address,
			(size_t)snapshot->pixels->bytes);
	} else {
		error = bcm2711_native_texture_copy_texel(
			snapshot->width,
			snapshot->height,
			snapshot->texel_bytes,
			raster,
			snapshot->pitch,
			raster_bytes,
			snapshot->pixels->view->buffer->address,
			(size_t)snapshot->pixels->bytes);
	}

	/* A conversion that failed leaves the snapshot invalid. */
	if (error != 0)
		return error;

	/* The device reads the pixels only after the CPU cache wrote them back. */
	error = bcm2711_native_storage_clean(snapshot->pixels);
	if (error != 0)
		return error;

	/* Succeeded: the snapshot matches its source until the source's generation moves. */
	snapshot->valid = true;
	return 0;
}

/*
 * Zeroes an unused snapshot's whole pixels, which may hold another
 * session's earlier bytes, and leaves it to be converted whole.
 */
static int
snapshot_scrub(
	struct bcm2711_native_snapshot *snapshot)
{
	/* Nothing of the earlier session remains, and the pixels match no generation. */
	kern_memset(snapshot->pixels->view->buffer->address, 0, (size_t)snapshot->pixels->view->bytes);
	snapshot->pixels->owner = 0;
	snapshot->owner = 0;
	snapshot->valid = false;

	/* The zeroes reach memory before the device or a later unit job writes the pixels. */
	return bcm2711_native_storage_clean(snapshot->pixels);
}

/*
 * Converts only the box a valid snapshot's source changed since the snapshot's generation.
 *
 * updated stays false when the source's write ring cannot name every change,
 * or, when unit is set, when the change is too large for the CPU; the caller
 * then converts the whole image.  A failure leaves the snapshot invalid.
 */
static int
snapshot_update(
	struct bcm2711_v3d_space *space,
	struct bcm2711_native_snapshot *snapshot,
	const void *raster,
	size_t raster_bytes,
	bool unit,
	bool *updated)
{
	uint64_t generation;
	uint64_t bytes;
	uint64_t column_bytes;
	uint32_t box[4];
	uint32_t column;
	uint32_t first_row;
	uint32_t end_row;
	bool known;
	int error;

	/* Without the raster on the CPU only a whole conversion by the unit is possible. */
	*updated = false;
	if (raster == NULL)
		return 0;

	/*
	 * Reads the generation before the ring, so the box covers at least every
	 * write up to it; a later write only widens the box.
	 */
	generation = snapshot->buffer->generation;
	known = bcm2711_v3d_memory_changes(snapshot->buffer, snapshot->offset, snapshot->generation, box);
	if (!known)
		return 0;

	/* Another view of the run at the same offset may name pixels outside this image. */
	if (box[2] > snapshot->width)
		box[2] = snapshot->width;
	if (box[3] > snapshot->height)
		box[3] = snapshot->height;

	/* No change inside the image leaves the pixels valid for the new generation. */
	if (box[0] >= box[2] || box[1] >= box[3]) {
		snapshot->generation = generation;
		*updated = true;
		return 0;
	}

	/* Next to the unit, the CPU takes only a small box of whole utiles. */
	bytes = (uint64_t)(((box[2] + 3U) & ~3U) - (box[0] & ~3U)) * (((box[3] + 3U) & ~3U) - (box[1] & ~3U)) * 4U;
	if (unit && bytes > NATIVE_SNAPSHOT_UPDATE_BYTES)
		return 0;

	/* The pixels are invalid from the first byte the CPU writes until the clean completes. */
	snapshot->valid = false;
	error = bcm2711_native_texture_copy_box(
		snapshot->width,
		snapshot->height,
		raster,
		snapshot->pitch,
		raster_bytes,
		snapshot->pixels->view->buffer->address,
		(size_t)snapshot->pixels->bytes,
		box);
	if (error != 0)
		return error;

	/*
	 * Writes back the changed block rows of each touched 32-pixel column, which
	 * is contiguous in the column-major layout.  Widening to utiles stays in the
	 * block rows and columns of the box.
	 */
	column_bytes = (uint64_t)((snapshot->height + 7U) / 8U) * 1024U;
	first_row = box[1] / 8U;
	end_row = (box[3] - 1U) / 8U + 1U;
	for (column = box[0] / 32U; column <= (box[2] - 1U) / 32U; column++) {
		error = bcm2711_native_storage_clean_range(
			snapshot->pixels,
			column * column_bytes + (uint64_t)first_row * 1024U,
			(uint64_t)(end_row - first_row) * 1024U);
		if (error != 0)
			return error;
	}

	/* The pixels match the source at the generation read before the ring. */
	snapshot->generation = generation;
	snapshot->valid = true;
	space->snapshot_updates++;
	*updated = true;

	/* Succeeded: the snapshot is current without a whole conversion. */
	return 0;
}

/*
 * Evicts unused snapshots until bytes more fit, or every unused one when all is set.
 *
 * Snapshots whose source only the cache still holds go first, then the least
 * recently used.  ENOTSUP means the used snapshots alone fill the cache.
 */
static int
snapshot_evict(
	struct bcm2711_v3d_space *space,
	uint64_t bytes,
	bool all)
{
	struct bcm2711_native_snapshot **position;
	struct bcm2711_native_snapshot **oldest;
	struct bcm2711_native_snapshot *snapshot;
	uint64_t padded;
	int first;
	int error;

	/* Releases snapshots of destroyed sources and, for a drain, every unused one. */
	first = 0;
	position = &space->snapshots;
	while (*position != NULL) {
		snapshot = *position;

		/* A sampled snapshot stays until its draw retires. */
		if (snapshot->users != 0 || (!all && snapshot->buffer->references > 1)) {
			position = &snapshot->next;
			continue;
		}
		*position = snapshot->next;
		error = snapshot_release(space, snapshot);
		if (first == 0 && error != 0)
			first = error;
	}

	/* Evicts least recently used snapshots until the new pixels fit. */
	padded = (bytes + 4095U) & ~4095ULL;
	while (space->snapshot_bytes + padded > space->snapshot_limit) {
		/* Finds the unused snapshot used longest ago. */
		oldest = NULL;
		for (position = &space->snapshots; *position != NULL; position = &(*position)->next) {
			/* Only an unused snapshot is a candidate. */
			if ((*position)->users == 0 && (oldest == NULL || (*position)->used < (*oldest)->used))
				oldest = position;
		}

		/* Used snapshots alone fill the cache; the caller keeps a private copy. */
		if (oldest == NULL) {
			if (first != 0)
				return first;
			return ENOTSUP;
		}
		snapshot = *oldest;
		*oldest = snapshot->next;
		error = snapshot_release(space, snapshot);
		if (first == 0 && error != 0)
			first = error;
	}

	/* Reports the first failed release, which stays in the space's quarantine. */
	if (first != 0)
		return first;

	/* Succeeded: the requested bytes fit beside the remaining snapshots. */
	return 0;
}

/* Releases one unlinked unused snapshot's pixels and source hold. */
static int
snapshot_release(
	struct bcm2711_v3d_space *space,
	struct bcm2711_native_snapshot *snapshot)
{
	int error;

	/*
	 * Every pass that sampled the pixels has retired, so they can join the
	 * idle views, unmarked: other sessions may have converted or updated
	 * them since the session that made them.
	 */
	space->snapshot_bytes -= snapshot->pixels->view->bytes;
	snapshot->pixels->owner = 0;
	error = bcm2711_native_storage_release(space, &snapshot->pixels, true);
	bcm2711_buffer_release(snapshot->buffer);
	kern_free(snapshot);
	if (error != 0)
		return error;

	/* Succeeded: neither the pixels nor the source hold remain. */
	return 0;
}
