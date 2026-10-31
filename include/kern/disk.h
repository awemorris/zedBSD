/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Disk (block device)
 */

#ifndef KERN_KERN_DISK_H
#define KERN_KERN_DISK_H

#include <stddef.h>
#include <stdint.h>
#include <uapi/types.h>
#include <kern/atomic.h>
#include <kern/lock.h>
#include <kern/waitq.h>

#define DISK_NAME_MAX	32U
#define DISK_MAX	80U
#define DISK_IDENTITY_TEXT_MAX	64U

#define DISK_READ_ONLY	0x00000001U
#define DISK_REMOVABLE	0x00000002U
#define DISK_PARTITION	0x00000004U
/* Driver guarantees flush persistence and invalidates proofs across reset. */
#define DISK_FLUSH_PROOF	0x00000008U
#define DISK_FILE_BACKED	0x00000010U
/*
 * Writes through the buffer cache stay dirty in memory and are written by
 * the flusher, fsync, sync or eviction rather than before the write
 * returns.  A filesystem sets it for a write-cached mount.
 */
#define DISK_WRITE_CACHED	0x00000020U
/*
 * The driver moves a transfer to and from a list of physical pages
 * (struct bio's b_pages) by DMA, without b_data.  Only a leaf disk's
 * driver sets it; the block layer refuses a page list for any other disk.
 */
#define DISK_PAGE_VECTOR	0x00000040U

#define DISK_IOCTL_GET_GEOMETRY	1UL

enum bio_op {
	BIO_READ,
	BIO_WRITE,
	BIO_FLUSH
};

enum bio_state {
	BIO_NEW,
	BIO_SUBMITTED,
	BIO_COMPLETED
};

struct buf_view;
struct disk;
struct bio;
struct thread;
struct backing_claim;
struct kern_block_info;

int disk_block_info(struct disk *, struct kern_block_info *);

struct disk_geometry {
	uint32_t cylinders;
	uint16_t heads;
	uint16_t sectors_per_track;
};

/*
 * Scalar registry snapshot.  It deliberately contains no disk pointer.
 */
struct disk_info {
	char name[DISK_NAME_MAX];
	dev_t dev;
	uint32_t flags;
	uint32_t block_size;
	uint64_t block_count;
};

struct disk_ops {
	int (*open)(struct disk *disk);
	void (*close)(struct disk *disk);
	int (*submit)(struct disk *disk, struct bio *bio);
	int (*ioctl)(struct disk *disk, unsigned long request, void *argument);
};

#include <kern/io-error.h>
#include <kern/io-context.h>

