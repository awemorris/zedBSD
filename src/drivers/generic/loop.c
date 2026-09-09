/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * File-backed loop block devices
 */

#include "kern/loop.h"
#include "kern/backing-claim.h"
#include "kern/block-identity.h"
#include "kern/disk.h"
#include "kern/io-stats.h"
#include "kern/fat.h"
#include "kern/file.h"
#include "kern/file-backing.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/lock.h"
#include "kern/mount.h"
#include "kern/namei.h"

#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LOOP_SECTOR_SIZE 512U
#define LOOP_MAX_TRANSFER_BLOCKS 128U

struct loop_extent_collection {
	struct backing_claim_extent *extents;
	struct fat_loop_extent *map;
	uint64_t next_block;
	unsigned count;
	unsigned capacity;
};

struct loop_device {
	unsigned index;
	unsigned flags;
	bool reserved;
	bool attached;
	bool detaching;
	struct file *backing;
	struct inode *backing_inode;
	struct disk *disk;
	struct backing_claim *claim;
	struct fat_loop_extent *map;
	uint64_t size_bytes;
};

extern unsigned vm_object_cache_drain(struct mount *) __attribute__((weak));

static struct loop_device loops[LOOP_MAX_DEVICES] __attribute__((section(".vfs_bss")));
static struct spinlock loop_lock;

static int loop_backing_valid(struct file *backing, unsigned flags);
static int loop_finalize_claim(struct file *backing, struct backing_claim *claim, struct fat_loop_extent **map, unsigned *map_count);
static int loop_collect_extent(uint64_t file_block, uint64_t disk_block, uint32_t count, void *argument);
static int loop_open(struct disk *disk);
static void loop_close(struct disk *disk);
static int loop_ioctl(struct disk *disk, unsigned long request, void *argument);
static int loop_submit(struct disk *disk, struct bio *bio);

/*
 * Loopback Device
 */

static const struct disk_ops loop_disk_ops = {
	.open = loop_open,
	.close = loop_close,
	.submit = loop_submit,
	.ioctl = loop_ioctl,
};

/*
 * Implements the drv loop init operation.
 */
int
drv_loop_init(
	void)
{
	unsigned i;

	spin_init(&loop_lock, LOCK_RANK_DISK, "loop registry");
	memset(loops, 0, sizeof(loops));
	/* Process each element required by the operation. */
	for (i = 0; i < LOOP_MAX_DEVICES; i++)
		loops[i].index = i;

	/* Succeeded. */
	return 0;
}

/* Pins the backing file while the attachment cannot be withdrawn. */
int
drv_loop_backing_file_ref(struct disk *disk, struct file **result, unsigned *flags)
{
	struct loop_device *loop;
	unsigned long irq;
	unsigned index;

	if (disk == NULL || result == NULL || flags == NULL)
		return EINVAL;
	*result = NULL;
	*flags = 0;
	if (disk->d_ops != &loop_disk_ops)
		return EOPNOTSUPP;

	/* file_ref only increments its refcount while the loop owns the file. */
	irq = spin_lock_irqsave(&loop_lock);
	for (index = 0; index < LOOP_MAX_DEVICES; index++) {
		loop = &loops[index];
		if (loop->disk != disk || !loop->attached || loop->detaching)
			continue;
		if (loop->backing != NULL) {
			file_ref(loop->backing);
			*result = loop->backing;
			*flags = loop->flags;
		}
		break;
	}
	spin_unlock_irqrestore(&loop_lock, irq);

	if (*result == NULL)
		return ENXIO;
	return 0;
}

/*
 * Pins an attached loop's backing disk without changing block-address ancestry.
 */
int
drv_loop_backing_disk_ref(
	struct disk *disk,
	struct disk **result)
{
	struct loop_device *loop;
	struct disk *backing;
	unsigned long irq;
	unsigned index;

	/*
	 * Distinguishes unsupported drivers from a recognized loop in teardown.
	 */
	if (disk == NULL || result == NULL)
		return EINVAL;
	*result = NULL;
	/* Handles the disk condition. */
	if (disk->d_ops != &loop_disk_ops)
		return EOPNOTSUPP;

