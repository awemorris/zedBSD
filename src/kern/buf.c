/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

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
#include "kern/disk.h"
#include "kern/page.h"

#include <errno.h>
#include <string.h>

#define BUF_HASH_BUCKETS 64U
#define BUF_MIN_BYTES (64U * 1024U)
#define BUF_MAX_DEFAULT (16U * 1024U * 1024U)
#ifndef CONFIG_BUF_CACHE_KIB
#define CONFIG_BUF_CACHE_KIB 0
#endif
#define BUF_SLAB_BYTES ZEDBSD_PAGE_SIZE

struct thread;
struct thread *thread_current(void);

struct buf_slab {
	struct hal_pmem memory;
	struct buf_slab *next;
	uint64_t free_mask;
	unsigned capacity;
	unsigned used;
};

static struct spinlock cache_lock;
static struct mutex cache_control;
static struct buf *cache_hash[BUF_HASH_BUCKETS];
static struct buf *lru_head;
static struct buf *lru_tail;
static struct buf_slab *slabs;
static uint64_t cache_max_bytes;
static uint64_t cache_current_bytes;
static uint64_t cache_reserved_bytes;
static uint64_t cache_data_bytes;
static uint64_t cache_metadata_bytes;
static volatile uint64_t cache_dirty_bytes;
static volatile uint64_t stat_buffers;
static volatile uint64_t stat_hits;
static volatile uint64_t stat_misses;
static volatile uint64_t stat_read_bios;
static volatile uint64_t stat_write_bios;
static volatile uint64_t stat_evictions;
static volatile uint64_t stat_waits;
static volatile uint64_t stat_writeback_errors;
static unsigned cache_initialized;

