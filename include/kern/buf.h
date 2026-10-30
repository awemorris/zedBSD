/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_KERN_BUF_H
#define KERN_KERN_BUF_H

#include <stddef.h>
#include <stdint.h>
#include <kern/pmem.h>
#include <kern/atomic.h>
#include <kern/lock.h>
#include <kern/waitq.h>
#include <uapi/sysctl.h>

struct disk;
struct io_context;

enum buf_io_state {
	BUF_IO_IDLE = 0,
	BUF_IO_READING,
	BUF_IO_WRITING,
};

#define BUF_VALID	0x0001U
#define BUF_DIRTY	0x0002U
#define BUF_ERROR	0x0004U
#define BUF_INVALID	0x0008U
#define BUF_GENERATION_EXHAUSTED 0x0010U

#define BUF_INVALIDATE_DISCARD	0x0001U
#define BUF_RECLAIM_WRITE	0x0001U

struct buf {
	struct disk *b_disk;
	uint64_t b_block;
	uint32_t b_block_count;
	size_t b_size;
	struct kern_pmem b_memory;
	void *b_data;
	refcount_t b_refs;
	struct spinlock b_lock;
	struct wait_queue b_waitq;
	enum buf_io_state b_io_state;
	unsigned b_flags;
	unsigned b_busy;
	uint64_t b_generation;
	uint64_t b_dirty_generation;
	int b_error;
	unsigned b_io_inflight;
	unsigned b_on_lru;
	struct buf *b_hash_next;
	struct buf *b_lru_prev;
	struct buf *b_lru_next;
	void *b_slab;
	unsigned b_slab_slot;
	struct buf *b_dirty_previous;
	struct buf *b_dirty_next;
	struct buf *b_device_dirty_previous;
	struct buf *b_device_dirty_next;
	unsigned b_dirty_linked;
	/* The scheduler tick at which the buffer last became dirty. */
	uint64_t b_dirty_tick;
	/*
	 * Held by a filesystem journal: the buffer stays dirty and is not
	 * written back until the journal has committed it and unpins it.
	 * Zero is no pin; otherwise it is the tag (the journal's transaction
	 * sequence) the last buf_write_pinned() gave, and only buf_unpin() with
	 * the same tag clears it, so that a later transaction's write keeps the
	 * line home-bound until its own commit (ws073-p051).
	 */
	uint64_t b_journal_pin;
	/*
	 * The blocks of the line that ordinary writes, outside any journal,
	 * left and the disk does not have yet: a bit each, the line's first
	 * block lowest (kern/buf-unjournaled.h).  A journal's pinned write
	 * over a block takes its bit out; a clean write-back clears them all.
	 * Under b_lock, changed by the busy owner (ws073-p053, BUG-163).
	 */
	uint32_t b_unjournaled;
};

/* Zero-initialize before first use. Pins common cache lines, never their busy state. */
#define BUF_VIEW_MAX_LINES 17U
struct buf_view {
	struct disk *disk;
	struct buf *lines[BUF_VIEW_MAX_LINES];
	uint64_t generations[BUF_VIEW_MAX_LINES];
	unsigned count;
	unsigned valid;
};

int buf_read_view(struct disk *, uint64_t, uint32_t, void *, struct buf_view *);
int buf_view_matches(const struct buf_view *);
void buf_view_release(struct buf_view *);

int
buf_init(void);

/*
 * Starts the thread that writes out aged dirty buffers of write-cached
 * disks; later calls do nothing.
 */
int
buf_flusher_start(void);

/*
 * Writes into the cache and pins the lines: they stay dirty and nothing
 * writes them to the disk until buf_unpin() releases them.  For a journal,
 * whose blocks may reach their home only after the journal holds them.
 * The pin is a nonzero tag, the journal's transaction; a line pinned by an
 * earlier transaction takes the new tag.
 */
int
buf_write_pinned(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data,
	uint64_t pin);

