/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The buffer cache.
 *
 * Every cached line covers one page or one block, whichever is larger,
 * of a leaf disk.  Buffer headers live in page-sized slabs and the data
 * in page-aligned physical memory, both accounted against a hard byte
 * cap that reclaim keeps by evicting clean buffers from the LRU tail
 * and writing back dirty ones.  Writes go through to the disk before
 * they return.
 */

#include "kern/buf.h"
#include "kern/io-stats.h"
#include "kern/io-pool.h"
#include "kern/disk.h"
#include "kern/page.h"
#include "kern/cache-memory.h"
#include <errno.h>
#include <string.h>

#define BUF_HASH_BUCKETS	64U
#define BUF_MIN_BYTES		(64U * 1024U)
#define BUF_SLAB_BYTES		KERN_PAGE_SIZE
#define BUF_RUN_LINES		(KERN_IO_BATCH_MAX / KERN_PAGE_SIZE)

#ifndef CONFIG_BUF_CACHE_KIB
#define CONFIG_BUF_CACHE_KIB 0
#endif

/*
 * XXX: 説明を入れる。
 */
struct buf_slab {
	struct kern_pmem memory;
	struct buf_slab *next;
	uint64_t free_mask;
	unsigned capacity;
	unsigned used;
};

/* XXX: これはincludeに置き換えできるかも？. */
struct thread;

/*
 * XXX: 説明を入れる。
 */
static struct spinlock cache_lock;

/*
 * XXX: 説明を入れる。
 */
static struct spinlock dirty_index_lock;

/*
 * XXX: 説明を入れる。
 */
static struct buf *dirty_head;

/*
 * XXX: 説明を入れる。
 */
static struct buf *dirty_tail;

/*
 * XXX: 説明を入れる。
 */
static struct mutex cache_control;

/*
 * Keeps another allocator from consuming space reclaimed for an admission.
 * Held only across component accounting and clean eviction, never backend I/O,
 * shared-cache reclaim, or physical allocation. Resize has its own control lock.
 */
static struct mutex cache_admission;

/*
 * XXX: 説明を入れる。
 */
static struct buf *cache_hash[BUF_HASH_BUCKETS];

/*
 * XXX: 説明を入れる。
 */
static struct buf *lru_head;

/*
 * XXX: 説明を入れる。
 */
static struct buf *lru_tail;

/*
 * XXX: 説明を入れる。
 */
static struct buf_slab *slabs;

/*
 * XXX: 説明を入れる。
 */
static uint64_t cache_max_bytes;

/*
 * XXX: 説明を入れる。
 */
static uint64_t cache_current_bytes;

/*
 * XXX: 説明を入れる。
 */
static uint64_t cache_reserved_bytes;

/*
 * XXX: 説明を入れる。
 */
static uint64_t cache_data_bytes;

/*
 * XXX: 説明を入れる。
 */
static uint64_t cache_metadata_bytes;

/*
 * XXX: 説明を入れる。
 */
static volatile uint64_t cache_dirty_bytes;

/*
 * XXX: 説明を入れる。
 */
static volatile uint64_t stat_buffers;

/*
 * XXX: 説明を入れる。
 */
static volatile uint64_t stat_hits;

/*
 * XXX: 説明を入れる。
 */
static volatile uint64_t stat_misses;

/*
 * XXX: 説明を入れる。
 */
static volatile uint64_t stat_read_bios;

/*
 * XXX: 説明を入れる。
 */
static volatile uint64_t stat_write_bios;

/*
 * XXX: 説明を入れる。
 */
static volatile uint64_t stat_evictions;

/* Counts hard-cap admission and physical allocation failures independently. */
static volatile uint64_t stat_capacity_failures;
static volatile uint64_t stat_physical_failures;

/*
 * XXX: 説明を入れる。
 */
static volatile uint64_t stat_waits;

/*
 * XXX: 説明を入れる。
 */
static volatile uint64_t stat_writeback_errors;

/*
 * XXX: 説明を入れる。
 */
static unsigned cache_initialized;

/* XXX: これはincludeで解決するかも？. */
struct thread *thread_current(void);

/*
 * XXX: なぜweakなのa説明を入れる。
 */
extern int cache_memory_reserve(enum cache_memory_kind, size_t, int) __attribute__((weak));
extern void cache_memory_commit(enum cache_memory_kind, size_t) __attribute__((weak));
extern void cache_memory_cancel(enum cache_memory_kind, size_t) __attribute__((weak));
extern void cache_memory_release(enum cache_memory_kind, size_t) __attribute__((weak));
extern size_t cache_memory_reclaim(size_t) __attribute__((weak));

/*
 * Forward declaration
 */
static unsigned buf_hash_key(const struct disk *disk, uint64_t block);
static size_t slab_header_size(void);
static struct buf * slab_slot(struct buf_slab *slab, unsigned slot);
static void lru_remove_locked(struct buf *buffer);
static void lru_add_locked(struct buf *buffer);
static void hash_remove_locked(struct buf *buffer);
static struct buf * hash_find_locked(struct disk *disk, uint64_t block);
static void stat_add(volatile uint64_t *counter, uint64_t value);
static int reserve_bytes(size_t size, int metadata);
static void cancel_reservation(size_t size, int metadata);
static void commit_reservation(size_t size, int metadata);
static int alloc_pmem(size_t size, struct kern_pmem *memory, int metadata);
static int slab_grow(void);
static struct buf * alloc_metadata(void);
static void free_metadata(struct buf *buffer);
static void free_buffer(struct buf *buffer);
static int busy_acquire(struct buf *buffer);
static void drop_caller_reference(struct buf *buffer);
static int read_buffer(struct buf *buffer);
static int acquire_line(struct disk *disk, uint64_t block, int read_data, struct buf **result);
static int reference_line(struct disk *disk, uint64_t block, struct buf **result);
static int transfer_run(struct disk *disk, uint64_t block, uint64_t remaining, void *data, int write, uint32_t *transferred, const struct io_context *context);
static void release_run(struct buf **lines, unsigned count, unsigned busy);
static void finish_run_line(struct buf *buffer, uint64_t generation, int write, int error);
static int disk_cache_range(struct disk *disk, struct disk **leaf_out, uint64_t *start_out, uint64_t *end_out);
static int evict_one(struct disk *disk, uint64_t start, uint64_t end, int range, unsigned flags, size_t *freed);
static int writeback_one_reclaimable(void);
static void dirty_link(struct buf *buffer);
static void dirty_clear(struct buf *buffer);
static void dirty_unlink_locked(struct buf *buffer);
static struct buf *dirty_reference(struct disk *disk, uint64_t start, uint64_t end, int reclaim);

/*
 * Initializes the buffer cache with its default byte cap.
 *
 * The cap is the configured size, or a sixteenth of physical memory,
 * clamped to the built-in minimum and maximum.
 */
int
buf_init(
	void)
{
	uint64_t value;
#if CONFIG_BUF_CACHE_KIB == 0
	uint64_t total;
#endif

	/* Initializes only once. */
	if (cache_initialized)
		return 0;

	/* Sets up the locks and the empty hash table. */
	spin_init(&cache_lock, LOCK_RANK_BUFCACHE, "buffer cache");
	spin_init(&dirty_index_lock, LOCK_RANK_DIRTY_INDEX, "dirty buffer index");
	if (mutex_init(&cache_control, LOCK_RANK_BUFCACHE,
	    "buffer cache control") != 0)
		return ENOMEM;
	if (mutex_init(&cache_admission, LOCK_RANK_BUFCACHE,
	    "buffer cache admission") != 0)
		return ENOMEM;
	memset(cache_hash, 0, sizeof(cache_hash));

	/* Sizes the cache. */
#if CONFIG_BUF_CACHE_KIB > 0
	value = (uint64_t)CONFIG_BUF_CACHE_KIB * 1024U;
#else
	total = hal_pmem_get_total_size();
	value = total / 16U;
#endif
	if (value < BUF_MIN_BYTES)
		value = BUF_MIN_BYTES;
	value &= ~(uint64_t)(KERN_PAGE_SIZE - 1U);
	cache_max_bytes = value;

	/* Allocates the first header slab. */
	cache_initialized = 1;
	if (slab_grow() != 0) {
		cache_initialized = 0;
		return ENOMEM;
	}

	/* Reports the initialized cache. */
	return 0;
}