static unsigned buf_hash_key(const struct disk *disk, uint64_t block);
static size_t slab_header_size(void);
static struct buf * slab_slot(struct buf_slab *slab, unsigned slot);
static void lru_remove_locked(struct buf *buffer);
static void lru_add_locked(struct buf *buffer);
static void hash_remove_locked(struct buf *buffer);
static struct buf * hash_find_locked(struct disk *disk, uint64_t block);
static void stat_add(volatile uint64_t *counter, uint64_t value);
static int reserve_bytes(size_t size);
static void cancel_reservation(size_t size);
static void commit_reservation(size_t size, int metadata);
static int alloc_pmem(size_t size, struct hal_pmem *memory);
static int slab_grow(void);
static struct buf * alloc_metadata(void);
static void free_metadata(struct buf *buffer);
static void free_buffer(struct buf *buffer);
static int busy_acquire(struct buf *buffer);
static void drop_caller_reference(struct buf *buffer);
static int read_buffer(struct buf *buffer);
static int acquire_line(struct disk *disk, uint64_t block, int read_data, struct buf **result);
static int disk_cache_range(struct disk *disk, struct disk **leaf_out, uint64_t *start_out, uint64_t *end_out);
static int evict_one(struct disk *disk, uint64_t start, uint64_t end, int range, unsigned flags, size_t *freed);
static int writeback_one_reclaimable(void);

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
	if (mutex_init(&cache_control, LOCK_RANK_BUFCACHE,
	    "buffer cache control") != 0)
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
	if (value > BUF_MAX_DEFAULT)
		value = BUF_MAX_DEFAULT;
	value &= ~(uint64_t)(ZEDBSD_PAGE_SIZE - 1U);
	cache_max_bytes = value;
	cache_initialized = 1;

	/* Allocates the first header slab. */
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
	error = acquire_line(leaf, mapped, 1, result);

	/* Reports the acquisition result. */
	return error;
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
	buffer->b_generation++;
	if (buffer->b_generation == 0)
		buffer->b_generation++;
	buffer->b_dirty_generation = buffer->b_generation;
	buffer->b_flags |= BUF_VALID;
	if (!(buffer->b_flags & BUF_DIRTY)) {
		buffer->b_flags |= BUF_DIRTY;
		stat_add(&cache_dirty_bytes, buffer->b_size);
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
	uint64_t generation;
	int error;
	unsigned long irq;

	/* Rejects a missing buffer. */
	if (buffer == NULL)
		return EINVAL;

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
	error = disk_write_direct(buffer->b_disk, buffer->b_block,
	    buffer->b_block_count, buffer->b_data);

	/* Records the outcome; only an unmodified buffer becomes clean. */
	irq = spin_lock_irqsave(&buffer->b_lock);
	buffer->b_io_state = BUF_IO_IDLE;
	buffer->b_io_inflight = 0;
	buffer->b_error = error;
	if (error == 0 && generation == buffer->b_dirty_generation) {
		buffer->b_flags &= ~(BUF_DIRTY | BUF_ERROR);
		stat_add(&cache_dirty_bytes,
		    (uint64_t)-(int64_t)buffer->b_size);
	} else if (error != 0) {
		buffer->b_flags |= BUF_ERROR | BUF_DIRTY | BUF_VALID;
		stat_add(&stat_writeback_errors, 1);
	}
	waitq_wake_all(&buffer->b_waitq);
	spin_unlock_irqrestore(&buffer->b_lock, irq);

	/* Reports the write result. */
	return error;
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

	out = data;

	/* Rejects a missing buffer or an empty range. */
	if (data == NULL || count == 0)
		return EINVAL;

	/* Resolves the range to the leaf disk. */
	error = disk_resolve_range(disk, block, count, &leaf, &mapped);
	if (error != 0)
		return error;

	/* Copies out of each cached line in turn. */
	end = mapped + count;
	while (mapped < end) {
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

	in = data;

	/* Rejects a missing buffer, an empty range, or a read-only disk. */
	if (data == NULL || count == 0)
		return EINVAL;
	if (disk->d_flags & DISK_READ_ONLY)
		return EROFS;

	/* Resolves the range to the leaf disk. */
	error = disk_resolve_range(disk, block, count, &leaf, &mapped);
	if (error != 0)
		return error;

	/* Copies into each line in turn and writes it back. */
	end = mapped + count;
	while (mapped < end) {
		if (leaf->d_block_size > ZEDBSD_PAGE_SIZE)
			line_bytes = leaf->d_block_size;
		else
			line_bytes = ZEDBSD_PAGE_SIZE;
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
		error = buf_writeback(buffer);
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
	unsigned bucket;
	unsigned long irq;
	struct buf *buffer;

	/* Resolves the disk's block range on its leaf. */
	error = disk_cache_range(disk, &leaf, &start, &end);
	if (error != 0)
		return error;

	/* Writes back one dirty buffer at a time until none is left. */
	for (;;) {
		candidate = NULL;
		irq = spin_lock_irqsave(&cache_lock);
		for (bucket = 0; bucket < BUF_HASH_BUCKETS && candidate == NULL;
		     bucket++) {
			for (buffer = cache_hash[bucket]; buffer != NULL;
			     buffer = buffer->b_hash_next) {
				if (buffer->b_disk == leaf &&
				    buffer->b_block < end &&
				    buffer->b_block + buffer->b_block_count > start &&
				    (buffer->b_flags & BUF_DIRTY)) {
					refcount_get(&buffer->b_refs);
					lru_remove_locked(buffer);
					candidate = buffer;
					break;
				}
			}
		}
		spin_unlock_irqrestore(&cache_lock, irq);
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
	    (value & (ZEDBSD_PAGE_SIZE - 1U)) != 0)
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
	size_t size)
{
	unsigned attempt;
	unsigned long irq;

	/* Tries before and after one reclaim pass. */
	for (attempt = 0; attempt < 2U; attempt++) {
		irq = spin_lock_irqsave(&cache_lock);
		if (cache_current_bytes <= cache_max_bytes &&
		    cache_reserved_bytes <= cache_max_bytes - cache_current_bytes &&
		    (uint64_t)size <= cache_max_bytes - cache_current_bytes -
		    cache_reserved_bytes) {
			cache_reserved_bytes += size;
			spin_unlock_irqrestore(&cache_lock, irq);
			return 0;
		}
		spin_unlock_irqrestore(&cache_lock, irq);
		if (buf_reclaim(size, BUF_RECLAIM_WRITE) == 0)
			break;
	}

	/* Reports that the bytes do not fit. */
	return ENOMEM;
}

/* Gives back a reservation that was not committed. */
static void
cancel_reservation(
	size_t size)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&cache_lock);
	if (cache_reserved_bytes >= size)
		cache_reserved_bytes -= size;
	spin_unlock_irqrestore(&cache_lock, irq);
}