struct disk {
	struct io_error_state d_write_error;
	/* Dirty buffer membership is protected by the buffer dirty-index guard. */
	struct buf *d_dirty_buffers;
	dev_t d_dev;
	char d_name[DISK_NAME_MAX];
	uint32_t d_flags;
	uint32_t d_block_size;
	uint64_t d_block_count;
	uint32_t d_max_transfer_blocks;
	const struct disk_ops *d_ops;
	void *d_data;
	struct disk *d_parent;
	/* Immutable non-address dependency, pinned from publication to destruction. */
	struct disk *d_media_backing;
	uint64_t d_parent_offset;
	unsigned d_open_count;
	refcount_t d_refs;
	struct spinlock d_lock;
	struct wait_queue d_waitq;
	unsigned d_state;
	unsigned d_inflight;
	unsigned d_opening;
	unsigned d_closing;
	unsigned d_cache_users;
	/*
	 * Cache admissions made through this disk itself (d_cache_users
	 * counts them on the physical device), and the requests in flight on
	 * a physical device, so that a partition's reservation can tell a
	 * sibling's work from work on its own range; registry lock.
	 */
	unsigned d_cache_entries;
	struct bio *d_inflight_head;
	/* Resident buffer pins are distinct from external users; registry lock. */
	unsigned d_buffer_refs;
	/*
	 * How many management operations have stopped the buffer cache's
	 * read-ahead on this physical device; the read-ahead lock protects it.
	 */
	unsigned d_readahead_stops;
	/* Protected by d_lock; outstanding links never retain completed BIOs. */
	struct bio *d_write_head;
	struct bio *d_write_tail;
	uint64_t d_write_accepted;
	uint64_t d_write_completed;
	uint64_t d_write_stable;
	uint64_t d_persist_epoch;
	/* Reset/media lifecycle is independent of ordinary write proof failures. */
	uint64_t d_media_epoch;
	/* Irreversible for this object; old references remain releasable. */
	volatile unsigned d_media_revoked;
	/* Whether its going (and its partitions') was posted to the system's events (disk_media_gone); registry lock. */
	unsigned d_media_gone_posted;
	uint64_t d_stable_epoch;
	unsigned d_stable_valid;
	unsigned d_flush_busy;
	/* Non-NULL only while the whole-disk replacement owner is admitted. */
	struct thread *d_reload_owner;
	/* Borrowed from the sole open description; registry lock protects it. */
	const struct backing_claim *d_admin_owner;
	/* Synchronous kernel call only; never retained across a syscall return. */
	struct thread *d_admin_thread;
	unsigned d_identity_valid;
	uint32_t d_identity_flags;
	char d_identity_type[16];
	char d_identity_uuid[DISK_IDENTITY_TEXT_MAX];
	char d_identity_label[DISK_IDENTITY_TEXT_MAX];
	char d_identity_partuuid[DISK_IDENTITY_TEXT_MAX];
	char d_identity_partlabel[DISK_IDENTITY_TEXT_MAX];
	struct disk *d_next;

	/*
	 * The work hw.diskstats reports (ws134-p006), counted only for a disk
	 * whose driver gave it a kind (DISK_STATS_KIND_*, a physical whole
	 * disk; zero for partitions, loop disks and file systems' disks).
	 * The registry lock protects them: the submission and the completion
	 * of a request change them where they change d_inflight.
	 * d_stats_generation is the set of disks' generation when the disk
	 * appeared; d_stats_busy_since_ns is when it last went from no request
	 * outstanding to one.
	 */
	uint32_t d_stats_kind;
	uint32_t d_stats_generation;
	uint64_t d_stats_read_ops;
	uint64_t d_stats_write_ops;
	uint64_t d_stats_read_bytes;
	uint64_t d_stats_write_bytes;
	uint64_t d_stats_read_ns;
	uint64_t d_stats_write_ns;
	uint64_t d_stats_busy_ns;
	uint64_t d_stats_busy_since_ns;
};

struct bio {
	struct io_context b_context;
	enum bio_op b_op;
	struct disk *b_disk;
	uint64_t b_block;
	uint32_t b_block_count;
	void *b_data;
	struct disk *b_leaf_disk;
	uint64_t b_mapped_block;
	size_t b_transferred;
	int b_error;
	/* Non-NULL transfers terminal ownership to the callback; do not bio_wait. */
	void (*b_done)(struct bio *bio);
	void *b_private;
	struct bio *b_write_previous;
	struct bio *b_write_next;
	uint64_t b_write_sequence;
	struct spinlock b_lock;
	struct wait_queue b_waitq;
	unsigned b_initialized;
	enum bio_state b_state;
	/* When the request was submitted, in nanoseconds, for its disk's hw.diskstats times (ws134-p006). */
	uint64_t b_submitted_ns;
	/*
	 * The data as physical pages instead of b_data: the address of
	 * each KERN_PAGE_SIZE page in transfer order, b_page_count of them,
	 * together exactly b_block_count blocks.  Only for a disk whose leaf
	 * has DISK_PAGE_VECTOR; b_data is then NULL.  The caller owns the pages
	 * and leaves them alone until the request completes.
	 */
	const uint64_t *b_pages;
	uint32_t b_page_count;
	/* Its place among its physical device's requests in flight; registry lock. */
	struct bio *b_inflight_previous;
	struct bio *b_inflight_next;
	unsigned b_inflight_listed;
};