	/*
	 * Pins the backing while detach cannot withdraw its retained file
	 * owner.
	 */
	backing = NULL;
	irq = spin_lock_irqsave(&loop_lock);

	/* Process each remaining element. */
	for (index = 0; index < LOOP_MAX_DEVICES; index++) {
		/* Handles the loop condition. */
		loop = &loops[index];
		if (loop->disk != disk || !loop->attached || loop->detaching)
			continue;

		/* Handles the backing inode availability. */
		if (loop->backing_inode != NULL &&
		    loop->backing_inode->i_mount != NULL)
			backing = loop->backing_inode->i_mount->m_disk;

		/* Handles the backing availability. */
		if (backing != NULL)
			disk_ref(backing);
		break;
	}

	spin_unlock_irqrestore(&loop_lock, irq);

	/* Handles the backing availability. */
	if (backing == NULL)
		return ENXIO;

	/* Transfers one ordinary disk reference to the caller. */
	*result = backing;
	/* Succeeded. */
	return 0;
}

/*
 * Implements the drv loop get index operation.
 */
int
drv_loop_get_index(
	const struct disk *disk,
	unsigned *index_out)
{
	unsigned i;
	unsigned long irq;

	/* Handles the disk availability. */
	if (disk == NULL || index_out == NULL)
		return EINVAL;
	irq = spin_lock_irqsave(&loop_lock);

	/* Process each element required by the operation. */
	for (i = 0; i < LOOP_MAX_DEVICES; i++) {
		/* Handles the loops condition. */
		if (loops[i].attached && loops[i].disk == disk) {
			*index_out = i;
			spin_unlock_irqrestore(&loop_lock, irq);

			/* Succeeded. */
			return 0;
		}
	}

	spin_unlock_irqrestore(&loop_lock, irq);

	/* Failed. */
	return ENODEV;
}

/*
 * Implements the drv loop attach file operation.
 */
int
drv_loop_attach_file(
	struct file *backing,
	unsigned flags,
	struct disk **disk_out)
{
	struct block_identity identity;
	struct loop_device *loop = NULL;
	struct inode *backing_inode;
	struct disk *disk;
	unsigned i;
	int error;
	unsigned long irq;
	struct backing_claim *claim = NULL;
	struct fat_loop_extent *map = NULL;
	unsigned map_count = 0;

	/* Handles the disk out availability. */
	if (disk_out == NULL)
		return EINVAL;
	*disk_out = NULL;

	/* Checks the operation status. */
	error = loop_backing_valid(backing, flags);
	if (error != 0)
		return error;

	/*
	 * Retires optional caches before reserving independent backing
	 * ownership.
	 */
	if (vm_object_cache_drain != NULL)
		(void)vm_object_cache_drain(NULL);
	backing_inode = backing->f_inode;

	/* Checks the operation status. */
	error = backing_claim_prepare_inode(backing_inode, BACKING_CLAIM_LOOP,
					    &claim);
	if (error == 0)
		error = loop_finalize_claim(backing, claim, &map, &map_count);
	else if (error == EOPNOTSUPP)
		error = 0;
	if (error != 0) {
		kern_free(map);
		backing_claim_release(claim);

		/* Failed. */
		return error;
	}

	/* Handles the backing condition. */
	irq = spin_lock_irqsave(&loop_lock);
	if ((backing->f_inode->i_flags & (INODE_SWAPFILE | INODE_LOOPFILE)) !=
	    0) {
		spin_unlock_irqrestore(&loop_lock, irq);
		kern_free(map);
		backing_claim_release(claim);

		/* Failed. */
		return EBUSY;
	}

	/* Process each element required by the operation. */
	for (i = 0; i < LOOP_MAX_DEVICES; i++) {
		/* Handles the loops condition. */
		if (!loops[i].attached && !loops[i].reserved) {
			loop = &loops[i];
			loop->reserved = true;
			break;
		}
	}