/*
 * Releases the pins buf_write_pinned() set on a range with the same tag;
 * those lines are then ordinary dirty lines.  A line a later transaction
 * pinned again keeps its pin; *kept, when given, is raised by the number of
 * such lines, so that the journal knows the cache will not write the
 * range's home until that transaction commits.
 */
int
buf_unpin(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	uint64_t pin,
	unsigned *kept);

/*
 * Writes the dirty lines of a range back to the disk, leaving a line a
 * journal pins alone; the device is not flushed.
 */
int
buf_writeback_range(
	struct disk *disk,
	uint64_t block,
	uint32_t count);

/*
 * Writes the dirty lines of a range that the cache holds back to the disk,
 * without bringing a missing line in and without flushing the device.  A
 * line a journal pins keeps its journaled blocks for the commit, while the
 * blocks of the range that ordinary writes left in it are written beneath
 * the cache.  For a file's sync, which writes its own blocks and no others
 * (BUG-307).
 */
int
buf_writeback_cached(
	struct disk *disk,
	uint64_t block,
	uint32_t count);

/*
 * Writes, beneath the cache, the blocks of a disk's dirty lines that
 * ordinary writes left and the disk does not have yet, and no other block
 * of those lines: a line a journal pins keeps its journaled blocks back for
 * the journal's commit, while its ordinary data reaches the disk now
 * (ws073-p053, BUG-163).  Does not flush the device.
 */
int
buf_write_unjournaled(
	struct disk *disk);

/*
 * Brings the cache's lines of a range in line with the disk before a
 * transfer that bypasses the cache: writes the dirty ones back and, with
 * invalidate, leaves every line of the range to be read again (EBUSY when
 * a journal still pins one).  Does not flush the device.
 */
int
buf_direct_prepare(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	int invalidate);

/*
 * Registers a function the flusher calls on every interval, before it
 * writes aged buffers; NULL removes it.  One slot per argument.
 */
int
buf_flusher_hook(
	void (*hook)(void *),
	void *argument,
	int add);
int
buf_read(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	void *data);
int
buf_write(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data);
int
buf_get(
	struct disk *disk,
	uint64_t block,
	struct buf **result);
void
buf_release(
	struct buf *buffer);
void
buf_mark_dirty(
	struct buf *buffer);
int buf_writeback_context(struct buf *buffer, const struct io_context *context);
int buf_write_context(struct disk *disk, uint64_t block, uint32_t count, const void *data, const struct io_context *context);

int
buf_writeback(
	struct buf *buffer);
int
buf_sync(
	struct disk *disk);
int
buf_invalidate(
	struct disk *disk,
	uint64_t block,
	uint64_t count,
	unsigned flags);
int
buf_invalidate_disk(
	struct disk *disk,
	unsigned flags);
/* Retirement-only: caller excludes external users of the revoked physical disk.
 * Never submits writeback; referenced/in-flight buffers remain owned on EBUSY. */
int buf_discard_media(struct disk *disk);

/*
 * Read-ahead of raw block devices.
 *
 * observe records a completed read of an open block device (owner is the
 * open description) and reads ahead of a sequential reader; forget drops
 * the owner's stream when it closes.  stop keeps read-ahead off a disk's
 * physical device until the matching resume; with wait it sleeps until the
 * read-ahead running there has ended, otherwise it reports EBUSY while
 * some does.
 */
void buf_readahead_observe(const void *owner, struct disk *disk, uint64_t block, uint32_t count);
void buf_readahead_forget(const void *owner);
uint32_t buf_prefetch(struct disk *disk, uint64_t block, uint32_t count);
int buf_write_delayed(struct disk *disk, uint64_t block, uint32_t count, const void *data);
uint64_t buf_prefetch_window(void);
int buf_readahead_stop(struct disk *disk, int wait);
void buf_readahead_resume(struct disk *disk);
size_t
buf_reclaim(
	size_t target_bytes,
	unsigned flags);
void
buf_get_stats(
	struct bufcache_stats *stats);
int
buf_set_max_bytes(
	uint64_t value);
void
buf_reset(void);

#endif
