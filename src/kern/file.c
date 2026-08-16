/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/file.h"
#include "kern/namei.h"
#include "kern/cred.h"

#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define FILE_MAX 192U
#define VFS_BSS __attribute__((section(".vfs_bss")))
#define FILE_HIGH __attribute__((section(".hightext")))
#ifdef ZEDBSD_USER_ABI_LP64
#define OFF_T_MAX ((off_t)INT64_MAX)
#define OFF_T_MIN ((off_t)INT64_MIN)
#else
#define OFF_T_MAX ((off_t)INT32_MAX)
#define OFF_T_MIN ((off_t)INT32_MIN)
#endif

static struct file files[FILE_MAX] VFS_BSS;
static uint8_t file_used[FILE_MAX] VFS_BSS;
static struct spinlock file_pool_lock = {
	{ 0 }, LOCK_RANK_FILE, "file pool", 0, 0
};

extern void vm_object_truncate_inode(struct inode *, off_t)
	__attribute__((weak));

static struct file *
file_alloc(void)
{
	unsigned i;
	unsigned long irq = spin_lock_irqsave(&file_pool_lock);
	for (i = 0; i < FILE_MAX; i++) {
		if (!file_used[i]) {
			file_used[i] = 1;
			memset(&files[i], 0, sizeof(files[i]));
			refcount_init(&files[i].f_refs, 1);
			(void)mutex_init(&files[i].f_lock, LOCK_RANK_FILE,
			    "open file");
			spin_unlock_irqrestore(&file_pool_lock, irq);
			return &files[i];
		}
	}
	spin_unlock_irqrestore(&file_pool_lock, irq);
	return NULL;
}

static void
file_free(struct file *file)
{
	unsigned i;
	unsigned long irq = spin_lock_irqsave(&file_pool_lock);
	for (i = 0; i < FILE_MAX; i++) {
		if (&files[i] == file) {
			memset(file, 0, sizeof(*file));
			file_used[i] = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&file_pool_lock, irq);
}

int
file_openat(struct cwdinfo *context, const char *path, int flags,
	    mode_t mode, struct file **result)
{
	return file_openat_cred(context, NULL, path, flags, mode, result);
}

int
file_openat_cred(struct cwdinfo *context, const struct ucred *cred,
		 const char *path, int flags, mode_t mode, struct file **result)
{
	struct path found;
	struct inode *inode = NULL;
	struct file *file;
	int error;

	if (context == NULL || path == NULL || result == NULL)
		return EINVAL;
	if ((flags & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND |
		       O_DIRECTORY | O_NONBLOCK)) != 0 ||
	    (flags & O_ACCMODE) > O_RDWR ||
	    ((flags & O_EXCL) != 0 && (flags & O_CREAT) == 0))
		return EINVAL;
	error = namei_path_at(context, path, &found);
	if (error == ENOENT &&
	    ((flags & O_ACCMODE) != O_RDONLY ||
	     (flags & (O_CREAT | O_TRUNC | O_APPEND)) != 0)) {
		struct path parent;
		struct inode *collision;
		struct componentname last;
		char storage[NAME_MAX + 1U];
		error = namei_parent_path_at(context, path, &parent, &last, storage);
		if (error != 0)
			return error;
		if (cred != NULL &&
		    (error = vfs_access(parent.p_inode, cred, W_OK | X_OK)) != 0) {
			path_release(&parent);
			return error;
		}
		error = inode_lookup_casefold(parent.p_inode, &last, &collision);
		if (error == 0) {
			inode_release(collision);
			path_release(&parent);
			return EEXIST;
		}
		if (error != ENOENT && error != EOPNOTSUPP) {
			path_release(&parent);
			return error;
		}
		if ((flags & O_CREAT) == 0) {
			path_release(&parent);
			return ENOENT;
		}
		error = inode_create(parent.p_inode, &last, mode, &inode);
		if (error == 0) {
			path_set(&found, parent.p_mount, inode);
			inode_release(inode);
		}
		path_release(&parent);
		if (error != 0)
			return error;
	} else if (error != 0) {
		return error;
	} else if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
		path_release(&found);
		return EEXIST;
	}
	inode = found.p_inode;
	if (cred != NULL) {
		int requested = 0;
		if ((flags & O_ACCMODE) != O_WRONLY)
			requested |= R_OK;
		if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC) != 0)
			requested |= W_OK;
		error = vfs_access(inode, cred, requested);
		if (error != 0) {
			path_release(&found);
			return error;
		}
	}
	if ((flags & O_DIRECTORY) && inode->i_type != INODE_DIR) {
		path_release(&found);
		return ENOTDIR;
	}
	if (inode->i_type == INODE_DIR && (flags & O_ACCMODE) != O_RDONLY) {
		path_release(&found);
		return EISDIR;
	}
	if ((flags & O_TRUNC) && inode->i_type == INODE_REG &&
	    (flags & O_ACCMODE) != O_RDONLY) {
		error = inode_truncate(inode, 0);
		if (error != 0) {
			path_release(&found);
			return error;
		}
		if (vm_object_truncate_inode != NULL)
			vm_object_truncate_inode(inode, 0);
	}
	file = file_alloc();
	if (file == NULL) {
		path_release(&found);
		return ENFILE;
	}
	file->f_path = found;
	file->f_inode = inode;
	file->f_vm_inode = inode;
	file->f_ops = inode->i_fop;
	file->f_flags = flags;
	file->f_offset = (flags & O_APPEND) ? inode->i_size : 0;
	if (file->f_ops != NULL && file->f_ops->open != NULL) {
		error = file->f_ops->open(file);
		if (error != 0) {
			path_release(&file->f_path);
			file_free(file);
			return error;
		}
	}
	*result = file;
	return 0;
}

