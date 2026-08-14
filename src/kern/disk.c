/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/disk.h"
#include "kern/sched.h"

#include <errno.h>
#include <hal/hal.h>

extern struct thread *thread_current(void) __attribute__((weak));
extern void sched_sleep(uint64_t) __attribute__((weak));
extern void sched_wakeup(struct thread *) __attribute__((weak));
extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));

#define DISK_ALLOCATED 1U
#define DISK_LIVE 2U
#define DISK_GONE 3U

static struct disk disks[DISK_MAX];
static uint8_t disk_used[DISK_MAX];
static struct disk *disk_head;
static unsigned live_count;
static dev_t next_dev = 1;

static void zero_bytes(void *p, size_t n)
{
	uint8_t *q = p;
	while (n--)
		*q++ = 0;
}

static int name_equal(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b)
		a++, b++;
	return *a == *b;
}

static int name_valid(const char name[DISK_NAME_MAX])
{
	unsigned i;
	if (name[0] == '\0')
		return 0;
	for (i = 0; i < DISK_NAME_MAX; i++)
		if (name[i] == '\0')
			return 1;
	return 0;
}

struct disk *disk_alloc(void)
{
	unsigned i;
	for (i = 0; i < DISK_MAX; i++) {
		if (disk_used[i])
			continue;
		disk_used[i] = 1;
		zero_bytes(&disks[i], sizeof(disks[i]));
		disks[i].d_state = DISK_ALLOCATED;
		disks[i].d_refcount = 1;
		return &disks[i];
	}
	return NULL;
}

int disk_create(struct disk *disk)
{
	struct disk **tail;
	if (disk == NULL || disk->d_state != DISK_ALLOCATED ||
	    !name_valid(disk->d_name) || disk->d_block_size == 0 ||
	    disk->d_block_count == 0 ||
	    (disk->d_parent == NULL &&
	     (disk->d_ops == NULL || disk->d_ops->submit == NULL)))
		return EINVAL;
	if (disk_find(disk->d_name) != NULL)
		return EEXIST;
	if (next_dev == 0)
		return ENOSPC;
	disk->d_dev = next_dev++;
	disk->d_state = DISK_LIVE;
	disk->d_next = NULL;
	if (disk->d_parent != NULL)
		disk_ref(disk->d_parent);
	for (tail = &disk_head; *tail != NULL; tail = &(*tail)->d_next)
		;
	*tail = disk;
	live_count++;
	return 0;
}

void disk_gone(struct disk *disk)
{
	struct disk **link;
	if (disk == NULL || disk->d_state == DISK_GONE)
		return;
	if (disk->d_state != DISK_LIVE)
		return;
	for (link = &disk_head; *link != NULL; link = &(*link)->d_next) {
		if (*link == disk) {
			*link = disk->d_next;
			live_count--;
			break;
		}
	}
	disk->d_next = NULL;
	disk->d_state = DISK_GONE;
}

void disk_destroy(struct disk *disk)
{
	unsigned i;
	if (disk == NULL || disk->d_state != DISK_GONE ||
	    disk->d_open_count != 0 || disk->d_inflight != 0 ||
	    disk->d_refcount != 1)
		return;
	if (disk->d_parent != NULL)
		disk_release(disk->d_parent);
	for (i = 0; i < DISK_MAX; i++) {
		if (&disks[i] != disk)
			continue;
		zero_bytes(disk, sizeof(*disk));
		disk_used[i] = 0;
		return;
	}
}

struct disk *disk_find(const char *name)
{
	struct disk *disk;
	if (name == NULL)
		return NULL;
	for (disk = disk_head; disk != NULL; disk = disk->d_next)
		if (name_equal(disk->d_name, name))
			return disk;
	return NULL;
}

struct disk *disk_find_by_dev(dev_t dev)
{
	struct disk *disk;
	for (disk = disk_head; disk != NULL; disk = disk->d_next)
		if (disk->d_dev == dev)
			return disk;
	return NULL;
}

unsigned disk_count(void) { return live_count; }

struct disk *disk_at(unsigned index)
{
	struct disk *disk;
	for (disk = disk_head; disk != NULL && index != 0;
	     disk = disk->d_next, index--)
		;
	return disk;
}

void disk_ref(struct disk *disk)
{
	if (disk != NULL)
		disk->d_refcount++;
}

void disk_release(struct disk *disk)
{
	if (disk != NULL && disk->d_refcount > 0)
		disk->d_refcount--;
}

void disk_registry_reset(void)
{
	unsigned i;
	zero_bytes(disks, sizeof(disks));
	for (i = 0; i < DISK_MAX; i++)
		disk_used[i] = 0;
	disk_head = NULL;
	live_count = 0;
	next_dev = 1;
}

int disk_open(struct disk *disk)
{
	int error = 0;
	if (disk == NULL || disk->d_state != DISK_LIVE)
		return ENXIO;
	if (disk->d_ops != NULL && disk->d_ops->open != NULL)
		error = disk->d_ops->open(disk);
	if (error == 0) {
		disk->d_open_count++;
		disk_ref(disk);
	}
	return error;
}