	/* Handles the loop availability. */
	if (loop == NULL) {
		spin_unlock_irqrestore(&loop_lock, irq);
		kern_free(map);
		backing_claim_release(claim);

		/* Failed. */
		return ENOSPC;
	}

	backing->f_inode->i_flags |= INODE_LOOPFILE;

	spin_unlock_irqrestore(&loop_lock, irq);

	file_ref(backing);
	inode_ref(backing->f_inode);

	/* Handles the disk availability. */
	disk = disk_alloc();
	if (disk == NULL) {
		error = ENOSPC;
		goto fail_refs;
	}

	disk->d_name[0] = 'l';
	disk->d_name[1] = 'o';
	disk->d_name[2] = 'o';
	disk->d_name[3] = 'p';
	disk->d_name[4] = (char)('0' + loop->index);
	disk->d_name[5] = '\0';
	disk->d_flags = DISK_FILE_BACKED;
	if (flags == LOOP_READ_ONLY)
		disk->d_flags |= DISK_READ_ONLY;
	disk->d_block_size = LOOP_SECTOR_SIZE;
	disk->d_block_count =
		(uint64_t)(uint32_t)backing->f_inode->i_size / LOOP_SECTOR_SIZE;
	disk->d_max_transfer_blocks = LOOP_MAX_TRANSFER_BLOCKS;
	disk->d_ops = &loop_disk_ops;
	disk->d_data = loop;

	/*
	 * Keeps media admission tied to the backing without remapping loop
	 * blocks.
	 */
	if (backing_inode->i_mount != NULL)
		disk->d_media_backing = backing_inode->i_mount->m_disk;
	loop->flags = flags;
	loop->backing = backing;
	loop->backing_inode = backing->f_inode;
	loop->disk = disk;
	loop->claim = claim;
	backing->f_backing_claim = claim;
	/* The FAT map is an optional cache optimization, not the retained claim. */
	if (map != NULL && backing_inode->i_mount->m_type != &drv_fat_filesystem_type) {
		kern_free(map);
		map = NULL;
		map_count = 0;
	}
	loop->map = map;

	/* Handles the map availability. */
	if (map != NULL) {
		/* Checks the operation status. */
		error = drv_fat_file_set_loop_map(backing, map, map_count);
		if (error != 0) {
			(void)disk_destroy(disk);
			goto fail_refs;
		}
	}

	loop->size_bytes = (uint64_t)(uint32_t)backing->f_inode->i_size;

	/* Checks the operation status. */
	error = disk_create(disk);
	if (error != 0) {
		(void)disk_destroy(disk);
		goto fail_refs;
	}

	irq = spin_lock_irqsave(&loop_lock);

	loop->attached = true;
	loop->reserved = false;

	spin_unlock_irqrestore(&loop_lock, irq);

	/*
	 * Probe before the backing filesystem is hidden below a mounted loop.
	 * Runtime BLKGETIDENTITY then reads the immutable cached identity and
	 * never re-enters the mounted backing file.
	 */

	(void)block_identity_get(disk, &identity);
	*disk_out = disk;
	/* Succeeded. */
	return 0;

fail_refs:

	/* Handles the map availability. */
	if (map != NULL && backing->f_backing_claim != NULL)
		(void)drv_fat_file_set_loop_map(backing, NULL, 0);
	kern_free(map);
	irq = spin_lock_irqsave(&loop_lock);

	backing_inode->i_flags &= ~INODE_LOOPFILE;

	spin_unlock_irqrestore(&loop_lock, irq);

	inode_release(backing_inode);
	backing->f_backing_claim = NULL;
	(void)file_close(backing);
	backing_claim_release(claim);
	irq = spin_lock_irqsave(&loop_lock);

	memset(loop, 0, sizeof(*loop));
	loop->index = i;

