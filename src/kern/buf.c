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

#include "kern/backing-claim.h"
#include "kern/buf.h"
#include "kern/buf-unjournaled.h"
#include "kern/buf-slab.h"
#include "kern/buf-readahead.h"
#include "kern/kmem.h"
#include "kern/io-stats.h"
#include "kern/io-pool.h"
#include "kern/disk.h"
#include "kern/page.h"
#include "kern/cache-memory.h"
#include "kern/clock.h"
#include "kern/klog.h"
#include "kern/sched.h"
#include "kern/thread.h"
#include <uapi/errno.h>
#include <kern/kcrt.h>
#include <hal/hal.h>

/*
 * The hash table's size: 2^16 buckets.  The cache holds an eighth of memory
 * in page-sized lines (16384 lines for 512 MiB, 65536 for 2 GiB), and a lookup
 * walks one chain under the cache lock on every cached read and file fault,
 * so the chains must stay short (BUG-033).
 */
#define BUF_HASH_BITS		16U
#define BUF_HASH_BUCKETS	(1U << BUF_HASH_BITS)
#define BUF_MIN_BYTES		(1024U * 1024U)
#define BUF_SLAB_BYTES		KERN_PAGE_SIZE
#define BUF_RUN_LINES		(KERN_IO_BATCH_MAX / KERN_PAGE_SIZE)

/*
 * The longest run, in page-sized lines, sent as one physical page list to a
 * disk whose driver moves pages itself (DISK_PAGE_VECTOR): 1 MiB.  Dirty
 * lines written back together (a cluster) are bounded the same way.
 */
#define BUF_RUN_PAGE_LINES	256U

/*
 * Delayed writes of write-cached disks.
 *
 * A write to a DISK_WRITE_CACHED disk only dirties its buffers.  The
 * flusher wakes every BUF_FLUSH_INTERVAL ticks and writes each buffer that
 * has been dirty for BUF_FLUSH_AGE ticks, oldest first, then flushes the
 * devices it wrote to (at most BUF_FLUSH_LEAVES of them, and at most
 * BUF_FLUSH_PASS_MAX buffers a pass).  A short-lived file is usually gone
 * before then.
 *
 * Dirty memory is bounded by a 1/BUF_DIRTY_SHARE part of the cache, so it
 * never fills a small cache, and by BUF_DIRTY_SMALL as well for a write to a
 * disk that writes one line at a time (no page lists), so that such a disk
 * is not left with more to write at a sync than before.  Past half of the
 * bound the read-ahead workers
 * also write the oldest dirty lines back while they have no read to do,
 * down to a quarter of the bound; past the bound a writer first writes some
 * back itself, and only when it can write none goes through at once, so
 * dirty memory stays bounded however fast a writer is.  A writer helps only
 * with its own device; the workers skip a device read-ahead is stopped on
 * and lines whose last write failed (the flusher retries those), and leave
 * the device flush to the flusher.  Write-back sends the adjacent dirty
 * lines of a line along with it, as one page list, gathered at most
 * BUF_GATHER_HOLD at a time under the cache lock, and looks at
 * BUF_WRITEBACK_SCAN lines from the oldest for one to start at.
 */
#define BUF_FLUSH_INTERVAL	kern_ms_to_ticks(1000U)
#define BUF_FLUSH_AGE		kern_ms_to_ticks(2000U)
#define BUF_DIRTY_SHARE		4U
#define BUF_DIRTY_SMALL		(UINT64_C(32) * 1024U * 1024U)
#define BUF_WRITEBACK_SCAN	1024U
#define BUF_GATHER_HOLD		64U
#define BUF_FLUSH_LEAVES	8U
#define BUF_FLUSH_PASS_MAX	65536U

/* The number of functions the flusher can call each interval. */
#define BUF_FLUSHER_HOOKS	8U

#ifndef CONFIG_BUF_CACHE_KIB
#define CONFIG_BUF_CACHE_KIB 0
#endif

/*
 * Read-ahead of raw block devices.
 *
 * A sequential reader of an open block device gets up to
 * BUF_READAHEAD_BYTES past its last read (an eighth of the cache at most),
 * and a file system may ask for BUF_FS_READAHEAD_BYTES of its own blocks
 * past its reader (buf_prefetch()),
 * read by BUF_READAHEAD_WORKERS threads, one run of BUF_READAHEAD_RUN_LINES lines
 * a job, so that the device reads the next runs while the reader copies.
 * BUF_READAHEAD_QUEUE jobs wait at most, BUF_READAHEAD_STREAMS readers are
 * followed at once, and a log line sums up every BUF_READAHEAD_LOG_EVERY
 * jobs.
 */
#define BUF_READAHEAD_BYTES	(2U * 1024U * 1024U)
#define BUF_FS_READAHEAD_BYTES	(4U * 1024U * 1024U)
#define BUF_READAHEAD_WORKERS	4U
#define BUF_READAHEAD_QUEUE	32U
#define BUF_READAHEAD_STREAMS	8U
#define BUF_READAHEAD_LOG_EVERY	1024U
#define BUF_READAHEAD_RUN_LINES	64U
#define BUF_READAHEAD_RUN_BYTES	((uint64_t)BUF_READAHEAD_RUN_LINES * KERN_PAGE_SIZE)
#define BUF_READAHEAD_SPLIT_MAX	(BUF_READAHEAD_BYTES / BUF_READAHEAD_RUN_BYTES + 1U)

/* How many lines from the LRU tail a miss in a full cache looks at to take one over. */
#define BUF_RECYCLE_LOOK	8U

/*
 * One page of buffer headers.
 *
 * The record comes first, so the slab index (kern/buf-slab.h) and the slab
 * name the same address.  The page lives while a header in it is in use,
 * or while the index keeps it as a spare empty slab.
 */
struct buf_slab {
	struct buf_slab_record record;
	struct kern_pmem memory;
};

/*
 * The bookkeeping of one long run sent as a page list.
 *
 * It is too large for the kernel stack of every port, so one is taken from
 * the heap for each such run and given back when the run ends.
 */
struct buf_run_scratch {
	struct buf *lines[BUF_RUN_PAGE_LINES];
	uint64_t generations[BUF_RUN_PAGE_LINES];
	uint64_t pages[BUF_RUN_PAGE_LINES];
};

/*
 * Dirty lines written back together: the slots hold the lines before the
 * first one taken (from the middle down), it, and the lines after it, so
 * that the taken run is contiguous in the slots; lines points at its first
 * line, own at the line the caller owns.  Taken from the heap for each
 * cluster, too large for every port's kernel stack.
 */
struct buf_cluster {
	struct buf *slots[2U * BUF_RUN_PAGE_LINES - 1U];
	uint64_t generations[BUF_RUN_PAGE_LINES];
	uint64_t pages[BUF_RUN_PAGE_LINES];
	struct buf **lines;
	unsigned count;
	unsigned own;
};

/*
 * One reader followed by read-ahead: the open description it reads through
 * (owner, NULL for a free entry), the disk it opened and that disk's
 * physical device, its stream, and when it last read (for reuse).
 */
struct buf_readahead_entry {
	const void *owner;
	struct disk *disk;
	struct disk *leaf;
	struct buf_readahead_stream stream;
	uint64_t used;
};

/*
 * One run to read ahead: count blocks at block of the disk an owner opened,
 * proposed by the owner's stream in a generation.  The job holds a
 * reference to the disk.
 */
struct buf_readahead_job {
	const void *owner;
	struct disk *disk;
	struct disk *leaf;
	uint64_t block;
	uint32_t count;
	uint64_t generation;
	uint64_t queued_ns;
};

/*
 * One read-ahead thread: its run bookkeeping, and the physical device of
 * the job it holds (NULL while it waits for one).
 */
struct buf_readahead_worker {
	struct thread *thread;
	struct buf_run_scratch *scratch;
	struct disk *running;
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
 * The header slabs that have a free slot.
 *
 * Changed only under cache_lock.  A full slab is on no list; its headers
 * lead back to it.  Zero is an index with no slab.
 */
static struct buf_slab_index slab_index;

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
 * The bound of dirty memory, a 1/BUF_DIRTY_SHARE part of the cache's cap;
 * written under cache_lock with the cap, read atomically.
 */
static volatile uint64_t dirty_limit_bytes;

/*
 * The flusher's thread.
 *
 * It is made by the first buf_flusher_start() and runs until the machine
 * stops; NULL until then.
 */
static struct thread *flusher_thread;

/*
 * Whether a caller has claimed the making of the flusher.
 *
 * The first buf_flusher_start() sets it and makes the thread; it is cleared
 * again only when the thread could not be made, so that a later call tries.
 */
static atomic_uint_t flusher_started;

/*
 * Serializes the hook registry against the flusher's calls.
 *
 * The flusher holds it while it calls the hooks, so a hook removed under
 * it has made its last call.  Hooks take filesystem locks, so it ranks
 * below them.
 */
static struct mutex flusher_hooks_lock;

/* Whether flusher_hooks_lock has been initialized; set once, never cleared. */
static atomic_uint_t flusher_hooks_ready;

/*
 * The functions the flusher calls each interval before it writes aged
 * buffers, and the argument each is called with.
 *
 * A slot is empty when its function is NULL.  Both arrays change only
 * under flusher_hooks_lock; a filesystem fills a slot while its journal
 * is in service and empties it before the journal goes.
 */
static void (*flusher_hooks[BUF_FLUSHER_HOOKS])(void *);
static void *flusher_hook_arguments[BUF_FLUSHER_HOOKS];

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

/*
 * Guards the read-ahead streams, the job queue, the workers' running
 * devices and every disk's d_readahead_stops.  Nothing else is taken under
 * it, and it is never taken under the disk registry's lock.
 */
static struct spinlock readahead_lock;

/* The workers sleep here for a job; readahead_lock. */
static struct wait_queue readahead_work;

/* A stopper sleeps here for a worker to finish its job; readahead_lock. */
static struct wait_queue readahead_changed;

/* The readers followed; readahead_lock. */
static struct buf_readahead_entry readahead_streams[BUF_READAHEAD_STREAMS];

/* Counts reads, to find the entry used longest ago; readahead_lock. */
static uint64_t readahead_clock;

/* The jobs waiting, a ring from readahead_queue_head; readahead_lock. */
static struct buf_readahead_job readahead_queue[BUF_READAHEAD_QUEUE];
static unsigned readahead_queue_head;
static unsigned readahead_queue_count;

/* The workers; the first readahead_ready of them run.  readahead_lock. */
static struct buf_readahead_worker readahead_workers[BUF_READAHEAD_WORKERS];
static unsigned readahead_ready;

/* Whether a reader has claimed the making of the workers; set once. */
static atomic_uint_t readahead_started;

/*
 * Whether the workers are asked to write the oldest dirty lines back, and
 * whether a thread is gathering a cluster (others wait on
 * readahead_changed, so that two do not split one run); readahead_lock.
 */
static volatile unsigned writeback_wanted;
static unsigned cluster_picking;

/* The devices the workers wrote to since the flusher's last interval, referenced; readahead_lock. */
static struct disk *writeback_unflushed[BUF_FLUSH_LEAVES];
static unsigned writeback_unflushed_count;

/*
 * What read-ahead did, for its log line: jobs queued and finished, bytes
 * read, and jobs that read nothing because the queue was full, read-ahead
 * was stopped, the stream had moved on, the first line was already there,
 * the disk refused cached access, or the device failed.
 */
static volatile uint64_t readahead_queued;
static volatile uint64_t readahead_finished;
static volatile uint64_t readahead_read_bytes;
static volatile uint64_t readahead_full;
static volatile uint64_t readahead_stopped;
static volatile uint64_t readahead_stale;
static volatile uint64_t readahead_present;
static volatile uint64_t readahead_refused;
static volatile uint64_t readahead_errors;

/*
 * Why jobs read nothing: the run's shape, no memory for a line, a line in
 * use, a line already there (counted by the run path that gave up).
 */
static volatile uint64_t readahead_skip_geometry;
static volatile uint64_t readahead_skip_memory;
static volatile uint64_t readahead_skip_busy;
static volatile uint64_t readahead_skip_hit;

/*
 * Where a prefetch's time goes, in nanoseconds since the last log line:
 * taking and allocating its lines, and the device transfer.
 */
static uint64_t readahead_prepare_ns;
static uint64_t readahead_transfer_ns;

/*
 * How long jobs waited in the queue and ran, in nanoseconds, summed and at
 * most, over the jobs since the last log line; readahead_lock.
 */
static uint64_t readahead_wait_ns;
static uint64_t readahead_wait_max_ns;
static uint64_t readahead_run_ns;
static uint64_t readahead_run_max_ns;

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
static void flusher(void *argument);
static struct buf *oldest_aged(uint64_t cutoff);
static int flush_aged(uint64_t cutoff);
static int writes_delayed(const struct disk *disk, const struct io_context *context, int raw);
static int write_range(struct disk *disk, uint64_t block, uint32_t count, const void *data, const struct io_context *context, int raw);
static int writeback_line(struct buf *buffer, const struct io_context *context, int delayed);
static int writeback_line_whole(struct buf *buffer, const struct io_context *drain, int delayed);
static int writeback_line_runs(struct buf *buffer, const struct io_context *drain, const struct backing_claim *writer);
static int write_lines(struct disk *disk, uint64_t block, uint32_t count, const void *data, uint64_t pin);
static void flusher_hooks_init(void);
static int reserve_bytes(size_t size, int metadata, int needed);
static void cancel_reservation(size_t size, int metadata);
static void commit_reservation(size_t size, int metadata);
static int alloc_pmem(size_t size, struct kern_pmem *memory, int metadata, int needed);
static int slab_grow(int needed);
static struct buf * alloc_metadata(int needed);
static void free_metadata(struct buf *buffer);
static void free_buffer(struct buf *buffer);
static int busy_acquire(struct buf *buffer);
static void drop_caller_reference(struct buf *buffer);
static int read_buffer(struct buf *buffer);
static int acquire_line(struct disk *disk, uint64_t block, int read_data, struct buf **result);
static int reference_line(struct disk *disk, uint64_t block, int needed, struct buf **result);
static int transfer_run(struct disk *disk, uint64_t block, uint64_t remaining, void *data, int write, uint32_t *transferred, const struct io_context *context);
static int transfer_run_lines(struct disk *disk, uint64_t block, uint64_t remaining, void *data, int write, uint32_t *transferred, const struct io_context *context, struct buf **lines, uint64_t *generations, uint64_t *pages, unsigned limit, int prefetch);
static void run_record(int prefetch, enum io_stat_event event, uint64_t bytes);
static void finish_unread_line(struct buf *buffer);
static void release_run(struct buf **lines, unsigned count, unsigned busy);
static void finish_run_line(struct buf *buffer, uint64_t generation, int write, int error);
static int disk_cache_range(struct disk *disk, struct disk **leaf_out, uint64_t *start_out, uint64_t *end_out);
static int evict_one(struct disk *disk, uint64_t start, uint64_t end, int range, unsigned flags, size_t *freed);
static int evict_take_locked(struct buf *buffer, unsigned flags, int range);
static int writeback_one_reclaimable(void);
static void dirty_link(struct buf *buffer);
static void mark_dirty(struct buf *buffer, uint64_t offset, uint64_t count, int pinned);
static struct buf *unjournaled_reference(struct disk *disk, uint64_t start, uint64_t end, uint64_t after);
static int unjournaled_write_line(struct buf *buffer, uint64_t start, uint64_t end);
static void dirty_clear(struct buf *buffer);
static void dirty_unlink_locked(struct buf *buffer);
static struct buf *dirty_reference(struct disk *disk, uint64_t start, uint64_t end, int reclaim);
static struct disk *readahead_leaf(struct disk *disk);
static int readahead_start(void);
static struct buf_readahead_entry *readahead_entry(const void *owner, struct disk *disk, struct disk *leaf);
static struct buf_readahead_entry *readahead_current(const void *owner, uint64_t generation);
static void readahead_worker(void *argument);
static void readahead_run(struct buf_readahead_worker *worker, const struct buf_readahead_job *job);
static int readahead_running(struct disk *leaf);
static uint64_t readahead_now_ns(void);
static void readahead_account(uint64_t *sum, uint64_t value);
static uint32_t copy_cached_lines(struct disk *disk, uint64_t block, uint64_t remaining, uint8_t *out);
static uint32_t write_cached_lines(const struct disk *disk, struct disk *leaf, uint64_t block, uint64_t remaining, const uint8_t *in);
static struct buf *recycle_line_locked(struct disk *disk, uint64_t line_block, uint64_t line_blocks, uint64_t line_bytes, int needed);
static uint64_t dirty_limit_for(uint64_t cap);
static int cluster_line_locked(const struct buf *buffer, uint64_t line_blocks);
static int cluster_take(struct buf *buffer, uint64_t line_blocks);
static int cluster_pick_begin(void);
static void cluster_pick_end(void);
static void cluster_gather(struct buf *buffer, struct buf_cluster *cluster, uint64_t line_blocks);
static void cluster_release(struct buf_cluster *cluster);
static int cluster_write(struct buf_cluster *cluster, uint64_t line_blocks, int *sent);
static int writeback_cluster(struct buf *buffer);
static int leaf_takes_pages(const struct disk *disk);
static uint64_t dirty_limit_of(const struct disk *disk);
static struct buf *writeback_oldest(struct disk *leaf, int worker);
static void writeback_flush_note(struct disk *leaf);
static void writeback_flush_noted(void);
static int writeback_oldest_cluster(struct disk *leaf, struct buf_readahead_worker *worker);
static void writeback_assist(const struct disk *disk, struct disk *leaf, const struct io_context *context, int raw);
static void writeback_kick(const struct disk *disk);
static void writeback_background(struct buf_readahead_worker *worker);

/*
 * Initializes the buffer cache with its default byte cap.
 *
 * The cap is the configured size, or an eighth of physical memory,
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
	int error;

	/* Initializes only once. */
	if (cache_initialized)
		return 0;