void disk_close(struct disk *disk)
{
	if (disk == NULL || disk->d_open_count == 0)
		return;
	if (disk->d_ops != NULL && disk->d_ops->close != NULL)
		disk->d_ops->close(disk);
	disk->d_open_count--;
	disk_release(disk);
}

int disk_ioctl(struct disk *disk, unsigned long request, void *argument)
{
	if (disk == NULL || disk->d_state != DISK_LIVE)
		return ENXIO;
	if (disk->d_ops == NULL || disk->d_ops->ioctl == NULL)
		return EOPNOTSUPP;
	return disk->d_ops->ioctl(disk, request, argument);
}

int bio_submit(struct disk *disk, struct bio *bio)
{
	struct disk *leaf;
	uint64_t mapped;
	int error;
	if (disk == NULL || bio == NULL || bio->b_state != BIO_NEW ||
	    disk->d_state != DISK_LIVE)
		return EINVAL;
	if (bio->b_op != BIO_FLUSH) {
		if (bio->b_block_count == 0 || bio->b_data == NULL)
			return EINVAL;
		if (bio->b_block >= disk->d_block_count ||
		    bio->b_block_count > disk->d_block_count - bio->b_block)
			return EOVERFLOW;
		if (bio->b_op == BIO_WRITE && (disk->d_flags & DISK_READ_ONLY))
			return EROFS;
	} else if (bio->b_block_count != 0 || bio->b_data != NULL) {
		return EINVAL;
	}
	leaf = disk;
	mapped = bio->b_block;
	while (leaf->d_parent != NULL) {
		if (mapped >= leaf->d_block_count ||
		    bio->b_block_count > leaf->d_block_count - mapped ||
		    mapped > UINT64_MAX - leaf->d_parent_offset)
			return EOVERFLOW;
		mapped += leaf->d_parent_offset;
		leaf = leaf->d_parent;
		if (leaf->d_state != DISK_LIVE)
			return ENXIO;
	}
	if (leaf->d_ops == NULL || leaf->d_ops->submit == NULL)
		return EOPNOTSUPP;
	if (bio->b_op != BIO_FLUSH &&
	    (mapped >= leaf->d_block_count ||
	     bio->b_block_count > leaf->d_block_count - mapped))
		return EOVERFLOW;
	bio->b_disk = disk;
	bio->b_leaf_disk = leaf;
	bio->b_mapped_block = mapped;
	bio->b_transferred = 0;
	bio->b_error = 0;
	bio->b_state = BIO_SUBMITTED;
	leaf->d_inflight++;
	error = leaf->d_ops->submit(leaf, bio);
	if (error != 0) {
		leaf->d_inflight--;
		bio->b_state = BIO_NEW;
		bio->b_leaf_disk = NULL;
	}
	return error;
}

void bio_complete(struct bio *bio, int error, size_t transferred)
{
	struct disk *leaf;
	if (bio == NULL || bio->b_state != BIO_SUBMITTED)
		return;
	leaf = bio->b_leaf_disk;
	bio->b_error = error;
	bio->b_transferred = transferred;
	bio->b_state = BIO_COMPLETED;
	if (leaf != NULL && leaf->d_inflight != 0)
		leaf->d_inflight--;
	if (bio->b_done != NULL)
		bio->b_done(bio);
	if (bio->b_waiter != NULL && sched_wakeup != NULL)
		sched_wakeup(bio->b_waiter);
}

int bio_wait(struct bio *bio)
{
	struct thread *thread;
	bool enabled;

	if (bio == NULL)
		return EINVAL;
	thread = thread_current != NULL ? thread_current() : NULL;
	if (thread != NULL && sched_sleep != NULL &&
	    hal_irq_disable != NULL && hal_irq_enable != NULL) {
		enabled = hal_irq_disable();
		while (bio->b_state == BIO_SUBMITTED) {
			if (bio->b_waiter != NULL && bio->b_waiter != thread) {
				if (enabled) hal_irq_enable();
				return EBUSY;
			}
			bio->b_waiter = thread;
			sched_sleep(0);
		}
		bio->b_waiter = NULL;
		if (enabled)
			hal_irq_enable();
		return bio->b_state == BIO_COMPLETED ? bio->b_error : EINVAL;
	}
	while (bio->b_state == BIO_SUBMITTED)
		hal_compiler_barrier();
	return bio->b_state == BIO_COMPLETED ? bio->b_error : EINVAL;
}

int bio_flush(struct disk *disk)
{
	struct bio bio = { .b_op = BIO_FLUSH };
	int error = bio_submit(disk, &bio);
	return error != 0 ? error : bio_wait(&bio);
}

static int disk_transfer(struct disk *disk, enum bio_op op, uint64_t block,
			 uint32_t count, void *data)
{
	struct bio bio = {
		.b_op = op, .b_block = block, .b_block_count = count,
		.b_data = data,
	};
	int error = bio_submit(disk, &bio);
	return error != 0 ? error : bio_wait(&bio);
}

int disk_read(struct disk *disk, uint64_t block, uint32_t count, void *data)
{
	return disk_transfer(disk, BIO_READ, block, count, data);
}

int disk_write(struct disk *disk, uint64_t block, uint32_t count,
	       const void *data)
{
	return disk_transfer(disk, BIO_WRITE, block, count, (void *)data);
}
