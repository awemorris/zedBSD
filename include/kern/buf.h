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

enum buf_io_state {
	BUF_IO_IDLE = 0,
	BUF_IO_READING,
	BUF_IO_WRITING,
};

#define BUF_VALID	0x0001U
#define BUF_DIRTY	0x0002U
#define BUF_ERROR	0x0004U
#define BUF_INVALID	0x0008U

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
};

int
buf_init(void);
int
buf_read(
	struct disk *,
	uint64_t,
	uint32_t,
	void *);
int
buf_write(
	struct disk *,
	uint64_t,
	uint32_t,
	const void *);
int
buf_get(
	struct disk *,
	uint64_t,
	struct buf **);
void
buf_release(
	struct buf *);
void
buf_mark_dirty(
	struct buf *);
int
buf_writeback(
	struct buf *);
int
buf_sync(
	struct disk *);
int
buf_invalidate(
	struct disk *,
	uint64_t,
	uint64_t,
	unsigned);
int
buf_invalidate_disk(
	struct disk *,
	unsigned);
size_t
buf_reclaim(
	size_t,
	unsigned);
void
buf_get_stats(
	struct bufcache_stats *);
int
buf_set_max_bytes(
	uint64_t);
void
buf_reset(void);

#endif