FILE_HIGH int
file_open_resolved(const struct path *resolved, int flags,
		   struct file **result)
{
	struct file *file;
	int error;
	if (resolved == NULL || resolved->p_mount == NULL ||
	    resolved->p_inode == NULL || result == NULL)
		return EINVAL;
	if ((flags & (O_CREAT | O_EXCL | O_TRUNC)) != 0 ||
	    (flags & ~(O_ACCMODE | O_APPEND | O_DIRECTORY | O_NONBLOCK)) != 0 ||
	    (flags & O_ACCMODE) > O_RDWR)
		return EINVAL;
	if ((flags & O_DIRECTORY) != 0 && resolved->p_inode->i_type != INODE_DIR)
		return ENOTDIR;
	if (resolved->p_inode->i_type == INODE_DIR &&
	    (flags & O_ACCMODE) != O_RDONLY)
		return EISDIR;
	file = file_alloc();
	if (file == NULL)
		return ENFILE;
	path_set(&file->f_path, resolved->p_mount, resolved->p_inode);
	file->f_inode = resolved->p_inode;
	file->f_vm_inode = resolved->p_inode;
	file->f_ops = resolved->p_inode->i_fop;
	file->f_flags = flags;
	file->f_offset = (flags & O_APPEND) ? resolved->p_inode->i_size : 0;
	if (file->f_ops != NULL && file->f_ops->open != NULL) {
		error = file->f_ops->open(file);
		if (error != 0) {
			path_release(&file->f_path);
			file_free(file);
			return error;
		}
	}
	*result = file;
	return 0;
}

int
file_create_pseudo(const struct file_ops *ops, int flags, void *data,
		   struct file **result)
{
	struct file *file;

	if (ops == NULL || result == NULL)
		return EINVAL;
	file = file_alloc();
	if (file == NULL)
		return ENFILE;
	file->f_ops = ops;
	file->f_flags = flags;
	file->f_data = data;
	*result = file;
	return 0;
}