/* Converts a reservation into data or metadata usage. */
static void
commit_reservation(
	size_t size,
	int metadata)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&cache_lock);
	cache_reserved_bytes -= size;
	cache_current_bytes += size;
	if (metadata)
		cache_metadata_bytes += size;
	else
		cache_data_bytes += size;
	spin_unlock_irqrestore(&cache_lock, irq);
}

/* Allocates page-aligned physical memory reserved against the cap. */
static int
alloc_pmem(
	size_t size,
	struct hal_pmem *memory)
{
	size_t reserved;
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, size, ZEDBSD_PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	int error;

	/* Reserves the page-rounded size first. */
	if (size > SIZE_MAX - (ZEDBSD_PAGE_SIZE - 1U))
		return ENOMEM;
	reserved = (size + ZEDBSD_PAGE_SIZE - 1U) &
	    ~(size_t)(ZEDBSD_PAGE_SIZE - 1U);
	if (reserve_bytes(reserved) != 0)
		return ENOMEM;
	error = hal_pmem_alloc(&request, memory);
	if (error != HAL_OK) {
		cancel_reservation(reserved);
		if (error == HAL_ERR_NOMEM)
			return ENOMEM;
		return EIO;
	}

	/*
	 * HAL reports the actual page-rounded allocation.  Keep the hard cap
	 * exact even for a host fixture or architecture with different
	 * rounding.
	 */
	if (memory->size > reserved) {
		if (reserve_bytes(memory->size - reserved) != 0) {
			(void)hal_pmem_free(memory);
			cancel_reservation(reserved);
			return ENOMEM;
		}
	} else if (memory->size < reserved) {
		cancel_reservation(reserved - memory->size);
	}

	/* Reports the reserved allocation. */
	return 0;
}

/* Adds one slab of buffer headers. */
static int
slab_grow(
	void)
{
	struct hal_pmem memory;
	struct buf_slab *slab;
	size_t header;
	unsigned capacity;
	int error;
	unsigned long irq;

	header = slab_header_size();
	error = alloc_pmem(BUF_SLAB_BYTES, &memory);

	/* Rejects a failed allocation or a slab the free mask cannot cover. */
	if (error != 0)
		return error;
	capacity = (unsigned)((BUF_SLAB_BYTES - header) / sizeof(struct buf));
	if (capacity == 0 || capacity > 64U) {
		(void)hal_pmem_free(&memory);
		cancel_reservation(BUF_SLAB_BYTES);
		return EOVERFLOW;
	}

	/* Initializes the slab with every slot free. */
	memset(memory.vaddr, 0, BUF_SLAB_BYTES);
	slab = memory.vaddr;
	slab->memory = memory;
	slab->capacity = capacity;
	if (capacity == 64U)
		slab->free_mask = UINT64_MAX;
	else
		slab->free_mask = ((uint64_t)1 << capacity) - 1U;
	commit_reservation(BUF_SLAB_BYTES, 1);

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
	struct hal_pmem release;
	int free_slab;
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
		cache_current_bytes -= BUF_SLAB_BYTES;
		cache_metadata_bytes -= BUF_SLAB_BYTES;
		free_slab = 1;
	}
	spin_unlock_irqrestore(&cache_lock, irq);

	/* Releases the slab memory unlocked. */
	if (free_slab)
		(void)hal_pmem_free(&release);
}

/* Frees a buffer's data, disk reference, and header. */
static void
free_buffer(
	struct buf *buffer)
{
	struct hal_pmem memory;
	size_t size;
	struct disk *disk;
	unsigned long irq;

