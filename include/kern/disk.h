/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Disk (block device)
 */

#ifndef ZEDBSD_KERN_DISK_H
#define ZEDBSD_KERN_DISK_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <kern/atomic.h>
#include <kern/lock.h>
#include <kern/waitq.h>

#define DISK_NAME_MAX	32U
#define DISK_MAX	80U
#define DISK_IDENTITY_TEXT_MAX	64U

#define DISK_READ_ONLY	0x00000001U
#define DISK_REMOVABLE	0x00000002U
#define DISK_PARTITION	0x00000004U

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

struct disk;
struct bio;
struct thread;
struct backing_claim;
struct zedbsd_block_info;

int disk_block_info(struct disk *, struct zedbsd_block_info *);

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

struct disk {
	dev_t d_dev;
	char d_name[DISK_NAME_MAX];
	uint32_t d_flags;
	uint32_t d_block_size;
	uint64_t d_block_count;
	uint32_t d_max_transfer_blocks;
	const struct disk_ops *d_ops;
	void *d_data;
	struct disk *d_parent;
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
	/* Non-NULL only while the whole-disk replacement owner is admitted. */
	struct thread *d_reload_owner;
	unsigned d_identity_valid;
	uint32_t d_identity_flags;
	char d_identity_type[16];
	char d_identity_uuid[DISK_IDENTITY_TEXT_MAX];
	char d_identity_label[DISK_IDENTITY_TEXT_MAX];
	char d_identity_partuuid[DISK_IDENTITY_TEXT_MAX];
	char d_identity_partlabel[DISK_IDENTITY_TEXT_MAX];
	struct disk *d_next;
};

struct bio {
	enum bio_op b_op;
	struct disk *b_disk;
	uint64_t b_block;
	uint32_t b_block_count;
	void *b_data;
	struct disk *b_leaf_disk;
	uint64_t b_mapped_block;
	size_t b_transferred;
	int b_error;
	void (*b_done)(struct bio *bio);
	void *b_private;
	struct spinlock b_lock;
	struct wait_queue b_waitq;
	unsigned b_initialized;
	enum bio_state b_state;
};

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

#endif