int
file_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	int error;
	if (file == NULL)
		return EBADF;
	if (file->f_ops == NULL || file->f_ops->ioctl == NULL)
		return EOPNOTSUPP;
	mutex_lock(&file->f_lock);
	error = file->f_ops->ioctl(file, request, argument);
	mutex_unlock(&file->f_lock);
	return error;
}

ssize_t
file_read(struct file *file, void *buffer, size_t length)
{
	ssize_t result;
	if (file == NULL || buffer == NULL)
		return -EINVAL;
	if ((file->f_flags & O_ACCMODE) == O_WRONLY)
		return -EBADF;
	if (file->f_inode != NULL && file->f_inode->i_type == INODE_DIR)
		return -EISDIR;
	if (file->f_ops == NULL || file->f_ops->read == NULL)
		return -EOPNOTSUPP;
	mutex_lock(&file->f_lock);
	result = file->f_ops->read(file, buffer, length);
	if (result >= 0 && file->f_inode != NULL)
		inode_touch(file->f_inode, INODE_ATTR_ATIME);
	mutex_unlock(&file->f_lock);
	return result;
}

ssize_t
file_pread(struct file *file, void *buffer, size_t length, off_t offset)
{
	ssize_t result;
	if (file == NULL || buffer == NULL || offset < 0)
		return -EINVAL;
	if ((file->f_flags & O_ACCMODE) == O_WRONLY)
		return -EBADF;
	if (file->f_ops == NULL || file->f_ops->pread == NULL)
		return -EOPNOTSUPP;
	mutex_lock(&file->f_lock);
	result = file->f_ops->pread(file, buffer, length, offset);
	if (result >= 0 && file->f_inode != NULL)
		inode_touch(file->f_inode, INODE_ATTR_ATIME);
	mutex_unlock(&file->f_lock);
	return result;
}

ssize_t
file_pwrite(struct file *file, const void *buffer, size_t length, off_t offset)
{
	return file_pwrite_internal(file, buffer, length, offset, 0);
}

