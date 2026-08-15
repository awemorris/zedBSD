/*
 * File-backed loop block devices
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include "kern/loop.h"
#include "kern/disk.h"
#include "kern/file.h"
#include "kern/inode.h"
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

struct loop_device {
	unsigned index;
	unsigned flags;
	bool reserved;
	bool attached;
	bool detaching;
	struct file *backing;
	struct inode *backing_inode;
	struct disk *disk;
	uint64_t size_bytes;
};

static struct loop_device loops[LOOP_MAX_DEVICES]
	__attribute__((section(".vfs_bss")));

static void
loop_unlock(bool enabled)
{
	if (enabled)
		hal_irq_enable();
}

static int
loop_open(struct disk *disk)
{
	struct loop_device *loop = disk != NULL ? disk->d_data : NULL;
	return loop == NULL || !loop->attached || loop->detaching ? ENXIO : 0;
}

static void
loop_close(struct disk *disk)
{
	(void)disk;
}

static int
loop_ioctl(struct disk *disk, unsigned long request, void *argument)
{
	(void)disk;
	(void)request;
	(void)argument;
	return ENOTTY;
}

static int
loop_submit(struct disk *disk, struct bio *bio)
{
	struct loop_device *loop = disk != NULL ? disk->d_data : NULL;
	uint64_t offset64, bytes64;
	ssize_t done;
	int error = 0;

	if (loop == NULL || !loop->attached || loop->detaching)
		return ENXIO;
	if (bio->b_op == BIO_FLUSH) {
		error = file_fsync(loop->backing);
		bio_complete(bio, error, 0);
		return 0;
	}
	if (bio->b_op != BIO_READ && bio->b_op != BIO_WRITE)
		return EOPNOTSUPP;
	if (bio->b_block_count == 0 ||
	    bio->b_block_count > LOOP_MAX_TRANSFER_BLOCKS)
		return EINVAL;
	bytes64 = (uint64_t)bio->b_block_count * LOOP_SECTOR_SIZE;
	offset64 = bio->b_mapped_block * LOOP_SECTOR_SIZE;
	if (offset64 > loop->size_bytes || bytes64 > loop->size_bytes - offset64 ||
	    offset64 > INT32_MAX || bytes64 > (uint64_t)INT32_MAX - offset64)
		return EOVERFLOW;
	if (bio->b_op == BIO_READ)
		done = file_pread(loop->backing, bio->b_data, (size_t)bytes64,
				  (off_t)offset64);
	else if ((loop->flags & LOOP_READ_WRITE) == 0)
		done = -EROFS;
	else
		done = file_pwrite_internal(loop->backing, bio->b_data,
			(size_t)bytes64, (off_t)offset64, FILE_IO_LOOP_BACKING);
	if (done < 0)
		error = (int)-done;
	else if ((uint64_t)done != bytes64)
		error = bio->b_op == BIO_WRITE ? ENOSPC : EIO;
	bio_complete(bio, error, done > 0 ? (size_t)done : 0);
	return 0;
}

static const struct disk_ops loop_disk_ops = {
	.open = loop_open,
	.close = loop_close,
	.submit = loop_submit,
	.ioctl = loop_ioctl,
};

int
loop_init(void)
{
	unsigned i;
	memset(loops, 0, sizeof(loops));
	for (i = 0; i < LOOP_MAX_DEVICES; i++)
		loops[i].index = i;
	return 0;
}

int
loop_get_index(const struct disk *disk, unsigned *index_out)
{
	unsigned i;
	if (disk == NULL || index_out == NULL)
		return EINVAL;
	for (i = 0; i < LOOP_MAX_DEVICES; i++)
		if (loops[i].attached && loops[i].disk == disk) {
			*index_out = i;
			return 0;
		}
	return ENODEV;
}

static int
loop_backing_valid(struct file *backing, unsigned flags)
{
	struct inode *inode;
	unsigned loop_index;
	if (backing == NULL || backing->f_inode == NULL)
		return EINVAL;
	inode = backing->f_inode;
	if (flags != LOOP_READ_ONLY && flags != LOOP_READ_WRITE)
		return EINVAL;
	if (inode->i_type != INODE_REG || inode->i_size <= 0 ||
	    ((uint32_t)inode->i_size & (LOOP_SECTOR_SIZE - 1U)) != 0)
		return EINVAL;
	if ((uint64_t)(uint32_t)inode->i_size > (uint64_t)INT32_MAX)
		return EFBIG;
	if (flags == LOOP_READ_WRITE &&
	    (backing->f_flags & O_ACCMODE) == O_RDONLY)
		return EBADF;
	if ((inode->i_flags & (INODE_SWAPFILE | INODE_LOOPFILE)) != 0)
		return EBUSY;
	if (inode->i_mount != NULL && inode->i_mount->m_type != NULL &&
	    inode->i_mount->m_type->fs_name != NULL &&
	    !strcmp(inode->i_mount->m_type->fs_name, "overlay"))
		return ELOOP;
	if (inode->i_mount != NULL && inode->i_mount->m_disk != NULL &&
	    loop_get_index(inode->i_mount->m_disk, &loop_index) == 0)
		return ELOOP;
	return 0;
}

int
loop_attach_file(struct file *backing, unsigned flags, struct disk **disk_out)
{
	struct loop_device *loop = NULL;
	struct inode *backing_inode;
	struct disk *disk;
	unsigned i;
	int error;
	bool enabled;

	if (disk_out == NULL)
		return EINVAL;
	*disk_out = NULL;
	error = loop_backing_valid(backing, flags);
	if (error != 0)
		return error;
	backing_inode = backing->f_inode;
	enabled = hal_irq_disable();
	if ((backing->f_inode->i_flags & (INODE_SWAPFILE | INODE_LOOPFILE)) != 0) {
		loop_unlock(enabled);
		return EBUSY;
	}
	for (i = 0; i < LOOP_MAX_DEVICES; i++)
		if (!loops[i].attached && !loops[i].reserved) {
			loop = &loops[i];
			loop->reserved = true;
			break;
		}
	if (loop == NULL) {
		loop_unlock(enabled);
		return ENOSPC;
	}
	backing->f_inode->i_flags |= INODE_LOOPFILE;
	loop_unlock(enabled);

	file_ref(backing);
	inode_ref(backing->f_inode);
	disk = disk_alloc();
	if (disk == NULL) {
		error = ENOSPC;
		goto fail_refs;
	}
	disk->d_name[0] = 'l'; disk->d_name[1] = 'o'; disk->d_name[2] = 'o';
	disk->d_name[3] = 'p'; disk->d_name[4] = (char)('0' + loop->index);
	disk->d_name[5] = '\0';
	disk->d_flags = flags == LOOP_READ_ONLY ? DISK_READ_ONLY : 0;
	disk->d_block_size = LOOP_SECTOR_SIZE;
	disk->d_block_count = (uint64_t)(uint32_t)backing->f_inode->i_size /
		LOOP_SECTOR_SIZE;
	disk->d_max_transfer_blocks = LOOP_MAX_TRANSFER_BLOCKS;
	disk->d_ops = &loop_disk_ops;
	disk->d_data = loop;
	loop->flags = flags;
	loop->backing = backing;
	loop->backing_inode = backing->f_inode;
	loop->disk = disk;
	loop->size_bytes = (uint64_t)(uint32_t)backing->f_inode->i_size;
	error = disk_create(disk);
	if (error != 0) {
		(void)disk_destroy(disk);
		goto fail_refs;
	}
	enabled = hal_irq_disable();
	loop->attached = true;
	loop->reserved = false;
	loop_unlock(enabled);
	*disk_out = disk;
	return 0;

fail_refs:
	enabled = hal_irq_disable();
	backing_inode->i_flags &= ~INODE_LOOPFILE;
	loop_unlock(enabled);
	inode_release(backing_inode);
	(void)file_close(backing);
	enabled = hal_irq_disable();
	memset(loop, 0, sizeof(*loop));
	loop->index = i;
	loop_unlock(enabled);
	return error;
}

int
loop_attach_path(const struct path *root, const char *path, unsigned flags,
		 struct disk **disk_out)
{
	struct cwdinfo context;
	struct file *file;
	int open_flags, error;
	if (root == NULL || path == NULL)
		return EINVAL;
	error = cwdinfo_init(&context, root);
	if (error != 0)
		return error;
	open_flags = flags == LOOP_READ_WRITE ? O_RDWR : O_RDONLY;
	error = file_openat(&context, path, open_flags, 0, &file);
	if (error == 0) {
		error = loop_attach_file(file, flags, disk_out);
		(void)file_close(file);
	}
	cwdinfo_destroy(&context);
	return error;
}

int
loop_detach(struct disk *disk)
{
	struct loop_device *loop;
	int error;
	bool enabled;
	unsigned index;
	if (loop_get_index(disk, &index) != 0)
		return ENODEV;
	loop = &loops[index];
	enabled = hal_irq_disable();
	if (loop->detaching || disk->d_open_count != 0 ||
	    disk->d_inflight != 0 || disk->d_refcount != 1) {
		loop_unlock(enabled);
		return EBUSY;
	}
	loop->detaching = true;
	loop_unlock(enabled);
	if ((loop->flags & LOOP_READ_WRITE) != 0) {
		error = file_fsync(loop->backing);
		if (error != 0)
			goto retryable;
	}
	error = disk_gone_if_idle(disk);
	if (error != 0)
		goto retryable;
	error = disk_destroy(disk);
	if (error != 0)
		return error; /* Invariant failure: keep the slot pinned for diagnosis. */
	enabled = hal_irq_disable();
	loop->backing_inode->i_flags &= ~INODE_LOOPFILE;
	loop_unlock(enabled);
	inode_release(loop->backing_inode);
	(void)file_close(loop->backing);
	memset(loop, 0, sizeof(*loop));
	loop->index = index;
	return 0;

retryable:
	enabled = hal_irq_disable();
	loop->detaching = false;
	loop_unlock(enabled);
	return error;
}