void disk_set_stats_kind(struct disk *disk, uint32_t kind);
int disk_stats_copy(void *output, size_t capacity, size_t *needed);

int disk_write_context(struct disk *disk, uint64_t block, uint32_t count, const void *data, const struct io_context *context);
int disk_write_filesystem_context(struct disk *disk, uint64_t block, uint32_t count, const void *data, const struct io_context *context);
int disk_write_direct_context(struct disk *disk, uint64_t block, uint32_t count, const void *data, const struct io_context *context);
int disk_transfer_progress_context(struct disk *disk, enum bio_op op, uint64_t block, uint32_t count, void *data, uint32_t *completed, const struct io_context *context);
/*
 * Moves whole physical pages by DMA when the disk's leaf has
 * DISK_PAGE_VECTOR; EOPNOTSUPP otherwise, before anything is sent.  The pages hold
 * exactly count blocks; completed reports the confirmed prefix in blocks.
 */
int disk_transfer_pages_context(struct disk *disk, enum bio_op op, uint64_t block, uint32_t count, const uint64_t *pages, uint32_t page_count, uint32_t *completed, const struct io_context *context);
/* The same under a backing claim that may own the range (a reserved raw device). */
int disk_transfer_pages_claimed(struct disk *disk, enum bio_op op, uint64_t block, uint32_t count, const uint64_t *pages, uint32_t page_count, const struct backing_claim *claim, uint32_t *completed);
/*
 * Admits a transfer that bypasses the cache under the reload and lifetime
 * barriers of a cached access, so that it may first bring the cache's lines
 * of its range in line with the disk; disk_direct_end() takes the token
 * (as disk_cache_acquire's) back.
 */
int disk_direct_begin(struct disk *disk, struct disk **token);
void disk_direct_end(struct disk *token);
int disk_page_vector_supported(struct disk *disk);
/*
 * Whether reading ahead a block range of a disk can only be seen by its one
 * opener: no other open disk on the same device overlaps the range.
 */
int disk_readahead_private(struct disk *disk, uint64_t block, uint64_t count);
int disk_write_delayed(struct disk *disk, uint64_t block, uint32_t count, const void *data);

#define DISK_VECTOR_MAX 32U
struct disk_vector {
	void *data;
	size_t length;
};
/* Synchronous contiguous-LBA vector adapter; callers retain every byte range.
 * Only the confirmed whole-BIO prefix is scattered back after a read error. */
int disk_transfer_vector_context(struct disk *disk, enum bio_op op, uint64_t block,
	const struct disk_vector *vectors, unsigned count, uint32_t *completed,
	const struct io_context *context);

/* Retires persistence proof after reset or uncertain media/device state. */
void disk_persistence_invalidate(struct disk *disk);
/* A serialized, locally initiated transport reset retires durability proof.
 * The driver must revoke the medium separately if recovery makes identity uncertain. */
void disk_persistence_forget(struct disk *disk);
/* Closes old-medium admission without draining or flushing the current BIO. */
void disk_media_revoke(struct disk *disk);
/*
 * A physical device that went away while in use (BUG-192): revokes its
 * medium and posts the REMOVE of its partitions and of itself, once.
 */
void disk_media_gone(struct disk *disk);
/* Retires idle revoked physical ancestry; caller then destroys the root slot. */
int disk_media_retire(struct disk *disk);
/* Caller retains the disk and its immutable physical ancestry. */
int disk_media_status(const struct disk *disk);
int disk_buffer_acquire(struct disk *disk);
void disk_buffer_release(struct disk *disk);

struct disk *
disk_alloc(void);

/*
 * Allocate the next Linux-compatible sd name: sda, ..., sdz, sdaa, ... .
 */
int
disk_alloc_sd_name(
	struct disk *disk);

