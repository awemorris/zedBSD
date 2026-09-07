/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_BUF_H
#define ZEDBSD_KERN_BUF_H

#include <stddef.h>
#include <stdint.h>
#include <hal/hal.h>
#include <kern/atomic.h>
#include <kern/lock.h>
#include <kern/waitq.h>
#include <zedbsd/sysctl.h>

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
	struct hal_pmem b_memory;
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