/*
 * Gets the buffer holding a block, reading it in when necessary.
 *
 * The buffer is returned busy and referenced; buf_release() gives it
 * back.
 */
int
buf_get(
	struct disk *disk,
	uint64_t block,
	struct buf **result)
{
	struct disk *leaf;
	uint64_t mapped;
	int error;

	/* Resolves the block to its leaf disk, then acquires the line. */
	error = disk_resolve_range(disk, block, 1, &leaf, &mapped);
	if (error != 0)
		return error;

	/* Reports why the acquisition failed. */
	error = acquire_line(leaf, mapped, 1, result);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Releases a buffer acquired by buf_get().
 *
 * The buffer returns to the LRU when the caller held its last reference.
 */
void
buf_release(
	struct buf *buffer)
{
	unsigned long irq;

	/* Ignores a missing buffer. */
	if (buffer == NULL)
		return;

	/* Clears the busy flag and wakes any waiter. */
	irq = spin_lock_irqsave(&buffer->b_lock);

	buffer->b_busy = 0;
	waitq_wake_all(&buffer->b_waitq);

	spin_unlock_irqrestore(&buffer->b_lock, irq);

	/* Drops the reference, making the buffer evictable when it was the last. */
	irq = spin_lock_irqsave(&cache_lock);

	if (refcount_put_not_last(&buffer->b_refs) == 1)
		lru_add_locked(buffer);

	spin_unlock_irqrestore(&cache_lock, irq);
}

/*
 * Marks a buffer's data modified.
 *
 * The dirty generation lets a writeback that raced a later modification
 * leave the buffer dirty.
 */
void
buf_mark_dirty(
	struct buf *buffer)
{
	unsigned long irq;

	/* Ignores a missing buffer. */
	if (buffer == NULL)
		return;

	/* Advances the generation, skipping zero, and sets the flags. */
	irq = spin_lock_irqsave(&buffer->b_lock);

	if (buffer->b_generation == UINT64_MAX)
		buffer->b_flags |= BUF_GENERATION_EXHAUSTED;

	buffer->b_generation++;
	if (buffer->b_generation == 0)
		buffer->b_generation++;

	buffer->b_dirty_generation = buffer->b_generation;

	/* Publishes the new contents and counts the buffer as dirty once. */
	buffer->b_flags |= BUF_VALID;
	if (!(buffer->b_flags & BUF_DIRTY)) {
		buffer->b_flags |= BUF_DIRTY;
		stat_add(&cache_dirty_bytes, buffer->b_size);
		dirty_link(buffer);
	}

	spin_unlock_irqrestore(&buffer->b_lock, irq);
}

/*
 * Writes a dirty buffer to its disk.
 *
 * A modification during the write leaves the buffer dirty; a failed
 * write keeps it dirty and marks the error.
 */
int
buf_writeback(
	struct buf *buffer)
{
	int error;