ssize_t
file_pwrite_internal(struct file *file, const void *buffer, size_t length,
		     off_t offset, unsigned internal_flags)
{
	ssize_t result;
	if (file == NULL || buffer == NULL || offset < 0 ||
	    (internal_flags & ~FILE_IO_LOOP_BACKING) != 0)
		return -EINVAL;
	if ((file->f_flags & O_ACCMODE) == O_RDONLY)
		return -EBADF;
	if (file->f_inode != NULL && (file->f_inode->i_flags & INODE_SWAPFILE))
		return -EBUSY;
	if (file->f_inode != NULL && (file->f_inode->i_flags & INODE_LOOPFILE) &&
	    (internal_flags & FILE_IO_LOOP_BACKING) == 0)
		return -EBUSY;
	if (file->f_ops == NULL || file->f_ops->pwrite == NULL)
		return -EOPNOTSUPP;
	mutex_lock(&file->f_lock);
	result = file->f_ops->pwrite(file, buffer, length, offset);
	if (result >= 0 && file->f_inode != NULL)
		inode_touch(file->f_inode, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	mutex_unlock(&file->f_lock);
	return result;
}

ssize_t
file_write(struct file *file, const void *buffer, size_t length)
{
	ssize_t result;
	if (file == NULL || buffer == NULL)
		return -EINVAL;
	if ((file->f_flags & O_ACCMODE) == O_RDONLY)
		return -EBADF;
	if (file->f_inode != NULL &&
	    (file->f_inode->i_flags & (INODE_SWAPFILE | INODE_LOOPFILE)) != 0)
		return -EBUSY;
	if (file->f_inode != NULL && file->f_inode->i_type == INODE_DIR)
		return -EISDIR;
	if (file->f_ops == NULL || file->f_ops->write == NULL)
		return -EOPNOTSUPP;
	mutex_lock(&file->f_lock);
	result = file->f_ops->write(file, buffer, length);
	if (result >= 0 && file->f_inode != NULL)
		inode_touch(file->f_inode, INODE_ATTR_MTIME | INODE_ATTR_CTIME);
	mutex_unlock(&file->f_lock);
	return result;
}

int
file_readdir(struct file *file, struct dirent *entry, int *eof)
{
	int error;
	if (file == NULL || entry == NULL || eof == NULL)
		return EINVAL;
	if (file->f_inode == NULL || file->f_inode->i_type != INODE_DIR)
		return ENOTDIR;
	if (file->f_ops == NULL || file->f_ops->readdir == NULL)
		return EOPNOTSUPP;
	mutex_lock(&file->f_lock);
	error = mount_readdir_child(&file->f_path, &file->f_mount_cursor, entry);
	if (error == 0) {
		*eof = 0;
		mutex_unlock(&file->f_lock);
		return 0;
	}
	if (error != ENOENT) {
		mutex_unlock(&file->f_lock);
		return error;
	}
	for (;;) {
		error = file->f_ops->readdir(file, entry, eof);
		if (error != 0 || *eof ||
		    !mount_child_shadows(&file->f_path, entry->d_name)) {
			mutex_unlock(&file->f_lock);
			return error;
		}
	}
}

off_t
file_seek(struct file *file, off_t offset, int whence)
{
	off_t base, target;
	if (file == NULL)
		return -EINVAL;
	mutex_lock(&file->f_lock);
	if (file->f_ops != NULL && file->f_ops->seek != NULL)
		base = file->f_ops->seek(file, offset, whence);
	else {
	if (whence == 0)
		base = 0;
	else if (whence == 1)
		base = file->f_offset;
	else if (whence == 2 && file->f_inode != NULL)
		base = file->f_inode->i_size;
	else
		base = OFF_T_MIN;
	if (base == OFF_T_MIN) {
		mutex_unlock(&file->f_lock);
		return -EINVAL;
	}
	if ((offset > 0 && base > OFF_T_MAX - offset) ||
	    (offset < 0 && base < OFF_T_MIN - offset)) {
		mutex_unlock(&file->f_lock);
		return -EOVERFLOW;
	}
	target = base + offset;
	if (target < 0) {
		mutex_unlock(&file->f_lock);
		return -EINVAL;
	}
	file->f_offset = target;
	base = target;
	}
	mutex_unlock(&file->f_lock);
	return base;
}

int
file_fsync(struct file *file)
{
	int error;
	if (file == NULL)
		return EINVAL;
	mutex_lock(&file->f_lock);
	if (file->f_ops != NULL && file->f_ops->fsync != NULL)
		error = file->f_ops->fsync(file);
	else
		error = file->f_inode != NULL ? inode_sync(file->f_inode) : 0;
	mutex_unlock(&file->f_lock);
	return error;
}

int
file_close(struct file *file)
{
	int error = 0;
	if (file == NULL)
		return EBADF;
	if (refcount_load(&file->f_refs) == 0)
		return EBADF;
	if (!refcount_put(&file->f_refs))
		return 0;
	if (file->f_ops != NULL && file->f_ops->close != NULL)
		error = file->f_ops->close(file);
	if (file->f_path.p_inode != NULL)
		path_release(&file->f_path);
	else if (file->f_inode != NULL)
		inode_release(file->f_inode);
	file_free(file);
	return error;
}

void
file_ref(struct file *file)
{
	if (file == NULL)
		return;
	refcount_get(&file->f_refs);
}

struct inode *
file_vm_inode(struct file *file)
{
	return file != NULL ? file->f_vm_inode : NULL;
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

unsigned
file_count(void)
{
	unsigned i, count = 0;
	unsigned long irq = spin_lock_irqsave(&file_pool_lock);
	for (i = 0; i < FILE_MAX; i++)
		count += file_used[i] != 0;
	spin_unlock_irqrestore(&file_pool_lock, irq);
	return count;
}