	/* Sets up the spin locks and the flusher's hook registry. */
	spin_init(&cache_lock, LOCK_RANK_BUFCACHE, "buffer cache");
	spin_init(&dirty_index_lock, LOCK_RANK_DIRTY_INDEX, "dirty buffer index");
	spin_init(&readahead_lock, LOCK_RANK_DISK, "buffer read-ahead");
	waitq_init(&readahead_work, "buffer read-ahead work");
	waitq_init(&readahead_changed, "buffer read-ahead stop");
	flusher_hooks_init();

	/* Sets up the lock that serializes changes to the cache's size. */
	error = mutex_init(&cache_control, LOCK_RANK_BUFCACHE, "buffer cache control");
	if (error != 0)
		return ENOMEM;

	/* Sets up the lock that serializes admission of new buffers. */
	error = mutex_init(&cache_admission, LOCK_RANK_BUFCACHE, "buffer cache admission");
	if (error != 0)
		return ENOMEM;

	/* Empties the hash table. */
	kern_memset(cache_hash, 0, sizeof(cache_hash));

	/* Sizes the cache. */
#if CONFIG_BUF_CACHE_KIB > 0
	value = (uint64_t)CONFIG_BUF_CACHE_KIB * 1024U;
#else
	total = hal_pmem_get_total_size();
	value = total / 8U;
#endif
	if (value < BUF_MIN_BYTES)
		value = BUF_MIN_BYTES;
	value &= ~(uint64_t)(KERN_PAGE_SIZE - 1U);
	cache_max_bytes = value;
	atomic_u64_store_release(&dirty_limit_bytes, dirty_limit_for(value));