	/* Reports the failure. */
	error = buf_writeback_context(buffer, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Executes the synchronous operation with explicit inherited provenance. */
int
buf_writeback_context(
	struct buf *buffer,
	const struct io_context *context)
{
	uint64_t generation;
	struct io_context drain;
	int error;
	unsigned long irq;

	/* Rejects a missing buffer. */
	if (buffer == NULL)
		return EINVAL;

	/* Rejects unsupported provenance before changing buffer completion state. */
	error = io_context_child(&drain, context, IO_CONTEXT_DRAIN);
	if (error != 0)
		return error;

	/* A clean buffer needs nothing; an invalid dirty one cannot be written. */
	irq = spin_lock_irqsave(&buffer->b_lock);

	if (!(buffer->b_flags & BUF_DIRTY)) {
		spin_unlock_irqrestore(&buffer->b_lock, irq);
		return 0;
	}

	if (!(buffer->b_flags & BUF_VALID)) {
		spin_unlock_irqrestore(&buffer->b_lock, irq);
		return EIO;
	}

	/* Writes the data unlocked, remembering which generation it was. */
	generation = buffer->b_dirty_generation;
	buffer->b_io_state = BUF_IO_WRITING;
	buffer->b_io_inflight = 1;

	spin_unlock_irqrestore(&buffer->b_lock, irq);

	stat_add(&stat_write_bios, 1);
	error = disk_write_direct_context(buffer->b_disk,
					  buffer->b_block,
					  buffer->b_block_count,
					  buffer->b_data,
					  &drain);

	/* Records the outcome; only an unmodified buffer becomes clean. */
	irq = spin_lock_irqsave(&buffer->b_lock);

	/* Clears the dirty record on a write nothing raced, and keeps it on a failure. */
	buffer->b_io_state = BUF_IO_IDLE;
	buffer->b_io_inflight = 0;
	buffer->b_error = error;
	if (error == 0 && generation == buffer->b_dirty_generation) {
		buffer->b_flags &= ~(BUF_DIRTY | BUF_ERROR);
		dirty_clear(buffer);
		stat_add(&cache_dirty_bytes,
		    (uint64_t)-(int64_t)buffer->b_size);
	} else if (error != 0) {
		buffer->b_flags |= BUF_ERROR | BUF_DIRTY | BUF_VALID;
		stat_add(&stat_writeback_errors, 1);
	}

	waitq_wake_all(&buffer->b_waitq);

	spin_unlock_irqrestore(&buffer->b_lock, irq);

	/* Reports why the write failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Releases a metadata copy's bounded cache pins and disk lifetime reference.
 */
void
buf_view_release(
	struct buf_view *view)
{
	unsigned index;

	/* Drops references without changing another caller's busy ownership. */
	if (view == NULL)
		return;
	for (index = 0; index < view->count; index++)
		drop_caller_reference(view->lines[index]);
	if (view->disk != NULL)
		disk_release(view->disk);
	memset(view, 0, sizeof(*view));
}

/*
 * Checks the content generations of pinned cache identities without performing I/O.
 */
int
buf_view_matches(
	const struct buf_view *view)
{
	struct buf *buffer;
	unsigned index;
	unsigned long irq;
	int matches;

	/* Rejects an unpublished copy token. */
	if (view == NULL || !view->valid || view->count == 0)
		return 0;

	/* Every matching generation remained unchanged since before the original copy. */
	for (index = 0; index < view->count; index++) {
		buffer = view->lines[index];
		irq = spin_lock_irqsave(&buffer->b_lock);
		matches = !buffer->b_busy && !buffer->b_io_inflight &&
		    buffer->b_generation == view->generations[index] &&
		    (buffer->b_flags & BUF_VALID) != 0 &&
		    (buffer->b_flags & (BUF_INVALID | BUF_ERROR | BUF_DIRTY |
		     BUF_GENERATION_EXHAUSTED)) == 0;
		spin_unlock_irqrestore(&buffer->b_lock, irq);
		if (!matches)
			return 0;
	}

	/* Reports an unchanged cached copy at this observation point. */
	return 1;
}

/*
 * Copies a bounded metadata range and retains a generation-checked common-cache view.
 * Resource pressure falls back to the ordinary cache read with no persistent pins.
 */
int
buf_read_view(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	void *data,
	struct buf_view *view)
{
	struct disk *leaf;
	struct buf *buffer;
	uint64_t mapped;
	uint64_t end;
	unsigned index;
	unsigned long irq;
	int eligible;
	int error;

	/* Clears an old token and validates this range before preparing any pins. */
	if (view == NULL)
		return EINVAL;
	buf_view_release(view);
	error = disk_resolve_range(disk, block, count, &leaf, &mapped);
	if (error != 0)
		return error;
	if (data == NULL || count == 0)
		return EINVAL;
	if (leaf->d_block_size == 0 || count > KERN_IO_BATCH_MAX / leaf->d_block_size) {
		error = buf_read(disk, block, count, data);
		return error;
	}

	view->disk = disk;
	disk_ref(disk);
	end = mapped + count;

	/* Prepares all references without holding any line busy over allocation or reclaim. */
	while (mapped < end && view->count < BUF_VIEW_MAX_LINES) {
		error = reference_line(leaf, mapped, &buffer);
		if (error != 0)
			break;
		view->lines[view->count++] = buffer;
		mapped = buffer->b_block + buffer->b_block_count;
	}

	if (mapped < end) {
		buf_view_release(view);
		error = buf_read(disk, block, count, data);
		return error;
	}

	/* Samples generations before copying so concurrent modification cannot be hidden. */
	eligible = 1;
	for (index = 0; index < view->count; index++) {
		buffer = view->lines[index];
		irq = spin_lock_irqsave(&buffer->b_lock);
		view->generations[index] = buffer->b_generation;
		if (buffer->b_busy || buffer->b_io_inflight)
			eligible = 0;
		spin_unlock_irqrestore(&buffer->b_lock, irq);
	}

	error = buf_read(disk, block, count, data);
	if (error != 0) {
		buf_view_release(view);

		/* Drops optional pins before one ordinary retry under nested-cache pressure. */
		if (error == ENOMEM)
			error = buf_read(disk, block, count, data);
		return error;
	}

	view->valid = eligible;
	if (!buf_view_matches(view))
		buf_view_release(view);

	/* Returns the copied bytes even when no reusable token could be retained. */
	return 0;
}

/*
 * Reads blocks through the cache.
 */
int
buf_read(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	void *data)
{
	struct disk *leaf;
	uint64_t mapped;
	uint64_t end;
	uint8_t *out;
	int error;
	struct buf *buffer;
	uint64_t offset_blocks;
	uint64_t amount_blocks;
	uint32_t run_blocks;

	out = data;

	/* Rejects a missing buffer or an empty range. */
	if (data == NULL || count == 0)
		return EINVAL;

	/* Resolves the range to the leaf disk. */
	error = disk_resolve_range(disk, block, count, &leaf, &mapped);
	if (error != 0)
		return error;

	io_stats_record(IO_BUF_READ, (uint64_t)count * leaf->d_block_size);

	/* Copies out of each cached line in turn. */
	end = mapped + count;
	while (mapped < end) {
		error = transfer_run(leaf, mapped, end - mapped, out, 0, &run_blocks, NULL);
		if (error != 0)
			return error;
		if (run_blocks != 0) {
			mapped += run_blocks;
			out += (size_t)run_blocks * leaf->d_block_size;
			continue;
		}

		error = acquire_line(leaf, mapped, 1, &buffer);
		if (error != 0)
			return error;
		offset_blocks = mapped - buffer->b_block;
		amount_blocks = buffer->b_block_count - offset_blocks;
		if (amount_blocks > end - mapped)
			amount_blocks = end - mapped;
		memcpy(out, (uint8_t *)buffer->b_data +
		    offset_blocks * leaf->d_block_size,
		    (size_t)(amount_blocks * leaf->d_block_size));
		buf_release(buffer);
		out += amount_blocks * leaf->d_block_size;
		mapped += amount_blocks;
	}

	/* Reports the completed read. */
	return 0;
}

/*
 * Writes blocks through the cache, writing each line to the disk.
 *
 * A line that is overwritten in full is not read in first.
 */
int
buf_write(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data)
{
	int error;

	/* Reports the failure. */
	error = buf_write_context(disk, block, count, data, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Executes the synchronous operation with explicit inherited provenance. */
int
buf_write_context(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data,
	const struct io_context *context)
{
	struct disk *leaf;
	uint64_t mapped;
	uint64_t end;
	const uint8_t *in;
	int error;
	struct buf *buffer;
	uint64_t line_bytes;
	uint64_t line_blocks;
	uint64_t line_start;
	uint64_t offset_blocks;
	uint64_t amount_blocks;
	int full;
	uint32_t run_blocks;

	in = data;

	/* Rejects a missing buffer, an empty range, or a read-only disk. */
	if (data == NULL || count == 0)
		return EINVAL;
	if (disk->d_flags & DISK_READ_ONLY)
		return EROFS;

	error = io_context_validate(context);
	if (error != 0)
		return error;

	/* Resolves the range to the leaf disk. */
	error = disk_resolve_range(disk, block, count, &leaf, &mapped);
	if (error != 0)
		return error;

	io_stats_record(IO_BUF_WRITE, (uint64_t)count * leaf->d_block_size);

	/* Copies into each line in turn and writes it back. */
	end = mapped + count;
	while (mapped < end) {
		error = transfer_run(leaf, mapped, end - mapped, (void *)in, 1, &run_blocks, context);
		if (error != 0)
			return error;
		if (run_blocks != 0) {
			mapped += run_blocks;
			in += (size_t)run_blocks * leaf->d_block_size;
			continue;
		}

		if (leaf->d_block_size > KERN_PAGE_SIZE)
			line_bytes = leaf->d_block_size;
		else
			line_bytes = KERN_PAGE_SIZE;
		line_blocks = line_bytes / leaf->d_block_size;
		line_start = mapped - mapped % line_blocks;
		offset_blocks = mapped - line_start;
		amount_blocks = line_blocks - offset_blocks;
		if (amount_blocks > end - mapped)
			amount_blocks = end - mapped;

		/* A whole line inside the disk needs no read before the write. */
		full = 0;
		if (offset_blocks == 0 &&
		    amount_blocks == line_blocks &&
		    line_start + line_blocks <= leaf->d_block_count)
			full = 1;

		error = acquire_line(leaf, mapped, !full, &buffer);
		if (error != 0)
			return error;

		memcpy((uint8_t *)buffer->b_data +
		       offset_blocks * leaf->d_block_size, in,
		       (size_t)(amount_blocks * leaf->d_block_size));

		buf_mark_dirty(buffer);
		error = buf_writeback_context(buffer, context);
		buf_release(buffer);

		if (error != 0)
			return error;

		in += amount_blocks * leaf->d_block_size;
		mapped += amount_blocks;
	}

	/* Reports the completed write. */
	return 0;
}

/*
 * Writes back every dirty buffer of a disk.
 */
int
buf_sync(
	struct disk *disk)
{
	struct disk *leaf;
	uint64_t start;
	uint64_t end;
	int error;
	struct buf *candidate;

	/* Resolves the disk's block range on its leaf. */
	error = disk_cache_range(disk, &leaf, &start, &end);
	if (error != 0)
		return error;

	/* Writes back one dirty buffer at a time until none is left. */
	for (;;) {
		candidate = dirty_reference(leaf, start, end, 0);
		if (candidate == NULL)
			return 0;
		error = busy_acquire(candidate);
		if (error == 0)
			error = buf_writeback(candidate);
		buf_release(candidate);
		if (error != 0)
			return error;
	}
}

/*
 * Frees cache memory by evicting unreferenced buffers.
 *
 * With BUF_RECLAIM_WRITE a dirty buffer is written back so that it can
 * be evicted next.  Reports how many bytes were freed.
 */
size_t
buf_reclaim(
	size_t target_bytes,
	unsigned flags)
{
	size_t total;
	size_t freed;

	total = 0;

	/* Evicts from the LRU tail until enough is freed or nothing is evictable. */
	while (total < target_bytes) {
		freed = 0;
		if (evict_one(NULL, 0, 0, 0, 0, &freed) != 0) {
			if (!(flags & BUF_RECLAIM_WRITE) ||
			    writeback_one_reclaimable() != 0)
				break;
			continue;
		}

		total += freed;
	}

	/* Reports the freed bytes. */
	return total;
}

/*
 * Drops every buffer overlapping a block range.
 *
 * Dirty buffers are written back first unless BUF_INVALIDATE_DISCARD is
 * set; a buffer in use makes the whole call fail with EBUSY.
 */
int
buf_invalidate(
	struct disk *disk,
	uint64_t block,
	uint64_t count,
	unsigned flags)
{
	struct disk *leaf;
	uint64_t start;
	uint64_t end;
	int error;
	size_t freed;

	/* Rejects an empty or oversized range. */
	if (count == 0 || count > UINT32_MAX)
		return EINVAL;

	/* Resolves the range to the leaf disk. */
	error = disk_resolve_range(disk, block, (uint32_t)count, &leaf, &start);
	if (error != 0)
		return error;
	end = start + count;

	/* Writes the dirty data back unless it is to be discarded. */
	if (!(flags & BUF_INVALIDATE_DISCARD)) {
		error = buf_sync(disk);
		if (error != 0)
			return error;
	}

	/* Evicts every overlapping buffer. */
	for (;;) {
		error = evict_one(leaf, start, end, 1, flags, &freed);
		if (error == ENOENT)
			return 0;
		if (error != 0)
			return error;
	}
}

/*
 * Discards a revoked physical medium without ordinary I/O admission.
 *
 * The retirement owner excludes external disk users before calling this.
 * Raw physical geometry belongs to the retained object, never a new medium.
 */
int
buf_discard_media(
	struct disk *disk)
{
	struct buf *buffer;
	unsigned bucket;
	unsigned long irq;
	unsigned long birq;
	unsigned long dirty_irq;
	size_t freed;
	int error;

	/* Only a whole device whose media is gone may be discarded. */
	if (disk == NULL || disk->d_parent != NULL ||
	    !atomic_raw_load_acquire(&disk->d_media_revoked))
		return EINVAL;

	/* Refuse before any dirty data is discarded. The caller keeps users excluded
	 * across this check and eviction; this scan alone is not an admission gate. */
	irq = spin_lock_irqsave(&cache_lock);
	for (bucket = 0; bucket < BUF_HASH_BUCKETS; bucket++) {
		for (buffer = cache_hash[bucket]; buffer != NULL;
		     buffer = buffer->b_hash_next) {
			if (buffer->b_disk != disk)
				continue;
			birq = spin_lock_irqsave(&buffer->b_lock);
			dirty_irq = spin_lock_irqsave(&dirty_index_lock);
			error = refcount_load(&buffer->b_refs) != 1 ||
			    buffer->b_busy || buffer->b_io_inflight ? EBUSY : 0;
			spin_unlock_irqrestore(&dirty_index_lock, dirty_irq);
			spin_unlock_irqrestore(&buffer->b_lock, birq);
			if (error != 0) {
				spin_unlock_irqrestore(&cache_lock, irq);
				return error;
			}
		}
	}
	spin_unlock_irqrestore(&cache_lock, irq);

	/* Discards one buffer at a time until the cache holds none. */
	for (;;) {
		error = evict_one(disk, 0, disk->d_block_count, 1,
		    BUF_INVALIDATE_DISCARD, &freed);
		if (error == ENOENT)
			return 0;
		if (error != 0)
			return error;
	}
}

/*
 * Drops every buffer of a disk.
 */
int
buf_invalidate_disk(
	struct disk *disk,
	unsigned flags)
{
	struct disk *leaf;
	uint64_t start;
	uint64_t end;
	int error;
	size_t freed;

	/* Resolves the disk's block range on its leaf. */
	error = disk_cache_range(disk, &leaf, &start, &end);
	if (error != 0)
		return error;

	/* Writes the dirty data back unless it is to be discarded. */
	if (!(flags & BUF_INVALIDATE_DISCARD)) {
		error = buf_sync(disk);
		if (error != 0)
			return error;
	}

	/* Evicts every buffer of the disk. */
	for (;;) {
		error = evict_one(leaf, start, end, 1, flags, &freed);
		if (error == ENOENT)
			return 0;
		if (error != 0)
			return error;
	}
}

/*
 * Copies the cache statistics.
 */
void
buf_get_stats(
	struct bufcache_stats *stats)
{
	unsigned long irq;

	/* Ignores a missing result. */
	if (stats == NULL)
		return;

	/* Samples the accounting under the lock and the counters atomically. */
	memset(stats, 0, sizeof(*stats));
	irq = spin_lock_irqsave(&cache_lock);

	stats->max_bytes = cache_max_bytes;
	stats->current_bytes = cache_current_bytes;
	stats->data_bytes = cache_data_bytes;
	stats->metadata_bytes = cache_metadata_bytes;

	spin_unlock_irqrestore(&cache_lock, irq);

	stats->dirty_bytes = atomic_u64_load_acquire(&cache_dirty_bytes);
	stats->buffers = atomic_u64_load_acquire(&stat_buffers);
	stats->hits = atomic_u64_load_acquire(&stat_hits);
	stats->misses = atomic_u64_load_acquire(&stat_misses);
	stats->read_bios = atomic_u64_load_acquire(&stat_read_bios);
	stats->write_bios = atomic_u64_load_acquire(&stat_write_bios);
	stats->evictions = atomic_u64_load_acquire(&stat_evictions);
	stats->waits = atomic_u64_load_acquire(&stat_waits);
	stats->writeback_errors =
	    atomic_u64_load_acquire(&stat_writeback_errors);
	stats->capacity_failures = atomic_u64_load_acquire(&stat_capacity_failures);
	stats->physical_failures = atomic_u64_load_acquire(&stat_physical_failures);
}

/*
 * Changes the cache byte cap, shrinking the cache to fit.
 *
 * When the cache cannot be shrunk enough the old cap is restored and
 * EBUSY reported.
 */
int
buf_set_max_bytes(
	uint64_t value)
{
	uint64_t total;
	uint64_t old;
	int result;
	unsigned long irq;
	uint64_t current;

	total = hal_pmem_get_total_size();
	result = 0;

	/* Rejects a cap below the minimum, above half of memory, or unaligned. */
	if (value < BUF_MIN_BYTES ||
	    value > total / 2U ||
	    value > SIZE_MAX ||
	    (value & (KERN_PAGE_SIZE - 1U)) != 0)
		return EINVAL;

	/* Installs the new cap under the control mutex. */
	mutex_lock(&cache_control);
	irq = spin_lock_irqsave(&cache_lock);

	old = cache_max_bytes;
	cache_max_bytes = value;

	spin_unlock_irqrestore(&cache_lock, irq);

	/* Reclaims until the usage fits, restoring the old cap on failure. */
	for (;;) {
		irq = spin_lock_irqsave(&cache_lock);
		current = cache_current_bytes + cache_reserved_bytes;
		spin_unlock_irqrestore(&cache_lock, irq);
		if (current <= value)
			break;
		if (buf_reclaim((size_t)(current - value), BUF_RECLAIM_WRITE) == 0) {
			irq = spin_lock_irqsave(&cache_lock);
			cache_max_bytes = old;
			spin_unlock_irqrestore(&cache_lock, irq);
			result = EBUSY;
			break;
		}
	}

	mutex_unlock(&cache_control);

	/* Reports the resize result. */
	return result;
}

/*
 * Discards every evictable buffer.
 */
void
buf_reset(
	void)
{
	size_t freed;

	/* Ignores a cache that was never initialized. */
	if (!cache_initialized)
		return;

	/* Evicts, discarding dirty data, until nothing is evictable. */
	for (;;) {
		if (evict_one(NULL, 0, 0, 0, BUF_INVALIDATE_DISCARD,
		    &freed) != 0)
			break;
	}
}

/* Hashes a disk and block to a bucket. */
static unsigned
buf_hash_key(
	const struct disk *disk,
	uint64_t block)
{
	uintptr_t value;

	value = (uintptr_t)disk;
	return (unsigned)((value >> 4) ^ block ^ (block >> 32)) &
	    (BUF_HASH_BUCKETS - 1U);
}

/* Reports the slab header size rounded to pointer alignment. */
static size_t
slab_header_size(
	void)
{
	size_t alignment;

	alignment = sizeof(void *);
	return (sizeof(struct buf_slab) + alignment - 1U) & ~(alignment - 1U);
}

/* Locates a buffer header slot inside a slab. */
static struct buf *
slab_slot(
	struct buf_slab *slab,
	unsigned slot)
{
	return (struct buf *)((uint8_t *)slab + slab_header_size()) + slot;
}

/* Unlinks a buffer from the LRU list. */
static void
lru_remove_locked(
	struct buf *buffer)
{
	/* Ignores a buffer that is not on the list. */
	if (!buffer->b_on_lru)
		return;

	/* Splices the buffer out, fixing the ends. */
	if (buffer->b_lru_prev != NULL)
		buffer->b_lru_prev->b_lru_next = buffer->b_lru_next;
	else
		lru_head = buffer->b_lru_next;
	if (buffer->b_lru_next != NULL)
		buffer->b_lru_next->b_lru_prev = buffer->b_lru_prev;
	else
		lru_tail = buffer->b_lru_prev;
	buffer->b_lru_prev = NULL;
	buffer->b_lru_next = NULL;
	buffer->b_on_lru = 0;
}

/* Puts a buffer at the recently-used head of the LRU list. */
static void
lru_add_locked(
	struct buf *buffer)
{
	/* Moves a listed buffer rather than linking it twice. */
	if (buffer->b_on_lru)
		lru_remove_locked(buffer);

	/* Links at the head. */
	buffer->b_lru_prev = NULL;
	buffer->b_lru_next = lru_head;
	if (lru_head != NULL)
		lru_head->b_lru_prev = buffer;
	else
		lru_tail = buffer;
	lru_head = buffer;
	buffer->b_on_lru = 1;
}

/* Unlinks a buffer from its hash bucket. */
static void
hash_remove_locked(
	struct buf *buffer)
{
	struct buf **link;
	unsigned bucket;

	bucket = buf_hash_key(buffer->b_disk, buffer->b_block);
	for (link = &cache_hash[bucket]; *link != NULL;
	     link = &(*link)->b_hash_next) {
		if (*link == buffer) {
			*link = buffer->b_hash_next;
			buffer->b_hash_next = NULL;
			return;
		}
	}
}

/* Finds the live buffer for a disk block in the hash table. */
static struct buf *
hash_find_locked(
	struct disk *disk,
	uint64_t block)
{
	struct buf *buffer;
	unsigned bucket;

	/* Searches the bucket, skipping buffers being evicted. */
	bucket = buf_hash_key(disk, block);
	for (buffer = cache_hash[bucket]; buffer != NULL;
	     buffer = buffer->b_hash_next) {
		if (buffer->b_disk == disk &&
		    buffer->b_block == block &&
		    !(buffer->b_flags & BUF_INVALID))
			return buffer;
	}

	return NULL;
}

/* Adds to a statistics counter atomically. */
static void
stat_add(
	volatile uint64_t *counter,
	uint64_t value)
{
	(void)atomic_u64_fetch_add_relaxed(counter, value);
}

/* Reserves bytes under the cap, reclaiming once when they do not fit. */
static int
reserve_bytes(
	size_t size,
	int metadata)
{
	enum cache_memory_kind kind;
	int error;
	unsigned attempt;
	unsigned long irq;

	/* Reserves shared ownership before the component cap and physical allocation. */
	kind = metadata ? CACHE_MEMORY_BUF_META : CACHE_MEMORY_BUF_DATA;
	if (cache_memory_reserve != NULL) {
		error = cache_memory_reserve(kind, size, 1);
		if (error != 0 && cache_memory_reclaim != NULL &&
		    cache_memory_reclaim(size) != 0)
			error = cache_memory_reserve(kind, size, 1);
		if (error != 0)
			return error;
	}

	/* Reserves reclaimed space before another allocator can claim it. */
	mutex_lock(&cache_admission);

	/* Tries before and after one clean reclaim pass; never waits on busy buffers. */
	for (attempt = 0; attempt < 2U; attempt++) {
		irq = spin_lock_irqsave(&cache_lock);
		if (cache_current_bytes <= cache_max_bytes &&
		    cache_reserved_bytes <= cache_max_bytes - cache_current_bytes &&
		    (uint64_t)size <= cache_max_bytes - cache_current_bytes -
		    cache_reserved_bytes) {
			cache_reserved_bytes += size;
			spin_unlock_irqrestore(&cache_lock, irq);
			mutex_unlock(&cache_admission);
			return 0;
		}

		spin_unlock_irqrestore(&cache_lock, irq);
		if (buf_reclaim(size, 0) == 0)
			break;
	}

	mutex_unlock(&cache_admission);

	/* Returns shared credit when the component cap cannot admit ownership. */
	if (cache_memory_cancel != NULL)
		cache_memory_cancel(kind, size);

	/* Reports that the bytes do not fit. */
	stat_add(&stat_capacity_failures, 1);
	return ENOMEM;
}

/* Gives back a reservation that was not committed. */
static void
cancel_reservation(
	size_t size,
	int metadata)
{
	unsigned long irq;

	/* Takes the bytes off the cache's own reservation. */
	irq = spin_lock_irqsave(&cache_lock);

	if (cache_reserved_bytes >= size)
		cache_reserved_bytes -= size;

	spin_unlock_irqrestore(&cache_lock, irq);

	/* Tells the shared budget that the reservation is gone. */
	if (cache_memory_cancel != NULL)
		cache_memory_cancel(metadata ? CACHE_MEMORY_BUF_META :
		    CACHE_MEMORY_BUF_DATA, size);
}

/* Converts a reservation into data or metadata usage. */
static void
commit_reservation(
	size_t size,
	int metadata)
{
	unsigned long irq;

	/* Moves the bytes from reserved to resident in the cache. */
	irq = spin_lock_irqsave(&cache_lock);

	cache_reserved_bytes -= size;
	cache_current_bytes += size;
	if (metadata)
		cache_metadata_bytes += size;
	else
		cache_data_bytes += size;

	spin_unlock_irqrestore(&cache_lock, irq);

	/* Tells the shared budget that the reservation became memory. */
	if (cache_memory_commit != NULL)
		cache_memory_commit(metadata ? CACHE_MEMORY_BUF_META :
		    CACHE_MEMORY_BUF_DATA, size);
}

/* Allocates page-aligned physical memory reserved against the cap. */
static int
alloc_pmem(
	size_t size,
	struct kern_pmem *memory,
	int metadata)
{
	size_t reserved;
	int error;

	/* Reserves the page-rounded size first. */
	if (size > SIZE_MAX - (KERN_PAGE_SIZE - 1U))
		return ENOMEM;
	reserved = (size + KERN_PAGE_SIZE - 1U) &
	    ~(size_t)(KERN_PAGE_SIZE - 1U);
	if (reserve_bytes(reserved, metadata) != 0)
		return ENOMEM;
	memory->size = reserved;
	error = hal_pmem_alloc(reserved, KERN_PAGE_SIZE, &memory->paddr);
	if (error != HAL_OK) {
		cancel_reservation(reserved, metadata);
		if (error == HAL_ERR_NOMEM) {
			stat_add(&stat_physical_failures, 1);
			return ENOMEM;
		}
		return EIO;
	}

	/*
	 * HAL reports the actual page-rounded allocation.  Keep the hard cap
	 * exact even for a host fixture or architecture with different
	 * rounding.
	 */
	if (memory->size > reserved) {
		if (reserve_bytes(memory->size - reserved, metadata) != 0) {
			if (hal_pmem_free(&memory->paddr, memory->size) != HAL_OK)
				HAL_FATAL("buffer allocation rollback failed");
			cancel_reservation(reserved, metadata);
			return ENOMEM;
		}
	} else if (memory->size < reserved) {
		cancel_reservation(reserved - memory->size, metadata);
	}

	/* Reports the reserved allocation. */
	return 0;
}

/* Adds one slab of buffer headers. */
static int
slab_grow(
	void)
{
	struct kern_pmem memory;
	struct buf_slab *slab;
	size_t header;
	size_t charged;
	unsigned capacity;
	int error;
	unsigned long irq;

	header = slab_header_size();

	/* Rejects a failed allocation or a slab the free mask cannot cover. */
	error = alloc_pmem(BUF_SLAB_BYTES, &memory, 1);
	if (error != 0)
		return error;
	charged = memory.size;
	capacity = (unsigned)((BUF_SLAB_BYTES - header) / sizeof(struct buf));
	if (capacity == 0 || capacity > 64U) {
		if (hal_pmem_free(&memory.paddr, memory.size) != HAL_OK)
			HAL_FATAL("buffer slab rollback failed");
		cancel_reservation(charged, 1);
		return EOVERFLOW;
	}

	/* Initializes the slab with every slot free. */
	memset(hal_pmem_to_kernel(memory.paddr), 0, BUF_SLAB_BYTES);
	slab = hal_pmem_to_kernel(memory.paddr);
	slab->memory = memory;
	slab->capacity = capacity;
	if (capacity == 64U)
		slab->free_mask = UINT64_MAX;
	else
		slab->free_mask = ((uint64_t)1 << capacity) - 1U;
	commit_reservation(memory.size, 1);

	/* Publishes the slab. */
	irq = spin_lock_irqsave(&cache_lock);

	slab->next = slabs;
	slabs = slab;

	spin_unlock_irqrestore(&cache_lock, irq);

	/* Reports the added slab. */
	return 0;
}

/* Takes a zeroed buffer header from the slabs, growing them when full. */
static struct buf *
alloc_metadata(
	void)
{
	struct buf_slab *slab;
	unsigned long irq;
	unsigned slot;

	/* Retries after growing until a slot is found or growth fails. */
	for (;;) {
		irq = spin_lock_irqsave(&cache_lock);
		for (slab = slabs; slab != NULL; slab = slab->next) {
			if (slab->free_mask != 0) {
				for (slot = 0; slot < slab->capacity; slot++) {
					if (slab->free_mask & ((uint64_t)1 << slot))
						break;
				}

				slab->free_mask &= ~((uint64_t)1 << slot);
				slab->used++;
				spin_unlock_irqrestore(&cache_lock, irq);
				memset(slab_slot(slab, slot), 0, sizeof(struct buf));
				slab_slot(slab, slot)->b_slab = slab;
				slab_slot(slab, slot)->b_slab_slot = slot;
				return slab_slot(slab, slot);
			}
		}

		spin_unlock_irqrestore(&cache_lock, irq);
		if (slab_grow() != 0)
			return NULL;
	}
}

/* Returns a buffer header to its slab, freeing an emptied non-head slab. */
static void
free_metadata(
	struct buf *buffer)
{
	struct buf_slab *slab;
	unsigned slot;
	struct kern_pmem release;
	int free_slab;
	size_t released_bytes;
	unsigned long irq;
	struct buf_slab **link;

	slab = buffer->b_slab;
	slot = buffer->b_slab_slot;
	free_slab = 0;

	/* Ignores a header that is not in a slab. */
	if (slab == NULL || slot >= slab->capacity)
		return;

	/* Frees the slot, and the slab when it emptied and is not the first. */
	irq = spin_lock_irqsave(&cache_lock);

	slab->free_mask |= (uint64_t)1 << slot;
	if (slab->used != 0)
		slab->used--;
	if (slab->used == 0 && slabs != slab) {
		for (link = &slabs; *link != NULL; link = &(*link)->next) {
			if (*link == slab) {
				*link = slab->next;
				break;
			}
		}

		release = slab->memory;
		cache_current_bytes -= release.size;
		cache_metadata_bytes -= release.size;
		free_slab = 1;
	}

	spin_unlock_irqrestore(&cache_lock, irq);

	/* Releases the slab memory unlocked. */
	if (free_slab) {
		released_bytes = release.size;
		if (hal_pmem_free(&release.paddr, release.size) != HAL_OK)
			HAL_FATAL("buffer metadata retirement failed");
		if (cache_memory_release != NULL)
			cache_memory_release(CACHE_MEMORY_BUF_META, released_bytes);
	}
}

/* Frees a buffer's data, disk reference, and header. */
static void
free_buffer(
	struct buf *buffer)
{
	struct kern_pmem memory;
	size_t size;
	struct disk *disk;
	unsigned long irq;

	memory = buffer->b_memory;
	size = memory.size;
	disk = buffer->b_disk;

	/* Frees the data and un-accounts it. */
	if (size != 0) {
		if (hal_pmem_free(&memory.paddr, memory.size) != HAL_OK)
			HAL_FATAL("buffer data retirement failed");
		if (cache_memory_release != NULL)
			cache_memory_release(CACHE_MEMORY_BUF_DATA, size);
		irq = spin_lock_irqsave(&cache_lock);
		cache_current_bytes -= size;
		cache_data_bytes -= size;
		spin_unlock_irqrestore(&cache_lock, irq);
	}

	/* Drops the disk pin and the header. */
	if (disk != NULL)
		disk_buffer_release(disk);
	free_metadata(buffer);
}

/* Marks a buffer busy, waiting for the current holder to release it. */
static int
busy_acquire(
	struct buf *buffer)
{
	unsigned long irq;
	uint64_t sequence;
	int error;

	/* Retries until the busy flag is taken. */
	for (;;) {
		irq = spin_lock_irqsave(&buffer->b_lock);
		if (!buffer->b_busy) {
			buffer->b_busy = 1;
			spin_unlock_irqrestore(&buffer->b_lock, irq);
			return 0;
		}

		stat_add(&stat_waits, 1);

		/* A thread sleeps for the release; early boot spins. */
		if (thread_current() != NULL) {
			sequence = waitq_sequence(&buffer->b_waitq);
			error = waitq_sleep(&buffer->b_waitq, &buffer->b_lock,
			    sequence, 0, 0);
			spin_unlock_irqrestore(&buffer->b_lock, irq);
			if (error != 0 && error != EAGAIN)
				return error;
		} else {
			spin_unlock_irqrestore(&buffer->b_lock, irq);
			hal_compiler_barrier();
		}
	}
}

/* Drops a caller reference without touching the busy flag. */
static void
drop_caller_reference(
	struct buf *buffer)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&cache_lock);

	if (refcount_put_not_last(&buffer->b_refs) == 1)
		lru_add_locked(buffer);

	spin_unlock_irqrestore(&cache_lock, irq);
}

/* Reads a buffer's data from the disk unless it is already valid. */
static int
read_buffer(
	struct buf *buffer)
{
	int error;
	unsigned long irq;

	/* A valid buffer needs no read. */
	irq = spin_lock_irqsave(&buffer->b_lock);

	if (buffer->b_flags & BUF_VALID) {
		spin_unlock_irqrestore(&buffer->b_lock, irq);
		return 0;
	}

	/* Reads unlocked with the I/O state published. */
	buffer->b_io_state = BUF_IO_READING;
	buffer->b_io_inflight = 1;

	spin_unlock_irqrestore(&buffer->b_lock, irq);

	stat_add(&stat_read_bios, 1);
	error = disk_read_direct(buffer->b_disk, buffer->b_block,
	    buffer->b_block_count, buffer->b_data);

	/* Records the outcome and wakes the waiters. */
	irq = spin_lock_irqsave(&buffer->b_lock);

	buffer->b_io_state = BUF_IO_IDLE;
	buffer->b_io_inflight = 0;
	buffer->b_error = error;
	if (error == 0) {
		buffer->b_flags |= BUF_VALID;
		buffer->b_flags &= ~BUF_ERROR;
	} else {
		buffer->b_flags &= ~BUF_VALID;
		buffer->b_flags |= BUF_ERROR;
	}

	waitq_wake_all(&buffer->b_waitq);

	spin_unlock_irqrestore(&buffer->b_lock, irq);

	/* Reports why the read failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Acquires one referenced line, with no other line ownership while waiting. */
static int
acquire_line(
	struct disk *disk,
	uint64_t block,
	int read_data,
	struct buf **result)
{
	struct buf *buffer;
	int error;

	/* Prepares the reference before taking busy ownership. */
	error = reference_line(disk, block, &buffer);
	if (error != 0)
		return error;
	error = busy_acquire(buffer);
	if (error != 0) {
		drop_caller_reference(buffer);
		return error;
	}

	/* Fills a missing line only when the caller needs its previous contents. */
	if (read_data) {
		error = read_buffer(buffer);
		if (error != 0) {
			buf_release(buffer);
			return error;
		}
	}

	*result = buffer;

	/* Returns the single busy line. */
	return 0;
}

/* Pins a line without taking busy ownership or performing its data I/O. */
static int
reference_line(
	struct disk *disk,
	uint64_t block,
	struct buf **result)
{
	struct buf *buffer;
	struct buf *candidate;
	uint64_t line_bytes;
	uint64_t line_blocks;
	uint64_t line_block;
	uint64_t remaining;
	int error;
	unsigned bucket;
	unsigned long irq;
	struct kern_pmem memory;

	candidate = NULL;

	/* Rejects a missing disk or result, or a disk without a block size. */
	if (disk == NULL || result == NULL || disk->d_block_size == 0)
		return EINVAL;

	/* Lines are one page or one block; the block size must divide evenly. */
	if (disk->d_block_size > KERN_PAGE_SIZE)
		line_bytes = disk->d_block_size;
	else
		line_bytes = KERN_PAGE_SIZE;
	if ((disk->d_block_size & (disk->d_block_size - 1U)) != 0 ||
	    line_bytes % disk->d_block_size != 0)
		return EOPNOTSUPP;
	line_blocks = line_bytes / disk->d_block_size;
	line_block = block - block % line_blocks;
	remaining = disk->d_block_count - line_block;
	if (remaining < line_blocks)
		line_blocks = remaining;

	/* Looks the line up, inserting a new one when it is missing. */
	for (;;) {
		irq = spin_lock_irqsave(&cache_lock);
		buffer = hash_find_locked(disk, line_block);
		if (buffer != NULL) {
			/* A hit: references it and discards any prepared candidate. */
			refcount_get(&buffer->b_refs);
			lru_remove_locked(buffer);
			spin_unlock_irqrestore(&cache_lock, irq);
			stat_add(&stat_hits, 1);
			if (candidate != NULL)
				free_buffer(candidate);
			*result = buffer;
			return 0;
		}

		spin_unlock_irqrestore(&cache_lock, irq);

		/* A miss: prepares a candidate line unlocked. */
		if (candidate == NULL) {
			candidate = alloc_metadata();
			if (candidate == NULL)
				return ENOMEM;
			error = alloc_pmem((size_t)(line_blocks * disk->d_block_size),
			    &memory, 0);
			if (error != 0) {
				free_metadata(candidate);
				return error;
			}

			commit_reservation(memory.size, 0);
			candidate->b_block = line_block;
			candidate->b_block_count = (uint32_t)line_blocks;
			candidate->b_size = memory.size;
			candidate->b_memory = memory;
			candidate->b_data = hal_pmem_to_kernel(memory.paddr);
			error = disk_buffer_acquire(disk);
			if (error != 0) {
				free_buffer(candidate);
				return error;
			}

			candidate->b_disk = disk;
			refcount_init(&candidate->b_refs, 2);
			spin_init(&candidate->b_lock, LOCK_RANK_BUF, "buffer");
			waitq_init(&candidate->b_waitq, "buffer state");
			candidate->b_busy = 0;
		}

		/* Inserts the candidate unless another thread won the race. */
		irq = spin_lock_irqsave(&cache_lock);
		if (hash_find_locked(disk, line_block) != NULL) {
			spin_unlock_irqrestore(&cache_lock, irq);
			continue;
		}

		bucket = buf_hash_key(disk, line_block);
		candidate->b_hash_next = cache_hash[bucket];
		cache_hash[bucket] = candidate;
		spin_unlock_irqrestore(&cache_lock, irq);
		stat_add(&stat_misses, 1);
		stat_add(&stat_buffers, 1);
		*result = candidate;
		return 0;
	}
}

/* Drops busy ownership before releasing every prepared reference. */
static void
release_run(
	struct buf **lines,
	unsigned count,
	unsigned busy)
{
	unsigned index;

	/* Releases acquired lines and then the references without ownership. */
	for (index = 0; index < count; index++) {
		if (index < busy)
			buf_release(lines[index]);
		else
			drop_caller_reference(lines[index]);
	}
}

/* Publishes one line's confirmed completion or retains its retryable contents. */
static void
finish_run_line(
	struct buf *buffer,
	uint64_t generation,
	int write,
	int error)
{
	unsigned long irq;

	/* Changes completion state while preserving a newer dirty generation. */
	irq = spin_lock_irqsave(&buffer->b_lock);

	buffer->b_io_state = BUF_IO_IDLE;
	buffer->b_io_inflight = 0;
	buffer->b_error = error;
	if (write) {
		if (error == 0 && generation == buffer->b_dirty_generation) {
			buffer->b_flags &= ~(BUF_DIRTY | BUF_ERROR);
			dirty_clear(buffer);
			stat_add(&cache_dirty_bytes, (uint64_t)-(int64_t)buffer->b_size);
		} else if (error != 0) {
			buffer->b_flags |= BUF_DIRTY | BUF_VALID | BUF_ERROR;
			stat_add(&stat_writeback_errors, 1);
		}
	} else if (error == 0) {
		buffer->b_flags |= BUF_VALID;
		buffer->b_flags &= ~BUF_ERROR;
	} else {
		buffer->b_flags &= ~BUF_VALID;
		buffer->b_flags |= BUF_ERROR;
	}

	waitq_wake_all(&buffer->b_waitq);

	spin_unlock_irqrestore(&buffer->b_lock, irq);
}

/* Transfers full adjacent lines using caller storage, without a staging pool. */
static int
transfer_run(
	struct disk *disk,
	uint64_t block,
	uint64_t remaining,
	void *data,
	int write,
	uint32_t *transferred,
	const struct io_context *context)
{
	struct buf *lines[BUF_RUN_LINES];
	uint64_t generations[BUF_RUN_LINES];
	uint64_t line_bytes;
	uint32_t line_blocks;
	uint32_t completed;
	unsigned count;
	unsigned index;
	unsigned acquired;
	unsigned long irq;
	int error;
	int line_error;
	uint8_t *bytes;

	/* Leaves partial lines and small requests to the single-line path. */
	*transferred = 0;
	line_bytes = disk->d_block_size > KERN_PAGE_SIZE ?
	    disk->d_block_size : KERN_PAGE_SIZE;
	if (disk->d_block_size == 0 ||
	    (disk->d_block_size & (disk->d_block_size - 1U)) != 0 ||
	    line_bytes > KERN_IO_BATCH_MAX / 2U) {
		io_stats_record(IO_BUF_SINGLE_GEOMETRY, 0);
		return 0;
	}

	line_blocks = (uint32_t)(line_bytes / disk->d_block_size);
	if (block % line_blocks != 0 || remaining / line_blocks < 2U) {
		io_stats_record(IO_BUF_SINGLE_GEOMETRY, 0);
		return 0;
	}

	count = (unsigned)(remaining / line_blocks > KERN_IO_BATCH_MAX / line_bytes ?
	    KERN_IO_BATCH_MAX / line_bytes : remaining / line_blocks);

	/* Pins and allocates all lines before acquiring any busy ownership. */
	for (index = 0; index < count; index++) {
		error = reference_line(disk, block + (uint64_t)index * line_blocks,
		    &lines[index]);
		if (error != 0) {
			release_run(lines, index, 0);
			io_stats_record(IO_BUF_SINGLE_MEMORY, 0);
			return 0;
		}

		/* Avoids preparing cold successors when this read already reaches a hit. */
		if (!write) {
			irq = spin_lock_irqsave(&lines[index]->b_lock);
			line_error = (lines[index]->b_flags & BUF_VALID) != 0;
			spin_unlock_irqrestore(&lines[index]->b_lock, irq);
			if (line_error) {
				drop_caller_reference(lines[index]);
				count = index;
				break;
			}
		}
	}

	/* Never waits for another line while owning one; a conflict shortens to one. */
	acquired = 0;
	for (index = 0; index < count; index++) {
		irq = spin_lock_irqsave(&lines[index]->b_lock);
		if (lines[index]->b_busy || lines[index]->b_io_inflight) {
			spin_unlock_irqrestore(&lines[index]->b_lock, irq);
			release_run(lines, count, acquired);
			io_stats_record(IO_BUF_SINGLE_BUSY, 0);
			return 0;
		}

		lines[index]->b_busy = 1;
		acquired++;

		/* Ends a cold read run before its first cache hit. */
		if (!write && (lines[index]->b_flags & BUF_VALID)) {
			spin_unlock_irqrestore(&lines[index]->b_lock, irq);
			release_run(lines + index, count - index, 1);
			count = index;
			acquired = index;
			break;
		}

		spin_unlock_irqrestore(&lines[index]->b_lock, irq);
	}

	if (count < 2U) {
		release_run(lines, count, acquired);
		io_stats_record(IO_BUF_SINGLE_HIT, 0);
		return 0;
	}

	/* Copies accepted writes into their durable retry owner before submitting. */
	bytes = data;
	for (index = 0; index < count; index++) {
		if (write) {
			memcpy(lines[index]->b_data, bytes + index * line_bytes, (size_t)line_bytes);
			buf_mark_dirty(lines[index]);
		}

		irq = spin_lock_irqsave(&lines[index]->b_lock);
		generations[index] = lines[index]->b_dirty_generation;
		lines[index]->b_io_state = write ? BUF_IO_WRITING : BUF_IO_READING;
		lines[index]->b_io_inflight = 1;
		spin_unlock_irqrestore(&lines[index]->b_lock, irq);
	}

	/* Carries a contiguous caller buffer to the device's splitting boundary. */
	stat_add(write ? &stat_write_bios : &stat_read_bios, 1);
	io_stats_record(write ? IO_BUF_RUN_WRITE : IO_BUF_RUN_READ, count * line_bytes);
	error = disk_transfer_progress_context(disk, write ? BIO_WRITE : BIO_READ, block,
	    count * line_blocks, data, &completed, context);

	/* Publishes only complete lines in the confirmed prefix; uncertainty stays dirty. */
	for (index = 0; index < count; index++) {
		line_error = error;
		if ((index + 1U) * line_blocks <= completed) {
			line_error = 0;
			if (!write) {
				memcpy(lines[index]->b_data, bytes + index * line_bytes,
				    (size_t)line_bytes);
			}
		} else if (line_error == 0) {
			line_error = EIO;
		}

		finish_run_line(lines[index], generations[index], write, line_error);
	}

	release_run(lines, count, count);
	if (error != 0)
		return error;
	*transferred = count * line_blocks;

	/* Reports the complete run. */
	return 0;
}

/* Resolves the whole block range of a disk on its leaf. */
static int
disk_cache_range(
	struct disk *disk,
	struct disk **leaf_out,
	uint64_t *start_out,
	uint64_t *end_out)
{
	struct disk *leaf;
	uint64_t start;
	int error;

	/* Rejects a missing or empty disk. */
	if (disk == NULL || disk->d_block_count == 0)
		return EINVAL;

	/* Maps the first block, then extends by the disk's size. */
	error = disk_resolve_range(disk, 0, 1, &leaf, &start);
	if (error != 0)
		return error;
	if (disk->d_block_count > UINT64_MAX - start)
		return EOVERFLOW;
	*leaf_out = leaf;
	*start_out = start;
	*end_out = start + disk->d_block_count;

	/* Reports the resolved range. */
	return 0;
}

/* Evicts the least recently used evictable buffer, optionally within a range. */
static int
evict_one(
	struct disk *disk,
	uint64_t start,
	uint64_t end,
	int range,
	unsigned flags,
	size_t *freed)
{
	struct buf *candidate;
	unsigned long irq;
	struct buf *buffer;
	unsigned long birq;
	unsigned long dirty_irq;
	unsigned bucket;

	candidate = NULL;
	irq = spin_lock_irqsave(&cache_lock);

	/* Walks the LRU from the tail for an unreferenced, idle buffer. */
	for (buffer = lru_tail; buffer != NULL;
	     buffer = buffer->b_lru_prev) {
		if (range && (buffer->b_disk != disk ||
		    buffer->b_block >= end ||
		    buffer->b_block + buffer->b_block_count <= start))
			continue;
		if (refcount_load(&buffer->b_refs) != 1)
			continue;
		birq = spin_lock_irqsave(&buffer->b_lock);
		dirty_irq = spin_lock_irqsave(&dirty_index_lock);
		if (refcount_load(&buffer->b_refs) != 1 || buffer->b_busy ||
		    buffer->b_io_inflight ||
		    ((buffer->b_flags & BUF_DIRTY) &&
		     !(flags & BUF_INVALIDATE_DISCARD))) {
			spin_unlock_irqrestore(&dirty_index_lock, dirty_irq);
			spin_unlock_irqrestore(&buffer->b_lock, birq);
			continue;
		}

		dirty_unlink_locked(buffer);
		buffer->b_flags |= BUF_INVALID;
		spin_unlock_irqrestore(&dirty_index_lock, dirty_irq);
		spin_unlock_irqrestore(&buffer->b_lock, birq);
		lru_remove_locked(buffer);
		hash_remove_locked(buffer);
		(void)refcount_put(&buffer->b_refs);
		candidate = buffer;
		break;
	}

	/* A ranged eviction that found nothing fails when a buffer is in use. */
	if (candidate == NULL && range) {
		for (bucket = 0; bucket < BUF_HASH_BUCKETS; bucket++) {
			for (buffer = cache_hash[bucket]; buffer != NULL;
			     buffer = buffer->b_hash_next) {
				if (buffer->b_disk == disk &&
				    buffer->b_block < end &&
				    buffer->b_block + buffer->b_block_count > start) {
					spin_unlock_irqrestore(&cache_lock, irq);
					return EBUSY;
				}
			}
		}
	}

	spin_unlock_irqrestore(&cache_lock, irq);

	if (candidate == NULL)
		return ENOENT;

	/* Frees the candidate and updates the statistics. */
	if (candidate->b_flags & BUF_DIRTY)
		stat_add(&cache_dirty_bytes,
		    (uint64_t)-(int64_t)candidate->b_size);
	*freed = candidate->b_memory.size;
	stat_add(&stat_buffers, UINT64_MAX);
	stat_add(&stat_evictions, 1);
	free_buffer(candidate);

	/* Reports the eviction. */
	return 0;
}

/* Writes back the least recently used dirty buffer that reclaim could then evict. */
static int
writeback_one_reclaimable(
	void)
{
	struct buf *candidate;
	int error;

	/* Pins an indexed dirty candidate before dropping the index guard. */
	candidate = dirty_reference(NULL, 0, 0, 1);
	if (candidate == NULL)
		return ENOENT;

	/* Writes it back and drops the reference. */
	error = busy_acquire(candidate);
	if (error == 0)
		error = buf_writeback(candidate);
	buf_release(candidate);

	/* Reports why the writeback failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Publishes a first dirty transition while the caller holds the buffer lock. */
static void
dirty_link(
	struct buf *buffer)
{
	unsigned long irq;

	/* Inserts into the global age list and the owning device's list in O(1). */
	irq = spin_lock_irqsave(&dirty_index_lock);

	if (buffer->b_dirty_linked)
		HAL_FATAL("dirty buffer linked twice");
	buffer->b_dirty_previous = dirty_tail;
	buffer->b_dirty_next = NULL;
	if (dirty_tail != NULL)
		dirty_tail->b_dirty_next = buffer;
	else
		dirty_head = buffer;
	dirty_tail = buffer;
	buffer->b_device_dirty_previous = NULL;
	buffer->b_device_dirty_next = buffer->b_disk->d_dirty_buffers;
	if (buffer->b_device_dirty_next != NULL)
		buffer->b_device_dirty_next->b_device_dirty_previous = buffer;
	buffer->b_disk->d_dirty_buffers = buffer;
	buffer->b_dirty_linked = 1;

	spin_unlock_irqrestore(&dirty_index_lock, irq);
}

/* Removes a resolved dirty transition while the caller holds the buffer lock. */
static void
dirty_clear(
	struct buf *buffer)
{
	unsigned long irq;

	/* Serializes clean publication against dirty-candidate reference acquisition. */
	irq = spin_lock_irqsave(&dirty_index_lock);

	dirty_unlink_locked(buffer);

	spin_unlock_irqrestore(&dirty_index_lock, irq);
}

/* Detaches both memberships with the index guard already held. */
static void
dirty_unlink_locked(
	struct buf *buffer)
{
	/* Clean or never-published buffers have no membership to remove. */
	if (!buffer->b_dirty_linked)
		return;

	/* Repairs the global age list before clearing the descriptor links. */
	if (buffer->b_dirty_previous != NULL)
		buffer->b_dirty_previous->b_dirty_next = buffer->b_dirty_next;
	else
		dirty_head = buffer->b_dirty_next;
	if (buffer->b_dirty_next != NULL)
		buffer->b_dirty_next->b_dirty_previous = buffer->b_dirty_previous;
	else
		dirty_tail = buffer->b_dirty_previous;

	/* Repairs the device index without scanning its other dirty buffers. */
	if (buffer->b_device_dirty_previous != NULL)
		buffer->b_device_dirty_previous->b_device_dirty_next = buffer->b_device_dirty_next;
	else
		buffer->b_disk->d_dirty_buffers = buffer->b_device_dirty_next;
	if (buffer->b_device_dirty_next != NULL)
		buffer->b_device_dirty_next->b_device_dirty_previous = buffer->b_device_dirty_previous;
	buffer->b_dirty_previous = NULL;
	buffer->b_dirty_next = NULL;
	buffer->b_device_dirty_previous = NULL;
	buffer->b_device_dirty_next = NULL;
	buffer->b_dirty_linked = 0;
}

/* Pins one dirty member before exposing it outside the index guard. */
static struct buf *
dirty_reference(
	struct disk *disk,
	uint64_t start,
	uint64_t end,
	int reclaim)
{
	struct buf *buffer;
	unsigned long irq;

	/* Scans only dirty membership, never unrelated clean cache hash buckets. */
	irq = spin_lock_irqsave(&dirty_index_lock);

	buffer = disk != NULL ? disk->d_dirty_buffers : dirty_head;
	while (buffer != NULL) {
		if ((!reclaim || refcount_load(&buffer->b_refs) == 1) &&
		    (disk == NULL || (buffer->b_block < end &&
		    buffer->b_block + buffer->b_block_count > start))) {
			refcount_get(&buffer->b_refs);
			break;
		}

		buffer = disk != NULL ? buffer->b_device_dirty_next : buffer->b_dirty_next;
	}

	spin_unlock_irqrestore(&dirty_index_lock, irq);

	/* Takes the lower-ranked cache lock only after releasing the index guard. */
	if (buffer != NULL) {
		irq = spin_lock_irqsave(&cache_lock);
		lru_remove_locked(buffer);
		spin_unlock_irqrestore(&cache_lock, irq);
	}

	return buffer;
}