	spin_unlock_irqrestore(&loop_lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Implements the drv loop attach path operation.
 */
int
drv_loop_attach_path(
	const struct path *root,
	const char *path,
	unsigned flags,
	struct disk **disk_out)
{
	struct cwdinfo context;
	struct file *file;
	int open_flags, error;

	/* Handles the root availability. */
	if (root == NULL || path == NULL)
		return EINVAL;

	/* Checks the operation status. */
	error = cwdinfo_init(&context, root);
	if (error != 0)
		return error;
	open_flags = flags == LOOP_READ_WRITE ? O_RDWR : O_RDONLY;

	/* Checks the operation status. */
	error = file_openat(&context, path, open_flags, 0, &file);
	if (error == 0) {
		/* Checks the operation status. */
		error = drv_loop_attach_file(file, flags, disk_out);
		if (error != 0) {
			hal_printf("loop: attach %s mode=%s file-flags=%x "
				   "size=%u failed (%d)\n",
				   path, flags == LOOP_READ_WRITE ? "rw" : "ro",
				   (unsigned)file_status_flags_get(file),
				   file->f_inode != NULL
					   ? (uint32_t)file->f_inode->i_size
					   : 0U,
				   error);
		}

		(void)file_close(file);
	} else {
		hal_printf("loop: open %s flags=%x failed (%d)\n", path,
			   (unsigned)open_flags, error);
	}

	cwdinfo_destroy(&context);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Implements the drv loop detach operation.
 */
int
drv_loop_detach(
	struct disk *disk)
{
	struct loop_device *loop;
	int error;
	unsigned long irq;
	unsigned index;

	/* Checks the drv loop get index result. */
	if (drv_loop_get_index(disk, &index) != 0)
		return ENODEV;
	loop = &loops[index];

	/* Handles the loop condition. */
	irq = spin_lock_irqsave(&loop_lock);
	if (loop->detaching) {
		spin_unlock_irqrestore(&loop_lock, irq);

		/* Failed. */
		return EBUSY;
	}

	loop->detaching = true;

	spin_unlock_irqrestore(&loop_lock, irq);

	/* Handles the loop condition. */
	if ((loop->flags & LOOP_READ_WRITE) != 0) {
		/* Checks the operation status. */
		error = file_fsync_backend(loop->backing);
		if (error != 0)
			goto retryable;
	}

	/* Checks the operation status. */
	error = disk_gone_if_idle(disk);
	if (error != 0)
		goto retryable;

	/* Checks the operation status. */
	error = disk_destroy(disk);
	if (error != 0)
		return error; /*
 * Invariant failure: keep the slot pinned for
				 diagnosis. */
	irq = spin_lock_irqsave(&loop_lock);

	loop->backing_inode->i_flags &= ~INODE_LOOPFILE;

	spin_unlock_irqrestore(&loop_lock, irq);

	inode_release(loop->backing_inode);

	/* Handles the map availability. */
	if (loop->map != NULL)
		(void)drv_fat_file_set_loop_map(loop->backing, NULL, 0);
	kern_free(loop->map);
	loop->backing->f_backing_claim = NULL;
	backing_claim_release(loop->claim);
	(void)file_close(loop->backing);
	memset(loop, 0, sizeof(*loop));
	loop->index = index;

	/* Succeeded. */
	return 0;

retryable:
	irq = spin_lock_irqsave(&loop_lock);

	loop->detaching = false;

	spin_unlock_irqrestore(&loop_lock, irq);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the loop backing valid operation. */
static int
loop_backing_valid(
	struct file *backing,
	unsigned flags)
{
	struct inode *inode;
	unsigned loop_index;

	/* Handles the backing availability. */
	if (backing == NULL || backing->f_inode == NULL)
		return EINVAL;

	/* Checks the active flags. */
	inode = backing->f_inode;
	if (flags != LOOP_READ_ONLY && flags != LOOP_READ_WRITE)
		return EINVAL;

	/* Handles the inode condition. */
	if (inode->i_type != INODE_REG || inode->i_size <= 0 ||
	    ((uint32_t)inode->i_size & (LOOP_SECTOR_SIZE - 1U)) != 0) {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the uint64 t condition. */
	if ((uint64_t)inode->i_size > (uint64_t)INT32_MAX)
		return EFBIG;

	/* Checks the file status flags get result. */
	if (flags == LOOP_READ_WRITE &&
	    (file_status_flags_get(backing) & O_ACCMODE) == O_RDONLY) {
		/* Failed. */
		return EBADF;
	}

	/* Handles the i mount availability. */
	if (flags == LOOP_READ_WRITE && inode->i_mount != NULL &&
	    (inode->i_mount->m_flags & MOUNT_READ_ONLY) != 0) {
		/* Failed. */
		return EROFS;
	}

	/* Handles the inode condition. */
	if ((inode->i_flags & (INODE_SWAPFILE | INODE_LOOPFILE)) != 0)
		return EBUSY;

	/* Handles the i mount availability. */
	if (inode->i_mount != NULL && inode->i_mount->m_type != NULL &&
	    inode->i_mount->m_type->fs_name != NULL &&
	    !strcmp(inode->i_mount->m_type->fs_name, "overlay")) {
		/* Failed. */
		return ELOOP;
	}

	/* Checks the drv loop get index result. */
	if (inode->i_mount != NULL && inode->i_mount->m_disk != NULL &&
	    drv_loop_get_index(inode->i_mount->m_disk, &loop_index) == 0) {
		/* Failed. */
		return ELOOP;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the loop finalize claim operation. */
static int
loop_finalize_claim(
	struct file *backing,
	struct backing_claim *claim,
	struct fat_loop_extent **map,
	unsigned *map_count)
{
	struct loop_extent_collection collection;
	struct disk *disk;
	unsigned i;
	int error;

	/* Handles the claim availability. */
	if (claim == NULL)
		return 0;
	memset(&collection, 0, sizeof(collection));

	/* Checks the operation status. */
	error = file_backing_extents(backing, loop_collect_extent, &collection);
	if (error != 0)
		return error;

	/* Handles the collection condition. */
	if (collection.count == 0)
		return EIO;
	collection.extents =
		kern_calloc(collection.count, sizeof(*collection.extents));

	/* Handles the extents availability. */
	if (collection.extents == NULL)
		return ENOMEM;
	collection.map = kern_calloc(collection.count, sizeof(*collection.map));

	/* Handles the map availability. */
	if (collection.map == NULL) {
		kern_free(collection.extents);

		/* Failed. */
		return ENOMEM;
	}

	collection.capacity = collection.count;
	collection.next_block = 0;
	collection.count = 0;

	/* Checks the operation status. */
	error = file_backing_extents(backing, loop_collect_extent, &collection);
	if (error != 0)
		goto out;

	/* Handles the collection condition. */
	if (collection.next_block !=
	    (uint64_t)backing->f_inode->i_size / 512U) {
		error = EIO;
		goto out;
	}

	disk = backing->f_inode->i_mount->m_disk;
	/* Process each remaining element. */
	for (i = 0; i < collection.count; i++)
		collection.extents[i].disk = disk;
	error = backing_claim_finalize_file(claim, backing, collection.extents,
				       collection.count);
out:
	if (error == 0) {
		*map = collection.map;
		*map_count = collection.count;
	} else {
		kern_free(collection.map);
	}

	kern_free(collection.extents);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the loop collect extent operation. */
static int
loop_collect_extent(
	uint64_t file_block,
	uint64_t disk_block,
	uint32_t count,
	void *argument)
{
	struct loop_extent_collection *collection = argument;

	/* Handles the file block condition. */
	if (file_block != collection->next_block || count == 0 ||
	    file_block > UINT64_MAX - count) {
		/* Failed. */
		return EIO;
	}
	collection->next_block += count;

	/* Handles the collection condition. */
	if (collection->count == UINT32_MAX)
		return E2BIG;

	/* Handles the extents availability. */
	if (collection->extents != NULL) {
		/* Handles the collection condition. */
		if (collection->count >= collection->capacity)
			return EAGAIN;
		collection->map[collection->count] =
			(struct fat_loop_extent){file_block, disk_block, count};
		collection->extents[collection->count].block = disk_block;
		collection->extents[collection->count].block_count = count;
	}

	collection->count++;

	/* Succeeded. */
	return 0;
}

/* Supports the loop open operation. */
static int
loop_open(
	struct disk *disk)
{
	struct loop_device *loop = disk != NULL ? disk->d_data : NULL;

	/* Returns the computed result. */
	return loop == NULL || !loop->attached || loop->detaching ? ENXIO : 0;
}

/* Supports the loop close operation. */
static void
loop_close(
	struct disk *disk)
{
	(void)disk;
}

/* Supports the loop ioctl operation. */
static int
loop_ioctl(
	struct disk *disk,
	unsigned long request,
	void *argument)
{
	(void)disk;
	(void)request;
	(void)argument;

	/* Failed. */
	return ENOTTY;
}

/* Supports the loop submit operation. */
static int
loop_submit(
	struct disk *disk,
	struct bio *bio)
{
	struct loop_device *loop = disk != NULL ? disk->d_data : NULL;
	uint64_t offset64, bytes64;
	struct io_context context;
	ssize_t done;
	int error = 0;

	/* Handles the loop availability. */
	if (loop == NULL || !loop->attached || loop->detaching)
		return ENXIO;

	/* Checks the operation status. */
	error = io_context_child(&context, &bio->b_context, IO_CONTEXT_DRAIN);
	if (error != 0)
		return error;

	/* Handles the bio condition. */
	if (bio->b_op == BIO_FLUSH) {
		io_stats_record(IO_LOOP_FLUSH, 0);
		error = file_fsync_backend(loop->backing);
		bio_complete(bio, error, 0);

		/* Succeeded. */
		return 0;
	}

	/* Handles the bio condition. */
	if (bio->b_op != BIO_READ && bio->b_op != BIO_WRITE)
		return EOPNOTSUPP;

	/* Handles the bio condition. */
	if (bio->b_block_count == 0 ||
	    bio->b_block_count > LOOP_MAX_TRANSFER_BLOCKS) {
		/* Failed. */
		return EINVAL;
	}
	bytes64 = (uint64_t)bio->b_block_count * LOOP_SECTOR_SIZE;

	/* Handles the bio condition. */
	if (bio->b_mapped_block > UINT64_MAX / LOOP_SECTOR_SIZE)
		return EOVERFLOW;

	/* Handles the offset64 condition. */
	offset64 = bio->b_mapped_block * LOOP_SECTOR_SIZE;
	if (offset64 > loop->size_bytes ||
	    bytes64 > loop->size_bytes - offset64 || offset64 > INT32_MAX ||
	    bytes64 > (uint64_t)INT32_MAX - offset64) {
		/* Failed. */
		return EOVERFLOW;
	}
	io_stats_record(bio->b_op == BIO_READ ? IO_LOOP_READ : IO_LOOP_WRITE,
			bytes64);

	/* Handles the bio condition. */
	if (bio->b_op == BIO_READ) {
		done = file_pread(loop->backing, bio->b_data, (size_t)bytes64,
				  (off_t)offset64);
	} else if ((loop->flags & LOOP_READ_WRITE) == 0)
		done = -EROFS;
	else
		done = file_pwrite_context(loop->backing, bio->b_data,
					   (size_t)bytes64, (off_t)offset64,
					   FILE_IO_LOOP_BACKING | FILE_IO_DRAIN,
					   NULL, &context);

	/* Handles the done condition. */
	if (done < 0)
		error = (int)-done;
	else if ((uint64_t)done != bytes64)
		error = bio->b_op == BIO_WRITE ? ENOSPC : EIO;
	if (error != 0) {
		hal_printf(
			"loop%u: %s block=%u count=%u flags=%x error=%d\n",
			loop->index, bio->b_op == BIO_READ ? "read" : "write",
			(uint32_t)bio->b_mapped_block, bio->b_block_count,
			(unsigned)file_status_flags_get(loop->backing), error);
	}

	bio_complete(bio, error, done > 0 ? (size_t)done : 0);

	/* Succeeded. */
	return 0;
}
