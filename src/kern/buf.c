/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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
static struct buf *lru_head, *lru_tail;
static struct buf_slab *slabs;
static uint64_t cache_max_bytes;
static uint64_t cache_current_bytes;
static uint64_t cache_reserved_bytes;
static uint64_t cache_data_bytes;
static uint64_t cache_metadata_bytes;
static volatile uint64_t cache_dirty_bytes;
static volatile uint64_t stat_buffers;
static volatile uint64_t stat_hits, stat_misses;
static volatile uint64_t stat_read_bios, stat_write_bios;
static volatile uint64_t stat_evictions, stat_waits;
static volatile uint64_t stat_writeback_errors;
static unsigned cache_initialized;

static unsigned
buf_hash_key(const struct disk *disk, uint64_t block)
{
	uintptr_t value = (uintptr_t)disk;
	return (unsigned)((value >> 4) ^ block ^ (block >> 32)) &
	    (BUF_HASH_BUCKETS - 1U);
}
static size_t
slab_header_size(void)
{
	size_t alignment = sizeof(void *);
	return (sizeof(struct buf_slab) + alignment - 1U) & ~(alignment - 1U);
}

static struct buf *
slab_slot(struct buf_slab *slab, unsigned slot)
{
	return (struct buf *)((uint8_t *)slab + slab_header_size()) + slot;
}

static void
lru_remove_locked(struct buf *buffer)
{
	if (!buffer->b_on_lru)
		return;
	if (buffer->b_lru_prev != NULL)
		buffer->b_lru_prev->b_lru_next = buffer->b_lru_next;
	else
		lru_head = buffer->b_lru_next;
	if (buffer->b_lru_next != NULL)
		buffer->b_lru_next->b_lru_prev = buffer->b_lru_prev;
	else
		lru_tail = buffer->b_lru_prev;
	buffer->b_lru_prev = buffer->b_lru_next = NULL;
	buffer->b_on_lru = 0;
}

static void
lru_add_locked(struct buf *buffer)
{
	if (buffer->b_on_lru)
		lru_remove_locked(buffer);
	buffer->b_lru_prev = NULL;
	buffer->b_lru_next = lru_head;
	if (lru_head != NULL)
		lru_head->b_lru_prev = buffer;
	else
		lru_tail = buffer;
	lru_head = buffer;
	buffer->b_on_lru = 1;
}

static void
hash_remove_locked(struct buf *buffer)
{
	struct buf **link;
	unsigned bucket = buf_hash_key(buffer->b_disk, buffer->b_block);
	for (link = &cache_hash[bucket]; *link != NULL;
	     link = &(*link)->b_hash_next)
		if (*link == buffer) {
			*link = buffer->b_hash_next;
			buffer->b_hash_next = NULL;
			return;
		}
}

static struct buf *
hash_find_locked(struct disk *disk, uint64_t block)
{
	struct buf *buffer;
	unsigned bucket = buf_hash_key(disk, block);
	for (buffer = cache_hash[bucket]; buffer != NULL;
	     buffer = buffer->b_hash_next)
		if (buffer->b_disk == disk && buffer->b_block == block &&
		    !(buffer->b_flags & BUF_INVALID))
			return buffer;
	return NULL;
}

static void
stat_add(volatile uint64_t *counter, uint64_t value)
{
	(void)atomic_u64_fetch_add_relaxed(counter, value);
}