/* Assign a stable NVMe namespace name such as nvme0n1. */
int
disk_alloc_nvme_name(
	struct disk *disk,
	unsigned controller,
	unsigned namespace_id);

int
disk_create(
	struct disk *disk);

void
disk_gone(
	struct disk *disk);

int
disk_gone_if_idle(
	struct disk *disk);

int
disk_destroy(
	struct disk *disk);

/* Whole-disk replacement gate. Caller holds the sole whole-disk open. */
int disk_reload_begin(struct disk *);
int disk_reload_begin_claimed(struct disk *, const struct backing_claim *);
int disk_admin_begin(struct disk *, const struct backing_claim *);
void disk_admin_end(struct disk *, const struct backing_claim *);
int disk_admin_io_begin(struct disk *, const struct backing_claim *);
void disk_admin_io_end(struct disk *, const struct backing_claim *);
void disk_reload_end(struct disk *);
int disk_reload_replace(struct disk *, struct disk **, unsigned);

struct disk *
disk_find(
	const char *name);

struct disk *
disk_find_by_dev(
	dev_t dev);

unsigned
disk_count(void);

unsigned
disk_inflight_count(void);

struct disk *
disk_at(
	unsigned index);

void
disk_ref(
	struct disk *disk);

void
disk_release(
	struct disk *disk);

void
disk_registry_reset(void);

int
disk_get_info(
	const char *name,
	struct disk_info *result);

int
disk_registry_snapshot(
	struct disk_info *entries,
	unsigned capacity,
	unsigned *count_out);

int
disk_open_by_dev(
	dev_t dev,
	struct disk **result);

int
disk_open(
	struct disk *disk);

void
disk_close(
	struct disk *disk);

int
disk_ioctl(
	struct disk *disk,
	unsigned long request,
	void *argument);

int
bio_submit(
	struct disk *disk,
	struct bio *bio);

void
bio_complete(
	struct bio *bio,
	int error,
	size_t transferred);

int
bio_wait(
	struct bio *bio);

int
bio_flush(
	struct disk *disk);

/*
 * Pins cached file access against detach and partition reload.  The token
 * is the disk the access came through (a partition's reservation tells it
 * from a sibling's); disk_cache_leaf names its physical device.
 */
int disk_cache_acquire(struct disk *, struct disk **);
void disk_cache_release(struct disk *);
struct disk *disk_cache_leaf(struct disk *token);

/* Lifecycle-checked metadata copies and validation of their pinned cache identities. */
int disk_read_view(struct disk *, uint64_t, uint32_t, void *, struct buf_view *);
int disk_view_matches(struct disk *, const struct buf_view *);

/*
 * Uncached synchronous primitives for cache fill/writeback and swap only.
 */
int
disk_read_direct(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	void *data);

int
disk_write_direct(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data);

/* Only whole successful BIOs contribute to the confirmed block prefix. */
int
disk_transfer_progress(
	struct disk *disk,
	enum bio_op op,
	uint64_t block,
	uint32_t count,
	void *data,
	uint32_t *completed);

int
disk_write_direct_claimed(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data,
	const struct backing_claim *claim);

/* Trusted filesystem volume writes are distinct from raw block aliases. */
int
disk_write_filesystem(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data);

int
disk_resolve_range(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	struct disk **leaf_out,
	uint64_t *mapped_out);

int
disk_sync(
	struct disk *disk);


int
disk_read(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	void *data);

int
disk_write(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data);

/*
 * Sectors of 512 bytes, the unit FAT, UFS and a swap file's extents number
 * a volume in, moved on a disk of any block size from 512 bytes to a page
 * (disk.c).
 */
#define DISK_SECTOR_SIZE	512U

int
disk_sector_transfer(
	struct disk *disk,
	uint64_t sector,
	uint32_t count,
	void *data,
	int write,
	int direct,
	const struct io_context *context);

int
disk_sector_supported(
	const struct disk *disk);

#endif
