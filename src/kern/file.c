/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/file.h"
#include "kern/namei.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>

#define FILE_MAX 64U
#define VFS_BSS __attribute__((section(".vfs_bss")))

static struct file files[FILE_MAX] VFS_BSS;
static uint8_t file_used[FILE_MAX] VFS_BSS;

static struct file *
file_alloc(void)
{
	unsigned i;
	for (i = 0; i < FILE_MAX; i++) {
		if (!file_used[i]) {
			file_used[i] = 1;
			memset(&files[i], 0, sizeof(files[i]));
			files[i].f_usecount = 1;
			return &files[i];
		}
	}
	return NULL;
}

static void
file_free(struct file *file)
{
	unsigned i;
	for (i = 0; i < FILE_MAX; i++) {
		if (&files[i] == file) {
			memset(file, 0, sizeof(*file));
			file_used[i] = 0;
			return;
		}
	}
}

int
file_openat(struct cwdinfo *context, const char *path, int flags,
	    mode_t mode, struct file **result)
{
	struct inode *inode;
	struct file *file;
	int error;

	if (context == NULL || path == NULL || result == NULL)
		return EINVAL;
	error = namei_at(context, path, &inode);
	if (error == ENOENT && (flags & O_CREAT)) {
		struct inode *parent;
		struct componentname last;
		char storage[NAME_MAX + 1U];
		error = namei_parent_at(context, path, &parent, &last, storage);
		if (error != 0)
			return error;
		error = inode_create(parent, &last, mode, &inode);
		inode_release(parent);
		if (error != 0)
			return error;
	} else if (error != 0) {
		return error;
	} else if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
		inode_release(inode);
		return EEXIST;
	}
	if ((flags & O_DIRECTORY) && inode->i_type != INODE_DIR) {
		inode_release(inode);
		return ENOTDIR;
	}
	if (inode->i_type == INODE_DIR && (flags & O_ACCMODE) != O_RDONLY) {
		inode_release(inode);
		return EISDIR;
	}
	if ((flags & O_TRUNC) && inode->i_type == INODE_REG &&
	    (flags & O_ACCMODE) != O_RDONLY) {
		error = inode_truncate(inode, 0);
		if (error != 0) {
			inode_release(inode);
			return error;
		}
	}
	file = file_alloc();
	if (file == NULL) {
		inode_release(inode);
		return ENOSPC;
	}
	file->f_inode = inode;
	file->f_ops = inode->i_fop;
	file->f_flags = flags;
	file->f_offset = (flags & O_APPEND) ? inode->i_size : 0;
	*result = file;
	return 0;
}

ssize_t
file_read(struct file *file, void *buffer, size_t length)
{
	if (file == NULL || buffer == NULL)
		return -EINVAL;
	if ((file->f_flags & O_ACCMODE) == O_WRONLY)
		return -EBADF;
	if (file->f_inode->i_type == INODE_DIR)
		return -EISDIR;
	if (file->f_ops == NULL || file->f_ops->read == NULL)
		return -EOPNOTSUPP;
	return file->f_ops->read(file, buffer, length);
}

ssize_t
file_write(struct file *file, const void *buffer, size_t length)
{
	if (file == NULL || buffer == NULL)
		return -EINVAL;
	if ((file->f_flags & O_ACCMODE) == O_RDONLY)
		return -EBADF;
	if (file->f_inode->i_type == INODE_DIR)
		return -EISDIR;
	if (file->f_ops == NULL || file->f_ops->write == NULL)
		return -EOPNOTSUPP;
	return file->f_ops->write(file, buffer, length);
}

int
file_readdir(struct file *file, struct dirent *entry, int *eof)
{
	if (file == NULL || entry == NULL || eof == NULL)
		return EINVAL;
	if (file->f_inode->i_type != INODE_DIR)
		return ENOTDIR;
	if (file->f_ops == NULL || file->f_ops->readdir == NULL)
		return EOPNOTSUPP;
	return file->f_ops->readdir(file, entry, eof);
}

off_t
file_seek(struct file *file, off_t offset, int whence)
{
	off_t base, target;
	if (file == NULL)
		return -EINVAL;
	if (file->f_ops != NULL && file->f_ops->seek != NULL)
		return file->f_ops->seek(file, offset, whence);
	if (whence == 0)
		base = 0;
	else if (whence == 1)
		base = file->f_offset;
	else if (whence == 2)
		base = file->f_inode->i_size;
	else
		return -EINVAL;
	if ((offset > 0 && base > (off_t)INT32_MAX - offset) ||
	    (offset < 0 && base < (off_t)INT32_MIN - offset))
		return -EOVERFLOW;
	target = base + offset;
	if (target < 0)
		return -EINVAL;
	file->f_offset = target;
	return target;
}

int
file_fsync(struct file *file)
{
	if (file == NULL)
		return EINVAL;
	if (file->f_ops != NULL && file->f_ops->fsync != NULL)
		return file->f_ops->fsync(file);
	return inode_sync(file->f_inode);
}

int
file_close(struct file *file)
{
	int error = 0;
	if (file == NULL || file->f_usecount == 0)
		return EBADF;
	if (file->f_ops != NULL && file->f_ops->close != NULL)
		error = file->f_ops->close(file);
	inode_release(file->f_inode);
	file_free(file);
	return error;
}

void
file_pool_reset(void)
{
	unsigned i;
	for (i = 0; i < FILE_MAX; i++) {
		if (file_used[i])
			(void)file_close(&files[i]);
	}
}