static int
reserve_bytes(size_t size)
{
	unsigned attempt;
	for (attempt = 0; attempt < 2U; attempt++) {
		unsigned long irq = spin_lock_irqsave(&cache_lock);
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
	return ENOMEM;
}

static void
cancel_reservation(size_t size)
{
	unsigned long irq = spin_lock_irqsave(&cache_lock);
	if (cache_reserved_bytes >= size)
		cache_reserved_bytes -= size;
	spin_unlock_irqrestore(&cache_lock, irq);
}

static void
commit_reservation(size_t size, int metadata)
{
	unsigned long irq = spin_lock_irqsave(&cache_lock);
	cache_reserved_bytes -= size;
	cache_current_bytes += size;
	if (metadata)
		cache_metadata_bytes += size;
	else
		cache_data_bytes += size;
	spin_unlock_irqrestore(&cache_lock, irq);
}

static int
alloc_pmem(size_t size, struct hal_pmem *memory)
{
	size_t reserved;
	const struct hal_pmem_request request = {
		HAL_PMEM_PADDR_ANY, size, ZEDBSD_PAGE_SIZE,
		HAL_PMEM_TYPE_RAM, 0
	};
	int error;
	if (size > SIZE_MAX - (ZEDBSD_PAGE_SIZE - 1U))
		return ENOMEM;
	reserved = (size + ZEDBSD_PAGE_SIZE - 1U) &
	    ~(size_t)(ZEDBSD_PAGE_SIZE - 1U);
	if (reserve_bytes(reserved) != 0)
		return ENOMEM;
	error = hal_pmem_alloc(&request, memory);
	if (error != HAL_OK) {
		cancel_reservation(reserved);
		return error == HAL_ERR_NOMEM ? ENOMEM : EIO;
	}
	/* HAL reports the actual page-rounded allocation.  Keep the hard cap
	 * exact even for a host fixture or architecture with different rounding. */
	if (memory->size > reserved) {
		if (reserve_bytes(memory->size - reserved) != 0) {
			(void)hal_pmem_free(memory);
			cancel_reservation(reserved);
			return ENOMEM;
		}
	} else if (memory->size < reserved) {
		cancel_reservation(reserved - memory->size);
	}
	return 0;
}

static int
slab_grow(void)
{
	struct hal_pmem memory;
	struct buf_slab *slab;
	size_t header = slab_header_size();
	unsigned capacity;
	int error = alloc_pmem(BUF_SLAB_BYTES, &memory);
	if (error != 0)
		return error;
	capacity = (unsigned)((BUF_SLAB_BYTES - header) / sizeof(struct buf));
	if (capacity == 0 || capacity > 64U) {
		(void)hal_pmem_free(&memory);
		cancel_reservation(BUF_SLAB_BYTES);
		return EOVERFLOW;
	}
	memset(memory.vaddr, 0, BUF_SLAB_BYTES);
	slab = memory.vaddr;
	slab->memory = memory;
	slab->capacity = capacity;
	slab->free_mask = capacity == 64U ? UINT64_MAX :
	    (((uint64_t)1 << capacity) - 1U);
	commit_reservation(BUF_SLAB_BYTES, 1);
	{
		unsigned long irq = spin_lock_irqsave(&cache_lock);
		slab->next = slabs;
		slabs = slab;
		spin_unlock_irqrestore(&cache_lock, irq);
	}
	return 0;
}

static struct buf *
alloc_metadata(void)
{
	for (;;) {
		struct buf_slab *slab;
		unsigned long irq = spin_lock_irqsave(&cache_lock);
		for (slab = slabs; slab != NULL; slab = slab->next)
			if (slab->free_mask != 0) {
				unsigned slot;
				for (slot = 0; slot < slab->capacity; slot++)
					if (slab->free_mask & ((uint64_t)1 << slot))
						break;
				slab->free_mask &= ~((uint64_t)1 << slot);
				slab->used++;
				spin_unlock_irqrestore(&cache_lock, irq);
				memset(slab_slot(slab, slot), 0, sizeof(struct buf));
				slab_slot(slab, slot)->b_slab = slab;
				slab_slot(slab, slot)->b_slab_slot = slot;
				return slab_slot(slab, slot);
			}
		spin_unlock_irqrestore(&cache_lock, irq);
		if (slab_grow() != 0)
			return NULL;
	}
}

static void
free_metadata(struct buf *buffer)
{
	struct buf_slab *slab = buffer->b_slab;
	unsigned slot = buffer->b_slab_slot;
	struct hal_pmem release;
	int free_slab = 0;
	unsigned long irq;
	if (slab == NULL || slot >= slab->capacity)
		return;
	irq = spin_lock_irqsave(&cache_lock);
	slab->free_mask |= (uint64_t)1 << slot;
	if (slab->used != 0)
		slab->used--;
	if (slab->used == 0 && slabs != slab) {
		struct buf_slab **link;
		for (link = &slabs; *link != NULL; link = &(*link)->next)
			if (*link == slab) {
				*link = slab->next;
				break;
			}
		release = slab->memory;
		cache_current_bytes -= BUF_SLAB_BYTES;
		cache_metadata_bytes -= BUF_SLAB_BYTES;
		free_slab = 1;
	}
	spin_unlock_irqrestore(&cache_lock, irq);
	if (free_slab)
		(void)hal_pmem_free(&release);
}

static void
free_buffer(struct buf *buffer)
{
	struct hal_pmem memory = buffer->b_memory;
	size_t size = memory.size;
	struct disk *disk = buffer->b_disk;
	if (size != 0) {
		(void)hal_pmem_free(&memory);
		{
			unsigned long irq = spin_lock_irqsave(&cache_lock);
			cache_current_bytes -= size;
			cache_data_bytes -= size;
			spin_unlock_irqrestore(&cache_lock, irq);
		}
	}
	if (disk != NULL)
		disk_release(disk);
	free_metadata(buffer);
}

static int
busy_acquire(struct buf *buffer)
{
	for (;;) {
		unsigned long irq = spin_lock_irqsave(&buffer->b_lock);
		if (!buffer->b_busy) {
			buffer->b_busy = 1;
			spin_unlock_irqrestore(&buffer->b_lock, irq);
			return 0;
		}
		stat_add(&stat_waits, 1);
		if (thread_current() != NULL) {
			uint64_t sequence = waitq_sequence(&buffer->b_waitq);
			int error = waitq_sleep(&buffer->b_waitq, &buffer->b_lock,
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

static void
drop_caller_reference(struct buf *buffer)
{
	unsigned long irq = spin_lock_irqsave(&cache_lock);
	if (refcount_put_not_last(&buffer->b_refs) == 1)
		lru_add_locked(buffer);
	spin_unlock_irqrestore(&cache_lock, irq);
}

static int
read_buffer(struct buf *buffer)
{
	int error;
	unsigned long irq = spin_lock_irqsave(&buffer->b_lock);
	if (buffer->b_flags & BUF_VALID) {
		spin_unlock_irqrestore(&buffer->b_lock, irq);
		return 0;
	}
	buffer->b_io_state = BUF_IO_READING;
	buffer->b_io_inflight = 1;
	spin_unlock_irqrestore(&buffer->b_lock, irq);
	stat_add(&stat_read_bios, 1);
	error = disk_read_direct(buffer->b_disk, buffer->b_block,
	    buffer->b_block_count, buffer->b_data);
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
	return error;
}

static int
acquire_line(struct disk *disk, uint64_t block, int read_data,
	struct buf **result)
{
	struct buf *buffer, *candidate = NULL;
	uint64_t line_bytes, line_blocks, line_block, remaining;
	int error;
	if (disk == NULL || result == NULL || disk->d_block_size == 0)
		return EINVAL;
	line_bytes = disk->d_block_size > ZEDBSD_PAGE_SIZE ?
	    disk->d_block_size : ZEDBSD_PAGE_SIZE;
	if ((disk->d_block_size & (disk->d_block_size - 1U)) != 0 ||
	    line_bytes % disk->d_block_size != 0)
		return EOPNOTSUPP;
	line_blocks = line_bytes / disk->d_block_size;
	line_block = block - block % line_blocks;
	remaining = disk->d_block_count - line_block;
	if (remaining < line_blocks)
		line_blocks = remaining;
	for (;;) {
		unsigned bucket;
		unsigned long irq = spin_lock_irqsave(&cache_lock);
		buffer = hash_find_locked(disk, line_block);
		if (buffer != NULL) {
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
		if (candidate == NULL) {
			struct hal_pmem memory;
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

int
buf_init(void)
{
	uint64_t value;
#if CONFIG_BUF_CACHE_KIB == 0
	uint64_t total;
#endif
	if (cache_initialized)
		return 0;
	spin_init(&cache_lock, LOCK_RANK_BUFCACHE, "buffer cache");
	if (mutex_init(&cache_control, LOCK_RANK_BUFCACHE,
	    "buffer cache control") != 0)
		return ENOMEM;
	memset(cache_hash, 0, sizeof(cache_hash));
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
	if (slab_grow() != 0) {
		cache_initialized = 0;
		return ENOMEM;
	}
	return 0;
}

int
buf_get(struct disk *disk, uint64_t block, struct buf **result)
{
	struct disk *leaf;
	uint64_t mapped;
	int error = disk_resolve_range(disk, block, 1, &leaf, &mapped);
	return error != 0 ? error : acquire_line(leaf, mapped, 1, result);
}

void
buf_release(struct buf *buffer)
{
	unsigned long irq;
	if (buffer == NULL)
		return;
	irq = spin_lock_irqsave(&buffer->b_lock);
	buffer->b_busy = 0;
	waitq_wake_all(&buffer->b_waitq);
	spin_unlock_irqrestore(&buffer->b_lock, irq);
	irq = spin_lock_irqsave(&cache_lock);
	if (refcount_put_not_last(&buffer->b_refs) == 1)
		lru_add_locked(buffer);
	spin_unlock_irqrestore(&cache_lock, irq);
}

void
buf_mark_dirty(struct buf *buffer)
{
	unsigned long irq;
	if (buffer == NULL)
		return;
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

int
buf_writeback(struct buf *buffer)
{
	uint64_t generation;
	int error;
	unsigned long irq;
	if (buffer == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&buffer->b_lock);
	if (!(buffer->b_flags & BUF_DIRTY)) {
		spin_unlock_irqrestore(&buffer->b_lock, irq);
		return 0;
	}
	if (!(buffer->b_flags & BUF_VALID)) {
		spin_unlock_irqrestore(&buffer->b_lock, irq);
		return EIO;
	}
	generation = buffer->b_dirty_generation;
	buffer->b_io_state = BUF_IO_WRITING;
	buffer->b_io_inflight = 1;
	spin_unlock_irqrestore(&buffer->b_lock, irq);
	stat_add(&stat_write_bios, 1);
	error = disk_write_direct(buffer->b_disk, buffer->b_block,
	    buffer->b_block_count, buffer->b_data);
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
	return error;
}

int
buf_read(struct disk *disk, uint64_t block, uint32_t count, void *data)
{
	struct disk *leaf;
	uint64_t mapped, end;
	uint8_t *out = data;
	int error;
	if (data == NULL || count == 0)
		return EINVAL;
	error = disk_resolve_range(disk, block, count, &leaf, &mapped);
	if (error != 0)
		return error;
	end = mapped + count;
	while (mapped < end) {
		struct buf *buffer;
		uint64_t offset_blocks, amount_blocks;
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
	return 0;
}

int
buf_write(struct disk *disk, uint64_t block, uint32_t count,
	const void *data)
{
	struct disk *leaf;
	uint64_t mapped, end;
	const uint8_t *in = data;
	int error;
	if (data == NULL || count == 0)
		return EINVAL;
	if (disk->d_flags & DISK_READ_ONLY)
		return EROFS;
	error = disk_resolve_range(disk, block, count, &leaf, &mapped);
	if (error != 0)
		return error;
	end = mapped + count;
	while (mapped < end) {
		struct buf *buffer;
		uint64_t line_bytes = leaf->d_block_size > ZEDBSD_PAGE_SIZE ?
		    leaf->d_block_size : ZEDBSD_PAGE_SIZE;
		uint64_t line_blocks = line_bytes / leaf->d_block_size;
		uint64_t line_start = mapped - mapped % line_blocks;
		uint64_t offset_blocks = mapped - line_start;
		uint64_t amount_blocks = line_blocks - offset_blocks;
		int full;
		if (amount_blocks > end - mapped)
			amount_blocks = end - mapped;
		full = offset_blocks == 0 && amount_blocks == line_blocks &&
		    line_start + line_blocks <= leaf->d_block_count;
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
	return 0;
}

static int
disk_cache_range(struct disk *disk, struct disk **leaf_out,
	uint64_t *start_out, uint64_t *end_out)
{
	struct disk *leaf;
	uint64_t start;
	int error;
	if (disk == NULL || disk->d_block_count == 0)
		return EINVAL;
	error = disk_resolve_range(disk, 0, 1, &leaf, &start);
	if (error != 0)
		return error;
	if (disk->d_block_count > UINT64_MAX - start)
		return EOVERFLOW;
	*leaf_out = leaf;
	*start_out = start;
	*end_out = start + disk->d_block_count;
	return 0;
}

int
buf_sync(struct disk *disk)
{
	struct disk *leaf;
	uint64_t start, end;
	int error = disk_cache_range(disk, &leaf, &start, &end);
	if (error != 0)
		return error;
	for (;;) {
		struct buf *candidate = NULL;
		unsigned bucket;
		unsigned long irq = spin_lock_irqsave(&cache_lock);
		for (bucket = 0; bucket < BUF_HASH_BUCKETS && candidate == NULL;
		     bucket++) {
			struct buf *buffer;
			for (buffer = cache_hash[bucket]; buffer != NULL;
			     buffer = buffer->b_hash_next)
				if (buffer->b_disk == leaf && buffer->b_block < end &&
				    buffer->b_block + buffer->b_block_count > start &&
				    (buffer->b_flags & BUF_DIRTY)) {
					refcount_get(&buffer->b_refs);
					lru_remove_locked(buffer);
					candidate = buffer;
					break;
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

static int
evict_one(struct disk *disk, uint64_t start, uint64_t end, int range,
	unsigned flags, size_t *freed)
{
	struct buf *candidate = NULL;
	unsigned long irq = spin_lock_irqsave(&cache_lock);
	{
		struct buf *buffer;
		for (buffer = lru_tail; buffer != NULL;
		     buffer = buffer->b_lru_prev) {
			unsigned long birq;
			if (range && (buffer->b_disk != disk ||
			    buffer->b_block >= end ||
			    buffer->b_block + buffer->b_block_count <= start))
				continue;
			if (refcount_load(&buffer->b_refs) != 1)
				continue;
			birq = spin_lock_irqsave(&buffer->b_lock);
			if (buffer->b_busy || buffer->b_io_inflight ||
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
	}
	if (candidate == NULL && range) {
		unsigned bucket;
		for (bucket = 0; bucket < BUF_HASH_BUCKETS; bucket++) {
			struct buf *buffer;
			for (buffer = cache_hash[bucket]; buffer != NULL;
			     buffer = buffer->b_hash_next)
				if (buffer->b_disk == disk && buffer->b_block < end &&
				    buffer->b_block + buffer->b_block_count > start) {
					spin_unlock_irqrestore(&cache_lock, irq);
					return EBUSY;
				}
		}
	}
	spin_unlock_irqrestore(&cache_lock, irq);
	if (candidate == NULL)
		return ENOENT;
	if (candidate->b_flags & BUF_DIRTY)
		stat_add(&cache_dirty_bytes,
		    (uint64_t)-(int64_t)candidate->b_size);
	*freed = candidate->b_memory.size;
	stat_add(&stat_buffers, UINT64_MAX);
	stat_add(&stat_evictions, 1);
	free_buffer(candidate);
	return 0;
}

static int
writeback_one_reclaimable(void)
{
	struct buf *candidate = NULL;
	int error;
	unsigned long irq = spin_lock_irqsave(&cache_lock);
	{
		struct buf *buffer;
		for (buffer = lru_tail; buffer != NULL;
		     buffer = buffer->b_lru_prev) {
			unsigned long birq;
			if (refcount_load(&buffer->b_refs) != 1)
				continue;
			birq = spin_lock_irqsave(&buffer->b_lock);
			if (!buffer->b_busy && !buffer->b_io_inflight &&
			    (buffer->b_flags & BUF_DIRTY)) {
				refcount_get(&buffer->b_refs);
				lru_remove_locked(buffer);
				candidate = buffer;
				spin_unlock_irqrestore(&buffer->b_lock, birq);
				break;
			}
			spin_unlock_irqrestore(&buffer->b_lock, birq);
		}
	}
	spin_unlock_irqrestore(&cache_lock, irq);
	if (candidate == NULL)
		return ENOENT;
	error = busy_acquire(candidate);
	if (error == 0)
		error = buf_writeback(candidate);
	buf_release(candidate);
	return error;
}

size_t
buf_reclaim(size_t target_bytes, unsigned flags)
{
	size_t total = 0;
	while (total < target_bytes) {
		size_t freed = 0;
		if (evict_one(NULL, 0, 0, 0, 0, &freed) != 0) {
			if (!(flags & BUF_RECLAIM_WRITE) ||
			    writeback_one_reclaimable() != 0)
				break;
			continue;
		}
		total += freed;
	}
	return total;
}

int
buf_invalidate(struct disk *disk, uint64_t block, uint64_t count,
	unsigned flags)
{
	struct disk *leaf;
	uint64_t start, end;
	int error;
	if (count == 0 || count > UINT32_MAX)
		return EINVAL;
	error = disk_resolve_range(disk, block, (uint32_t)count, &leaf, &start);
	if (error != 0)
		return error;
	end = start + count;
	if (!(flags & BUF_INVALIDATE_DISCARD)) {
		error = buf_sync(disk);
		if (error != 0)
			return error;
	}
	for (;;) {
		size_t freed;
		error = evict_one(leaf, start, end, 1, flags, &freed);
		if (error == ENOENT)
			return 0;
		if (error != 0)
			return error;
	}
}

int
buf_invalidate_disk(struct disk *disk, unsigned flags)
{
	struct disk *leaf;
	uint64_t start, end;
	int error = disk_cache_range(disk, &leaf, &start, &end);
	if (error != 0)
		return error;
	if (!(flags & BUF_INVALIDATE_DISCARD)) {
		error = buf_sync(disk);
		if (error != 0)
			return error;
	}
	for (;;) {
		size_t freed;
		error = evict_one(leaf, start, end, 1, flags, &freed);
		if (error == ENOENT)
			return 0;
		if (error != 0)
			return error;
	}
}

void
buf_get_stats(struct zedbsd_bufcache_stats *stats)
{
	unsigned long irq;
	if (stats == NULL)
		return;
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

int
buf_set_max_bytes(uint64_t value)
{
	uint64_t total = hal_pmem_get_total_size();
	uint64_t old;
	int result = 0;
	if (value < BUF_MIN_BYTES || value > total / 2U ||
	    value > SIZE_MAX || (value & (ZEDBSD_PAGE_SIZE - 1U)) != 0)
		return EINVAL;
	mutex_lock(&cache_control);
	{
		unsigned long irq = spin_lock_irqsave(&cache_lock);
		old = cache_max_bytes;
		cache_max_bytes = value;
		spin_unlock_irqrestore(&cache_lock, irq);
	}
	for (;;) {
		uint64_t current;
		unsigned long irq = spin_lock_irqsave(&cache_lock);
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
	return result;
}

void
buf_reset(void)
{
	if (!cache_initialized)
		return;
	for (;;) {
		size_t freed;
		if (evict_one(NULL, 0, 0, 0, BUF_INVALIDATE_DISCARD,
		    &freed) != 0)
			break;
	}
}