	/* Allocates the first header slab. */
	cache_initialized = 1;
	error = slab_grow(0);
	if (error != 0) {
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
	/* Marks the buffer dirty, leaving its record of unjournaled blocks as it is. */
	mark_dirty(buffer, 0, 0, 0);
}

/*
 * Marks a buffer dirty after a write of count blocks from the line's block
 * offset, and records them in the same step: an ordinary write's blocks as
 * unjournaled, a journal's pinned write's blocks as the journal's
 * (ws073-p053).  A count of zero leaves the record as it is.
 */
static void
mark_dirty(
	struct buf *buffer,
	uint64_t offset,
	uint64_t count,
	int pinned)
{
	unsigned long irq;

	/* Ignores a missing buffer. */
	if (buffer == NULL)
		return;

	/* Records the written blocks under the line's lock. */
	irq = spin_lock_irqsave(&buffer->b_lock);
	if (count != 0)
		buffer->b_unjournaled = buf_unjournaled_after_write(buffer->b_unjournaled, offset, count, pinned);

	/* Advances the generation, skipping zero, and sets the flags. */
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
		buffer->b_dirty_tick = sched_ticks();
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

	/* Writes the line back on behalf of whoever dirtied it earlier, with its dirty neighbours. */
	error = writeback_cluster(buffer);
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
	int error;

	/* Writes the line inside the caller's own write. */
	error = writeback_line(buffer, context, 0);
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
	kern_memset(view, 0, sizeof(*view));
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
		error = reference_line(leaf, mapped, view->count == 0, &buffer);
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
		/* Copies a run of lines already in the cache, a few at a time. */
		run_blocks = copy_cached_lines(leaf, mapped, end - mapped, out);
		if (run_blocks != 0) {
			mapped += run_blocks;
			out += (size_t)run_blocks * leaf->d_block_size;
			continue;
		}

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
		kern_memcpy_ram(out, (uint8_t *)buffer->b_data +
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
	int error;

	/* Writes as the disk's own delay allows. */
	error = write_range(disk, block, count, data, context, 0);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes blocks of a raw block device that its opener alone uses, leaving
 * them dirty in the cache as a write-cached disk's would be (the flusher,
 * the write-back workers, fsync and the last close write them), so that a
 * writer's copies and the device's writes overlap.
 */
int
buf_write_delayed(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data)
{
	int error;

	/* Writes with the delay a raw writer may have. */
	error = write_range(disk, block, count, data, NULL, 1);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes blocks through the cache: delayed on a write-cached disk, or for a
 * raw writer (raw), and otherwise written through.
 */
static int
write_range(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data,
	const struct io_context *context,
	int raw)
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
	int delayed;
	unsigned batch_wait;
	uint32_t run_blocks;

	in = data;
	batch_wait = 0;

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
		/*
		 * A write-cached disk takes the write into its lines, which
		 * stay dirty; only a write-through one sends a run straight
		 * to the device.
		 */
		run_blocks = 0;
		writeback_assist(disk, leaf, context, raw);
		delayed = writes_delayed(disk, context, raw);
		error = 0;
		if (!delayed)
			error = transfer_run(leaf, mapped, end - mapped, (void *)in, 1, &run_blocks, context);
		if (error != 0)
			return error;
		if (run_blocks != 0) {
			mapped += run_blocks;
			in += (size_t)run_blocks * leaf->d_block_size;
			continue;
		}

		/*
		 * A delayed write of whole lines takes several at once, under
		 * one hold of the cache lock to look them up and one to give
		 * them back.  When it cannot (the cache has room to allocate, a
		 * line is busy, the start is inside a line), the next
		 * BUF_RUN_LINES lines go one at a time before it is tried again,
		 * so that a cache with room does not pay for both.
		 */
		if (delayed && batch_wait == 0) {
			run_blocks = write_cached_lines(disk, leaf, mapped, end - mapped, in);
			if (run_blocks == 0)
				batch_wait = BUF_RUN_LINES;
		}
		if (run_blocks != 0) {
			mapped += run_blocks;
			in += (size_t)run_blocks * leaf->d_block_size;
			writeback_kick(disk);
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

		kern_memcpy_ram((uint8_t *)buffer->b_data +
		       offset_blocks * leaf->d_block_size, in,
		       (size_t)(amount_blocks * leaf->d_block_size));

		/*
		 * A write-cached disk keeps the line dirty for the flusher,
		 * unless dirty memory is already at its bound.  The blocks are
		 * recorded as written outside any journal, which a journal's
		 * pin on the line would otherwise hold back (ws073-p053).
		 */
		mark_dirty(buffer, offset_blocks, amount_blocks, 0);
		delayed = writes_delayed(disk, context, raw);
		if (!delayed && ((disk->d_flags & DISK_WRITE_CACHED) != 0 || raw)) {
			writeback_assist(disk, leaf, context, raw);
			delayed = writes_delayed(disk, context, raw);
		}
		error = 0;
		if (!delayed)
			error = buf_writeback_context(buffer, context);
		buf_release(buffer);
		if (delayed)
			writeback_kick(disk);

		if (error != 0)
			return error;

		in += amount_blocks * leaf->d_block_size;
		mapped += amount_blocks;

		/* One more line has gone one at a time before the next batch. */
		if (batch_wait != 0)
			batch_wait--;
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

	/*
	 * Refuse before any dirty data is discarded.  The caller keeps users
	 * excluded across this check and eviction; this scan alone is not an
	 * admission gate.
	 */
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
	kern_memset(stats, 0, sizeof(*stats));
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
	atomic_u64_store_release(&dirty_limit_bytes, dirty_limit_for(value));

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
			atomic_u64_store_release(&dirty_limit_bytes, dirty_limit_for(old));
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

/*
 * Starts the thread that writes out aged dirty buffers of write-cached
 * disks; later calls do nothing.
 */
int
buf_flusher_start(
	void)
{
	int first;
	int error;

	/* Prepares the hook registry the flusher reads. */
	flusher_hooks_init();

	/* Only the first caller makes the thread. */
	first = atomic_try_acquire_zero(&flusher_started);
	if (!first)
		return 0;

	/* Makes the thread; a failure lets a later caller try again. */
	error = kthread_create(flusher, NULL, SCHED_PRIORITY_DEFAULT, &flusher_thread);
	if (error != 0) {
		atomic_store_release(&flusher_started, 0);
		return error;
	}

	/* Runs the flusher from here on. */
	thread_start(flusher_thread);

	/* Succeeded: the flusher runs. */
	return 0;
}

/*
 * Adds or removes a function the flusher calls each interval.
 *
 * A removal returns only after the function's last call has finished.
 */
int
buf_flusher_hook(
	void (*hook)(void *),
	void *argument,
	int add)
{
	unsigned index;
	int error;

	/* Prepares the registry on first use. */
	flusher_hooks_init();

	/* Changes the registry while no hook runs. */
	mutex_lock(&flusher_hooks_lock);

	/* Finds the slot to change; a full registry, or one without the hook, has none. */
	error = ENOSPC;
	for (index = 0; index < BUF_FLUSHER_HOOKS; index++) {
		/* An addition takes the first empty slot. */
		if (add && flusher_hooks[index] == NULL) {
			flusher_hooks[index] = hook;
			flusher_hook_arguments[index] = argument;
			error = 0;
			break;
		}

		/* A removal empties the slot that holds the hook and its argument. */
		if (!add &&
		    flusher_hooks[index] == hook &&
		    flusher_hook_arguments[index] == argument) {
			flusher_hooks[index] = NULL;
			flusher_hook_arguments[index] = NULL;
			error = 0;
			break;
		}
	}

	mutex_unlock(&flusher_hooks_lock);

	/* Reports why the registry could not change. */
	if (error != 0)
		return error;

	/* Succeeded: the registry changed. */
	return 0;
}

/*
 * Writes into the cache and pins the lines for a journal: they stay dirty
 * and nothing writes them to the disk until buf_unpin() with the same tag
 * releases them.
 */
int
buf_write_pinned(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data,
	uint64_t pin)
{
	int error;

	/* A pin of zero would mean no pin at all. */
	if (pin == 0)
		return EINVAL;

	/* Writes the lines, pinned and dirty, without writing them back. */
	error = write_lines(disk, block, count, data, pin);
	if (error != 0)
		return error;

	/* Succeeded: the lines hold the data until they are unpinned. */
	return 0;
}

/*
 * Releases the journal pins of the lines a range covers, where the pin
 * carries the given tag; those lines are then ordinary dirty lines.  The
 * lines a later tag holds are counted in *kept when the caller asks.
 */
int
buf_unpin(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	uint64_t pin,
	unsigned *kept)
{
	struct disk *leaf;
	struct buf *buffer;
	uint64_t mapped;
	uint64_t end;
	uint64_t line_blocks;
	uint64_t line_start;
	unsigned long irq;
	int error;

	/* An empty range has no lines to unpin. */
	if (count == 0)
		return 0;

	/* Resolves the range to the leaf disk the lines belong to. */
	error = disk_resolve_range(disk, block, count, &leaf, &mapped);
	if (error != 0)
		return error;

	/* Measures a line in blocks: a page, or one block larger than a page. */
	line_blocks = KERN_PAGE_SIZE / leaf->d_block_size;
	if (leaf->d_block_size > KERN_PAGE_SIZE)
		line_blocks = 1;

	/* Clears the pin of each line the range covers. */
	end = mapped + count;
	line_start = mapped - mapped % line_blocks;
	while (line_start < end) {
		/* Takes the line. */
		error = reference_line(leaf, line_start, 0, &buffer);
		if (error != 0)
			return error;

		/*
		 * Clears the pin: write-back and the flusher, which left the
		 * line alone while the journal held it, may write it home now.
		 * A line a later transaction pinned again stays pinned for that
		 * transaction's own commit.
		 */
		irq = spin_lock_irqsave(&buffer->b_lock);
		if (buffer->b_journal_pin == pin)
			buffer->b_journal_pin = 0;
		else if (buffer->b_journal_pin != 0 && kept != NULL)
			(*kept)++;
		spin_unlock_irqrestore(&buffer->b_lock, irq);

		/* Lets the line go and steps to the next one. */
		drop_caller_reference(buffer);
		line_start += line_blocks;
	}

	/* Succeeded: the lines are ordinary dirty lines now. */
	return 0;
}

/*
 * Writes, beneath the cache, the unjournaled blocks of a disk's dirty
 * lines (ws073-p053, BUG-163): what ordinary writes left, which the
 * journal does not carry, so that a line the journal pins does not hold it
 * back.  Each line is visited once, in the order of its block; the busy
 * owner of a line is the only one that changes its data and its record,
 * so a run written here is the line's newest content of those blocks, and
 * a later write-back of the whole line writes the same or newer.
 */
int
buf_write_unjournaled(
	struct disk *disk)
{
	struct disk *leaf;
	struct buf *buffer;
	uint64_t start;
	uint64_t end;
	uint64_t after;
	int error;

	/* Resolves the disk's block range on its leaf. */
	error = disk_cache_range(disk, &leaf, &start, &end);
	if (error != 0)
		return error;

	/* Takes the lines with unjournaled blocks one at a time, in the order of their blocks. */
	after = start;
	for (;;) {
		buffer = unjournaled_reference(leaf, start, end, after);
		if (buffer == NULL)
			break;
		after = buffer->b_block + buffer->b_block_count;

		/* Writes its unjournaled blocks as its busy owner. */
		error = busy_acquire(buffer);
		if (error == 0)
			error = unjournaled_write_line(buffer, start, end);
		buf_release(buffer);

		/* Reports why the line could not be written. */
		if (error != 0)
			return error;
	}

	/* Succeeded: the unjournaled blocks are on their way to the disk. */
	return 0;
}

/*
 * Writes the dirty lines of a range back to the disk, leaving a line a
 * journal pins alone and not flushing the device.  A journal uses it to
 * send a committed transaction's homes on their way (ws073-p051).
 */
int
buf_writeback_range(
	struct disk *disk,
	uint64_t block,
	uint32_t count)
{
	struct disk *leaf;
	struct buf *buffer;
	uint64_t mapped;
	uint64_t end;
	uint64_t line_blocks;
	uint64_t line_start;
	unsigned long irq;
	int error;

	/* An empty range has no lines to write. */
	if (count == 0)
		return 0;

	/* Resolves the range to the leaf disk the lines belong to. */
	error = disk_resolve_range(disk, block, count, &leaf, &mapped);
	if (error != 0)
		return error;

	/* Measures a line in blocks: a page, or one block larger than a page. */
	line_blocks = KERN_PAGE_SIZE / leaf->d_block_size;
	if (leaf->d_block_size > KERN_PAGE_SIZE)
		line_blocks = 1;

	/* Writes each line the range covers that is dirty and not pinned. */
	end = mapped + count;
	line_start = mapped - mapped % line_blocks;
	while (line_start < end) {
		/*
		 * Takes the line only when the cache holds it: a missing line has
		 * nothing to write, and bringing one in could fail for memory.
		 */
		irq = spin_lock_irqsave(&cache_lock);

		buffer = hash_find_locked(leaf, line_start);
		if (buffer != NULL) {
			refcount_get(&buffer->b_refs);
			lru_remove_locked(buffer);
		}

		spin_unlock_irqrestore(&cache_lock, irq);

		/* Steps over a line the cache does not hold. */
		if (buffer == NULL) {
			line_start += line_blocks;
			continue;
		}

		/* Becomes the line's busy owner, so that its data holds still. */
		error = busy_acquire(buffer);
		if (error != 0) {
			drop_caller_reference(buffer);
			return error;
		}

		/* Writes it back as its own dirtier would; a clean or pinned line is left as it is. */
		error = buf_writeback(buffer);
		buf_release(buffer);

		/* Reports why the line could not be written. */
		if (error != 0)
			return error;

		/* Steps to the next line. */
		line_start += line_blocks;
	}

	/* Succeeded: the range's dirty lines are on their way to the disk. */
	return 0;
}

/*
 * Writes the dirty lines of a range that the cache holds back to the disk
 * (BUG-307).  A missing line has nothing to write and is not brought in, so
 * a large file's sync takes no memory for the blocks it does not have in
 * the cache.  A line a journal pins keeps its journaled blocks for the
 * commit; the blocks of the range that ordinary writes left in it are
 * written beneath the cache, as buf_write_unjournaled() writes them.  The
 * device is not flushed.
 */
int
buf_writeback_cached(
	struct disk *disk,
	uint64_t block,
	uint32_t count)
{
	struct disk *leaf;
	struct buf *buffer;
	uint64_t mapped;
	uint64_t end;
	uint64_t line_blocks;
	uint64_t line_start;
	unsigned long irq;
	int error;

	/* An empty range has no lines to write. */
	if (count == 0)
		return 0;

	/* Resolves the range to the leaf disk the lines belong to. */
	error = disk_resolve_range(disk, block, count, &leaf, &mapped);
	if (error != 0)
		return error;

	/* Measures a line in blocks: a page, or one block larger than a page. */
	line_blocks = KERN_PAGE_SIZE / leaf->d_block_size;
	if (leaf->d_block_size > KERN_PAGE_SIZE)
		line_blocks = 1;

	/* Writes each line of the range that the cache holds. */
	end = mapped + count;
	line_start = mapped - mapped % line_blocks;
	while (line_start < end) {
		/* Takes the line only when the cache already holds it. */
		irq = spin_lock_irqsave(&cache_lock);

		buffer = hash_find_locked(leaf, line_start);
		if (buffer != NULL) {
			refcount_get(&buffer->b_refs);
			lru_remove_locked(buffer);
		}

		spin_unlock_irqrestore(&cache_lock, irq);

		/* A line the cache does not hold has nothing waiting to be written. */
		if (buffer == NULL) {
			line_start += line_blocks;
			continue;
		}

		/* Becomes the line's busy owner, so that its data holds still. */
		error = busy_acquire(buffer);
		if (error != 0) {
			drop_caller_reference(buffer);
			return error;
		}

		/*
		 * Writes the whole line when no journal pins it; a pinned line
		 * is left whole, and only the range's ordinary blocks in it are
		 * written beneath the cache.  After a whole write the line is
		 * clean and has no such blocks left.
		 */
		error = buf_writeback(buffer);
		if (error == 0)
			error = unjournaled_write_line(buffer, mapped, end);
		buf_release(buffer);

		/* Reports why the line could not be written. */
		if (error != 0)
			return error;

		/* Steps to the next line. */
		line_start += line_blocks;
	}

	/* Succeeded: the range's cached dirty blocks are on their way to the disk. */
	return 0;
}

/*
 * Brings the cache's lines of a range in line with the disk before a
 * transfer that bypasses the cache (O_DIRECT).
 *
 * Every dirty line the range touches is written back first; a line a
 * journal pins keeps its journaled blocks, and only the range's ordinary
 * blocks in it are written beneath the cache, as buf_writeback_cached()
 * does.  With invalidate, every line of the range is also made invalid, so
 * that the next cached read reads what the transfer wrote; a line a
 * journal still pins cannot be, and the range is refused with EBUSY.  A
 * line in transfer is waited for, one line at a time, owning no other.
 * A line the cache does not hold costs one hash lookup, BUF_GATHER_HOLD
 * of them to a hold of the cache lock.  The device is not flushed.
 */
int
buf_direct_prepare(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	int invalidate)
{
	struct buf *lines[BUF_GATHER_HOLD];
	struct disk *leaf;
	uint64_t mapped;
	uint64_t end;
	uint64_t line_blocks;
	uint64_t line_start;
	unsigned found;
	unsigned held;
	unsigned index;
	unsigned long irq;
	uint64_t sequence;
	int error;

	/* An empty range has no lines. */
	if (count == 0)
		return 0;

	/* Resolves the range to the leaf disk the lines belong to. */
	error = disk_resolve_range(disk, block, count, &leaf, &mapped);
	if (error != 0)
		return error;

	/* Measures a line in blocks: a page, or one block larger than a page. */
	line_blocks = KERN_PAGE_SIZE / leaf->d_block_size;
	if (leaf->d_block_size > KERN_PAGE_SIZE)
		line_blocks = 1;

	end = mapped + count;
	line_start = mapped - mapped % line_blocks;
	error = 0;
	while (line_start < end) {
		/* References the lines the cache holds among the next ones. */
		found = 0;
		irq = spin_lock_irqsave(&cache_lock);

		for (held = 0; held < BUF_GATHER_HOLD && line_start < end; held++) {
			lines[found] = hash_find_locked(leaf, line_start);
			if (lines[found] != NULL) {
				refcount_get(&lines[found]->b_refs);
				lru_remove_locked(lines[found]);
				found++;
			}
			line_start += line_blocks;
		}

		spin_unlock_irqrestore(&cache_lock, irq);

		/* Brings each held line in line with the disk, one at a time. */
		for (index = 0; index < found; index++) {
			if (error != 0) {
				drop_caller_reference(lines[index]);
				continue;
			}

			/* Owns the line and waits until no transfer of it is in flight. */
			error = busy_acquire(lines[index]);
			if (error != 0) {
				drop_caller_reference(lines[index]);
				continue;
			}
			irq = spin_lock_irqsave(&lines[index]->b_lock);
			while (lines[index]->b_io_inflight) {
				sequence = waitq_sequence(&lines[index]->b_waitq);
				(void)waitq_sleep(&lines[index]->b_waitq,
				    &lines[index]->b_lock, sequence, 0, 0);
			}
			spin_unlock_irqrestore(&lines[index]->b_lock, irq);

			/* Writes what the line holds that the disk does not have yet. */
			error = buf_writeback(lines[index]);
			if (error == 0)
				error = unjournaled_write_line(lines[index], mapped, end);

			/* Leaves the line to be read again, when it holds nothing unwritten. */
			if (error == 0 && invalidate) {
				irq = spin_lock_irqsave(&lines[index]->b_lock);

				if ((lines[index]->b_flags & BUF_DIRTY) != 0) {
					error = EBUSY;
				} else {
					lines[index]->b_flags &= ~(BUF_VALID | BUF_ERROR);
					if (lines[index]->b_generation == UINT64_MAX)
						lines[index]->b_flags |= BUF_GENERATION_EXHAUSTED;
					lines[index]->b_generation++;
					if (lines[index]->b_generation == 0)
						lines[index]->b_generation++;
				}

				spin_unlock_irqrestore(&lines[index]->b_lock, irq);
			}
			buf_release(lines[index]);
		}
		if (error != 0)
			return error;
	}

	/* Succeeded: the disk holds everything the cache held of the range. */
	return 0;
}

/*
 * Records a completed read of an open block device and reads ahead of a
 * sequential reader.
 *
 * Only a disk whose driver moves pages itself is read ahead, only where
 * nothing else that is open on the device can see the blocks, and never
 * while a management operation has stopped read-ahead on the device.  The
 * reader never waits: a full queue just reads less ahead.
 */
void
buf_readahead_observe(
	const void *owner,
	struct disk *disk,
	uint64_t block,
	uint32_t count)
{
	uint64_t bounds[BUF_READAHEAD_SPLIT_MAX + 1U];
	struct buf_readahead_job *job;
	struct buf_readahead_entry *entry;
	struct disk *leaf;
	struct disk *resolved;
	uint64_t window;
	uint64_t unit;
	uint64_t start;
	uint64_t length;
	uint64_t mapped;
	uint64_t position;
	uint64_t end;
	uint64_t boundary;
	uint64_t generation;
	uint64_t lines;
	uint64_t now;
	unsigned job_count;
	unsigned queued;
	unsigned slot;
	unsigned index;
	unsigned long irq;
	int supported;
	int private;
	int error;

	/* Reads ahead only of a page-or-smaller block size that divides a page. */
	if (!cache_initialized || owner == NULL || disk == NULL || count == 0)
		return;
	if (disk->d_block_size == 0 ||
	    disk->d_block_size > KERN_PAGE_SIZE ||
	    KERN_PAGE_SIZE % disk->d_block_size != 0)
		return;

	/* Sizes the window in whole runs, an eighth of the cache at most. */
	window = BUF_READAHEAD_BYTES;
	if (window > cache_max_bytes / 8U)
		window = cache_max_bytes / 8U;
	window -= window % BUF_READAHEAD_RUN_BYTES;
	if (window == 0)
		return;
	window /= disk->d_block_size;
	unit = BUF_READAHEAD_RUN_BYTES / disk->d_block_size;

	/* Reads ahead only through a driver that moves pages itself. */
	supported = disk_page_vector_supported(disk);
	if (!supported)
		return;

	/* Makes the workers on the first sequential reader's first read. */
	error = readahead_start();
	if (error != 0)
		return;

	/* Plans in the reader's stream what to read ahead. */
	leaf = readahead_leaf(disk);
	irq = spin_lock_irqsave(&readahead_lock);

	entry = readahead_entry(owner, disk, leaf);
	buf_readahead_plan(&entry->stream, block, count, disk->d_block_count,
	    window, unit, &start, &length);
	generation = entry->stream.generation;
	if (length != 0 && leaf->d_readahead_stops != 0) {
		buf_readahead_rewind(&entry->stream);
		length = 0;
	}
	if (length != 0)
		buf_readahead_taken(&entry->stream, start + length);

	spin_unlock_irqrestore(&readahead_lock, irq);

	if (length == 0)
		return;

	/* Reads ahead only where the blocks are the reader's alone. */
	private = disk_readahead_private(disk, start, length);
	error = 0;
	if (private)
		error = disk_resolve_range(disk, start, (uint32_t)length, &resolved, &mapped);
	if (!private || error != 0) {
		irq = spin_lock_irqsave(&readahead_lock);

		entry = readahead_current(owner, generation);
		if (entry != NULL)
			buf_readahead_rewind(&entry->stream);

		spin_unlock_irqrestore(&readahead_lock, irq);
		return;
	}

	/*
	 * Splits the window at the device's run boundaries, as a reader's runs
	 * fall, from the first whole line on.
	 */
	job_count = 0;
	end = start + length;
	position = start;
	lines = KERN_PAGE_SIZE / disk->d_block_size;
	if (mapped % lines != 0)
		position += lines - mapped % lines;
	bounds[0] = position;
	while (position < end && job_count < BUF_READAHEAD_SPLIT_MAX) {
		boundary = mapped + (position - start);
		boundary = boundary - boundary % unit + unit;
		position += boundary - (mapped + (position - start));
		if (position > end)
			position = end;
		job_count++;
		bounds[job_count] = position;
	}

	/* Each job holds the disk, taken outside the read-ahead lock. */
	for (index = 0; index < job_count; index++)
		disk_ref(disk);

	/* Queues what fits while the stream and the device still want it. */
	queued = 0;
	now = readahead_now_ns();
	irq = spin_lock_irqsave(&readahead_lock);

	entry = readahead_current(owner, generation);
	while (entry != NULL &&
	    queued < job_count &&
	    readahead_queue_count < BUF_READAHEAD_QUEUE &&
	    leaf->d_readahead_stops == 0) {
		slot = (readahead_queue_head + readahead_queue_count) % BUF_READAHEAD_QUEUE;
		job = &readahead_queue[slot];
		job->owner = owner;
		job->disk = disk;
		job->leaf = leaf;
		job->block = bounds[queued];
		job->count = (uint32_t)(bounds[queued + 1U] - bounds[queued]);
		job->generation = generation;
		job->queued_ns = now;
		readahead_queue_count++;
		queued++;
	}
	if (entry != NULL && queued < job_count)
		buf_readahead_rewind_to(&entry->stream, bounds[queued]);
	if (queued != 0)
		waitq_wake_all(&readahead_work);

	spin_unlock_irqrestore(&readahead_lock, irq);

	/* Gives back the disk for every job that did not fit. */
	for (index = queued; index < job_count; index++)
		disk_release(disk);
	stat_add(&readahead_queued, queued);
	stat_add(&readahead_full, job_count - queued);
}

/*
 * How far ahead of its reader a file system should read: BUF_FS_READAHEAD_BYTES,
 * an eighth of the cache at most, in whole runs; zero when the cache is too
 * small for read-ahead.
 */
uint64_t
buf_prefetch_window(
	void)
{
	uint64_t window;

	/* Takes the smaller bound, in whole runs. */
	window = BUF_FS_READAHEAD_BYTES;
	if (window > cache_max_bytes / 8U)
		window = cache_max_bytes / 8U;
	window -= window % BUF_READAHEAD_RUN_BYTES;

	/* Succeeded: the window. */
	return window;
}

/*
 * Reads ahead blocks a file system knows hold data it is about to read: up
 * to count blocks of disk from block, as read-ahead jobs of whole runs.
 * The blocks are the file system's own, read through the cache as its
 * reads are, so the reader-private check of a raw device does not apply;
 * a stopped device, a full queue or a disk without page lists reads less.
 * Reports how many blocks from block were queued.
 */
uint32_t
buf_prefetch(
	struct disk *disk,
	uint64_t block,
	uint32_t count)
{
	struct buf_readahead_job *job;
	struct disk *leaf;
	struct disk *resolved;
	uint64_t mapped;
	uint64_t unit;
	uint64_t position;
	uint64_t end;
	uint64_t next;
	uint64_t now;
	unsigned long irq;
	int error;
	int fits;

	/* Reads ahead only of a page-or-smaller block size, through a driver that moves pages. */
	if (!cache_initialized || disk == NULL || count == 0 ||
	    disk->d_block_size == 0 ||
	    disk->d_block_size > KERN_PAGE_SIZE ||
	    KERN_PAGE_SIZE % disk->d_block_size != 0 ||
	    !leaf_takes_pages(disk))
		return 0;
	error = disk_resolve_range(disk, block, count, &resolved, &mapped);
	if (error != 0 || readahead_start() != 0)
		return 0;
	leaf = readahead_leaf(disk);
	unit = BUF_READAHEAD_RUN_BYTES / disk->d_block_size;

	/* Queues one job a run of the device, until the queue is full. */
	now = readahead_now_ns();
	position = block;
	end = block + count;
	while (position < end) {
		next = position + (unit - (mapped + (position - block)) % unit);
		if (next > end)
			next = end;

		/* Each job holds the disk, taken outside the read-ahead lock. */
		disk_ref(disk);

		irq = spin_lock_irqsave(&readahead_lock);

		fits = readahead_queue_count < BUF_READAHEAD_QUEUE && leaf->d_readahead_stops == 0;
		if (fits) {
			job = &readahead_queue[(readahead_queue_head + readahead_queue_count) % BUF_READAHEAD_QUEUE];
			job->owner = NULL;
			job->disk = disk;
			job->leaf = leaf;
			job->block = position;
			job->count = (uint32_t)(next - position);
			job->generation = 0;
			job->queued_ns = now;
			readahead_queue_count++;
			waitq_wake_all(&readahead_work);
		}

		spin_unlock_irqrestore(&readahead_lock, irq);

		if (!fits) {
			disk_release(disk);
			stat_add(&readahead_full, 1);
			break;
		}
		stat_add(&readahead_queued, 1);
		position = next;
	}

	/* Succeeded: the blocks queued. */
	return (uint32_t)(position - block);
}

/*
 * Drops an open description's read-ahead stream when the description closes.
 *
 * Its queued jobs become stale and read nothing.
 */
void
buf_readahead_forget(
	const void *owner)
{
	unsigned index;
	unsigned long irq;

	/* Nothing was followed before the cache was set up. */
	if (!cache_initialized || owner == NULL)
		return;

	irq = spin_lock_irqsave(&readahead_lock);

	/* Frees the owner's entry and makes its jobs stale. */
	for (index = 0; index < BUF_READAHEAD_STREAMS; index++) {
		if (readahead_streams[index].owner != owner)
			continue;
		buf_readahead_restart(&readahead_streams[index].stream);
		readahead_streams[index].owner = NULL;
		readahead_streams[index].disk = NULL;
		readahead_streams[index].leaf = NULL;
		readahead_streams[index].used = 0;
	}

	spin_unlock_irqrestore(&readahead_lock, irq);
}

/*
 * Stops read-ahead on a disk's physical device until buf_readahead_resume().
 *
 * The device's queued jobs are dropped and its streams restarted.  With
 * wait the caller sleeps, interruptibly, until no job runs on the device;
 * without it EBUSY reports a job still running.  Either way the stop stays
 * until the caller resumes, even when this reports an error, except that an
 * interrupted wait takes its stop back.  The caller must not hold the disk
 * registry's lock.
 */
int
buf_readahead_stop(
	struct disk *disk,
	int wait)
{
	struct disk *dropped[BUF_READAHEAD_QUEUE];
	struct buf_readahead_job *job;
	struct disk *leaf;
	unsigned dropped_count;
	unsigned kept;
	unsigned index;
	unsigned slot;
	unsigned long irq;
	uint64_t sequence;
	int busy;
	int error;

	/* A missing disk, or a cache not yet set up, has no read-ahead. */
	if (!cache_initialized || disk == NULL)
		return 0;

	/* Stops the device and takes its waiting jobs out of the queue. */
	leaf = readahead_leaf(disk);
	dropped_count = 0;
	error = 0;
	irq = spin_lock_irqsave(&readahead_lock);

	leaf->d_readahead_stops++;
	kept = 0;
	for (index = 0; index < readahead_queue_count; index++) {
		slot = (readahead_queue_head + index) % BUF_READAHEAD_QUEUE;
		job = &readahead_queue[slot];
		if (job->leaf == leaf) {
			dropped[dropped_count] = job->disk;
			dropped_count++;
			continue;
		}
		readahead_queue[(readahead_queue_head + kept) % BUF_READAHEAD_QUEUE] = *job;
		kept++;
	}
	readahead_queue_count = kept;

	/* Restarts the device's streams, so that their issued jobs are stale. */
	for (index = 0; index < BUF_READAHEAD_STREAMS; index++) {
		if (readahead_streams[index].owner != NULL &&
		    readahead_streams[index].leaf == leaf)
			buf_readahead_restart(&readahead_streams[index].stream);
	}

	/* Waits for, or reports, a job still running on the device. */
	busy = readahead_running(leaf);
	while (busy && wait) {
		sequence = waitq_sequence(&readahead_changed);
		error = waitq_sleep(&readahead_changed, &readahead_lock, sequence, 0,
		    WAITQ_INTERRUPTIBLE);
		if (error != 0 && error != EAGAIN) {
			leaf->d_readahead_stops--;
			break;
		}
		error = 0;
		busy = readahead_running(leaf);
	}
	if (busy && !wait)
		error = EBUSY;

	spin_unlock_irqrestore(&readahead_lock, irq);

	/* Gives back the disks the dropped jobs held. */
	for (index = 0; index < dropped_count; index++)
		disk_release(dropped[index]);
	stat_add(&readahead_stopped, dropped_count);

	/* Reports a job still running, or an interrupted wait. */
	if (error != 0)
		return error;

	/* Succeeded: nothing reads ahead on the device until it resumes. */
	return 0;
}

/*
 * Lets read-ahead on a disk's physical device run again after
 * buf_readahead_stop().
 */
void
buf_readahead_resume(
	struct disk *disk)
{
	struct disk *leaf;
	unsigned long irq;

	/* A missing disk, or a cache not yet set up, has no read-ahead. */
	if (!cache_initialized || disk == NULL)
		return;

	leaf = readahead_leaf(disk);
	irq = spin_lock_irqsave(&readahead_lock);

	if (leaf->d_readahead_stops != 0)
		leaf->d_readahead_stops--;

	spin_unlock_irqrestore(&readahead_lock, irq);
}

/*
 * Hashes a disk and block to a bucket.
 *
 * A line starts at a multiple of the blocks it holds, so the low bits of its
 * block are zero; multiplying by the golden ratio and keeping the top bits
 * spreads such keys over every bucket.
 */
static unsigned
buf_hash_key(
	const struct disk *disk,
	uint64_t block)
{
	uint64_t key;

	/* Mixes the disk into the block and keeps the product's top bits. */
	key = block ^ (uint64_t)((uintptr_t)disk >> 4);
	key *= 0x9e3779b97f4a7c15ULL;

	/* Reports the bucket the product's top bits name. */
	return (unsigned)(key >> (64U - BUF_HASH_BITS));
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

/*
 * Reserves bytes under the cap, reclaiming once when they do not fit.
 *
 * Bytes a reader or writer needs now (needed) are admitted past the cap
 * and past the shared budget's limit under memory pressure, so that the
 * pressure slows cached I/O instead of failing it; reclaim trims the cache
 * again once the line is idle.
 */
static int
reserve_bytes(
	size_t size,
	int metadata,
	int needed)
{
	enum cache_memory_kind kind;
	int error;
	unsigned attempt;
	unsigned long irq;

	/* Reserves shared ownership before the component cap and physical allocation. */
	kind = metadata ? CACHE_MEMORY_BUF_META : CACHE_MEMORY_BUF_DATA;
	if (cache_memory_reserve != NULL) {
		error = cache_memory_reserve(kind, size, !needed);
		if (error != 0 && cache_memory_reclaim != NULL &&
		    cache_memory_reclaim(size) != 0)
			error = cache_memory_reserve(kind, size, !needed);
		if (error != 0)
			return error;
	}

	/* Reserves reclaimed space before another allocator can claim it. */
	mutex_lock(&cache_admission);

	/* Tries before and after one clean reclaim pass; never waits on busy buffers. */
	for (attempt = 0; attempt < 2U; attempt++) {
		irq = spin_lock_irqsave(&cache_lock);
		if (needed ||
		    (cache_current_bytes <= cache_max_bytes &&
		    cache_reserved_bytes <= cache_max_bytes - cache_current_bytes &&
		    (uint64_t)size <= cache_max_bytes - cache_current_bytes -
		    cache_reserved_bytes)) {
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
	enum cache_memory_kind kind;
	unsigned long irq;

	/* Takes the bytes off the cache's own reservation. */
	irq = spin_lock_irqsave(&cache_lock);

	if (cache_reserved_bytes >= size)
		cache_reserved_bytes -= size;

	spin_unlock_irqrestore(&cache_lock, irq);

	/* Names the budget the bytes were reserved from. */
	kind = CACHE_MEMORY_BUF_DATA;
	if (metadata)
		kind = CACHE_MEMORY_BUF_META;

	/* Tells the shared budget that the reservation is gone. */
	if (cache_memory_cancel != NULL)
		cache_memory_cancel(kind, size);
}

/* Converts a reservation into data or metadata usage. */
static void
commit_reservation(
	size_t size,
	int metadata)
{
	enum cache_memory_kind kind;
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

	/* Names the budget the bytes were reserved from. */
	kind = CACHE_MEMORY_BUF_DATA;
	if (metadata)
		kind = CACHE_MEMORY_BUF_META;

	/* Tells the shared budget that the reservation became memory. */
	if (cache_memory_commit != NULL)
		cache_memory_commit(kind, size);
}

/* Allocates page-aligned physical memory reserved against the cap (past it when needed). */
static int
alloc_pmem(
	size_t size,
	struct kern_pmem *memory,
	int metadata,
	int needed)
{
	size_t reserved;
	int error;

	/* Reserves the page-rounded size first. */
	if (size > SIZE_MAX - (KERN_PAGE_SIZE - 1U))
		return ENOMEM;
	reserved = (size + KERN_PAGE_SIZE - 1U) &
	    ~(size_t)(KERN_PAGE_SIZE - 1U);
	error = reserve_bytes(reserved, metadata, needed);
	if (error != 0)
		return ENOMEM;
	memory->size = reserved;
	error = hal_pmem_alloc(reserved, KERN_PAGE_SIZE, &memory->paddr);
	if (error != HAL_OK) {
		cancel_reservation(reserved, metadata);
		if (error == HAL_ERR_NOMEM) {
			stat_add(&stat_physical_failures, 1);
			return ENOMEM;
		}

		/* Any other failure is the memory's, not the demand's. */
		return EIO;
	}

	/*
	 * HAL reports the actual page-rounded allocation.  Keep the hard cap
	 * exact even for a host fixture or architecture with different
	 * rounding.
	 */
	if (memory->size > reserved) {
		error = reserve_bytes(memory->size - reserved, metadata, needed);
		if (error != 0) {
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

/* Adds one slab of buffer headers, past the cap when a line is needed now. */
static int
slab_grow(
	int needed)
{
	struct kern_pmem memory;
	struct buf_slab *slab;
	size_t header;
	size_t charged;
	unsigned capacity;
	int error;
	int inserted;
	unsigned long irq;

	header = slab_header_size();

	/* Rejects a failed allocation or a slab the free mask cannot cover. */
	error = alloc_pmem(BUF_SLAB_BYTES, &memory, 1, needed);
	if (error != 0)
		return error;
	charged = memory.size;
	capacity = (unsigned)((BUF_SLAB_BYTES - header) / sizeof(struct buf));
	if (capacity == 0 || capacity > BUF_SLAB_SLOTS_MAX) {
		if (hal_pmem_free(&memory.paddr, memory.size) != HAL_OK)
			HAL_FATAL("buffer slab rollback failed");
		cancel_reservation(charged, 1);
		return EOVERFLOW;
	}

	/* Clears the page and records the memory the slab came from. */
	kern_memset(hal_pmem_to_kernel(memory.paddr), 0, BUF_SLAB_BYTES);
	slab = hal_pmem_to_kernel(memory.paddr);
	slab->memory = memory;

	/* Counts the page as the cache's metadata. */
	commit_reservation(memory.size, 1);

	/* Publishes the slab with every slot free; the capacity was checked above. */
	irq = spin_lock_irqsave(&cache_lock);

	inserted = buf_slab_insert(&slab_index, &slab->record, capacity);

	spin_unlock_irqrestore(&cache_lock, irq);

	/* A capacity the index refuses cannot happen after the check above. */
	if (inserted != 0)
		HAL_FATAL("buffer slab capacity refused");

	/* Succeeded: the slab's headers can be taken. */
	return 0;
}

/* Takes a zeroed buffer header from the slabs, growing them when full (past the cap when needed). */
static struct buf *
alloc_metadata(
	int needed)
{
	struct buf_slab_record *record;
	struct buf_slab *slab;
	struct buf *buffer;
	unsigned long irq;
	unsigned slot;
	int taken;
	int error;

	/* Retries after growing until a slot is taken or growth fails. */
	for (;;) {
		/* Takes a free slot from the first slab that has one, in a constant number of steps. */
		irq = spin_lock_irqsave(&cache_lock);

		taken = buf_slab_take(&slab_index, &record, &slot);

		spin_unlock_irqrestore(&cache_lock, irq);

		/* A slot was taken: the header in it is the caller's. */
		if (taken == 0)
			break;

		/* No slab has a free slot: adds one, or gives up when memory is short. */
		error = slab_grow(needed);
		if (error != 0)
			return NULL;
	}

	/* Clears the header and records the slot it came from. */
	slab = (struct buf_slab *)record;
	buffer = slab_slot(slab, slot);
	kern_memset(buffer, 0, sizeof(struct buf));
	buffer->b_slab = slab;
	buffer->b_slab_slot = slot;

	/* Succeeded: a zeroed header. */
	return buffer;
}

/* Returns a buffer header to its slab, retiring a slab the index lets go of. */
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

	slab = buffer->b_slab;
	slot = buffer->b_slab_slot;

	/* Ignores a header that is not in a slab. */
	if (slab == NULL)
		return;

	/*
	 * Gives the slot back; a slab left with no header in use beyond the
	 * spare the index keeps has left the index, and its page is retired.
	 */
	irq = spin_lock_irqsave(&cache_lock);

	free_slab = buf_slab_put(&slab_index, &slab->record, slot);
	if (free_slab == 1) {
		release = slab->memory;
		cache_current_bytes -= release.size;
		cache_metadata_bytes -= release.size;
	}

	spin_unlock_irqrestore(&cache_lock, irq);

	/* Releases the slab memory unlocked. */
	if (free_slab == 1) {
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

	/* Prepares the reference before taking busy ownership; the caller needs this line now. */
	error = reference_line(disk, block, 1, &buffer);
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

/*
 * Pins a line without taking busy ownership or performing its data I/O.
 *
 * A line the caller needs now (needed) is not refused for memory pressure:
 * when the cache cannot grow, an idle clean line is taken over, and
 * without one the line is allocated past the cap.  Only the physical
 * allocation failing still reports ENOMEM.
 */
static int
reference_line(
	struct disk *disk,
	uint64_t block,
	int needed,
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
	int pressed;

	candidate = NULL;
	pressed = 0;

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

		/*
		 * A miss in a full cache takes over the least recently used
		 * idle line of the same disk and size instead of freeing one
		 * line and allocating another.
		 */
		if (candidate == NULL) {
			buffer = recycle_line_locked(disk, line_block, line_blocks, line_bytes, pressed);
			if (buffer != NULL) {
				spin_unlock_irqrestore(&cache_lock, irq);
				stat_add(&stat_misses, 1);
				stat_add(&stat_evictions, 1);
				*result = buffer;
				return 0;
			}
		}

		spin_unlock_irqrestore(&cache_lock, irq);

		/*
		 * A miss: prepares a candidate line unlocked.  A needed line
		 * refused for memory goes around once more under pressure, to
		 * take over an idle line or to allocate past the cap.
		 */
		if (candidate == NULL) {
			candidate = alloc_metadata(pressed);
			if (candidate == NULL && needed && !pressed) {
				pressed = 1;
				continue;
			}

			/* Reports that no header could be had even under pressure. */
			if (candidate == NULL)
				return ENOMEM;

			/* Allocates the memory the line will hold. */
			error = alloc_pmem(
				(size_t)(line_blocks * disk->d_block_size),
				&memory,
				0,
				pressed);

			/* Gives the header back when the line's memory could not be had. */
			if (error != 0) {
				free_metadata(candidate);
				candidate = NULL;
			}

			/* A needed line refused for memory goes around once more under pressure. */
			if (error == ENOMEM && needed && !pressed) {
				pressed = 1;
				continue;
			}

			/* Reports why the line's memory could not be had. */
			if (error != 0)
				return error;

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
			buffer->b_unjournaled = 0;
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

/*
 * Transfers full adjacent lines using caller storage, without a staging pool.
 *
 * A disk whose driver moves physical pages gets runs of up to
 * BUF_RUN_PAGE_LINES lines sent straight to and from the lines' own pages;
 * any other disk, or a run whose bookkeeping cannot be allocated, gets the
 * runs of up to BUF_RUN_LINES lines through the caller's storage as before.
 */
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
	struct buf_run_scratch *scratch;
	uint64_t line_blocks;
	int supported;
	int error;

	/*
	 * A range shorter than two whole page-sized lines goes by the
	 * single-line path, so it neither asks the driver about page lists
	 * (the disk registry lock) nor takes run bookkeeping from the heap.
	 * Larger lines and odd sizes are left to transfer_run_lines.
	 */
	if (disk->d_block_size != 0 && disk->d_block_size <= KERN_PAGE_SIZE &&
	    (disk->d_block_size & (disk->d_block_size - 1U)) == 0) {
		line_blocks = KERN_PAGE_SIZE / disk->d_block_size;
		if (block % line_blocks != 0 || remaining / line_blocks < 2U) {
			*transferred = 0;
			run_record(0, IO_BUF_SINGLE_GEOMETRY, 0);
			return 0;
		}
	}

	/* Asks whether the disk's driver moves page lists itself. */
	supported = 0;
	if (disk->d_block_size != 0 && disk->d_block_size <= KERN_PAGE_SIZE)
		supported = disk_page_vector_supported(disk);

	/* Takes the bookkeeping of a long page-list run, when there is memory for it. */
	scratch = NULL;
	if (supported)
		scratch = kern_malloc(sizeof(*scratch));

	/* Runs the transfer the way the disk and the memory allow. */
	if (scratch != NULL) {
		error = transfer_run_lines(disk, block, remaining, data, write, transferred, context,
		    scratch->lines, scratch->generations, scratch->pages, BUF_RUN_PAGE_LINES, 0);
		kern_free(scratch);
	} else {
		error = transfer_run_lines(disk, block, remaining, data, write, transferred, context,
		    lines, generations, NULL, BUF_RUN_LINES, 0);
	}

	/* Reports why the run failed. */
	if (error != 0)
		return error;

	/* Succeeded: *transferred blocks moved, or zero for the single-line path. */
	return 0;
}

/*
 * Transfers up to limit full adjacent lines, as a page list when pages is
 * not NULL and through the caller's storage otherwise.
 *
 * A prefetch reads missing lines into the cache for a reader to come: it
 * has no caller storage (data is NULL), goes only as a page list, starts
 * only at a missing line, leaves a line it could not read missing rather
 * than failed, and records nothing in the I/O statistics.
 */
static int
transfer_run_lines(
	struct disk *disk,
	uint64_t block,
	uint64_t remaining,
	void *data,
	int write,
	uint32_t *transferred,
	const struct io_context *context,
	struct buf **lines,
	uint64_t *generations,
	uint64_t *pages,
	unsigned limit,
	int prefetch)
{
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
	uint64_t started_ns;
	uint64_t sent_ns;

	/* Times a prefetch for the read-ahead log. */
	started_ns = 0;
	if (prefetch)
		started_ns = readahead_now_ns();

	/* Leaves partial lines and small requests to the single-line path. */
	*transferred = 0;
	line_bytes = disk->d_block_size > KERN_PAGE_SIZE ?
	    disk->d_block_size : KERN_PAGE_SIZE;
	if (disk->d_block_size == 0 ||
	    (disk->d_block_size & (disk->d_block_size - 1U)) != 0 ||
	    line_bytes > KERN_IO_BATCH_MAX / 2U) {
		run_record(prefetch, IO_BUF_SINGLE_GEOMETRY, 0);
		return 0;
	}

	line_blocks = (uint32_t)(line_bytes / disk->d_block_size);
	if (block % line_blocks != 0 || remaining / line_blocks < 2U) {
		run_record(prefetch, IO_BUF_SINGLE_GEOMETRY, 0);
		return 0;
	}

	/* A page list carries whole page-sized lines only, and a prefetch has no other way. */
	if (line_bytes != KERN_PAGE_SIZE)
		pages = NULL;
	if (prefetch && (pages == NULL || write))
		return 0;
	if (pages == NULL && limit > KERN_IO_BATCH_MAX / line_bytes)
		limit = (unsigned)(KERN_IO_BATCH_MAX / line_bytes);

	count = (unsigned)(remaining / line_blocks > limit ?
	    limit : remaining / line_blocks);

	/* Pins and allocates all lines before acquiring any busy ownership. */
	for (index = 0; index < count; index++) {
		error = reference_line(
			disk,
			block + (uint64_t)index * line_blocks,
			0,
			&lines[index]);

		/* A run that already has two lines ends before the line that could not be had. */
		if (error != 0 && index >= 2U) {
			count = index;
			break;
		}
		if (error != 0) {
			release_run(lines, index, 0);
			run_record(prefetch, IO_BUF_SINGLE_MEMORY, 0);
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

	/*
	 * Never waits for another line while owning one.  A read that already
	 * owns two lines ends before a line in use (one being read ahead, say);
	 * any other conflict shortens to the single-line path.
	 */
	acquired = 0;
	for (index = 0; index < count; index++) {
		irq = spin_lock_irqsave(&lines[index]->b_lock);
		if ((lines[index]->b_busy || lines[index]->b_io_inflight) &&
		    !write && index >= 2U) {
			spin_unlock_irqrestore(&lines[index]->b_lock, irq);
			release_run(lines + index, count - index, 0);
			count = index;
			break;
		}
		if (lines[index]->b_busy || lines[index]->b_io_inflight) {
			spin_unlock_irqrestore(&lines[index]->b_lock, irq);
			release_run(lines, count, acquired);
			run_record(prefetch, IO_BUF_SINGLE_BUSY, 0);
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
		run_record(prefetch, IO_BUF_SINGLE_HIT, 0);
		return 0;
	}

	/* Copies accepted writes into their durable retry owner before submitting. */
	bytes = data;
	for (index = 0; index < count; index++) {
		if (write) {
			kern_memcpy_ram(lines[index]->b_data, bytes + index * line_bytes, (size_t)line_bytes);
			buf_mark_dirty(lines[index]);
		}

		irq = spin_lock_irqsave(&lines[index]->b_lock);
		generations[index] = lines[index]->b_dirty_generation;
		lines[index]->b_io_state = write ? BUF_IO_WRITING : BUF_IO_READING;
		lines[index]->b_io_inflight = 1;
		spin_unlock_irqrestore(&lines[index]->b_lock, irq);
	}

	/* Names each line's own page, when the run goes as a page list. */
	if (pages != NULL) {
		for (index = 0; index < count; index++) {
			if (lines[index]->b_memory.size != KERN_PAGE_SIZE)
				pages = NULL;
			else
				pages[index] = lines[index]->b_memory.paddr;
		}
	}

	/* A prefetch that cannot go as a page list leaves its lines missing. */
	if (prefetch && pages == NULL) {
		for (index = 0; index < count; index++)
			finish_unread_line(lines[index]);
		release_run(lines, count, count);
		return 0;
	}

	/*
	 * Sends the lines' pages to the device, or carries the contiguous
	 * caller buffer to the device's splitting boundary.  A driver that
	 * turns the page list down before sending anything gets the caller
	 * buffer instead; a write's caller buffer still holds what the lines
	 * hold.
	 */
	stat_add(write ? &stat_write_bios : &stat_read_bios, 1);
	run_record(prefetch, write ? IO_BUF_RUN_WRITE : IO_BUF_RUN_READ, count * line_bytes);
	sent_ns = 0;
	if (prefetch) {
		sent_ns = readahead_now_ns();
		if (sent_ns >= started_ns)
			readahead_account(&readahead_prepare_ns, sent_ns - started_ns);
	}
	completed = 0;
	error = EOPNOTSUPP;
	if (pages != NULL) {
		error = disk_transfer_pages_context(disk, write ? BIO_WRITE : BIO_READ, block,
		    count * line_blocks, pages, count, &completed, context);
	}
	if (error == EOPNOTSUPP && completed == 0 && !prefetch) {
		pages = NULL;
		error = disk_transfer_progress_context(disk, write ? BIO_WRITE : BIO_READ, block,
		    count * line_blocks, data, &completed, context);
	}
	if (prefetch) {
		started_ns = readahead_now_ns();
		if (started_ns >= sent_ns)
			readahead_account(&readahead_transfer_ns, started_ns - sent_ns);
	}

	/*
	 * Publishes only complete lines in the confirmed prefix; uncertainty
	 * stays dirty.  A read that went to the lines' pages is copied out to
	 * the caller; one that went to the caller's buffer is copied into the
	 * lines.
	 */
	for (index = 0; index < count; index++) {
		line_error = error;
		if ((index + 1U) * line_blocks <= completed) {
			line_error = 0;
			if (prefetch) {
				/* A prefetch leaves the data in the line for its reader. */
			} else if (!write && pages != NULL) {
				kern_memcpy_ram(bytes + index * line_bytes, lines[index]->b_data,
				    (size_t)line_bytes);
			} else if (!write) {
				kern_memcpy_ram(lines[index]->b_data, bytes + index * line_bytes,
				    (size_t)line_bytes);
			}
		} else if (line_error == 0) {
			line_error = EIO;
		}

		/* A line a prefetch could not read stays missing, for its reader to read. */
		if (prefetch && line_error != 0)
			finish_unread_line(lines[index]);
		else
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
	struct buf *previous;
	struct buf *last;
	unsigned bucket;
	int taken;

	candidate = NULL;
	irq = spin_lock_irqsave(&cache_lock);

	/*
	 * Walks the LRU from the tail for an unreferenced, idle buffer.  An
	 * unranged walk moves the lines it cannot take because they are dirty
	 * or being written to the head (evict_take_locked), and ends at the
	 * line that was the head when it began.
	 */
	last = lru_head;
	for (buffer = lru_tail; buffer != NULL; buffer = previous) {
		previous = buffer->b_lru_prev;

		/* A ranged eviction looks only at the lines of its range. */
		if (range && (buffer->b_disk != disk ||
		    buffer->b_block >= end ||
		    buffer->b_block + buffer->b_block_count <= start))
			continue;

		/* Takes the line when nobody holds, writes or needs it. */
		taken = 0;
		if (refcount_load(&buffer->b_refs) == 1)
			taken = evict_take_locked(buffer, flags, range);
		if (taken) {
			candidate = buffer;
			break;
		}

		/* The walk has seen every line that was on the list when it began. */
		if (buffer == last)
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
		stat_add(&cache_dirty_bytes, (uint64_t)-(int64_t)candidate->b_size);
	*freed = candidate->b_memory.size;
	stat_add(&stat_buffers, UINT64_MAX);
	stat_add(&stat_evictions, 1);
	free_buffer(candidate);

	/* Reports the eviction. */
	return 0;
}

/*
 * Takes an LRU line for eviction when it is idle: unlinks it from the dirty
 * index, the LRU and the hash table and drops the cache's reference, and
 * reports 1.  A line that is referenced, busy, being written or dirty (and
 * not to be discarded) is left, and an unranged walk moves it to the head,
 * so that the lines a writer keeps dirty at the tail are not walked over
 * again by every eviction.  This gives such a line a second turn through
 * the LRU: once written it is evicted after the clean lines that were
 * older than it.  cache_lock is held.
 */
static int
evict_take_locked(
	struct buf *buffer,
	unsigned flags,
	int range)
{
	unsigned long birq;
	unsigned long dirty_irq;
	int busy;

	/* Looks at the line's state under its own lock and the dirty index's. */
	birq = spin_lock_irqsave(&buffer->b_lock);
	dirty_irq = spin_lock_irqsave(&dirty_index_lock);
	busy = 0;
	if (refcount_load(&buffer->b_refs) != 1 ||
	    buffer->b_busy ||
	    buffer->b_io_inflight ||
	    ((buffer->b_flags & BUF_DIRTY) &&
	     !(flags & BUF_INVALIDATE_DISCARD)))
		busy = 1;
	if (busy) {
		spin_unlock_irqrestore(&dirty_index_lock, dirty_irq);
		spin_unlock_irqrestore(&buffer->b_lock, birq);

		/* A line kept dirty goes to the head, out of the next walk's way. */
		if (!range)
			lru_add_locked(buffer);
		return 0;
	}

	/* Invalidates the idle line and takes it out of every index. */
	dirty_unlink_locked(buffer);
	buffer->b_flags |= BUF_INVALID;
	spin_unlock_irqrestore(&dirty_index_lock, dirty_irq);
	spin_unlock_irqrestore(&buffer->b_lock, birq);
	lru_remove_locked(buffer);
	hash_remove_locked(buffer);
	(void)refcount_put(&buffer->b_refs);

	/* Succeeded: the caller frees the line. */
	return 1;
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
		if (!buffer->b_journal_pin &&
		    (!reclaim || refcount_load(&buffer->b_refs) == 1) &&
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

/*
 * References the dirty line of a disk's range [start, end) with
 * unjournaled blocks whose block is the lowest at or after after, or
 * returns NULL when there is none.
 */
static struct buf *
unjournaled_reference(
	struct disk *disk,
	uint64_t start,
	uint64_t end,
	uint64_t after)
{
	struct buf *buffer;
	struct buf *found;
	unsigned long irq;

	/* Scans the disk's dirty lines for the lowest one at or after the mark. */
	found = NULL;
	irq = spin_lock_irqsave(&dirty_index_lock);
	for (buffer = disk->d_dirty_buffers; buffer != NULL; buffer = buffer->b_device_dirty_next) {
		/* Leaves out a line without unjournaled blocks. */
		if (buffer->b_unjournaled == 0U)
			continue;

		/* Leaves out a line outside the range or before the mark. */
		if (buffer->b_block + buffer->b_block_count <= start)
			continue;
		if (buffer->b_block >= end)
			continue;
		if (buffer->b_block < after && buffer->b_block + buffer->b_block_count <= after)
			continue;

		/* Keeps the lowest. */
		if (found == NULL || buffer->b_block < found->b_block)
			found = buffer;
	}

	/* References the line found before the index is let go. */
	if (found != NULL)
		refcount_get(&found->b_refs);
	spin_unlock_irqrestore(&dirty_index_lock, irq);

	/* Takes the lower-ranked cache lock only after releasing the index guard. */
	if (found != NULL) {
		irq = spin_lock_irqsave(&cache_lock);
		lru_remove_locked(found);
		spin_unlock_irqrestore(&cache_lock, irq);
	}

	/* The line, or none. */
	return found;
}

/*
 * Writes a line's unjournaled blocks within [start, end) beneath the cache,
 * as the filesystem writes a delayed line, and takes them out of the
 * record.  The caller is the line's busy owner, so neither its data nor
 * its record changes meanwhile; no other block of the line is written, so
 * the blocks a journal pinned wait for its commit.  A run that crosses an
 * end of the range is cut to it, so that a narrow range (one file's
 * blocks, one sector of a journal) still has its own blocks written
 * (BUG-307); the rest of the run keeps its record for a later write.  A
 * run that touches another owner's claim (a swap file's, a loop image's)
 * is left for that owner, as writeback_line() leaves it.
 */
static int
unjournaled_write_line(
	struct buf *buffer,
	uint64_t start,
	uint64_t end)
{
	struct backing_mutation_guard guard;
	const struct backing_claim *foreign;
	unsigned long irq;
	uint64_t block;
	uint64_t low;
	uint64_t high;
	uint32_t record;
	uint32_t written;
	uint32_t first;
	uint32_t offset;
	uint32_t count;
	uint32_t run;
	int error;

	/* Reads the record; a line written clean meanwhile has none. */
	irq = spin_lock_irqsave(&buffer->b_lock);
	record = buffer->b_unjournaled;
	if ((buffer->b_flags & BUF_DIRTY) == 0U)
		record = 0;
	spin_unlock_irqrestore(&buffer->b_lock, irq);

	/* Writes each run of unjournaled blocks. */
	written = 0;
	first = 0;
	for (;;) {
		/* The next run, within the line and the range. */
		run = buf_unjournaled_next_run(record, &first);
		if (run == 0)
			break;
		if (first + run > buffer->b_block_count)
			run = buffer->b_block_count - first;
		block = buffer->b_block + first;

		/* Cuts the run to the range. */
		low = block;
		if (low < start)
			low = start;
		high = block + run;
		if (high > end)
			high = end;

		/* Leaves a run wholly outside the range as it is. */
		if (low >= high) {
			first += run;
			continue;
		}

		/* Leaves a run of another owner's claim as it is. */
		offset = (uint32_t)(low - buffer->b_block);
		count = (uint32_t)(high - low);
		foreign = NULL;
		error = backing_claim_find_extent_owner(buffer->b_disk, low, count, NULL, &foreign);
		if (error != 0)
			foreign = NULL;
		if (foreign != NULL) {
			first += run;
			continue;
		}

		/* Writes the run's part in the range as the filesystem, beneath the cache. */
		error = backing_mutation_begin_disk_filesystem(buffer->b_disk, low, count, &guard);
		if (error != 0)
			return error;
		error = disk_write_direct_context(buffer->b_disk, low, count,
						  (uint8_t *)buffer->b_data + (size_t)offset * buffer->b_disk->d_block_size,
						  NULL);
		backing_mutation_end(&guard);

		/* Reports why the run could not be written. */
		if (error != 0)
			return error;

		/* Counts what was written and steps past the run. */
		io_stats_record(IO_BUF_UNJOURNALED_WRITE, (uint64_t)count * buffer->b_disk->d_block_size);
		written |= buf_unjournaled_bits(offset, count);
		first += run;
	}

	/* Takes the written blocks out of the record. */
	irq = spin_lock_irqsave(&buffer->b_lock);
	buffer->b_unjournaled &= ~written;
	spin_unlock_irqrestore(&buffer->b_lock, irq);

	/* Succeeded: the line's unjournaled blocks are written. */
	return 0;
}

/* Writes out aged dirty buffers every interval, forever. */
static void
flusher(
	void *argument)
{
	uint64_t now;
	unsigned index;

	/* The flusher takes no argument. */
	(void)argument;

	/* Sleeps an interval, then writes what has been dirty long enough. */
	for (;;) {
		/* Waits for the next interval. */
		now = sched_ticks();
		sched_sleep(now + BUF_FLUSH_INTERVAL);

		/*
		 * Lets each registered filesystem commit first, under the
		 * registry lock, so that no hook is removed while it runs.
		 */
		mutex_lock(&flusher_hooks_lock);

		/* Calls every registered hook with its argument. */
		for (index = 0; index < BUF_FLUSHER_HOOKS; index++) {
			if (flusher_hooks[index] != NULL)
				flusher_hooks[index](flusher_hook_arguments[index]);
		}

		mutex_unlock(&flusher_hooks_lock);

		/* Writes the buffers that have been dirty for longer than the age. */
		now = sched_ticks();
		if (now > BUF_FLUSH_AGE)
			(void)flush_aged(now - BUF_FLUSH_AGE);

		/* Makes what the workers wrote durable. */
		writeback_flush_noted();
	}
}

/*
 * Takes a reference to the oldest dirty buffer a journal does not pin,
 * when it has been dirty since the cutoff tick or earlier.
 */
static struct buf *
oldest_aged(
	uint64_t cutoff)
{
	struct buf *candidate;
	unsigned long irq;

	/*
	 * Walks the dirty list, which is in the order buffers became dirty,
	 * past the old buffers a journal pins.
	 */
	irq = spin_lock_irqsave(&dirty_index_lock);

	candidate = dirty_head;
	while (candidate != NULL &&
	       candidate->b_dirty_tick <= cutoff &&
	       candidate->b_journal_pin)
		candidate = candidate->b_dirty_next;

	/* Holds a buffer old enough to write; a younger one is not taken. */
	if (candidate != NULL && candidate->b_dirty_tick <= cutoff) {
		refcount_get(&candidate->b_refs);
	} else {
		candidate = NULL;
	}

	spin_unlock_irqrestore(&dirty_index_lock, irq);

	/* Reports the buffer, or NULL when none is old enough. */
	return candidate;
}

/*
 * Writes back every buffer dirty since the cutoff tick or earlier, then
 * flushes the devices written to.
 *
 * The global dirty list is in the order buffers became dirty, so the pass
 * stops at the first younger one.  A failed write ends the pass: the
 * buffer stays dirty and is tried again at the next interval.
 */
static int
flush_aged(
	uint64_t cutoff)
{
	struct disk *leaves[BUF_FLUSH_LEAVES];
	struct buf *candidate;
	unsigned long irq;
	unsigned count;
	unsigned index;
	unsigned written;
	int error;
	int known;

	/*
	 * Writes the oldest dirty buffer while it is old enough.  A buffer
	 * rewritten during its own write stays first, so a pass is bounded.
	 */
	count = 0;
	error = 0;
	for (written = 0; written < BUF_FLUSH_PASS_MAX; written++) {
		/* Takes the oldest buffer; the pass ends when none is old enough. */
		candidate = oldest_aged(cutoff);
		if (candidate == NULL)
			break;

		/* Looks the buffer's device up among those already written to. */
		known = 0;
		for (index = 0; index < count; index++) {
			if (leaves[index] == candidate->b_disk)
				known = 1;
		}

		/* Remembers a new device, so that it is flushed after the pass. */
		if (!known && count < BUF_FLUSH_LEAVES) {
			leaves[count] = candidate->b_disk;
			count++;
		}

		/* Takes the buffer off the reclaim list while it is written. */
		irq = spin_lock_irqsave(&cache_lock);

		lru_remove_locked(candidate);

		spin_unlock_irqrestore(&cache_lock, irq);

		/* Writes the buffer, which leaves the dirty list when clean. */
		error = busy_acquire(candidate);
		if (error == 0)
			error = buf_writeback(candidate);
		buf_release(candidate);

		/* A failed write ends the pass; the buffer is tried again later. */
		if (error != 0)
			break;
	}

	/* Makes the written buffers durable on each device. */
	for (index = 0; index < count; index++)
		(void)bio_flush(leaves[index]);

	/* Reports the write that failed. */
	if (error != 0)
		return error;

	/* Succeeded: every aged buffer was written. */
	return 0;
}

/*
 * Asks whether a write to a disk may stay in the cache: the disk is
 * write-cached, the write is not made under a backing claim, and dirty
 * memory is within its bound.
 */
static int
writes_delayed(
	const struct disk *disk,
	const struct io_context *context,
	int raw)
{
	uint64_t dirty;

	/* A write-through disk sends every write to the device, but for a raw writer's. */
	if ((disk->d_flags & DISK_WRITE_CACHED) == 0 && !raw)
		return 0;

	/*
	 * A write made under a backing claim (a formatter's lease, a loop
	 * device's file) goes to the device while the claim authorizes it: a
	 * later write-back carries no claim, and the claimed range refuses it.
	 */
	if (context != NULL && context->claim != NULL)
		return 0;

	/* Past the bound a write goes through at once, which bounds dirty memory. */
	dirty = atomic_u64_load_acquire(&cache_dirty_bytes);
	if (dirty > dirty_limit_of(disk))
		return 0;

	/* Succeeded: reports that the write may wait in the cache. */
	return 1;
}

/*
 * Copies a range into its lines and marks them dirty without writing
 * them back; with a nonzero pin, the lines are also pinned for a journal
 * under that tag.
 */
static int
write_lines(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data,
	uint64_t pin)
{
	struct disk *leaf;
	struct buf *buffer;
	const uint8_t *in;
	uint64_t mapped;
	uint64_t end;
	uint64_t line_bytes;
	uint64_t line_blocks;
	uint64_t line_start;
	uint64_t offset_blocks;
	uint64_t amount_blocks;
	unsigned long irq;
	int full;
	int error;

	/* Rejects a missing buffer or an empty range. */
	in = data;
	if (data == NULL || count == 0)
		return EINVAL;

	/* Refuses a read-only disk. */
	if ((disk->d_flags & DISK_READ_ONLY) != 0)
		return EROFS;

	/* Resolves the range to the leaf disk. */
	error = disk_resolve_range(disk, block, count, &leaf, &mapped);
	if (error != 0)
		return error;

	/* Counts the write. */
	io_stats_record(IO_BUF_WRITE, (uint64_t)count * leaf->d_block_size);

	/* Copies into each line in turn, leaving it dirty. */
	end = mapped + count;
	while (mapped < end) {
		/* Measures the line: a page, or one block larger than a page. */
		if (leaf->d_block_size > KERN_PAGE_SIZE)
			line_bytes = leaf->d_block_size;
		else
			line_bytes = KERN_PAGE_SIZE;

		/* Finds the part of the line the range covers. */
		line_blocks = line_bytes / leaf->d_block_size;
		line_start = mapped - mapped % line_blocks;
		offset_blocks = mapped - line_start;
		amount_blocks = line_blocks - offset_blocks;

		/* Stops the part at the end of the range. */
		if (amount_blocks > end - mapped)
			amount_blocks = end - mapped;

		/* A whole line inside the disk needs no read before the write. */
		full = 0;
		if (offset_blocks == 0 &&
		    amount_blocks == line_blocks &&
		    line_start + line_blocks <= leaf->d_block_count)
			full = 1;

		/* Takes the line, read first unless the write covers all of it. */
		error = acquire_line(leaf, mapped, !full, &buffer);
		if (error != 0)
			return error;

		/*
		 * Pins the line for the journal before it is dirty, so that no
		 * writer sees it dirty and unpinned: write-back and the flusher
		 * leave a pinned line alone until buf_unpin() with this tag.  A
		 * line an earlier transaction pinned takes the new tag, so that
		 * the earlier commit's unpin leaves it to this one (ws073-p051).
		 */
		if (pin != 0) {
			irq = spin_lock_irqsave(&buffer->b_lock);
			buffer->b_journal_pin = pin;
			spin_unlock_irqrestore(&buffer->b_lock, irq);
		}

		/*
		 * Copies the data into the line and leaves it dirty; the blocks
		 * are the journal's now, no longer unjournaled (ws073-p053).
		 */
		kern_memcpy_ram((uint8_t *)buffer->b_data + offset_blocks * leaf->d_block_size,
			    in,
			    (size_t)(amount_blocks * leaf->d_block_size));
		mark_dirty(buffer, offset_blocks, amount_blocks, 1);
		buf_release(buffer);

		/* Steps past the part written. */
		in += amount_blocks * leaf->d_block_size;
		mapped += amount_blocks;
	}

	/* Succeeded: the lines hold the data. */
	return 0;
}

/*
 * Initializes the hook registry once.  Hooks take filesystem locks, so the
 * registry ranks below them.
 */
static void
flusher_hooks_init(
	void)
{
	int first;

	/* Only the first caller initializes the lock. */
	first = atomic_try_acquire_zero(&flusher_hooks_ready);
	if (first)
		(void)mutex_init(&flusher_hooks_lock, LOCK_RANK_WRITEBACK_CONTROL, "buffer flusher hooks");
}

/*
 * Writes one dirty line to its disk.
 *
 * A delayed write-back (the flusher, a sync) runs apart from the write that
 * dirtied the line and holds no filesystem guard of its own; it writes as
 * the filesystem, since the data was admitted when it was written.  An
 * immediate write-back runs inside its writer's guard and inherits it.
 */
static int
writeback_line(
	struct buf *buffer,
	const struct io_context *context,
	int delayed)
{
	uint64_t generation;
	struct io_context drain;
	const struct backing_claim *writer;
	const struct backing_claim *foreign;
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

	/* A clean buffer has nothing to write. */
	if (!(buffer->b_flags & BUF_DIRTY)) {
		spin_unlock_irqrestore(&buffer->b_lock, irq);
		return 0;
	}

	/* A journal's pinned buffer waits for its commit. */
	if (buffer->b_journal_pin) {
		spin_unlock_irqrestore(&buffer->b_lock, irq);
		return 0;
	}

	/* An invalid buffer holds nothing that could be written. */
	if (!(buffer->b_flags & BUF_VALID)) {
		spin_unlock_irqrestore(&buffer->b_lock, irq);
		return EIO;
	}

	/* Writes the data unlocked, remembering which generation it was. */
	generation = buffer->b_dirty_generation;
	buffer->b_io_state = BUF_IO_WRITING;
	buffer->b_io_inflight = 1;

	spin_unlock_irqrestore(&buffer->b_lock, irq);

	/* The writer's own claim, if it has one, may write its own blocks. */
	writer = NULL;
	if (context != NULL)
		writer = context->claim;

	/*
	 * A line wider than a FAT cluster can hold another owner's claimed
	 * blocks (a swap file's, a loop image's) beside the ones that were
	 * changed (BUG-073).  Those blocks are written only by their owner,
	 * straight to the device, so the line's copy of them is not the one
	 * to write: such a line is written in runs that leave them out.
	 */
	foreign = NULL;
	error = backing_claim_find_extent_owner(buffer->b_disk, buffer->b_block,
						buffer->b_block_count, writer,
						&foreign);
	if (error != 0)
		foreign = NULL;

	/* Writes the whole line, or the runs outside the other claims. */
	stat_add(&stat_write_bios, 1);
	if (foreign == NULL)
		error = writeback_line_whole(buffer, &drain, delayed);
	else
		error = writeback_line_runs(buffer, &drain, writer);

	/* Records the outcome; only an unmodified buffer becomes clean. */
	irq = spin_lock_irqsave(&buffer->b_lock);

	/* Clears the dirty record on a write nothing raced, and keeps it on a failure. */
	buffer->b_io_state = BUF_IO_IDLE;
	buffer->b_io_inflight = 0;
	buffer->b_error = error;
	if (error == 0 && generation == buffer->b_dirty_generation) {
		buffer->b_flags &= ~(BUF_DIRTY | BUF_ERROR);
		buffer->b_unjournaled = 0;
		dirty_clear(buffer);
		stat_add(&cache_dirty_bytes,
		    (uint64_t)-(int64_t)buffer->b_size);

		/*
		 * The skipped blocks may have changed on the device under
		 * their owner, so the next reader reads the line again.
		 */
		if (foreign != NULL)
			buffer->b_flags &= ~BUF_VALID;
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

/* The bound of dirty memory for a cache whose cap is cap bytes: a share of it, in whole pages. */
static uint64_t
dirty_limit_for(
	uint64_t cap)
{
	uint64_t limit;

	/* Takes the share, at least one page. */
	limit = cap / BUF_DIRTY_SHARE;
	limit &= ~(uint64_t)(KERN_PAGE_SIZE - 1U);
	if (limit < KERN_PAGE_SIZE)
		limit = KERN_PAGE_SIZE;

	/* Succeeded: the bound. */
	return limit;
}

/* Reports whether a disk's physical device takes page lists; read without the registry lock. */
static int
leaf_takes_pages(
	const struct disk *disk)
{
	const struct disk *leaf;

	/* Walks up the partitions, which stay while the disk is referenced. */
	leaf = disk;
	while (leaf->d_parent != NULL)
		leaf = leaf->d_parent;

	/* Succeeded: the device's capability. */
	return (leaf->d_flags & DISK_PAGE_VECTOR) != 0;
}

/* The bound of dirty memory a delayed write to a disk is held to. */
static uint64_t
dirty_limit_of(
	const struct disk *disk)
{
	uint64_t limit;

	/* A disk that writes one line at a time keeps the small bound. */
	limit = atomic_u64_load_acquire(&dirty_limit_bytes);
	if (!leaf_takes_pages(disk) && limit > BUF_DIRTY_SMALL)
		limit = BUF_DIRTY_SMALL;

	/* Succeeded: the bound. */
	return limit;
}

/*
 * Reports whether a line can go in a cluster: one whole page, dirty and
 * valid, pinned by no journal, and not being read or written; b_lock is
 * held.
 */
static int
cluster_line_locked(
	const struct buf *buffer,
	uint64_t line_blocks)
{
	/* A line of another shape, or one not ready to write, goes alone. */
	if (buffer->b_block_count != line_blocks ||
	    buffer->b_memory.size != KERN_PAGE_SIZE ||
	    (buffer->b_flags & (BUF_DIRTY | BUF_VALID)) != (BUF_DIRTY | BUF_VALID) ||
	    buffer->b_journal_pin != 0 ||
	    buffer->b_io_inflight)
		return 0;

	/* Succeeded: the line can be written with others. */
	return 1;
}

/* Takes a line's busy ownership for a cluster when nobody has it and it fits, without waiting. */
static int
cluster_take(
	struct buf *buffer,
	uint64_t line_blocks)
{
	unsigned long irq;
	int taken;

	/* Takes the line only as it is now. */
	irq = spin_lock_irqsave(&buffer->b_lock);

	taken = !buffer->b_busy && cluster_line_locked(buffer, line_blocks);
	if (taken)
		buffer->b_busy = 1;

	spin_unlock_irqrestore(&buffer->b_lock, irq);

	/* Reports whether the line joined. */
	return taken;
}

/*
 * Becomes the only thread gathering a cluster, so that two writers do not
 * split one run of dirty lines between them.  The gatherer never waits, so
 * the wait is short.  Reports EINTR when the wait is interrupted.
 */
static int
cluster_pick_begin(
	void)
{
	uint64_t sequence;
	unsigned long irq;
	int error;

	/* Waits for the gatherer before this one. */
	irq = spin_lock_irqsave(&readahead_lock);

	while (cluster_picking) {
		sequence = waitq_sequence(&readahead_changed);
		error = waitq_sleep(&readahead_changed, &readahead_lock, sequence, 0, 0);
		if (error != 0 && error != EAGAIN) {
			spin_unlock_irqrestore(&readahead_lock, irq);
			return error;
		}
	}
	cluster_picking = 1;

	spin_unlock_irqrestore(&readahead_lock, irq);

	/* Succeeded: this thread gathers. */
	return 0;
}

/* Lets the next gatherer in. */
static void
cluster_pick_end(
	void)
{
	unsigned long irq;

	/* Clears the mark and wakes whoever waits for it. */
	irq = spin_lock_irqsave(&readahead_lock);

	cluster_picking = 0;
	waitq_wake_all(&readahead_changed);

	spin_unlock_irqrestore(&readahead_lock, irq);
}

/*
 * Gathers the dirty lines on both sides of a line the caller owns: up to
 * BUF_RUN_PAGE_LINES adjacent lines of its disk, those before it first, so
 * that a run is written from its start.  The neighbours are referenced
 * under one hold of the cache lock and taken nearest first without
 * waiting; the first that cannot join ends that side.
 */
static void
cluster_gather(
	struct buf *buffer,
	struct buf_cluster *cluster,
	uint64_t line_blocks)
{
	struct buf *line;
	uint64_t block;
	unsigned middle;
	unsigned before;
	unsigned after;
	unsigned taken_before;
	unsigned taken_after;
	unsigned index;
	unsigned held;
	unsigned long irq;
	int ended;

	/*
	 * References the dirty neighbours the cache holds, before and then
	 * after, BUF_GATHER_HOLD lookups to a hold of the cache lock.  The
	 * dirty flag is only a hint here; cluster_take() looks again.
	 */
	middle = BUF_RUN_PAGE_LINES - 1U;
	cluster->slots[middle] = buffer;
	block = buffer->b_block;
	before = 0;
	after = 0;
	ended = 0;
	while (!ended && before < BUF_RUN_PAGE_LINES - 1U) {
		irq = spin_lock_irqsave(&cache_lock);

		for (held = 0; held < BUF_GATHER_HOLD && before < BUF_RUN_PAGE_LINES - 1U; held++) {
			line = NULL;
			if (block >= (uint64_t)(before + 1U) * line_blocks)
				line = hash_find_locked(buffer->b_disk, block - (uint64_t)(before + 1U) * line_blocks);
			if (line == NULL || (line->b_flags & BUF_DIRTY) == 0) {
				ended = 1;
				break;
			}
			refcount_get(&line->b_refs);
			lru_remove_locked(line);
			cluster->slots[middle - 1U - before] = line;
			before++;
		}

		spin_unlock_irqrestore(&cache_lock, irq);
	}
	ended = 0;
	while (!ended && before + after < BUF_RUN_PAGE_LINES - 1U) {
		irq = spin_lock_irqsave(&cache_lock);

		for (held = 0; held < BUF_GATHER_HOLD && before + after < BUF_RUN_PAGE_LINES - 1U; held++) {
			line = hash_find_locked(buffer->b_disk, block + (uint64_t)(after + 1U) * line_blocks);
			if (line == NULL || (line->b_flags & BUF_DIRTY) == 0) {
				ended = 1;
				break;
			}
			refcount_get(&line->b_refs);
			lru_remove_locked(line);
			cluster->slots[middle + 1U + after] = line;
			after++;
		}

		spin_unlock_irqrestore(&cache_lock, irq);
	}

	/* Takes the neighbours nearest first, and lets go of those past the first that cannot join. */
	for (taken_before = 0; taken_before < before; taken_before++) {
		if (!cluster_take(cluster->slots[middle - 1U - taken_before], line_blocks))
			break;
	}
	for (index = taken_before; index < before; index++)
		drop_caller_reference(cluster->slots[middle - 1U - index]);
	for (taken_after = 0; taken_after < after; taken_after++) {
		if (!cluster_take(cluster->slots[middle + 1U + taken_after], line_blocks))
			break;
	}
	for (index = taken_after; index < after; index++)
		drop_caller_reference(cluster->slots[middle + 1U + index]);

	/* Succeeded: the run of taken lines. */
	cluster->lines = &cluster->slots[middle - taken_before];
	cluster->count = taken_before + 1U + taken_after;
	cluster->own = taken_before;
}

/* Gives back the lines of a cluster other than the caller's own. */
static void
cluster_release(
	struct buf_cluster *cluster)
{
	unsigned index;

	/* Releases each neighbour's ownership and reference. */
	for (index = 0; index < cluster->count; index++) {
		if (index != cluster->own)
			buf_release(cluster->lines[index]);
	}
}

/*
 * Writes a cluster's lines to the disk as one page list, as the filesystem
 * would (a delayed write-back), and publishes each line's outcome.  When
 * nothing could be sent (the range is guarded, or the driver turned the
 * page list down) the lines are left as they were and *sent is zero.
 * Reports the outcome of the caller's own line.
 */
static int
cluster_write(
	struct buf_cluster *cluster,
	uint64_t line_blocks,
	int *sent)
{
	struct backing_mutation_guard guard;
	struct disk *disk;
	uint64_t start;
	uint64_t blocks;
	uint32_t completed;
	unsigned index;
	unsigned long irq;
	int error;
	int line_error;
	int own_error;

	/* Marks every line as being written, remembering the data's generation. */
	*sent = 0;
	disk = cluster->lines[0]->b_disk;
	start = cluster->lines[0]->b_block;
	blocks = (uint64_t)cluster->count * line_blocks;
	for (index = 0; index < cluster->count; index++) {
		irq = spin_lock_irqsave(&cluster->lines[index]->b_lock);
		cluster->generations[index] = cluster->lines[index]->b_dirty_generation;
		cluster->lines[index]->b_io_state = BUF_IO_WRITING;
		cluster->lines[index]->b_io_inflight = 1;
		cluster->pages[index] = cluster->lines[index]->b_memory.paddr;
		spin_unlock_irqrestore(&cluster->lines[index]->b_lock, irq);
	}

	/* Sends the run under the filesystem's guard, as one line's write-back would be. */
	completed = 0;
	error = backing_mutation_begin_disk_filesystem(disk, start, blocks, &guard);
	if (error == 0) {
		stat_add(&stat_write_bios, 1);
		io_stats_record(IO_BUF_RUN_WRITE, blocks * disk->d_block_size);
		error = disk_transfer_pages_context(disk, BIO_WRITE, start, (uint32_t)blocks,
		    cluster->pages, cluster->count, &completed, NULL);
		backing_mutation_end(&guard);
		if (error != EOPNOTSUPP || completed != 0)
			*sent = 1;
	}

	/* Nothing went: the lines are as they were, for the caller's other path. */
	if (!*sent) {
		for (index = 0; index < cluster->count; index++) {
			irq = spin_lock_irqsave(&cluster->lines[index]->b_lock);
			cluster->lines[index]->b_io_state = BUF_IO_IDLE;
			cluster->lines[index]->b_io_inflight = 0;
			waitq_wake_all(&cluster->lines[index]->b_waitq);
			spin_unlock_irqrestore(&cluster->lines[index]->b_lock, irq);
		}
		return error;
	}

	/* Publishes the confirmed prefix as clean; the rest stays dirty with the error. */
	own_error = 0;
	for (index = 0; index < cluster->count; index++) {
		line_error = 0;
		if ((uint64_t)(index + 1U) * line_blocks > completed)
			line_error = error != 0 ? error : EIO;
		finish_run_line(cluster->lines[index], cluster->generations[index], 1, line_error);
		if (index == cluster->own)
			own_error = line_error;
	}

	/* Reports the caller's line. */
	return own_error;
}

/*
 * Writes back a dirty line the caller owns, with the dirty lines beside it
 * as one page list when its disk takes page lists, and alone otherwise (a
 * line of another shape, a journal's pinned line, blocks another claim
 * owns, early boot).
 */
static int
writeback_cluster(
	struct buf *buffer)
{
	struct buf_cluster *cluster;
	const struct backing_claim *foreign;
	struct disk *disk;
	uint64_t line_blocks;
	unsigned long irq;
	int eligible;
	int error;
	int sent;

	/* Takes the cluster path only for a page-list disk and a thread that may wait. */
	disk = buffer->b_disk;
	if (thread_current() == NULL ||
	    disk->d_block_size == 0 ||
	    disk->d_block_size > KERN_PAGE_SIZE ||
	    KERN_PAGE_SIZE % disk->d_block_size != 0 ||
	    !leaf_takes_pages(disk))
		return writeback_line(buffer, NULL, 1);
	line_blocks = KERN_PAGE_SIZE / disk->d_block_size;

	/* A line that cannot be in a cluster goes the line's own way. */
	irq = spin_lock_irqsave(&buffer->b_lock);
	eligible = cluster_line_locked(buffer, line_blocks);
	spin_unlock_irqrestore(&buffer->b_lock, irq);
	if (!eligible)
		return writeback_line(buffer, NULL, 1);

	/* Gathers the neighbours, one gatherer at a time. */
	cluster = kern_malloc(sizeof(*cluster));
	if (cluster == NULL)
		return writeback_line(buffer, NULL, 1);
	if (cluster_pick_begin() != 0) {
		kern_free(cluster);
		return writeback_line(buffer, NULL, 1);
	}
	cluster_gather(buffer, cluster, line_blocks);
	cluster_pick_end();

	/* A lone line, or a run with blocks another claim owns (BUG-073), goes the line's own way. */
	foreign = NULL;
	error = 0;
	if (cluster->count > 1U)
		error = backing_claim_find_extent_owner(disk, cluster->lines[0]->b_block,
		    (uint64_t)cluster->count * line_blocks, NULL, &foreign);
	if (cluster->count == 1U || error != 0 || foreign != NULL) {
		cluster_release(cluster);
		kern_free(cluster);
		return writeback_line(buffer, NULL, 1);
	}

	/* Writes the run; when nothing could be sent, the line goes its own way. */
	error = cluster_write(cluster, line_blocks, &sent);
	cluster_release(cluster);
	kern_free(cluster);
	if (!sent)
		return writeback_line(buffer, NULL, 1);

	/* Reports the caller's line. */
	return error;
}

/*
 * Takes the oldest dirty line of a device (of any, when leaf is NULL) that
 * no journal pins, nobody uses and whose last write did not fail, busy and
 * referenced, looking at BUF_WRITEBACK_SCAN lines from the oldest (lines
 * being written stay on the list until they are clean).  A worker also
 * passes over a device whose medium is gone or whose read-ahead is
 * stopped.  NULL when there is none.
 */
static struct buf *
writeback_oldest(
	struct disk *leaf,
	int worker)
{
	struct buf *candidate;
	struct disk *device;
	unsigned long irq;
	unsigned seen;
	int taken;

	/* Walks a dirty list, in the order lines became dirty; the fields read are only hints here. */
	irq = spin_lock_irqsave(&dirty_index_lock);

	candidate = leaf != NULL ? leaf->d_dirty_buffers : dirty_head;
	for (seen = 0; candidate != NULL && seen < BUF_WRITEBACK_SCAN; seen++) {
		device = candidate->b_disk;
		if (candidate->b_journal_pin == 0 &&
		    !candidate->b_busy &&
		    !candidate->b_io_inflight &&
		    (candidate->b_flags & BUF_ERROR) == 0 &&
		    (!worker || (device->d_media_revoked == 0 && device->d_readahead_stops == 0))) {
			refcount_get(&candidate->b_refs);
			break;
		}
		candidate = leaf != NULL ? candidate->b_device_dirty_next : candidate->b_dirty_next;
	}
	if (seen == BUF_WRITEBACK_SCAN)
		candidate = NULL;

	spin_unlock_irqrestore(&dirty_index_lock, irq);

	/* Nothing is free to write. */
	if (candidate == NULL)
		return NULL;

	/* Takes the line off the reclaim list, then its ownership without waiting. */
	irq = spin_lock_irqsave(&cache_lock);

	lru_remove_locked(candidate);

	spin_unlock_irqrestore(&cache_lock, irq);

	irq = spin_lock_irqsave(&candidate->b_lock);

	taken = !candidate->b_busy && (candidate->b_flags & BUF_ERROR) == 0;
	if (taken)
		candidate->b_busy = 1;

	spin_unlock_irqrestore(&candidate->b_lock, irq);

	/* Another thread took it meanwhile, or its write failed meanwhile. */
	if (!taken) {
		drop_caller_reference(candidate);
		return NULL;
	}

	/* Succeeded: the line, owned. */
	return candidate;
}

/*
 * Remembers a device a worker wrote to, so that the flusher flushes it at
 * its next interval; the list holds a reference to each device.  A device
 * that does not fit is flushed here.
 */
static void
writeback_flush_note(
	struct disk *leaf)
{
	unsigned long irq;
	unsigned index;
	int kept;

	/* Takes a reference for the list before the lock. */
	disk_ref(leaf);
	kept = 0;

	irq = spin_lock_irqsave(&readahead_lock);

	for (index = 0; index < writeback_unflushed_count; index++) {
		if (writeback_unflushed[index] == leaf)
			kept = 2;
	}
	if (kept == 0 && writeback_unflushed_count < BUF_FLUSH_LEAVES) {
		writeback_unflushed[writeback_unflushed_count++] = leaf;
		kept = 1;
	}

	spin_unlock_irqrestore(&readahead_lock, irq);

	/* A device already listed keeps one reference; one that did not fit is flushed now. */
	if (kept == 0)
		(void)bio_flush(leaf);
	if (kept != 1)
		disk_release(leaf);
}

/* Flushes the devices the workers wrote to since the last interval; the flusher calls it. */
static void
writeback_flush_noted(
	void)
{
	struct disk *leaves[BUF_FLUSH_LEAVES];
	unsigned long irq;
	unsigned count;
	unsigned index;

	/* Takes the list over. */
	irq = spin_lock_irqsave(&readahead_lock);

	count = writeback_unflushed_count;
	for (index = 0; index < count; index++)
		leaves[index] = writeback_unflushed[index];
	writeback_unflushed_count = 0;

	spin_unlock_irqrestore(&readahead_lock, irq);

	/* Flushes each device and gives its reference back. */
	for (index = 0; index < count; index++) {
		(void)bio_flush(leaves[index]);
		disk_release(leaves[index]);
	}
}

/*
 * Writes back the oldest dirty line that can be written (of one device, or
 * of any), with its neighbours.  A worker marks the device as the one it
 * runs on while it writes, so that a stopper waits for it, and gives up a
 * line of a device stopped meanwhile.  ENOENT when there is nothing to
 * write.
 */
static int
writeback_oldest_cluster(
	struct disk *leaf,
	struct buf_readahead_worker *worker)
{
	struct buf *buffer;
	struct disk *device;
	unsigned long irq;
	int stopped;
	int error;

	/* Takes the line. */
	buffer = writeback_oldest(leaf, worker != NULL);
	if (buffer == NULL)
		return ENOENT;
	device = buffer->b_disk;

	/* A worker runs on the device only while it is not stopped. */
	if (worker != NULL) {
		irq = spin_lock_irqsave(&readahead_lock);

		stopped = device->d_readahead_stops != 0;
		if (!stopped)
			worker->running = device;

		spin_unlock_irqrestore(&readahead_lock, irq);

		if (stopped) {
			buf_release(buffer);
			return ENOENT;
		}
	}

	/* Writes the line and its neighbours, then gives it back. */
	error = writeback_cluster(buffer);
	buf_release(buffer);

	/* Lets a stopper go on, and leaves the device flush to the flusher. */
	if (worker != NULL) {
		irq = spin_lock_irqsave(&readahead_lock);

		worker->running = NULL;
		waitq_wake_all(&readahead_changed);

		spin_unlock_irqrestore(&readahead_lock, irq);

		if (error == 0)
			writeback_flush_note(device);
	}

	/* Reports the outcome. */
	return error;
}

/*
 * Before or after a delayed write past the dirty bound, writes back some
 * of the oldest dirty lines of the writer's own device in the writer's
 * thread, down to seven eighths of the bound, so that the write can stay
 * in the cache.  The writer may own one line here; it never waits for
 * another.  A failure is left to the flusher (the lines stay dirty and
 * marked); the writer's own write is not failed by it.
 */
static void
writeback_assist(
	const struct disk *disk,
	struct disk *leaf,
	const struct io_context *context,
	int raw)
{
	uint64_t limit;
	unsigned written;

	/* Only a write that would stay in the cache helps, on a device that takes page lists. */
	if (((disk->d_flags & DISK_WRITE_CACHED) == 0 && !raw) ||
	    (context != NULL && context->claim != NULL) ||
	    !leaf_takes_pages(leaf) ||
	    thread_current() == NULL)
		return;

	/* Writes clusters while dirty memory is past the mark, a bounded number of them. */
	limit = dirty_limit_of(disk);
	if (atomic_u64_load_acquire(&cache_dirty_bytes) <= limit)
		return;
	for (written = 0; written < BUF_FLUSH_LEAVES; written++) {
		if (atomic_u64_load_acquire(&cache_dirty_bytes) <= limit - limit / 8U)
			break;
		if (writeback_oldest_cluster(leaf, NULL) != 0)
			break;
	}
}

/*
 * Asks the read-ahead workers to write the oldest dirty lines back once
 * dirty memory is past half of its bound, making the workers if there are
 * none yet.  The caller owns no line and holds no lock.
 */
static void
writeback_kick(
	const struct disk *disk)
{
	unsigned long irq;

	/* Below half of the bound, or already asked, there is nothing to do. */
	if (writeback_wanted ||
	    atomic_u64_load_acquire(&cache_dirty_bytes) <= dirty_limit_of(disk) / 2U ||
	    thread_current() == NULL)
		return;

	/* The workers are made by the first read-ahead or write-back. */
	if (readahead_start() != 0)
		return;

	/* Wakes the workers once. */
	irq = spin_lock_irqsave(&readahead_lock);

	if (!writeback_wanted) {
		writeback_wanted = 1;
		waitq_wake_all(&readahead_work);
	}

	spin_unlock_irqrestore(&readahead_lock, irq);
}

/*
 * Writes back the oldest dirty lines in a worker until dirty memory is a
 * quarter of its bound, nothing can be written, a write fails, or a
 * read-ahead job waits.
 */
static void
writeback_background(
	struct buf_readahead_worker *worker)
{
	unsigned long irq;
	int waiting;
	int error;

	/* Writes clusters from the oldest. */
	error = 0;
	while (atomic_u64_load_acquire(&cache_dirty_bytes) >
	       atomic_u64_load_acquire(&dirty_limit_bytes) / 4U) {
		/* A read-ahead job goes first. */
		irq = spin_lock_irqsave(&readahead_lock);
		waiting = readahead_queue_count != 0;
		spin_unlock_irqrestore(&readahead_lock, irq);
		if (waiting)
			return;

		error = writeback_oldest_cluster(NULL, worker);
		if (error != 0)
			break;
	}

	/* Stops asking; the next delayed write past half of the bound asks again. */
	irq = spin_lock_irqsave(&readahead_lock);
	writeback_wanted = 0;
	spin_unlock_irqrestore(&readahead_lock, irq);
}

/*
 * Writes a whole line to its disk.
 *
 * A delayed write-back writes it as the filesystem, because a raw write
 * would be refused on a volume where a file is leased; an immediate one
 * inherits its writer's guard.
 */
static int
writeback_line_whole(
	struct buf *buffer,
	const struct io_context *drain,
	int delayed)
{
	struct backing_mutation_guard guard;
	int error;

	/* A delayed write-back takes the filesystem's guard itself. */
	kern_memset(&guard, 0, sizeof(guard));
	error = 0;
	if (delayed) {
		error = backing_mutation_begin_disk_filesystem(buffer->b_disk,
							       buffer->b_block,
							       buffer->b_block_count,
							       &guard);
	}

	/* Sends the line to the device once the write is admitted. */
	if (error == 0) {
		error = disk_write_direct_context(buffer->b_disk,
						  buffer->b_block,
						  buffer->b_block_count,
						  buffer->b_data,
						  drain);
	}

	/* Ends the filesystem's write a delayed write-back took. */
	if (delayed)
		backing_mutation_end(&guard);

	/* Reports why the write failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Writes a line in runs of blocks, leaving out the blocks of claims other
 * than the writer's.
 *
 * Each run is written as the filesystem under a guard of its own, so the
 * claim registry still judges every block that is written: a run that
 * came to touch another claim meanwhile is refused as before.
 */
static int
writeback_line_runs(
	struct buf *buffer,
	const struct io_context *drain,
	const struct backing_claim *writer)
{
	struct backing_mutation_guard guard;
	const struct backing_claim *owner;
	const struct backing_claim *next_owner;
	const uint8_t *data;
	uint64_t start;
	uint64_t end;
	uint32_t block_size;
	int error;

	/* The line's data and its block size place each run in it. */
	data = buffer->b_data;
	block_size = buffer->b_disk->d_block_size;

	/* Walks the line one run of blocks with the same kind of owner at a time. */
	for (start = 0; start < buffer->b_block_count; start = end) {
		/* Finds whether another claim owns the run's first block. */
		error = backing_claim_find_extent_owner(buffer->b_disk,
							buffer->b_block + start,
							1, writer, &owner);
		if (error != 0)
			return error;

		/* Extends the run while the blocks are owned the same way. */
		for (end = start + 1; end < buffer->b_block_count; end++) {
			error = backing_claim_find_extent_owner(buffer->b_disk,
								buffer->b_block + end,
								1, writer,
								&next_owner);
			if (error != 0)
				return error;
			if ((next_owner == NULL) != (owner == NULL))
				break;
		}

		/* Another claim's blocks are its owner's to write. */
		if (owner != NULL)
			continue;

		/* Writes the run as the filesystem. */
		error = backing_mutation_begin_disk_filesystem(buffer->b_disk,
							       buffer->b_block + start,
							       end - start,
							       &guard);
		if (error != 0)
			return error;
		error = disk_write_direct_context(buffer->b_disk,
						  buffer->b_block + start,
						  (uint32_t)(end - start),
						  data + start * block_size,
						  drain);
		backing_mutation_end(&guard);
		if (error != 0)
			return error;
	}

	/* Succeeded: every block outside the other claims was written. */
	return 0;
}

/* Records a run's outcome in the I/O statistics, unless it is a prefetch. */
static void
run_record(
	int prefetch,
	enum io_stat_event event,
	uint64_t bytes)
{
	/* A prefetch is no reader's I/O; it only says why it read nothing. */
	if (prefetch) {
		if (event == IO_BUF_SINGLE_GEOMETRY)
			stat_add(&readahead_skip_geometry, 1);
		else if (event == IO_BUF_SINGLE_MEMORY)
			stat_add(&readahead_skip_memory, 1);
		else if (event == IO_BUF_SINGLE_BUSY)
			stat_add(&readahead_skip_busy, 1);
		else if (event == IO_BUF_SINGLE_HIT)
			stat_add(&readahead_skip_hit, 1);
		return;
	}

	io_stats_record(event, bytes);
}

/* Ends a prefetch's I/O on a line it could not read, leaving the line missing. */
static void
finish_unread_line(
	struct buf *buffer)
{
	unsigned long irq;

	/* Neither valid nor failed: the next reader reads the line itself. */
	irq = spin_lock_irqsave(&buffer->b_lock);

	buffer->b_io_state = BUF_IO_IDLE;
	buffer->b_io_inflight = 0;
	buffer->b_error = 0;
	buffer->b_flags &= ~(BUF_VALID | BUF_ERROR);
	waitq_wake_all(&buffer->b_waitq);

	spin_unlock_irqrestore(&buffer->b_lock, irq);
}

/* Finds a disk's physical device; the caller holds the disk, which pins its parents. */
static struct disk *
readahead_leaf(
	struct disk *disk)
{
	struct disk *leaf;

	/* Walks up the partitions. */
	leaf = disk;
	while (leaf->d_parent != NULL)
		leaf = leaf->d_parent;

	/* Succeeded: the physical device. */
	return leaf;
}

/* Makes the read-ahead workers once, each with its run bookkeeping. */
static int
readahead_start(
	void)
{
	struct buf_readahead_worker *worker;
	struct buf_run_scratch *scratch;
	struct thread *thread;
	unsigned long irq;
	unsigned index;
	int first;
	int error;

	/* Only the first caller makes them; later callers use what it made. */
	first = atomic_try_acquire_zero(&readahead_started);
	if (!first) {
		if (readahead_ready == 0)
			return ENOMEM;
		return 0;
	}

	/* Makes each worker; one that cannot be made is simply not there. */
	for (index = 0; index < BUF_READAHEAD_WORKERS; index++) {
		worker = &readahead_workers[index];
		scratch = kern_malloc(sizeof(*scratch));
		if (scratch == NULL)
			break;
		worker->scratch = scratch;
		worker->running = NULL;
		error = kthread_create(readahead_worker, worker, SCHED_PRIORITY_DEFAULT, &thread);
		if (error != 0) {
			kern_free(scratch);
			worker->scratch = NULL;
			break;
		}
		worker->thread = thread;

		irq = spin_lock_irqsave(&readahead_lock);

		readahead_ready++;

		spin_unlock_irqrestore(&readahead_lock, irq);

		thread_start(thread);
	}

	/* Reports that no worker could be made. */
	if (readahead_ready == 0)
		return ENOMEM;

	/* Succeeded: the workers wait for jobs. */
	return 0;
}

/*
 * Finds the entry following an owner's reads of a disk, taking the entry
 * used longest ago when there is none; readahead_lock is held.
 */
static struct buf_readahead_entry *
readahead_entry(
	const void *owner,
	struct disk *disk,
	struct disk *leaf)
{
	struct buf_readahead_entry *entry;
	struct buf_readahead_entry *oldest;
	unsigned index;

	/* Looks for the owner's entry, remembering the oldest one. */
	readahead_clock++;
	oldest = &readahead_streams[0];
	for (index = 0; index < BUF_READAHEAD_STREAMS; index++) {
		entry = &readahead_streams[index];
		if (entry->owner == owner && entry->disk == disk) {
			entry->used = readahead_clock;
			return entry;
		}
		if (entry->used < oldest->used)
			oldest = entry;
	}

	/* Takes the oldest entry over, making its old jobs stale. */
	buf_readahead_restart(&oldest->stream);
	oldest->owner = owner;
	oldest->disk = disk;
	oldest->leaf = leaf;
	oldest->used = readahead_clock;

	/* Succeeded: a fresh entry for the owner. */
	return oldest;
}

/* Finds an owner's entry while its stream is still in a generation; readahead_lock is held. */
static struct buf_readahead_entry *
readahead_current(
	const void *owner,
	uint64_t generation)
{
	unsigned index;

	/* Looks for the owner's entry in that generation. */
	for (index = 0; index < BUF_READAHEAD_STREAMS; index++) {
		if (readahead_streams[index].owner == owner &&
		    readahead_streams[index].stream.generation == generation)
			return &readahead_streams[index];
	}

	/* The owner has gone or moved on. */
	return NULL;
}

/* Reports whether a worker runs a job on a physical device; readahead_lock is held. */
static int
readahead_running(
	struct disk *leaf)
{
	unsigned index;

	/* Looks at every worker's job. */
	for (index = 0; index < BUF_READAHEAD_WORKERS; index++) {
		if (readahead_workers[index].running == leaf)
			return 1;
	}

	/* No job runs there. */
	return 0;
}

/* Runs read-ahead jobs one after another. */
static void
readahead_worker(
	void *argument)
{
	struct buf_readahead_worker *worker;
	struct buf_readahead_job job;
	struct buf_readahead_entry *entry;
	uint64_t sequence;
	uint64_t started;
	uint64_t ended;
	unsigned long irq;
	int current;

	/* Takes each job, runs it unless it went stale, and gives back its disk. */
	worker = argument;
	for (;;) {
		irq = spin_lock_irqsave(&readahead_lock);

		while (readahead_queue_count == 0 && !writeback_wanted) {
			sequence = waitq_sequence(&readahead_work);
			(void)waitq_sleep(&readahead_work, &readahead_lock, sequence, 0, 0);
		}

		/* With no read to do, writes dirty lines back as asked. */
		if (readahead_queue_count == 0) {
			spin_unlock_irqrestore(&readahead_lock, irq);
			writeback_background(worker);
			continue;
		}
		job = readahead_queue[readahead_queue_head];
		readahead_queue_head = (readahead_queue_head + 1U) % BUF_READAHEAD_QUEUE;
		readahead_queue_count--;

		/* The job runs only while its stream and its device still want it. */
		current = 0;
		entry = NULL;
		if (job.owner != NULL)
			entry = readahead_current(job.owner, job.generation);
		if ((job.owner == NULL || entry != NULL) && job.leaf->d_readahead_stops == 0)
			current = 1;
		worker->running = job.leaf;

		spin_unlock_irqrestore(&readahead_lock, irq);

		/* Reads the run, or counts the stale job. */
		started = readahead_now_ns();
		if (current)
			readahead_run(worker, &job);
		else
			stat_add(&readahead_stale, 1);
		ended = readahead_now_ns();

		/* Gives back the disk before a stopper can see the device idle. */
		disk_release(job.disk);

		irq = spin_lock_irqsave(&readahead_lock);

		worker->running = NULL;
		waitq_wake_all(&readahead_changed);
		if (started >= job.queued_ns && ended >= started) {
			readahead_wait_ns += started - job.queued_ns;
			if (started - job.queued_ns > readahead_wait_max_ns)
				readahead_wait_max_ns = started - job.queued_ns;
			readahead_run_ns += ended - started;
			if (ended - started > readahead_run_max_ns)
				readahead_run_max_ns = ended - started;
		}

		spin_unlock_irqrestore(&readahead_lock, irq);
	}
}

/* Reads one job's run into the cache, under the disk's cached-access admission. */
static void
readahead_run(
	struct buf_readahead_worker *worker,
	const struct buf_readahead_job *job)
{
	struct disk *token;
	struct disk *leaf;
	uint64_t mapped;
	uint64_t finished;
	uint64_t wait_ns;
	uint64_t wait_max_ns;
	uint64_t run_ns;
	uint64_t run_max_ns;
	uint64_t prepare_ns;
	uint64_t transfer_ns;
	uint32_t transferred;
	uint32_t step;
	uint64_t done;
	uint64_t line_blocks;
	unsigned long irq;
	int error;

	/* Enters the disk as a cached reader would; a reserved or gone disk refuses. */
	error = disk_cache_acquire(job->disk, &token);
	if (error != 0) {
		stat_add(&readahead_refused, 1);
		return;
	}

	/*
	 * Reads the missing lines of the run straight into their pages.  A
	 * file system's job steps over lines already there (its reader's own
	 * reads may have made the first ones) to the first missing one.
	 */
	transferred = 0;
	error = disk_resolve_range(job->disk, job->block, job->count, &leaf, &mapped);
	done = 0;
	line_blocks = KERN_PAGE_SIZE / job->disk->d_block_size;
	while (error == 0 && done < job->count) {
		step = 0;
		error = transfer_run_lines(leaf, mapped + done, job->count - done, NULL, 0, &step, NULL,
		    worker->scratch->lines, worker->scratch->generations, worker->scratch->pages,
		    BUF_RUN_PAGE_LINES, 1);
		transferred += step;
		if (job->owner != NULL)
			break;
		done += step != 0 ? step : line_blocks - (mapped + done) % line_blocks;
	}
	disk_cache_release(token);

	/* Counts what the job did. */
	if (error != 0)
		stat_add(&readahead_errors, 1);
	else if (transferred == 0)
		stat_add(&readahead_present, 1);
	else
		stat_add(&readahead_read_bytes, (uint64_t)transferred * job->disk->d_block_size);

	/* Sums read-ahead up in the log now and then. */
	finished = atomic_u64_fetch_add_relaxed(&readahead_finished, 1) + 1U;
	if (finished % BUF_READAHEAD_LOG_EVERY == 0) {
		irq = spin_lock_irqsave(&readahead_lock);

		wait_ns = readahead_wait_ns;
		wait_max_ns = readahead_wait_max_ns;
		run_ns = readahead_run_ns;
		run_max_ns = readahead_run_max_ns;
		readahead_wait_ns = 0;
		readahead_wait_max_ns = 0;
		readahead_run_ns = 0;
		readahead_run_max_ns = 0;
		prepare_ns = readahead_prepare_ns;
		transfer_ns = readahead_transfer_ns;
		readahead_prepare_ns = 0;
		readahead_transfer_ns = 0;

		spin_unlock_irqrestore(&readahead_lock, irq);

		kern_logf("buf: readahead %llu jobs: queued %llu read %llu MiB, "
			  "full %llu stopped %llu stale %llu present %llu (geometry %llu memory %llu busy %llu hit %llu) "
			  "refused %llu errors %llu, "
			  "queue wait avg %llu us max %llu us, run avg %llu us max %llu us "
			  "(prepare %llu us transfer %llu us)\n",
			  (unsigned long long)finished,
			  (unsigned long long)readahead_queued,
			  (unsigned long long)(readahead_read_bytes / (1024U * 1024U)),
			  (unsigned long long)readahead_full,
			  (unsigned long long)readahead_stopped,
			  (unsigned long long)readahead_stale,
			  (unsigned long long)readahead_present,
			  (unsigned long long)readahead_skip_geometry,
			  (unsigned long long)readahead_skip_memory,
			  (unsigned long long)readahead_skip_busy,
			  (unsigned long long)readahead_skip_hit,
			  (unsigned long long)readahead_refused,
			  (unsigned long long)readahead_errors,
			  (unsigned long long)(wait_ns / BUF_READAHEAD_LOG_EVERY / 1000U),
			  (unsigned long long)(wait_max_ns / 1000U),
			  (unsigned long long)(run_ns / BUF_READAHEAD_LOG_EVERY / 1000U),
			  (unsigned long long)(run_max_ns / 1000U),
			  (unsigned long long)(prepare_ns / BUF_READAHEAD_LOG_EVERY / 1000U),
			  (unsigned long long)(transfer_ns / BUF_READAHEAD_LOG_EVERY / 1000U));
	}
}

/* Reads the time in nanoseconds for read-ahead's log; zero without a counter. */
static uint64_t
readahead_now_ns(
	void)
{
	uint64_t counter;
	uint64_t frequency;
	bool have;

	/* Reads the counter; a machine without one is not measured. */
	have = kern_rtc_read_counter(&counter, &frequency);
	if (!have || frequency == 0U)
		return 0;

	/* Succeeded: whole seconds, then the rest, so the product does not overflow. */
	return (counter / frequency) * 1000000000ULL +
	    (counter % frequency) * 1000000000ULL / frequency;
}

/*
 * Copies out the valid whole lines at the start of a range, up to
 * BUF_RUN_LINES of them, and reports the blocks copied.
 *
 * The lines are looked up and given back under one hold of the cache lock
 * each; a line that is missing, not valid, or in use ends the run without
 * waiting, and zero leaves the range to the paths that read and wait.
 */
static uint32_t
copy_cached_lines(
	struct disk *disk,
	uint64_t block,
	uint64_t remaining,
	uint8_t *out)
{
	struct buf *lines[BUF_RUN_LINES];
	uint64_t line_blocks;
	unsigned count;
	unsigned found;
	unsigned copied;
	unsigned index;
	unsigned long irq;
	int usable;

	/* Takes whole page-sized lines only, from a line's start. */
	if (disk->d_block_size == 0 || disk->d_block_size > KERN_PAGE_SIZE)
		return 0;
	line_blocks = KERN_PAGE_SIZE / disk->d_block_size;
	if (block % line_blocks != 0 || remaining < line_blocks)
		return 0;
	count = BUF_RUN_LINES;
	if (remaining / line_blocks < count)
		count = (unsigned)(remaining / line_blocks);

	/* References the lines that are there, from the first, under one hold. */
	found = 0;
	irq = spin_lock_irqsave(&cache_lock);

	for (index = 0; index < count; index++) {
		lines[index] = hash_find_locked(disk, block + index * line_blocks);
		if (lines[index] == NULL)
			break;
		refcount_get(&lines[index]->b_refs);
		lru_remove_locked(lines[index]);
		found++;
	}

	spin_unlock_irqrestore(&cache_lock, irq);

	/* Copies each valid idle line while owning it, stopping at the first other one. */
	copied = 0;
	for (index = 0; index < found; index++) {
		irq = spin_lock_irqsave(&lines[index]->b_lock);
		usable = 0;
		if (!lines[index]->b_busy &&
		    !lines[index]->b_io_inflight &&
		    (lines[index]->b_flags & BUF_VALID) != 0 &&
		    lines[index]->b_block_count == line_blocks &&
		    lines[index]->b_memory.size == KERN_PAGE_SIZE) {
			lines[index]->b_busy = 1;
			usable = 1;
		}
		spin_unlock_irqrestore(&lines[index]->b_lock, irq);
		if (!usable)
			break;

		kern_memcpy_ram(out + (size_t)index * KERN_PAGE_SIZE, lines[index]->b_data, KERN_PAGE_SIZE);

		irq = spin_lock_irqsave(&lines[index]->b_lock);

		lines[index]->b_busy = 0;
		waitq_wake_all(&lines[index]->b_waitq);

		spin_unlock_irqrestore(&lines[index]->b_lock, irq);
		copied++;
	}

	/* Nothing was there: the caller reads. */
	if (found == 0)
		return 0;

	/* Gives every reference back under one hold. */
	irq = spin_lock_irqsave(&cache_lock);

	for (index = 0; index < found; index++) {
		if (refcount_put_not_last(&lines[index]->b_refs) == 1)
			lru_add_locked(lines[index]);
	}

	spin_unlock_irqrestore(&cache_lock, irq);

	/* Succeeded: the blocks of the lines copied. */
	stat_add(&stat_hits, copied);
	return (uint32_t)(copied * line_blocks);
}

/*
 * Writes the whole lines at the start of a range into the cache and leaves
 * them dirty, up to BUF_RUN_LINES of them, and reports the blocks written;
 * the caller has found the write delayed.
 *
 * The lines are looked up, or taken over from idle clean lines when the
 * cache is full, under one hold of the cache lock, and given back under
 * one more.  A line that is in use, of another shape, or missing while the
 * cache still has room ends the run without waiting, and zero leaves the
 * range to the path that writes a line at a time.  The run is taken only
 * while it keeps dirty memory within the disk's bound, so that a write
 * past the bound still goes through at once.
 */
static uint32_t
write_cached_lines(
	const struct disk *disk,
	struct disk *leaf,
	uint64_t block,
	uint64_t remaining,
	const uint8_t *in)
{
	struct buf *lines[BUF_RUN_LINES];
	uint64_t line_blocks;
	uint64_t line_block;
	uint64_t dirty;
	uint64_t limit;
	unsigned count;
	unsigned found;
	unsigned taken;
	unsigned recycled;
	unsigned index;
	unsigned long irq;
	int usable;

	/* Takes whole page-sized lines only, from a line's start. */
	if (leaf->d_block_size == 0 || leaf->d_block_size > KERN_PAGE_SIZE)
		return 0;
	line_blocks = KERN_PAGE_SIZE / leaf->d_block_size;
	if (block % line_blocks != 0 || remaining < 2U * line_blocks)
		return 0;
	count = BUF_RUN_LINES;
	if (remaining / line_blocks < count)
		count = (unsigned)(remaining / line_blocks);

	/* Leaves out a line the disk's end cuts short. */
	if (block >= leaf->d_block_count)
		return 0;
	if ((leaf->d_block_count - block) / line_blocks < count)
		count = (unsigned)((leaf->d_block_count - block) / line_blocks);
	if (count < 2U)
		return 0;

	/* Keeps the run within the bound of dirty memory. */
	dirty = atomic_u64_load_acquire(&cache_dirty_bytes);
	limit = dirty_limit_of(disk);
	if (dirty > limit)
		return 0;
	if ((limit - dirty) / KERN_PAGE_SIZE < count)
		count = (unsigned)((limit - dirty) / KERN_PAGE_SIZE);
	if (count < 2U)
		return 0;

	/* References the lines that are there, or takes over idle ones, under one hold. */
	found = 0;
	recycled = 0;
	irq = spin_lock_irqsave(&cache_lock);

	for (index = 0; index < count; index++) {
		line_block = block + index * line_blocks;
		lines[index] = hash_find_locked(leaf, line_block);
		if (lines[index] != NULL) {
			/* A hit: referenced and kept out of the LRU while it is written. */
			refcount_get(&lines[index]->b_refs);
			lru_remove_locked(lines[index]);
		} else {
			/* A miss: an idle line of a full cache comes back hashed, invalid and referenced. */
			lines[index] = recycle_line_locked(leaf, line_block, line_blocks, KERN_PAGE_SIZE, 0);
			if (lines[index] == NULL)
				break;
			recycled++;
		}
		found++;
	}

	spin_unlock_irqrestore(&cache_lock, irq);

	/* Counts the lookups as the line-at-a-time path does. */
	stat_add(&stat_hits, found - recycled);
	stat_add(&stat_misses, recycled);
	stat_add(&stat_evictions, recycled);

	/* Takes each idle line of the page's shape in turn, stopping at the first other one. */
	taken = 0;
	for (index = 0; index < found; index++) {
		irq = spin_lock_irqsave(&lines[index]->b_lock);
		usable = 0;
		if (!lines[index]->b_busy &&
		    !lines[index]->b_io_inflight &&
		    lines[index]->b_block_count == line_blocks &&
		    lines[index]->b_memory.size == KERN_PAGE_SIZE) {
			lines[index]->b_busy = 1;
			usable = 1;
		}
		spin_unlock_irqrestore(&lines[index]->b_lock, irq);
		if (!usable)
			break;
		taken++;
	}

	/*
	 * Copies the caller's bytes into each line taken and marks it dirty,
	 * its blocks written outside any journal, as the line-at-a-time path
	 * does, then lets it go.
	 */
	for (index = 0; index < taken; index++) {
		kern_memcpy_ram(lines[index]->b_data, in + (size_t)index * KERN_PAGE_SIZE, KERN_PAGE_SIZE);
		mark_dirty(lines[index], 0, line_blocks, 0);

		irq = spin_lock_irqsave(&lines[index]->b_lock);

		lines[index]->b_busy = 0;
		waitq_wake_all(&lines[index]->b_waitq);

		spin_unlock_irqrestore(&lines[index]->b_lock, irq);
	}

	/* Nothing was looked up: the caller writes a line at a time. */
	if (found == 0)
		return 0;

	/*
	 * Gives every reference back under one hold.  A line taken over but
	 * not written stays hashed, invalid and idle, as a line whose read
	 * has not started would.
	 */
	irq = spin_lock_irqsave(&cache_lock);

	for (index = 0; index < found; index++) {
		if (refcount_put_not_last(&lines[index]->b_refs) == 1)
			lru_add_locked(lines[index]);
	}

	spin_unlock_irqrestore(&cache_lock, irq);

	/* Succeeded: the blocks of the lines written. */
	return (uint32_t)(taken * line_blocks);
}

/*
 * Takes over an idle line for another block when the cache is full;
 * cache_lock is held and the block is known to be missing.
 *
 * Only a clean, unreferenced line of the same disk and the same size near
 * the LRU tail is taken, so that its memory, its accounting and its disk
 * pin carry over unchanged.  The line comes back hashed under its new
 * block, invalid, with the cache's reference and the caller's.
 */
static struct buf *
recycle_line_locked(
	struct disk *disk,
	uint64_t line_block,
	uint64_t line_blocks,
	uint64_t line_bytes,
	int needed)
{
	struct buf *buffer;
	struct buf *previous;
	struct buf saved;
	unsigned long birq;
	unsigned long dirty_irq;
	unsigned bucket;
	unsigned looked;
	int idle;

	/* Recycles only when another line would not fit under the cap, or memory refused a needed one. */
	if (!needed &&
	    cache_current_bytes <= cache_max_bytes &&
	    cache_reserved_bytes <= cache_max_bytes - cache_current_bytes &&
	    line_bytes <= cache_max_bytes - cache_current_bytes - cache_reserved_bytes)
		return NULL;

	/* A disk whose medium is gone takes no new line; the caller's admission reports it. */
	if (disk_media_status(disk) != 0)
		return NULL;

	/*
	 * Looks at a few lines from the LRU tail for an idle one of this disk
	 * and size.  A dirty one is moved to the head, as evict_one() does, so
	 * that the next look sees other lines.
	 */
	looked = 0;
	for (buffer = lru_tail; buffer != NULL && looked < BUF_RECYCLE_LOOK; buffer = previous) {
		previous = buffer->b_lru_prev;
		looked++;
		if (buffer->b_disk != disk ||
		    buffer->b_block_count != line_blocks ||
		    buffer->b_memory.size != line_bytes ||
		    refcount_load(&buffer->b_refs) != 1)
			continue;

		/* A line kept dirty goes to the head, out of the next look's way. */
		if ((buffer->b_flags & BUF_DIRTY) != 0 || buffer->b_dirty_linked) {
			lru_add_locked(buffer);
			continue;
		}

		/* An idle, clean line is taken out of the LRU and the hash table. */
		birq = spin_lock_irqsave(&buffer->b_lock);
		dirty_irq = spin_lock_irqsave(&dirty_index_lock);
		idle = 1;
		if (refcount_load(&buffer->b_refs) != 1 ||
		    buffer->b_busy ||
		    buffer->b_io_inflight ||
		    (buffer->b_flags & BUF_DIRTY) ||
		    buffer->b_dirty_linked ||
		    buffer->b_journal_pin != 0)
			idle = 0;
		spin_unlock_irqrestore(&dirty_index_lock, dirty_irq);
		spin_unlock_irqrestore(&buffer->b_lock, birq);
		if (!idle)
			continue;
		lru_remove_locked(buffer);
		hash_remove_locked(buffer);

		/* Starts the header afresh, keeping its slab slot, memory, disk and generation. */
		saved = *buffer;
		kern_memset(buffer, 0, sizeof(*buffer));
		buffer->b_disk = disk;
		buffer->b_size = saved.b_size;
		buffer->b_memory = saved.b_memory;
		buffer->b_data = saved.b_data;
		buffer->b_slab = saved.b_slab;
		buffer->b_slab_slot = saved.b_slab_slot;
		buffer->b_generation = saved.b_generation;
		buffer->b_flags = saved.b_flags & BUF_GENERATION_EXHAUSTED;
		buffer->b_block = line_block;
		buffer->b_block_count = (uint32_t)line_blocks;
		refcount_init(&buffer->b_refs, 2);
		spin_init(&buffer->b_lock, LOCK_RANK_BUF, "buffer");
		waitq_init(&buffer->b_waitq, "buffer state");

		/* Publishes it under the new block. */
		bucket = buf_hash_key(disk, line_block);
		buffer->b_hash_next = cache_hash[bucket];
		cache_hash[bucket] = buffer;

		/* Succeeded: the line is the caller's to fill. */
		return buffer;
	}

	/* No line near the tail could be taken over. */
	return NULL;
}

/* Adds a prefetch's time to a read-ahead log sum under the read-ahead lock. */
static void
readahead_account(
	uint64_t *sum,
	uint64_t value)
{
	unsigned long irq;

	irq = spin_lock_irqsave(&readahead_lock);

	*sum += value;

	spin_unlock_irqrestore(&readahead_lock, irq);
}