	memory = buffer->b_memory;
	size = memory.size;
	disk = buffer->b_disk;

	/* Frees the data and un-accounts it. */
	if (size != 0) {
		(void)hal_pmem_free(&memory);
		irq = spin_lock_irqsave(&cache_lock);
		cache_current_bytes -= size;
		cache_data_bytes -= size;
		spin_unlock_irqrestore(&cache_lock, irq);
	}

	/* Drops the disk pin and the header. */
	if (disk != NULL)
		disk_release(disk);
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

	/* Reports the read result. */
	return error;
}

/* Finds or creates the busy, referenced line holding a leaf block. */
static int
acquire_line(
	struct disk *disk,
	uint64_t block,
	int read_data,
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
	struct hal_pmem memory;

	candidate = NULL;

	/* Rejects a missing disk or result, or a disk without a block size. */
	if (disk == NULL || result == NULL || disk->d_block_size == 0)
		return EINVAL;

	/* Lines are one page or one block; the block size must divide evenly. */
	if (disk->d_block_size > ZEDBSD_PAGE_SIZE)
		line_bytes = disk->d_block_size;
	else
		line_bytes = ZEDBSD_PAGE_SIZE;
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
			error = busy_acquire(buffer);
			if (error != 0) {
				drop_caller_reference(buffer);
				return error;
			}
			if (read_data && !(buffer->b_flags & BUF_VALID)) {
				error = read_buffer(buffer);
				if (error != 0) {
					buf_release(buffer);
					return error;
				}
			}
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
			    &memory);
			if (error != 0) {
				free_metadata(candidate);
				return error;
			}
			commit_reservation(memory.size, 0);
			candidate->b_disk = disk;
			disk_ref(disk);
			candidate->b_block = line_block;
			candidate->b_block_count = (uint32_t)line_blocks;
			candidate->b_size = memory.size;
			candidate->b_memory = memory;
			candidate->b_data = memory.vaddr;
			refcount_init(&candidate->b_refs, 2);
			spin_init(&candidate->b_lock, LOCK_RANK_BUF, "buffer");
			waitq_init(&candidate->b_waitq, "buffer state");
			candidate->b_busy = 1;
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
		if (read_data) {
			error = read_buffer(candidate);
			if (error != 0) {
				buf_release(candidate);
				return error;
			}
		}
		*result = candidate;
		return 0;
	}
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
		if (buffer->b_busy ||
		    buffer->b_io_inflight ||
		    ((buffer->b_flags & BUF_DIRTY) &&
		     !(flags & BUF_INVALIDATE_DISCARD))) {
			spin_unlock_irqrestore(&buffer->b_lock, birq);
			continue;
		}
		buffer->b_flags |= BUF_INVALID;
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
	unsigned long irq;
	struct buf *buffer;
	unsigned long birq;

	candidate = NULL;
	irq = spin_lock_irqsave(&cache_lock);

	/* Walks the LRU from the tail for an unreferenced, idle, dirty buffer. */
	for (buffer = lru_tail; buffer != NULL;
	     buffer = buffer->b_lru_prev) {
		if (refcount_load(&buffer->b_refs) != 1)
			continue;
		birq = spin_lock_irqsave(&buffer->b_lock);
		if (!buffer->b_busy &&
		    !buffer->b_io_inflight &&
		    (buffer->b_flags & BUF_DIRTY)) {
			refcount_get(&buffer->b_refs);
			lru_remove_locked(buffer);
			candidate = buffer;
			spin_unlock_irqrestore(&buffer->b_lock, birq);
			break;
		}
		spin_unlock_irqrestore(&buffer->b_lock, birq);
	}
	spin_unlock_irqrestore(&cache_lock, irq);
	if (candidate == NULL)
		return ENOENT;

	/* Writes it back and drops the reference. */
	error = busy_acquire(candidate);
	if (error == 0)
		error = buf_writeback(candidate);
	buf_release(candidate);

	/* Reports the writeback result. */
	return error;
}
