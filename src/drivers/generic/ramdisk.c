/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A block device whose blocks are a range of memory.
 *
 * A request is served at once by copying between the request's buffer and
 * the memory, so the disk needs no thread, no interrupt and no lock of its
 * own: the disk layer has already checked that the request lies inside the
 * disk, and the buffer cache keeps two requests from writing the same
 * block at the same time.  Writes change the memory and nothing else, so
 * they are lost at the next boot.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/ramdisk.h>

#include <kern/disk.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>
#include <uapi/sysctl.h>

/* The block size the disk reports; disk images are laid out in it. */
#define RAMDISK_BLOCK_SIZE	512U

/*
 * The longest request the disk layer hands over at once, in blocks.  A copy
 * has no transfer limit; this only keeps one request from holding the
 * caller for a long copy.
 */
#define RAMDISK_MAX_TRANSFER_BLOCKS	2048U

/* Marks a parameter the disk operations' signatures carry but a memory disk does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * The memory one published disk reads and writes.
 *
 * It is filled once by drv_ramdisk_create() before the disk is published
 * and never changed afterwards; the disk's d_data points at it.
 */
struct ramdisk_unit {
	uint8_t *base;
	uint64_t size;
};

/*
 * The memory of the one ramdisk a boot can have.
 *
 * A boot hands over a single initial ramdisk, so one unit is enough.  It
 * is filled before the disk is published and kept for the life of the
 * system.
 */
static struct ramdisk_unit ramdisk_unit;

/*
 * The published disk of the unit, NULL until drv_ramdisk_create() has
 * published it; kept for the life of the system, so a second creation is
 * refused.
 */
static struct disk *ramdisk_disk;

static int ramdisk_submit(struct disk *disk, struct bio *bio);
static int ramdisk_ioctl(struct disk *disk, unsigned long request, void *argument);

/*
 * The disk operations: requests are served by copying, and no control
 * request is known.
 */
static const struct disk_ops ramdisk_ops = {
	.submit = ramdisk_submit,
	.ioctl = ramdisk_ioctl
};

/*
 * Publishes a range of memory as a disk.
 */
int
drv_ramdisk_create(
	const char *name,
	void *base,
	uint64_t size,
	struct disk **disk_out)
{
	struct disk *disk;
	size_t length;
	size_t index;
	int error;

	/* Refuses a missing or empty range, and one that is not whole blocks. */
	if (name == NULL ||
	    base == NULL ||
	    disk_out == NULL)
		return EINVAL;
	if (size == 0)
		return EINVAL;
	if ((size % RAMDISK_BLOCK_SIZE) != 0)
		return EINVAL;

	/* Refuses a name the disk record cannot hold. */
	length = kern_strlen(name);
	if (length == 0 || length >= DISK_NAME_MAX)
		return EINVAL;

	/* Refuses a second ramdisk: the boot has only one. */
	if (ramdisk_disk != NULL)
		return EBUSY;

	/* Allocates the disk record. */
	disk = disk_alloc();
	if (disk == NULL)
		return ENOMEM;

	/* Records the memory the disk serves. */
	ramdisk_unit.base = (uint8_t *)base;
	ramdisk_unit.size = size;

	/* Names the disk, its terminator included. */
	for (index = 0; index <= length; index++)
		disk->d_name[index] = name[index];

	/* Describes the disk's blocks and how it is served. */
	disk->d_flags = 0;
	disk->d_block_size = RAMDISK_BLOCK_SIZE;
	disk->d_block_count = size / RAMDISK_BLOCK_SIZE;
	disk->d_max_transfer_blocks = RAMDISK_MAX_TRANSFER_BLOCKS;
	disk->d_ops = &ramdisk_ops;
	disk->d_data = &ramdisk_unit;

	/* Lists the disk among the system's disks in hw.diskstats. */
	disk_set_stats_kind(disk, DISK_STATS_KIND_OTHER);

	/* Publishes the disk; the root search reads its partitions later. */
	error = disk_create(disk);
	if (error != 0)
		return error;

	/* Keeps the disk, so that a second creation is refused. */
	ramdisk_disk = disk;

	/* Hands the disk to the caller. */
	*disk_out = disk;

	/* Succeeded: the memory is a disk. */
	return 0;
}

/*
 * Serves one block request by copying between its buffer and the memory.
 *
 * The disk layer checked that the request's blocks lie inside the disk.  A
 * flush has nothing to do: a written block is in the memory once the copy
 * returns.
 */
static int
ramdisk_submit(
	struct disk *disk,
	struct bio *bio)
{
	const struct ramdisk_unit *unit;
	uint8_t *memory;
	size_t length;
	size_t transferred;
	int error;

	/* Finds the memory the disk serves. */
	unit = (const struct ramdisk_unit *)disk->d_data;

	/* Works out where in the memory the request lies and how long it is. */
	memory = unit->base + (size_t)bio->b_mapped_block * RAMDISK_BLOCK_SIZE;
	length = (size_t)bio->b_block_count * RAMDISK_BLOCK_SIZE;

	/* Moves the blocks the way the request asks. */
	error = 0;
	transferred = 0;
	if (bio->b_op == BIO_READ) {
		/* A read copies the blocks out of the memory. */
		kern_memcpy(bio->b_data, memory, length);
		transferred = length;
	} else if (bio->b_op == BIO_WRITE) {
		/* A write copies the blocks into the memory. */
		kern_memcpy(memory, bio->b_data, length);
		transferred = length;
	} else if (bio->b_op == BIO_FLUSH) {
		/* A flush has nothing left to write. */
		transferred = 0;
	} else {
		/* Any other request is not one a memory disk serves. */
		error = EOPNOTSUPP;
	}

	/* Completes the request with what it moved. */
	bio_complete(bio, error, transferred);

	/* Succeeded: the request was completed, with its own error if any. */
	return 0;
}

/* Refuses every control request: the memory disk has none. */
static int
ramdisk_ioctl(
	struct disk *disk,
	unsigned long request,
	void *argument)
{
	UNUSED_PARAMETER(disk);
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(argument);

	/* No control request is known. */
	return EOPNOTSUPP;
}
