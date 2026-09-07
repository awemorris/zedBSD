/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Open file objects and the generic read/write transaction.
 *
 * A struct file records one open of an inode: its path, status flags,
 * and shared position.  All data transfer goes through file_io_begin(),
 * file_io_transfer(), and file_io_end(), which take the position and
 * inode I/O locks, keep a regular file coherent with any published
 * shared mapping, clear set-id bits before the first write, and apply
 * the RLIMIT_FSIZE growth limit.
 */

#include "kern/file.h"
#include "kern/io-stats.h"
#include "kern/cache-memory.h"
#include "kern/disk.h"
#include "kern/page.h"
#include "kern/namei.h"
#include "kern/cred.h"
#include "kern/record-lock.h"
#include "kern/vm-object.h"
#include "kern/fat.h"
#include "kern/kmem.h"
#include "kern/uaccess.h"

#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <zedbsd/fcntl.h>

extern void readahead_reset(struct readahead_state *) __attribute__((weak));
extern void readahead_cancel(struct file *) __attribute__((weak));
extern int readahead_demand_begin(void) __attribute__((weak));
extern void readahead_demand_end(void) __attribute__((weak));
extern int readahead_observe(struct readahead_state *, uint64_t, size_t, uint64_t, uint64_t, int, struct readahead_request *) __attribute__((weak));
extern int readahead_submit(struct file *, struct inode *, const struct readahead_request *) __attribute__((weak));
extern void cache_memory_get_stats(struct cache_memory_stats *) __attribute__((weak));
static void file_readahead_reset_owned(struct file *file);
static void file_readahead_completed(struct file *file, struct inode *inode, off_t start, ssize_t result, off_t eof, uint64_t generation, uint64_t useful);
extern int vm_object_read_coherent_useful(struct inode *, off_t, void *, size_t, ssize_t *, size_t *) __attribute__((weak));
extern int disk_cache_acquire(struct disk *, struct disk **) __attribute__((weak));
extern void disk_cache_release(struct disk *) __attribute__((weak));

extern void io_error_record(struct io_error_state *, int) __attribute__((weak));
extern void io_error_snapshot(struct io_error_state *, struct io_error_snapshot *) __attribute__((weak));
extern int io_error_observe(const struct io_error_snapshot *, volatile uint64_t *) __attribute__((weak));

extern int writeback_mount_admit(struct mount *, struct writeback_ticket *) __attribute__((weak));
extern void writeback_pressure(struct writeback_budget *) __attribute__((weak));
extern void writeback_ticket_release(struct writeback_ticket *) __attribute__((weak));
extern int vm_object_writeback_prepare(struct file *, struct vm_object **) __attribute__((weak));
extern void vm_object_writeback_release(struct vm_object *) __attribute__((weak));
extern int vm_object_content_prepare_delayed(struct vm_object_content *, struct vm_object *, struct writeback_ticket *) __attribute__((weak));
static void file_io_writeback_prepare(struct file_io *io, int flags);
static void file_io_resources_release(struct file_io *io);

extern int vm_object_sync_inode(struct inode *) __attribute__((weak));

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

struct file_format_extents {
	struct backing_claim_extent *entries;
	struct disk *disk;
	uint64_t blocks;
	uint64_t next_block;
	unsigned count;
	unsigned capacity;
};

static struct file files[FILE_MAX] VFS_BSS;
static uint8_t file_used[FILE_MAX] VFS_BSS;
static struct spinlock file_pool_lock = {
	{ 0 }, LOCK_RANK_FILE, "file pool", 0, 0
};

extern int vm_object_inode_io_wait(struct inode *inode) __attribute__((weak));
extern int vm_object_inode_resize_active(struct inode *inode)
    __attribute__((weak));
extern int vm_object_resize_begin(struct inode *inode, off_t size,
    struct vm_object_resize *resize) __attribute__((weak));
extern int vm_object_resize_prepare(struct vm_object_resize *resize)
    __attribute__((weak));
extern void vm_object_resize_commit(struct vm_object_resize *resize, off_t size)
    __attribute__((weak));
extern void vm_object_resize_abort(struct vm_object_resize *resize)
    __attribute__((weak));
extern int vm_object_content_begin(struct file *file, off_t offset, size_t length,
    struct vm_object_resize *resize, struct vm_object_content *content)
    __attribute__((weak));
extern int vm_object_content_prepare(struct vm_object_content *content)
    __attribute__((weak));
extern void vm_object_content_commit(struct vm_object_content *content,
    const void *buffer, size_t length) __attribute__((weak));
extern void vm_object_content_abort(struct vm_object_content *content)
    __attribute__((weak));
extern int vm_object_read_coherent(struct inode *inode, off_t offset,
    void *buffer, size_t length, ssize_t *count) __attribute__((weak));
extern int vm_object_content_read_begin(struct inode *inode) __attribute__((weak));
extern void vm_object_content_read_end(struct inode *inode) __attribute__((weak));
extern int vm_object_cache_published(struct inode *inode) __attribute__((weak));
extern void file_regular_io_lock_checkpoint(struct inode *inode)
	__attribute__((weak));

extern void vm_object_cache_prepare(struct file *) __attribute__((weak));
extern int vm_object_cache_pin(struct inode *, struct vm_object **) __attribute__((weak));
extern void vm_object_cache_unpin(struct vm_object *) __attribute__((weak));
static void file_io_cache_read_prepare(struct file_io *io);
extern unsigned vm_object_cache_drain(struct mount *) __attribute__((weak));
extern int vm_object_backing_busy(const struct backing_claim *)
	__attribute__((weak));

static int file_fsync_backend_locked(struct file *file);
static int file_format_reserve_locked(struct file *file, uint64_t size);
static int file_format_collect_extent(uint64_t file_block, uint64_t disk_block, uint32_t count, void *argument);
static int file_format_finalize(struct file *file, struct backing_claim *claim, uint64_t size);
static int file_format_ioctl(struct file *file, uintptr_t argument);
static struct file * file_alloc(void);
static void file_free(struct file *file);
static int file_io_is_positional(enum file_io_kind kind);
static int file_io_is_write(enum file_io_kind kind);
static int file_vm_resize_available(void);
static int file_vm_content_available(void);
static int file_regular_io_lock(struct inode *inode, unsigned internal_flags);
static int file_io_regular_locks_reacquire(struct file_io *io);
static void file_io_regular_locks_drop(struct file_io *io);
static ssize_t file_io_once(struct file *file, enum file_io_kind kind, void *buffer, size_t length, off_t offset, unsigned internal_flags);

/*
 * Reserves fixed-size formatting through an existing FAT file description.
 *
 * Idle readers remain usable; competing content and activation mutations fail.
 */
int
file_format_reserve(
	struct file *file,
	uint64_t size)
{
	int error;

	/* Rejects descriptors that cannot represent a regular backing object. */
	if (file == NULL)
		return EBADF;

	/* Validates the object before taking its regular-file I/O mutex. */
	if (file->f_inode == NULL || file->f_inode->i_type != INODE_REG)
		return EINVAL;

	/* Closes optional readers before taking any descriptor or inode mutex. */
	if (vm_object_cache_drain != NULL)
		(void)vm_object_cache_drain(NULL);

	/* Serializes reservation publication with positional and ordinary I/O. */
	mutex_lock(&file->f_lock);
	mutex_lock(&file->f_inode->i_io_lock);
	error = file_format_reserve_locked(file, size);
	mutex_unlock(&file->f_inode->i_io_lock);
	mutex_unlock(&file->f_lock);

	/* Reports the reservation result without changing descriptor position. */
	return error;
}

/*
 * Opens a path without a credential check.
 */
int
file_openat(
	struct cwdinfo *context,
	const char *path,
	int flags,
	mode_t mode,
	struct file **result)
{
	int error;

	error = file_openat_cred(context, NULL, path, flags, mode, result);
	return error;
}

/*
 * Opens a path with open(2) semantics, creating or truncating the file
 * as the flags ask and checking access against a credential.
 */
int
file_openat_cred(
	struct cwdinfo *context,
	const struct ucred *cred,
	const char *path,
	int flags,
	mode_t mode,
	struct file **result)
{
	struct path found;
	struct inode *inode;
	struct file *file;
	int error;
	struct path parent;
	struct inode *collision;
	struct componentname last;
	struct inode_creation_request request;
	char storage[NAME_MAX + 1U];
	unsigned namei_flags;
	int requested;

	inode = NULL;

	/* Rejects unsupported or inconsistent flags. */
	if (context == NULL || path == NULL || result == NULL)
		return EINVAL;
	if ((flags & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND |
		       O_DIRECTORY | O_NONBLOCK | O_NOCTTY | O_NOFOLLOW |
		       O_SYNC | O_DSYNC)) != 0 ||
	    (flags & O_ACCMODE) > O_RDWR ||
	    ((flags & O_EXCL) != 0 && (flags & O_CREAT) == 0))
		return EINVAL;

	/*
	 * Reserve the system-wide open-file object before pathname
	 * operations which may create or truncate an inode.  ENFILE must
	 * not leave a namespace or data side effect behind.
	 */
	file = file_alloc();
	if (file == NULL)
		return ENFILE;
	if ((flags & O_NOFOLLOW) != 0)
		namei_flags = NAMEI_NOFOLLOW_FINAL;
	else
		namei_flags = 0;
	error = namei_path_flags_at(context, path, namei_flags, &found);

	/* A missing file is created under the parent's namespace transaction. */
	if (error == ENOENT &&
	    ((flags & O_ACCMODE) != O_RDONLY ||
	     (flags & (O_CREAT | O_TRUNC | O_APPEND)) != 0)) {
		error = namei_parent_path_at(context, path, &parent, &last, storage);
		if (error != 0)
			goto fail_file;
		mount_vfs_transaction_enter(parent.p_mount);
		if (cred != NULL) {
			error = inode_creation_request_user(parent.p_inode, cred,
			    INODE_REG, mode, 0, NULL, &request);
			if (error != 0) {
				mount_vfs_transaction_leave(parent.p_mount);
				path_release(&parent);
				goto fail_file;
			}
		}
		error = inode_lookup_casefold(parent.p_inode, &last, &collision);
		if (error == 0) {
			inode_release(collision);
			mount_vfs_transaction_leave(parent.p_mount);
			path_release(&parent);
			error = EEXIST;
			goto fail_file;
		}
		if (error != ENOENT && error != EOPNOTSUPP) {
			mount_vfs_transaction_leave(parent.p_mount);
			path_release(&parent);
			goto fail_file;
		}
		if ((flags & O_CREAT) == 0) {
			mount_vfs_transaction_leave(parent.p_mount);
			path_release(&parent);
			error = ENOENT;
			goto fail_file;
		}
		if (cred != NULL)
			error = 0;
		else
			error = inode_creation_request_system(INODE_REG, mode, 0, 0,
			    0, &request);
		if (error == 0)
			error = inode_create(parent.p_inode, &last, &request, &inode);
		if (error == 0) {
			path_set(&found, parent.p_mount, inode);
			inode_release(inode);
		}
		mount_vfs_transaction_leave(parent.p_mount);
		path_release(&parent);
		if (error != 0)
			goto fail_file;
	} else if (error != 0) {
		goto fail_file;
	} else if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
		path_release(&found);
		error = EEXIST;
		goto fail_file;
	}

	/* Checks the object type and the access the flags need. */
	inode = found.p_inode;
	if ((flags & O_NOFOLLOW) != 0 && inode->i_type == INODE_SYMLINK) {
		path_release(&found);
		error = ELOOP;
		goto fail_file;
	}
	if (cred != NULL) {
		requested = 0;
		if ((flags & O_ACCMODE) != O_WRONLY)
			requested |= R_OK;
		if ((flags & O_ACCMODE) != O_RDONLY || (flags & O_TRUNC) != 0)
			requested |= W_OK;
		error = vfs_access(inode, cred, requested);
		if (error != 0) {
			path_release(&found);
			goto fail_file;
		}
	}
	if ((flags & O_DIRECTORY) && inode->i_type != INODE_DIR) {
		path_release(&found);
		error = ENOTDIR;
		goto fail_file;
	}
	if (inode->i_type == INODE_DIR && (flags & O_ACCMODE) != O_RDONLY) {
		path_release(&found);
		error = EISDIR;
		goto fail_file;
	}

	/*
	 * Truncation and set-id removal are one inode content transaction.
	 * A metadata failure must fail open before the backend is changed.
	 */
	if ((flags & O_TRUNC) && inode->i_type == INODE_REG &&
	    (flags & O_ACCMODE) != O_RDONLY) {
		error = inode_truncate_limited_cred(inode, 0, UINT64_MAX, cred,
		    NULL);
		if (error != 0) {
			path_release(&found);
			goto fail_file;
		}
	}

	/*
	 * O_APPEND controls each write operation; it does not change the
	 * initial open-file-description offset.
	 */
	file->f_path = found;
	file->f_inode = inode;
	file->f_vm_inode = inode;
	file->f_ops = inode->i_fop;
	atomic_store_release(&file->f_flags, (unsigned)flags);
	file->f_offset = 0;
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

fail_file:
	file_free(file);
	return error;
}

/*
 * Opens an already resolved path without creation or truncation.
 */
FILE_HIGH int
file_open_resolved(
	const struct path *resolved,
	int flags,
	struct file **result)
{
	struct file *file;
	int error;

	/* Rejects flags that would change the object. */
	if (resolved == NULL ||
	    resolved->p_mount == NULL ||
	    resolved->p_inode == NULL ||
	    result == NULL)
		return EINVAL;
	if ((flags & (O_CREAT | O_EXCL | O_TRUNC)) != 0 ||
	    (flags & ~(O_ACCMODE | O_APPEND | O_DIRECTORY | O_NONBLOCK |
	    O_NOCTTY | O_NOFOLLOW | O_SYNC | O_DSYNC)) != 0 ||
	    (flags & O_ACCMODE) > O_RDWR)
		return EINVAL;
	if ((flags & O_DIRECTORY) != 0 && resolved->p_inode->i_type != INODE_DIR)
		return ENOTDIR;
	if ((flags & O_NOFOLLOW) != 0 &&
	    resolved->p_inode->i_type == INODE_SYMLINK)
		return ELOOP;
	if (resolved->p_inode->i_type == INODE_SOCKET)
		return ENXIO;
	if (resolved->p_inode->i_type == INODE_DIR &&
	    (flags & O_ACCMODE) != O_RDONLY)
		return EISDIR;

	/* Binds a new file to the path and runs the backend open. */
	file = file_alloc();
	if (file == NULL)
		return ENFILE;
	path_set(&file->f_path, resolved->p_mount, resolved->p_inode);
	file->f_inode = resolved->p_inode;
	file->f_vm_inode = resolved->p_inode;
	file->f_ops = resolved->p_inode->i_fop;
	atomic_store_release(&file->f_flags, (unsigned)flags);
	file->f_offset = 0;
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

/*
 * Creates a file without an inode, served entirely by its operations.
 */
int
file_create_pseudo(
	const struct file_ops *ops,
	int flags,
	void *data,
	struct file **result)
{
	struct file *file;

	if (ops == NULL || result == NULL)
		return EINVAL;
	file = file_alloc();
	if (file == NULL)
		return ENFILE;
	file->f_ops = ops;
	atomic_store_release(&file->f_flags, (unsigned)flags);
	file->f_data = data;
	*result = file;
	return 0;
}

/*
 * Passes an ioctl request to the file's backend.
 */
int
file_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	int error;

	if (file == NULL)
		return EBADF;

	/* Handles the regular-file lease before filesystem-specific ioctls. */
	if (request == ZEDBSD_FILE_FORMAT_RESERVE)
		return file_format_ioctl(file, argument);
	if (file->f_ops == NULL || file->f_ops->ioctl == NULL)
		return EOPNOTSUPP;

	/*
	 * ioctl backends synchronize their own state.  A blocking ioctl
	 * must not exclude read/write on a full-duplex descriptor.
	 */
	error = file->f_ops->ioctl(file, request, argument);
	return error;
}

/*
 * Replaces the masked status flag bits of a file atomically.
 */
void
file_status_flags_update(
	struct file *file,
	int mask,
	int value)
{
	unsigned old;
	unsigned updated;

	if (file == NULL)
		return;
	old = atomic_load_acquire(&file->f_flags);
	do {
		updated = (old & ~(unsigned)mask) |
		    ((unsigned)value & (unsigned)mask);
	} while (!atomic_compare_exchange(&file->f_flags, &old, updated));
}

/*
 * Takes a lease on the complete content of a regular file so that a
 * sequence of reads sees one consistent image.
 *
 * The lease holds the inode I/O locks and a published content gate for
 * the whole file, which keeps writers and mapping faults out until
 * file_content_lease_end().
 */
int
file_content_lease_begin(
	struct file *file,
	struct file_content_lease *lease)
{
	struct inode *content_inode;
	uint64_t content_size;
	size_t content_length;
	int error;
	int flags;
	int visible_gate;

	visible_gate = 0;

	/* Only a readable regular file with content support can be leased. */
	if (file == NULL || lease == NULL)
		return EINVAL;
	memset(lease, 0, sizeof(*lease));
	content_inode = file_vm_inode(file);
	if (file->f_inode == NULL ||
	    file->f_inode->i_type != INODE_REG ||
	    content_inode == NULL ||
	    content_inode->i_type != INODE_REG ||
	    file->f_ops == NULL ||
	    file->f_ops->pread == NULL ||
	    !file_vm_content_available())
		return EOPNOTSUPP;
	flags = file_status_flags_get(file);
	if ((flags & O_ACCMODE) == O_WRONLY)
		return EBADF;
	if (file->f_inode->i_size < 0 || content_inode->i_size < 0)
		return EIO;
	content_size = (uint64_t)content_inode->i_size;
	if (content_size > SIZE_MAX)
		return EFBIG;

	/*
	 * Even an empty image needs a published gate: otherwise a concurrent
	 * grow could turn the sequence of short reads into a different
	 * image.
	 */
	if (content_size == 0)
		content_length = 1U;
	else
		content_length = (size_t)content_size;
	file_ref(file);

	/* Takes both I/O locks and publishes the content gate. */
	for (;;) {
		if (content_inode != file->f_inode) {
			error = vm_object_inode_io_wait(content_inode);
			if (error != 0)
				goto fail_file;
		}
		error = file_regular_io_lock(file->f_inode, 0);
		if (error != 0)
			goto fail_file;
		if (content_inode != file->f_inode &&
		    !mutex_trylock(&content_inode->i_io_lock)) {
			/*
			 * Stacked I/O normally takes outer then lower.  Never
			 * sleep on the lower mutex while retaining outer:
			 * drain the current lower owner without outer, then
			 * restart both gate checks.
			 */
			mutex_unlock(&file->f_inode->i_io_lock);
			error = file_regular_io_lock(content_inode, 0);
			if (error != 0)
				goto fail_file;
			mutex_unlock(&content_inode->i_io_lock);
			continue;
		}
		error = vm_object_content_begin(file, 0, content_length, NULL,
		    &lease->content);
		if (error != EBUSY && error != EAGAIN)
			break;
		if (content_inode != file->f_inode)
			mutex_unlock(&content_inode->i_io_lock);
		mutex_unlock(&file->f_inode->i_io_lock);
		error = vm_object_inode_io_wait(content_inode);
		if (error != 0)
			goto fail_file;
	}
	if (error != 0) {
		if (content_inode != file->f_inode)
			mutex_unlock(&content_inode->i_io_lock);
		mutex_unlock(&file->f_inode->i_io_lock);
		goto fail_file;
	}
	if (content_inode != file->f_inode) {
		error = vm_object_content_read_begin(file->f_inode);
		if (error != 0) {
			vm_object_content_abort(&lease->content);
			mutex_unlock(&content_inode->i_io_lock);
			mutex_unlock(&file->f_inode->i_io_lock);
			goto fail_file;
		}
		visible_gate = 1;
	}

	/*
	 * Prepare can perform old-dirty writeback.  CONTENT remains
	 * published while i_io is dropped, preventing a normal reader/writer
	 * from entering the backend and preventing new object faults from
	 * publishing PTEs.
	 */
	if (content_inode != file->f_inode)
		mutex_unlock(&content_inode->i_io_lock);
	mutex_unlock(&file->f_inode->i_io_lock);
	error = vm_object_content_prepare(&lease->content);
	mutex_lock(&file->f_inode->i_io_lock);
	if (content_inode != file->f_inode)
		mutex_lock(&content_inode->i_io_lock);
	if (error != 0) {
		vm_object_content_abort(&lease->content);
		if (visible_gate)
			vm_object_content_read_end(file->f_inode);
		if (content_inode != file->f_inode)
			mutex_unlock(&content_inode->i_io_lock);
		mutex_unlock(&file->f_inode->i_io_lock);
		goto fail_file;
	}
	lease->file = file;
	lease->io_inode = file->f_inode;
	lease->content_inode = content_inode;
	lease->size = content_inode->i_size;
	lease->held_content_inode_io = content_inode != file->f_inode;
	lease->held_visible_gate = visible_gate;
	lease->active = 1;
	return 0;

fail_file:
	(void)file_close(file);
	memset(lease, 0, sizeof(*lease));
	return error;
}

/* Selects a shared immutable-input lease without weakening mutable snapshots. */
int
file_exec_snapshot_begin(struct file *file, struct file_content_lease *lease)
{
	struct inode *inode;
	struct mount *mountp;
	int error;

	/* All unsupported identities retain the existing exclusive snapshot protocol. */
	inode = file != NULL ? file_vm_inode(file) : NULL;
	mountp = inode != NULL ? inode->i_mount : NULL;
	if (lease == NULL || inode == NULL || inode != file->f_inode ||
	    inode->i_type != INODE_REG || file->f_ops == NULL ||
	    file->f_ops->pread == NULL ||
	    (file_status_flags_get(file) & O_ACCMODE) == O_WRONLY ||
	    file->f_backing_claim != NULL || file->f_format_claim != NULL ||
	    mountp == NULL || (mountp->m_flags & MOUNT_READ_ONLY) == 0 ||
	    mountp->m_disk == NULL || (mountp->m_disk->d_flags & DISK_READ_ONLY) == 0 ||
	    disk_cache_acquire == NULL || disk_cache_release == NULL ||
	    !file_vm_content_available())
		return file_content_lease_begin(file, lease);
	memset(lease, 0, sizeof(*lease));
	if (inode->i_size < 0 || (uint64_t)inode->i_size > SIZE_MAX)
		return inode->i_size < 0 ? EIO : EFBIG;
	file_ref(file);
	error = disk_cache_acquire(mountp->m_disk, &lease->read_disk);
	if (error != 0)
		goto fail;
	if (vm_object_cache_prepare != NULL)
		vm_object_cache_prepare(file);

	/* Shared readers coexist; wait only when a replacement already owns the inode. */
	for (;;) {
		mutex_lock(&inode->i_io_lock);
		error = vm_object_content_read_begin(inode);
		if (error != EBUSY && error != EAGAIN)
			break;
		mutex_unlock(&inode->i_io_lock);
		error = vm_object_inode_io_wait(inode);
		if (error != 0)
			goto fail;
	}
	if (error == 0 && (inode->i_size < 0 || (uint64_t)inode->i_size > SIZE_MAX)) {
		error = inode->i_size < 0 ? EIO : EFBIG;
		vm_object_content_read_end(inode);
	}
	if (error == 0) {
		lease->file = file;
		lease->io_inode = lease->content_inode = inode;
		lease->size = inode->i_size;
		lease->active = lease->shared_read = 1;
	}
	mutex_unlock(&inode->i_io_lock);
	if (error != 0)
		goto fail;

	/* Optional cache refusal keeps the same content gate and serialized backend. */
	if (vm_object_cache_pin != NULL && vm_object_cache_unpin != NULL &&
	    vm_object_read_coherent != NULL)
		(void)vm_object_cache_pin(inode, &lease->read_object);
	return 0;

fail:
	if (lease->read_disk != NULL)
		disk_cache_release(lease->read_disk);
	memset(lease, 0, sizeof(*lease));
	(void)file_close(file);
	return error;
}

#include "file-exec-snapshot.inc"

/* Reads from a leased file within the size the lease captured. */
ssize_t
file_content_lease_pread(
	struct file_content_lease *lease,
	void *buffer,
	size_t length,
	off_t offset)
{
	ssize_t count;
	int error;

	if (lease == NULL ||
	    !lease->active ||
	    lease->file == NULL ||
	    offset < 0 ||
	    (buffer == NULL && length != 0))
		return -EINVAL;
	if (offset >= lease->size || length == 0)
		return 0;
	if ((uint64_t)length > (uint64_t)(lease->size - offset))
		length = (size_t)(lease->size - offset);
	if (lease->shared_read && lease->read_object != NULL) {
		error = vm_object_read_coherent(lease->content_inode, offset, buffer, length, &count);
		if (error != 0)
			return error == ENOENT ? -EIO : -error;
	} else {
		count = file_pread_internal(lease->file, buffer, length, offset,
		    FILE_IO_VM_OBJECT | (lease->shared_read ? 0 : FILE_IO_INODE_IO_OWNED));
	}
	if (count > (ssize_t)length)
		return -EIO;
	if (count > 0)
		lease->transferred = 1;
	return count;
}

/*
 * Releases a content lease and its file reference.
 */
void
file_content_lease_end(
	struct file_content_lease *lease)
{
	struct file *file;

	if (lease == NULL || !lease->active || lease->file == NULL)
		return;
	file = lease->file;
	if (lease->shared_read) {
		vm_object_content_read_end(lease->content_inode);
		if (lease->read_object != NULL)
			vm_object_cache_unpin(lease->read_object);
		if (lease->read_disk != NULL)
			disk_cache_release(lease->read_disk);
		if (lease->transferred)
			inode_touch(file->f_inode, INODE_ATTR_ATIME);
		memset(lease, 0, sizeof(*lease));
		(void)file_close(file);
		return;
	}
	vm_object_content_abort(&lease->content);
	if (lease->held_visible_gate)
		vm_object_content_read_end(lease->io_inode);
	if (lease->transferred)
		inode_touch(file->f_inode, INODE_ATTR_ATIME);
	if (lease->held_content_inode_io)
		mutex_unlock(&lease->content_inode->i_io_lock);
	mutex_unlock(&lease->io_inode->i_io_lock);
	memset(lease, 0, sizeof(*lease));
	(void)file_close(file);
}

/*
 * Starts a read or write transaction on a file with a credential.
 *
 * The transaction takes the shared position for stream reads and
 * writes, the inode I/O lock of a regular file, the content inode's
 * lock for a stacked write, and the backing mutation guard for a write.
 */
int
file_io_begin_cred(
	struct file *file,
	enum file_io_kind kind,
	off_t offset,
	unsigned internal_flags,
	const struct ucred *credential,
	struct file_io *io)
{
	int flags;
	unsigned long context_irq;
	int writing;
	int positional;
	struct backing_claim *claim;
	int error;

	/* Rejects an inconsistent request or an unsupported operation. */
	if (file == NULL ||
	    io == NULL ||
	    kind < FILE_IO_READ ||
	    kind > FILE_IO_PWRITE ||
	    (internal_flags & ~(FILE_IO_LOOP_BACKING | FILE_IO_VM_OBJECT |
	    FILE_IO_INODE_IO_OWNED | FILE_IO_CONTENT_CHANGE | FILE_IO_DRAIN |
	    FILE_IO_ORDERED)) != 0 ||
	    ((internal_flags & FILE_IO_INODE_IO_OWNED) != 0 &&
	    (internal_flags & FILE_IO_VM_OBJECT) == 0))
		return EINVAL;
	positional = file_io_is_positional(kind);
	writing = file_io_is_write(kind);
	if (!writing && (internal_flags & (FILE_IO_CONTENT_CHANGE | FILE_IO_DRAIN |
	    FILE_IO_ORDERED)) != 0)
		return EINVAL;
	if (positional && offset < 0)
		return EINVAL;
	flags = file_status_flags_get(file);
	if (writing) {
		if ((flags & O_ACCMODE) == O_RDONLY)
			return EBADF;
	} else {
		if ((flags & O_ACCMODE) == O_WRONLY)
			return EBADF;
	}
	if (file->f_inode != NULL && file->f_inode->i_type == INODE_DIR)
		return EISDIR;
	if (writing && file->f_inode != NULL &&
	    (file->f_inode->i_flags & INODE_SWAPFILE) != 0)
		return EBUSY;
	if (writing && file->f_inode != NULL &&
	    (file->f_inode->i_flags & INODE_LOOPFILE) != 0 &&
	    (internal_flags & FILE_IO_LOOP_BACKING) == 0)
		return EBUSY;
	if (file->f_ops == NULL ||
	    (kind == FILE_IO_READ && file->f_ops->read == NULL) ||
	    (kind == FILE_IO_WRITE && file->f_ops->write == NULL) ||
	    (kind == FILE_IO_PREAD && file->f_ops->pread == NULL) ||
	    (kind == FILE_IO_PWRITE && file->f_ops->pwrite == NULL))
		return EOPNOTSUPP;

	/* Records the transaction and finds the content inode. */
	memset(io, 0, sizeof(*io));
	io->file = file;
	if (file->f_inode != NULL && file->f_inode->i_type == INODE_REG)
		io->content_inode = file_vm_inode(file);
	else
		io->content_inode = NULL;
	if (file->f_inode != NULL && file->f_inode->i_type == INODE_REG &&
	    (io->content_inode == NULL ||
	     io->content_inode->i_type != INODE_REG)) {
		file_io_resources_release(io);
		memset(io, 0, sizeof(*io));
		return EIO;
	}
	/* Carries synchronous provenance independently of the current thread. */
	io->context.flags = IO_CONTEXT_THROUGH;
	io->context.origin_inode = io->content_inode;
	if (writing && (internal_flags & (FILE_IO_VM_OBJECT | FILE_IO_DRAIN)) != 0)
		io->context.flags |= IO_CONTEXT_DRAIN;
	if ((internal_flags & FILE_IO_ORDERED) != 0)
		io->context.flags |= IO_CONTEXT_ORDERED | IO_CONTEXT_DRAIN;
	if (io->content_inode != NULL) {
		context_irq = spin_lock_irqsave(&io->content_inode->i_vm_lock);
		io->context.content_generation = io->content_inode->i_vm_content_generation;
		spin_unlock_irqrestore(&io->content_inode->i_vm_lock, context_irq);
	}
	io->credential = credential;
	io->kind = kind;
	io->offset = offset;
	io->internal_flags = internal_flags;
	io->synchronous = writing && internal_flags == 0 &&
	    (flags & (O_SYNC | O_DSYNC)) != 0 && file->f_inode != NULL &&
	    (file->f_inode->i_type == INODE_REG || file->f_inode->i_type == INODE_BLOCK);
	io->append_requested = 0;
	if (!positional && writing && (flags & O_APPEND) != 0)
		io->append_requested = 1;

	/* Captures a stream witness before acquiring any inode/content lease. */
	if (internal_flags == 0 && io->content_inode != NULL &&
	    file->f_backing_claim == NULL && file->f_format_claim == NULL &&
	    readahead_demand_begin != NULL && readahead_demand_end != NULL) {
		if (mutex_trylock(&file->f_lock)) {
			if (writing)
				file_readahead_reset_owned(file);
			io->readahead_generation = file->f_readahead.generation;
			io->readahead_observer = 1;
			mutex_unlock(&file->f_lock);
		}
		if (readahead_demand_begin() == 0)
			io->readahead_demand = 1;
	}

	/* Prepares optional cache ownership before position and inode locks. */
	if (!writing && internal_flags == 0 && io->content_inode != NULL &&
	    vm_object_cache_prepare != NULL)
		vm_object_cache_prepare(file);

	/* Reserves delayed ownership before all position and content leases. */
	file_io_writeback_prepare(io, flags);

	/*
	 * Only regular files and block devices use the generic shared
	 * position.  Stream and device backends synchronize their queues
	 * independently.
	 */
	if (!positional && file->f_inode != NULL &&
	    (file->f_inode->i_type == INODE_REG ||
	     file->f_inode->i_type == INODE_BLOCK)) {
		mutex_lock(&file->f_lock);
		io->held_position = 1;
	}

	/* Pins cached reads before any speculative backend can serialize them. */
	file_io_cache_read_prepare(io);

	/* A regular file takes its I/O lock and, for a stacked write, the content inode's. */
	if (file->f_inode != NULL && file->f_inode->i_type == INODE_REG &&
	    (internal_flags & FILE_IO_INODE_IO_OWNED) == 0 &&
	    io->read_object == NULL) {
		error = file_regular_io_lock(file->f_inode, internal_flags);
		if (error != 0) {
			if (io->held_position)
				mutex_unlock(&file->f_lock);
			file_io_resources_release(io);
			memset(io, 0, sizeof(*io));
			return error;
		}
		io->held_inode_io = 1;
		if ((internal_flags & FILE_IO_VM_OBJECT) == 0 &&
		    io->content_inode != file->f_inode &&
		    vm_object_content_read_begin != NULL &&
		    vm_object_content_read_end != NULL) {
			error = vm_object_content_read_begin(file->f_inode);
			if (error != 0) {
				mutex_unlock(&file->f_inode->i_io_lock);
				if (io->held_position)
					mutex_unlock(&file->f_lock);
				file_io_resources_release(io);
				memset(io, 0, sizeof(*io));
				return error;
			}
			io->held_visible_gate = 1;
		}
		if (writing && io->content_inode != file->f_inode) {
			io->require_content_inode_io = 1;
			if (mutex_trylock(&io->content_inode->i_io_lock)) {
				io->held_content_inode_io = 1;
			} else {
				mutex_unlock(&file->f_inode->i_io_lock);
				io->held_inode_io = 0;
				mutex_lock(&io->content_inode->i_io_lock);
				mutex_unlock(&io->content_inode->i_io_lock);
				error = file_io_regular_locks_reacquire(io);
				if (error != 0) {
					if (io->held_visible_gate)
						vm_object_content_read_end(file->f_inode);
					if (io->held_position)
						mutex_unlock(&file->f_lock);
					file_io_resources_release(io);
					memset(io, 0, sizeof(*io));
					return error;
				}
			}
		}
	}

	/* A write to a regular file claims the backing against reclaim. */
	if (writing && file->f_inode != NULL &&
	    file->f_inode->i_type == INODE_REG) {
		if (file->f_format_claim != NULL)
			claim = file->f_format_claim;
		else if ((internal_flags & FILE_IO_LOOP_BACKING) != 0)
			claim = file->f_backing_claim;
		else
			claim = NULL;
		error = backing_mutation_begin_inode_claimed(file->f_inode,
		    claim, &io->backing_guard);
		if (error != 0) {
			if (io->held_visible_gate)
				vm_object_content_read_end(file->f_inode);
			if (io->held_content_inode_io)
				mutex_unlock(&io->content_inode->i_io_lock);
			if (io->held_inode_io)
				mutex_unlock(&file->f_inode->i_io_lock);
			if (io->held_position)
				mutex_unlock(&file->f_lock);
			file_io_resources_release(io);
			memset(io, 0, sizeof(*io));
			return error;
		}
	}

	/* A stream transfer starts at the shared position or at EOF. */
	if (!positional) {
		if (io->held_position)
			io->offset = file->f_offset;
		else
			io->offset = 0;
		if (io->append_requested && io->content_inode != NULL)
			io->offset = io->content_inode->i_size;
	}
	io->readahead_start = io->offset;
	return 0;
}

/*
 * Starts a read or write transaction without a credential.
 */
int
file_io_begin(
	struct file *file,
	enum file_io_kind kind,
	off_t offset,
	unsigned internal_flags,
	struct file_io *io)
{
	int error;

	error = file_io_begin_cred(file, kind, offset, internal_flags, NULL, io);
	return error;
}

/*
 * Sets the file size limit a write transaction may grow the file to.
 */
void
file_io_set_growth_limit(
	struct file_io *io,
	uint64_t limit)
{
	if (io == NULL || io->file == NULL)
		return;
	io->growth_limit = limit;
	io->growth_limit_enabled = limit != UINT64_MAX;
}

/*
 * Reports and clears whether the last transfer hit the growth limit.
 */
int
file_io_take_growth_limit_hit(
	struct file_io *io)
{
	int hit;

	if (io == NULL)
		return 0;
	hit = io->growth_limit_hit != 0;
	io->growth_limit_hit = 0;
	return hit;
}

/*
 * Transfers one chunk of a transaction.
 *
 * A read of a regular file with a published shared mapping is served
 * from the mapping's cache under a content read lease.  A write of a
 * regular file publishes a resize and a content gate so that shared
 * mappings observe the new bytes, clears set-id bits first, and
 * respects the growth limit.
 */
ssize_t
file_io_transfer(
	struct file_io *io,
	void *buffer,
	size_t length)
{
	struct file *file;
	struct vm_object_resize resize;
	struct vm_object_content content;
	struct vm_object_resize *resize_argument;
	off_t write_start;
	uint64_t limit_existing;
	uint64_t current_existing;
	size_t requested_length;
	size_t useful;
	int resize_error;
	int content_error;
	int delayed_eligible;
	ssize_t result;
	ssize_t cached;
	int cache_published;
	uint64_t maximum_end;
	uint64_t remaining;
	uint64_t end;
	unsigned forward_flags;
	off_t actual_end;

	write_start = 0;
	limit_existing = 0;
	requested_length = length;

	if (io == NULL || io->file == NULL || (buffer == NULL && length != 0))
		return -EINVAL;
	file = io->file;
	io_stats_record(file_io_is_write(io->kind) ? IO_FILE_WRITE : IO_FILE_READ,
	    length);
	memset(&resize, 0, sizeof(resize));
	memset(&content, 0, sizeof(content));

	/* Keeps every formatter transfer inside its original, immutable EOF. */
	if (file_io_is_write(io->kind) && file->f_format_claim != NULL) {
		/* Rejects append even when descriptor flags changed after reservation. */
		if (io->append_requested ||
		    (file_status_flags_get(file) & O_APPEND) != 0)
			return -EINVAL;

		/* Refuses stale backing metadata before calling the filesystem. */
		if (file->f_inode->i_size < 0 ||
		    (uint64_t)file->f_inode->i_size != file->f_format_size)
			return -ESTALE;

		/* Rejects the whole transfer rather than extending or clipping it. */
		if (io->offset < 0 ||
		    (uint64_t)io->offset > file->f_format_size ||
		    (uint64_t)length > file->f_format_size - (uint64_t)io->offset)
			return -EFBIG;
	}

	/*
	 * A published shared cache is the read source of truth.  Once
	 * selected, its inode read lease stays in struct file_io until
	 * file_io_end(), covering every syscall copy/iovec chunk.  Pinned
	 * reads reach resident pages without the backend inode mutex.  The
	 * fallback drops that mutex while the shared lease excludes writers
	 * and EOF changes; actual cache misses retain internal backend locking.
	 */
	if (length != 0 && !file_io_is_write(io->kind) &&
	    (io->held_inode_io || io->read_object != NULL) &&
	    (io->internal_flags & FILE_IO_VM_OBJECT) == 0 &&
	    vm_object_read_coherent != NULL &&
	    vm_object_content_read_begin != NULL &&
	    vm_object_content_read_end != NULL &&
	    vm_object_cache_published != NULL) {
		cached = 0;
		if (!io->held_content_read) {
read_cache_retry:
			cache_published = vm_object_cache_published(io->content_inode);
			if (cache_published == -EAGAIN) {
				mutex_unlock(&file->f_inode->i_io_lock);
				io->held_inode_io = 0;
				content_error = vm_object_inode_io_wait(io->content_inode);
				mutex_lock(&file->f_inode->i_io_lock);
				io->held_inode_io = 1;
				if (content_error != 0)
					return -content_error;
				goto read_cache_retry;
			}
			if (cache_published < 0)
				return cache_published;
			content_error = vm_object_content_read_begin(io->content_inode);
			if (content_error == EBUSY) {
				mutex_unlock(&file->f_inode->i_io_lock);
				io->held_inode_io = 0;
				content_error = vm_object_inode_io_wait(io->content_inode);
				mutex_lock(&file->f_inode->i_io_lock);
				io->held_inode_io = 1;
				if (content_error != 0)
					return -content_error;
				goto read_cache_retry;
			}
			if (content_error != 0)
				return -content_error;
			io->held_content_read = 1;

			/*
			 * With no published cache, the same final-inode read
			 * lease still excludes direct lower-layer writers
			 * across every backend chunk.
			 */
			io->coherent_read = cache_published != 0;
		}
		if (!io->coherent_read)
			goto backend_transfer;
		if (io->held_inode_io) {
			mutex_unlock(&file->f_inode->i_io_lock);
			io->held_inode_io = 0;
		}
		useful = 0;
		if (vm_object_read_coherent_useful != NULL) {
			content_error = vm_object_read_coherent_useful(io->content_inode,
			    io->offset, buffer, length, &cached, &useful);
		} else {
			content_error = vm_object_read_coherent(io->content_inode, io->offset,
			    buffer, length, &cached);
		}
		io->readahead_useful += useful;
		if (io->read_object == NULL) {
			mutex_lock(&file->f_inode->i_io_lock);
			io->held_inode_io = 1;
		}

		/* Never enter an unprotected backend if a pinned identity is lost. */
		if (content_error == ENOENT && io->read_object != NULL)
			return -EIO;
		if (content_error == ENOENT) {
			/*
			 * Final-mapping teardown flushed the old cache before
			 * removing it.  Continue the same leased read through
			 * the stable backend.
			 */
			io->coherent_read = 0;
			goto backend_transfer;
		}
		if (content_error != 0)
			return -content_error;
		io->offset += cached;
		if (io->held_position)
			file->f_offset = io->offset;
		if (cached > 0)
			io->transferred = 1;
		return cached;
	}

backend_transfer:
transaction_retry:
	/*
	 * This is a tentative EOF until resize/content begin publishes the
	 * final inode gate.  Any older generic writer makes begin return
	 * BUSY, and the retry samples its committed EOF before choosing the
	 * append range again.
	 */
	length = requested_length;
	io->growth_limit_hit = 0;
	memset(&resize, 0, sizeof(resize));
	memset(&content, 0, sizeof(content));
	if (io->append_requested && !io->append_positioned &&
	    io->content_inode != NULL)
		io->offset = io->content_inode->i_size;

	/* Clips a write to the growth limit. */
	if (length != 0 && file_io_is_write(io->kind) &&
	    io->growth_limit_enabled && io->content_inode != NULL) {
		if (io->content_inode->i_size > 0)
			limit_existing = (uint64_t)io->content_inode->i_size;
		else
			limit_existing = 0;
		if (limit_existing > io->growth_limit)
			maximum_end = limit_existing;
		else
			maximum_end = io->growth_limit;
		if (io->offset < 0 || (uint64_t)io->offset >= maximum_end) {
			io->growth_limit_hit = 1;
			return -EFBIG;
		}
		remaining = maximum_end - (uint64_t)io->offset;
		if (remaining < length) {
			if (remaining > SIZE_MAX)
				length = SIZE_MAX;
			else
				length = (size_t)remaining;
			io->growth_limit_hit = 1;
		}
		if (length == 0)
			return -EFBIG;
	} else if (io->content_inode != NULL) {
		if (io->content_inode->i_size > 0)
			limit_existing = (uint64_t)io->content_inode->i_size;
		else
			limit_existing = 0;
	}

	/* An extending write publishes a resize before touching the backend. */
	if (length != 0 && file_io_is_write(io->kind) &&
	    io->held_inode_io &&
	    (io->internal_flags & FILE_IO_VM_OBJECT) == 0 &&
	    file_vm_resize_available()) {
		write_start = io->offset;
		if (write_start < 0 || (uint64_t)write_start + length <
		    (uint64_t)write_start ||
		    (uint64_t)write_start + length > (uint64_t)OFF_T_MAX)
			return -EFBIG;
		end = (uint64_t)write_start + length;
resize_retry:
		if (vm_object_inode_resize_active(io->content_inode)) {
			file_io_regular_locks_drop(io);
			resize_error = vm_object_inode_io_wait(io->content_inode);
			if (file_io_regular_locks_reacquire(io) != 0)
				return -EINTR;
			if (resize_error != 0)
				return -resize_error;
			if (io->append_requested && !io->append_positioned)
				goto transaction_retry;
			goto resize_retry;
		}
		if (end > (uint64_t)io->content_inode->i_size) {
			resize_error = vm_object_resize_begin(io->content_inode,
			    (off_t)end, &resize);
			if (resize_error == EBUSY || resize_error == EAGAIN) {
				file_io_regular_locks_drop(io);
				resize_error = vm_object_inode_io_wait(io->content_inode);
				if (file_io_regular_locks_reacquire(io) != 0)
					return -EINTR;
				if (resize_error != 0)
					return -resize_error;
				if (io->append_requested && !io->append_positioned)
					goto transaction_retry;
				goto resize_retry;
			}
			if (resize_error != 0)
				return -resize_error;
			if (resize.active) {
				file_io_regular_locks_drop(io);
				resize_error = vm_object_resize_prepare(&resize);
				if (file_io_regular_locks_reacquire(io) != 0) {
					vm_object_resize_abort(&resize);
					return -EINTR;
				}
				if (resize_error != 0) {
					vm_object_resize_abort(&resize);
					return -resize_error;
				}
			}
		}
	}

	/*
	 * Same-EOF writes need the same gate as extending writes.  Prepare
	 * first revokes writable PTEs and flushes the old dirty image.
	 * Resident pages stay BUSY (pins are not orphaned) until the backend
	 * result is known.
	 */
	if (length != 0 && file_io_is_write(io->kind) &&
	    io->held_inode_io &&
	    (io->internal_flags & FILE_IO_VM_OBJECT) == 0 &&
	    file_vm_content_available()) {
		if (resize.active)
			resize_argument = &resize;
		else
			resize_argument = NULL;
		content_error = vm_object_content_begin(file, io->offset, length,
		    resize_argument, &content);
		if (content_error == EBUSY || content_error == EAGAIN) {
			if (resize.active)
				vm_object_resize_abort(&resize);
			file_io_regular_locks_drop(io);
			content_error = vm_object_inode_io_wait(io->content_inode);
			if (file_io_regular_locks_reacquire(io) != 0)
				return -EINTR;
			if (content_error != 0)
				return -content_error;
			goto transaction_retry;
		}
		if (content_error != 0) {
			if (resize.active)
				vm_object_resize_abort(&resize);
			return -content_error;
		}

		/*
		 * The published gate closes the sample/begin window.  An older
		 * writer can finish between those operations; retry rather
		 * than committing a stale append range or RLIMIT_FSIZE
		 * decision.
		 */
		if (io->content_inode->i_size > 0)
			current_existing = (uint64_t)io->content_inode->i_size;
		else
			current_existing = 0;
		if ((io->growth_limit_enabled ||
		    (io->append_requested && !io->append_positioned)) &&
		    current_existing != limit_existing) {
			vm_object_content_abort(&content);
			if (resize.active)
				vm_object_resize_abort(&resize);
			goto transaction_retry;
		}
		/* Proves allocation after publishing the final content gate. */
		delayed_eligible = 0;
		if (io->writeback_object != NULL && !resize.active &&
		    file->f_inode->i_mount->m_type->writeback_range != NULL) {
			delayed_eligible = file->f_inode->i_mount->m_type->writeback_range(
			    file, io->offset, length);
			if (delayed_eligible < 0) {
				vm_object_content_abort(&content);
				return delayed_eligible;
			}
		}
		file_io_regular_locks_drop(io);
		content_error = EAGAIN;
		if (delayed_eligible > 0) {
			content_error = vm_object_content_prepare_delayed(&content,
			    io->writeback_object, &io->writeback_ticket);
		}
		if (content_error == EAGAIN)
			content_error = vm_object_content_prepare(&content);
		if (file_io_regular_locks_reacquire(io) != 0) {
			vm_object_content_abort(&content);
			if (resize.active)
				vm_object_resize_abort(&resize);
			return -EINTR;
		}
		if (content_error != 0) {
			vm_object_content_abort(&content);
			if (resize.active)
				vm_object_resize_abort(&resize);
			return -content_error;
		}
		if (io->append_requested)
			io->append_positioned = 1;
	}
	if (io->append_requested && !io->append_positioned)
		io->append_positioned = 1;

	/*
	 * Clear set-user-ID/set-group-ID before the first externally
	 * requested regular-file backend mutation.  This is inside the same
	 * i_io/content transaction as the data write, so exec cannot observe
	 * new bytes with stale privilege metadata.  Failure leaves the
	 * backend untouched.
	 */
	if (length != 0 && file_io_is_write(io->kind) &&
	    file->f_inode != NULL && file->f_inode->i_type == INODE_REG &&
	    !io->setid_prepared &&
	    file->f_inode == file_vm_inode(file) &&
	    (io->credential != NULL ||
	    (io->internal_flags & FILE_IO_CONTENT_CHANGE) != 0)) {
		if (io->credential != NULL)
			content_error = vfs_clear_setid_on_write(file->f_inode,
			    io->credential);
		else
			content_error = vfs_clear_setid_on_content_change(
			    file->f_inode);
		if (content_error != 0) {
			if (content.active)
				vm_object_content_abort(&content);
			if (resize.active)
				vm_object_resize_abort(&resize);
			return -content_error;
		}
		io->setid_prepared = 1;
	}

	/* Records logical mutation before any backend or leaf write is accepted. */
	if (length != 0 && file_io_is_write(io->kind) &&
	    file->f_inode != NULL && file->f_inode->i_mount != NULL)
		io_epoch_begin(&file->f_inode->i_mount->m_write_epoch);

	/* Accepts prepared delayed bytes into the same coherent content transaction. */
	if (content.writeback_ticket != NULL) {
		result = (ssize_t)length;
		io->offset += result;
	} else {
		/* Runs the backend operation. */
		switch (io->kind) {
		case FILE_IO_READ:
			if (io->held_position)
				file->f_offset = io->offset;
			result = file->f_ops->read(file, buffer, length);
			if (result > 0 && io->held_position)
				io->offset = file->f_offset;
			break;
		case FILE_IO_WRITE:
			forward_flags = io->internal_flags;
			if (io->held_content_inode_io ||
			    (io->internal_flags & FILE_IO_INODE_IO_OWNED) != 0)
				forward_flags |= FILE_IO_VM_OBJECT | FILE_IO_INODE_IO_OWNED;

			/*
			 * A stacking backend must receive the originating credential
			 * and content-change marker.  The outer file_io owns
			 * f_offset/O_APPEND, so its positional internal callback is
			 * also the canonical forwarding path for ordinary
			 * write/writev.
			 */
			if (file->f_ops->pwrite_internal != NULL) {
				result = file->f_ops->pwrite_internal(file, buffer, length,
				    io->offset, forward_flags, io->credential, &io->context);
				if (result > 0)
					io->offset += result;
			} else {
				if (io->held_position)
					file->f_offset = io->offset;
				result = file->f_ops->write(file, buffer, length);
				if (result > 0 && io->held_position)
					io->offset = file->f_offset;
			}
			break;
		case FILE_IO_PREAD:
			if (io->internal_flags != 0 &&
			    file->f_ops->pread_internal != NULL)
				result = file->f_ops->pread_internal(file, buffer, length,
				    io->offset, io->internal_flags);
			else
				result = file->f_ops->pread(file, buffer, length, io->offset);
			if (result > 0)
				io->offset += result;
			break;
		case FILE_IO_PWRITE:
			forward_flags = io->internal_flags;
			if (io->held_content_inode_io ||
			    (io->internal_flags & FILE_IO_INODE_IO_OWNED) != 0)
				forward_flags |= FILE_IO_VM_OBJECT | FILE_IO_INODE_IO_OWNED;
			if (file->f_ops->pwrite_internal != NULL)
				result = file->f_ops->pwrite_internal(file, buffer, length,
				    io->offset, forward_flags, io->credential, &io->context);
			else
				result = file->f_ops->pwrite(file, buffer, length, io->offset);
			if (result > 0)
				io->offset += result;
			break;
		default:
			result = -EINVAL;
			break;
		}
	}

	/* Commits or aborts the published content and resize. */
	if (content.active) {
		if (result > 0)
			vm_object_content_commit(&content, buffer, (size_t)result);
		else
			vm_object_content_abort(&content);
	}
	if (resize.active) {
		if (result > 0) {
			actual_end = write_start + result;
			if (actual_end > io->content_inode->i_size)
				io->content_inode->i_size = actual_end;
			vm_object_resize_commit(&resize, io->content_inode->i_size);
		} else {
			io->content_inode->i_size = resize.old_size;
			vm_object_resize_abort(&resize);
		}
	}
	if (length != 0 && file_io_is_write(io->kind) &&
	    file->f_inode != NULL && file->f_inode->i_mount != NULL)
		io_epoch_end(&file->f_inode->i_mount->m_write_epoch);
	if (result > 0)
		io->transferred = 1;
	return result;
}

/*
 * Completes one public transfer and reports its synchronous durability boundary.
 */
ssize_t
file_io_complete(
	struct file_io *io,
	ssize_t result)
{
	struct file *file;
	struct inode *inode;
	off_t start;
	off_t eof;
	uint64_t generation;
	uint64_t useful;
	unsigned observe;
	unsigned synchronize;
	int error;

	/* Samples authoritative EOF while the coherent read still owns its content lease. */
	if (io == NULL || io->file == NULL)
		return result;
	file = io->file;
	inode = io->content_inode;
	start = io->readahead_start;
	generation = io->readahead_generation;
	useful = io->readahead_useful;
	observe = io->readahead_demand && io->readahead_observer &&
	    !file_io_is_write(io->kind) &&
	    (io->held_content_read || result <= 0) && inode != NULL;
	eof = observe && io->held_content_read ? inode->i_size : 0;
	synchronize = io->synchronous && io->transferred &&
	    (io->context.flags & (IO_CONTEXT_DRAIN | IO_CONTEXT_ORDERED)) == 0;
	file_io_end(io);

	/* Admits optional work only after all demand leases and priority ownership leave. */
	if (observe)
		file_readahead_completed(file, inode, start, result, eof, generation, useful);

	/* Uses full fsync for both flags; O_DSYNC permits this stronger guarantee. */
	if (synchronize) {
		error = file_fsync(file);
		if (error != 0 && result >= 0)
			return -error;
	}

	/* Preserves a prior transfer error while recording any later flush failure. */
	return result;
}

/*
 * Ends a transaction, publishing the position and timestamps and
 * releasing every lock and lease it holds.
 */
void
file_io_end(
	struct file_io *io)
{
	struct file *file;
	unsigned touch_mask;

	if (io == NULL || io->file == NULL)
		return;
	file = io->file;

	/* Publishes the new position and the access timestamps. */
	if (!file_io_is_positional(io->kind) && io->held_position &&
	    (!io->append_requested || io->transferred))
		file->f_offset = io->offset;
	if (io->transferred && file->f_inode != NULL) {
		if (file_io_is_write(io->kind))
			touch_mask = INODE_ATTR_MTIME | INODE_ATTR_CTIME;
		else
			touch_mask = INODE_ATTR_ATIME;
		inode_touch(file->f_inode, touch_mask);
	}

	/* Releases the leases, the gates, and the locks in reverse order. */
	if (io->held_content_read) {
		if (vm_object_content_read_end == NULL)
			HAL_FATAL("lost VM content read lease implementation");
		vm_object_content_read_end(io->content_inode);
		io->held_content_read = 0;
	}
	if (io->held_visible_gate) {
		if (vm_object_content_read_end == NULL)
			HAL_FATAL("lost visible VM content gate implementation");
		vm_object_content_read_end(file->f_inode);
		io->held_visible_gate = 0;
	}
	if (io->held_content_inode_io) {
		mutex_unlock(&io->content_inode->i_io_lock);
		io->held_content_inode_io = 0;
	}
	if (io->held_inode_io)
		mutex_unlock(&file->f_inode->i_io_lock);
	if (io->held_position)
		mutex_unlock(&file->f_lock);
	backing_mutation_end(&io->backing_guard);
	file_io_resources_release(io);
	memset(io, 0, sizeof(*io));
}

/*
 * Reads at the shared position.
 */
ssize_t
file_read(
	struct file *file,
	void *buffer,
	size_t length)
{
	ssize_t result;

	result = file_io_once(file, FILE_IO_READ, buffer, length, 0, 0);
	return result;
}

/*
 * Reads at an offset.
 */
ssize_t
file_pread(
	struct file *file,
	void *buffer,
	size_t length,
	off_t offset)
{
	ssize_t result;

	result = file_pread_internal(file, buffer, length, offset, 0);
	return result;
}

/*
 * Reads at an offset with internal transaction flags.
 */
ssize_t
file_pread_internal(
	struct file *file,
	void *buffer,
	size_t length,
	off_t offset,
	unsigned internal_flags)
{
	ssize_t result;

	result = file_io_once(file, FILE_IO_PREAD, buffer, length, offset,
	    internal_flags);
	return result;
}

/*
 * Writes at an offset.
 */
ssize_t
file_pwrite(
	struct file *file,
	const void *buffer,
	size_t length,
	off_t offset)
{
	ssize_t result;

	result = file_pwrite_internal(file, buffer, length, offset, 0);
	return result;
}

/*
 * Writes at an offset with internal transaction flags.
 */
ssize_t
file_pwrite_internal(
	struct file *file,
	const void *buffer,
	size_t length,
	off_t offset,
	unsigned internal_flags)
{
	ssize_t result;

	result = file_pwrite_internal_cred(file, buffer, length, offset,
	    internal_flags, NULL);
	return result;
}

/*
 * Writes at an offset with internal transaction flags and a credential.
 */
ssize_t
file_pwrite_internal_cred(
	struct file *file,
	const void *buffer,
	size_t length,
	off_t offset,
	unsigned internal_flags,
	const struct ucred *credential)
{
	ssize_t result;

	result = file_pwrite_context(file, buffer, length, offset,
	    internal_flags, credential, NULL);
	return result;
}

/*
 * Writes through a synchronous child operation with explicit inherited provenance.
 */
ssize_t
file_pwrite_context(
	struct file *file,
	const void *buffer,
	size_t length,
	off_t offset,
	unsigned internal_flags,
	const struct ucred *credential,
	const struct io_context *context)
{
	struct file_io io;
	struct io_context inherited;
	ssize_t result;
	int error;

	/* Validates inherited context before taking any file or content lease. */
	error = io_context_child(&inherited, context, 0);
	if (error != 0)
		return -error;
	error = file_io_begin_cred(file, FILE_IO_PWRITE, offset,
	    internal_flags, credential, &io);
	if (error != 0)
		return -error;
	if (context != NULL) {
		inherited.flags |= io.context.flags;
		io.context = inherited;
	}
	if ((internal_flags & FILE_IO_LOOP_BACKING) != 0)
		io.context.claim = file->f_backing_claim;
	result = file_io_transfer(&io, (void *)buffer, length);
	result = file_io_complete(&io, result);
	return result;
}

/*
 * Writes at the shared position.
 */
ssize_t
file_write(
	struct file *file,
	const void *buffer,
	size_t length)
{
	ssize_t result;

	result = file_io_once(file, FILE_IO_WRITE, (void *)buffer, length, 0, 0);
	return result;
}

/*
 * Reads the next directory entry, merging in mount points and skipping
 * entries a mount shadows.
 */
int
file_readdir(
	struct file *file,
	struct dirent *entry,
	int *eof)
{
	int error;

	if (file == NULL || entry == NULL || eof == NULL)
		return EINVAL;
	if (file->f_inode == NULL || file->f_inode->i_type != INODE_DIR)
		return ENOTDIR;
	if (file->f_ops == NULL || file->f_ops->readdir == NULL)
		return EOPNOTSUPP;

	/* Mount points are listed first, from the mount cursor. */
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

	/* Then the backend entries, minus those a mount shadows. */
	for (;;) {
		error = file->f_ops->readdir(file, entry, eof);
		if (error != 0 || *eof ||
		    !mount_child_shadows(&file->f_path, entry->d_name)) {
			mutex_unlock(&file->f_lock);
			return error;
		}
	}
}

/*
 * Moves the shared position, or asks the backend to.
 *
 * SEEK_DATA and SEEK_HOLE expose a conservative dense-file view in
 * which EOF is the only hole.
 */
off_t
file_seek(
	struct file *file,
	off_t offset,
	int whence)
{
	off_t base;
	off_t target;

	if (file == NULL)
		return -EINVAL;
	mutex_lock(&file->f_lock);
	if (whence == SEEK_DATA || whence == SEEK_HOLE) {
		if (file->f_inode == NULL || file->f_inode->i_type != INODE_REG) {
			mutex_unlock(&file->f_lock);
			return -EINVAL;
		}
		if (offset < 0 || offset >= file->f_inode->i_size) {
			mutex_unlock(&file->f_lock);
			return -ENXIO;
		}

		/*
		 * The current filesystems expose a conservative dense-file
		 * view.  Reporting EOF as the only hole is permitted even
		 * when a backend stores an all-zero extent sparsely.
		 */
		if (whence == SEEK_DATA)
			base = offset;
		else
			base = file->f_inode->i_size;
		file->f_offset = base;
	} else if (file->f_ops != NULL && file->f_ops->seek != NULL) {
		base = file->f_ops->seek(file, offset, whence);
	} else {
		/* Only seekable object types take the generic path. */
		if (file->f_inode == NULL ||
		    (file->f_inode->i_type != INODE_REG &&
		     file->f_inode->i_type != INODE_DIR &&
		     file->f_inode->i_type != INODE_BLOCK)) {
			mutex_unlock(&file->f_lock);
			return -ESPIPE;
		}
		if (whence == 0)
			base = 0;
		else if (whence == 1)
			base = file->f_offset;
		else if (whence == 2)
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
		if (file->f_inode->i_type == INODE_DIR && whence == 0 &&
		    target == 0)
			file->f_mount_cursor = 0;
		base = target;
	}
	if (base >= 0)
		file_readahead_reset_owned(file);
	mutex_unlock(&file->f_lock);
	return base;
}

/*
 * Flushes a file through its backend or its inode.
 */
int
file_fsync(
	struct file *file)
{
	struct inode *inode;
	struct io_error_snapshot snapshot;
	int error;
	int observed;

	/* Serializes one open-description drain and its error acknowledgement. */
	if (file == NULL)
		return EINVAL;
	mutex_lock(&file->f_lock);
	inode = file_vm_inode(file);
	error = 0;
	if (inode != NULL && vm_object_sync_inode != NULL)
		error = vm_object_sync_inode(inode);

	/* Preserves the current drain failure even after an earlier notification. */
	if (error == 0)
		error = file_fsync_backend_locked(file);

	/* Records storage failures without converting unsupported operations to history. */
	if (inode != NULL && error != 0 && error != EINVAL && error != EBADF &&
	    error != EOPNOTSUPP && io_error_record != NULL) {
		io_error_record(&inode->i_write_error, error);
		if (inode->i_mount != NULL)
			io_error_record(&inode->i_mount->m_write_error, error);
	}

	/* Acknowledges only the captured event actually represented by this result. */
	if (inode != NULL && io_error_snapshot != NULL && io_error_observe != NULL) {
		io_error_snapshot(&inode->i_write_error, &snapshot);
		if (error == 0 || error == snapshot.error) {
			observed = io_error_observe(&snapshot, &file->f_write_error_cursor);
			if (error == 0)
				error = observed;
		}
	}
	/* A shared metadata checkpoint failure must reach every independent opener. */
	if (inode != NULL && inode->i_mount != NULL &&
	    io_error_snapshot != NULL && io_error_observe != NULL) {
		io_error_snapshot(&inode->i_mount->m_metadata_error, &snapshot);
		if (error == 0 || error == snapshot.error) {
			observed = io_error_observe(&snapshot, &file->f_metadata_error_cursor);
			if (error == 0)
				error = observed;
		}
	}
	mutex_unlock(&file->f_lock);
	return error;
}

/*
 * Drains a stacked backend after its caller already owns shared-page writeback.
 *
 * This does not start another VM drain or consume a user description's error
 * cursor. The upper operation records and reports any resulting storage error.
 */
int
file_fsync_backend(
	struct file *file)
{
	int error;

	/* Serializes the backend description without re-entering inode VM ownership. */
	if (file == NULL)
		return EINVAL;
	mutex_lock(&file->f_lock);
	error = file_fsync_backend_locked(file);
	mutex_unlock(&file->f_lock);
	return error;
}

/*
 * Invalidates speculative work when a descriptor owner drops an open description.
 */
void
file_readahead_invalidate(
	struct file *file)
{
	/* Serializes close against pending observations and optional admission. */
	if (file == NULL)
		return;
	mutex_lock(&file->f_lock);
	file_readahead_reset_owned(file);
	mutex_unlock(&file->f_lock);
}

/*
 * Drops a reference on a file, closing it with the last one.
 */
int
file_close(
	struct file *file)
{
	int error;

	error = 0;

	if (file == NULL)
		return EBADF;
	if (refcount_load(&file->f_refs) == 0)
		return EBADF;
	if (!refcount_put(&file->f_refs))
		return 0;

	/* The last reference owns the description exclusively, including its stream state. */
	file_readahead_reset_owned(file);

	/* Releases the record locks, the backend state, and the path. */
	record_lock_release_file(file);
	if (file->f_ops != NULL && file->f_ops->close != NULL)
		error = file->f_ops->close(file);

	/* Releases owner exclusion only after the final backend close completes. */
	if (file->f_format_claim != NULL) {
		backing_claim_release(file->f_format_claim);
		file->f_format_claim = NULL;
	}
	if (file->f_path.p_inode != NULL)
		path_release(&file->f_path);
	else if (file->f_inode != NULL)
		inode_release(file->f_inode);
	file_free(file);
	return error;
}

/*
 * Takes a reference on a file.
 */
void
file_ref(
	struct file *file)
{
	if (file == NULL)
		return;
	refcount_get(&file->f_refs);
}

/*
 * Reports the inode whose content backs a file's shared mappings.
 */
struct inode *
file_vm_inode(
	struct file *file)
{
	if (file == NULL)
		return NULL;
	return file->f_vm_inode;
}

/*
 * Closes every open file in the pool.
 */
void
file_pool_reset(
	void)
{
	unsigned i;

	for (i = 0; i < FILE_MAX; i++) {
		if (file_used[i])
			(void)file_close(&files[i]);
	}
}

/*
 * Counts the open files in the pool.
 */
unsigned
file_count(
	void)
{
	unsigned i;
	unsigned count;
	unsigned long irq;

	count = 0;
	irq = spin_lock_irqsave(&file_pool_lock);
	for (i = 0; i < FILE_MAX; i++) {
		if (file_used[i] != 0)
			count++;
	}
	spin_unlock_irqrestore(&file_pool_lock, irq);
	return count;
}

/* Takes a zeroed file from the pool with one reference. */
static struct file *
file_alloc(
	void)
{
	unsigned i;
	unsigned long irq;

	irq = spin_lock_irqsave(&file_pool_lock);
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

/* Returns a file to the pool. */
static void
file_free(
	struct file *file)
{
	unsigned i;
	unsigned long irq;

	irq = spin_lock_irqsave(&file_pool_lock);
	for (i = 0; i < FILE_MAX; i++) {
		if (&files[i] == file) {
			memset(file, 0, sizeof(*file));
			file_used[i] = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&file_pool_lock, irq);
}

/* Tests whether an operation carries its own offset. */
static int
file_io_is_positional(
	enum file_io_kind kind)
{
	if (kind == FILE_IO_PREAD)
		return 1;
	if (kind == FILE_IO_PWRITE)
		return 1;
	return 0;
}

/* Tests whether an operation writes. */
static int
file_io_is_write(
	enum file_io_kind kind)
{
	if (kind == FILE_IO_WRITE)
		return 1;
	if (kind == FILE_IO_PWRITE)
		return 1;
	return 0;
}

/* Tests whether the VM object resize protocol is linked in. */
static int
file_vm_resize_available(
	void)
{
	if (vm_object_inode_io_wait == NULL)
		return 0;
	if (vm_object_inode_resize_active == NULL)
		return 0;
	if (vm_object_resize_begin == NULL)
		return 0;
	if (vm_object_resize_prepare == NULL)
		return 0;
	if (vm_object_resize_commit == NULL)
		return 0;
	if (vm_object_resize_abort == NULL)
		return 0;
	return 1;
}

/* Tests whether the VM object content protocol is linked in. */
static int
file_vm_content_available(
	void)
{
	if (!file_vm_resize_available())
		return 0;
	if (vm_object_content_begin == NULL)
		return 0;
	if (vm_object_content_prepare == NULL)
		return 0;
	if (vm_object_content_commit == NULL)
		return 0;
	if (vm_object_content_abort == NULL)
		return 0;
	if (vm_object_content_read_begin == NULL)
		return 0;
	if (vm_object_content_read_end == NULL)
		return 0;
	return 1;
}

/* Takes a regular file's I/O lock outside any EOF transaction. */
static int
file_regular_io_lock(
	struct inode *inode,
	unsigned internal_flags)
{
	int error;

	/*
	 * Take i_io only after an older EOF transaction has left its
	 * publication gate, then recheck the gate to close the wait/lock
	 * race.
	 */
	if ((internal_flags & FILE_IO_VM_OBJECT) != 0 ||
	    !file_vm_resize_available()) {
		mutex_lock(&inode->i_io_lock);
		return 0;
	}
	for (;;) {
		error = vm_object_inode_io_wait(inode);
		if (error != 0)
			return error;
		if (file_regular_io_lock_checkpoint != NULL)
			file_regular_io_lock_checkpoint(inode);
		mutex_lock(&inode->i_io_lock);
		if (!vm_object_inode_resize_active(inode))
			return 0;
		mutex_unlock(&inode->i_io_lock);
	}
}

/* Retakes the visible and, for a stacked write, the content inode I/O locks. */
static int
file_io_regular_locks_reacquire(
	struct file_io *io)
{
	/*
	 * Stacked writes use visible -> final ordering.  If final is busy,
	 * drop the visible mutex before waiting and retry, so a direct lower
	 * alias never forms a final -> visible cycle.  held_visible_gate,
	 * when present, reserves the visible domain while its mutex is
	 * temporarily dropped.
	 */
	for (;;) {
		/*
		 * The caller either just drained an older gate or owns the
		 * gate being prepared.  Take the mutex directly, then let the
		 * surrounding retry revalidate publication state.
		 */
		mutex_lock(&io->file->f_inode->i_io_lock);
		io->held_inode_io = 1;
		if (!io->require_content_inode_io)
			return 0;
		if (mutex_trylock(&io->content_inode->i_io_lock)) {
			io->held_content_inode_io = 1;
			return 0;
		}
		mutex_unlock(&io->file->f_inode->i_io_lock);
		io->held_inode_io = 0;

		/*
		 * Drain the current lower owner without retaining visible.
		 * Do not use vm_object_inode_io_wait here: this may be our own
		 * published CONTENT gate while prepare temporarily released
		 * the two mutexes.
		 */
		mutex_lock(&io->content_inode->i_io_lock);
		mutex_unlock(&io->content_inode->i_io_lock);
	}
}

/* Drops the inode I/O locks a transaction holds. */
static void
file_io_regular_locks_drop(
	struct file_io *io)
{
	if (io->held_content_inode_io) {
		mutex_unlock(&io->content_inode->i_io_lock);
		io->held_content_inode_io = 0;
	}
	if (io->held_inode_io) {
		mutex_unlock(&io->file->f_inode->i_io_lock);
		io->held_inode_io = 0;
	}
}

/* Runs one complete transfer as a single transaction. */
static ssize_t
file_io_once(
	struct file *file,
	enum file_io_kind kind,
	void *buffer,
	size_t length,
	off_t offset,
	unsigned internal_flags)
{
	struct file_io io;
	ssize_t result;
	int error;

	error = file_io_begin(file, kind, offset, internal_flags, &io);
	if (error != 0)
		return -error;
	result = file_io_transfer(&io, buffer, length);
	result = file_io_complete(&io, result);
	return result;
}

/* Copies and validates the fixed-width formatter request. */
static int
file_format_ioctl(
	struct file *file,
	uintptr_t argument)
{
	struct zedbsd_file_format_reserve request;
	int error;

	/* Copies the complete request before making any reservation. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Rejects unsupported versions and nonzero extension fields. */
	if (request.version != ZEDBSD_FILE_FORMAT_VERSION ||
	    request.struct_size != sizeof(request) ||
	    request.reserved[0] != 0 || request.reserved[1] != 0)
		return EINVAL;

	/* Reserves the opened object using its expected byte count. */
	error = file_format_reserve(file, request.size_bytes);
	return error;
}

/* Records complete, ordered FAT data extents without admitting holes. */
static int
file_format_collect_extent(
	uint64_t file_block,
	uint64_t disk_block,
	uint32_t count,
	void *argument)
{
	struct file_format_extents *collection;
	struct backing_claim_extent *entry;

	/* Checks the exact logical coverage before counting another extent. */
	collection = argument;
	if (count == 0 || file_block != collection->next_block ||
	    file_block > collection->blocks ||
	    count > collection->blocks - file_block)
		return EIO;

	/* Bounds the allocation count independently of file size. */
	if (collection->count == UINT32_MAX)
		return E2BIG;

	/* Fills the array on the second traversal only. */
	if (collection->entries != NULL) {
		if (collection->count >= collection->capacity)
			return EAGAIN;
		entry = &collection->entries[collection->count];
		entry->disk = collection->disk;
		entry->block = disk_block;
		entry->block_count = count;
	}

	/* Advances the expected contiguous logical position. */
	collection->count++;
	collection->next_block += count;
	return 0;
}

/* Publishes the exact physical extents protected by the prepared claim. */
static int
file_format_finalize(
	struct file *file,
	struct backing_claim *claim,
	uint64_t size)
{
	struct file_format_extents collection;
	int error;

	/* Counts complete extents while the preparing claim prevents mutation. */
	memset(&collection, 0, sizeof(collection));
	collection.disk = file->f_inode->i_mount->m_disk;
	collection.blocks = size / 512U;
	error = fat_file_extents(file, file_format_collect_extent, &collection);
	if (error != 0)
		return error;

	/* Requires the complete preallocated file rather than a sparse prefix. */
	if (collection.count == 0 || collection.next_block != collection.blocks)
		return EIO;

	/* Allocates the bounded physical extent array. */
	collection.entries = kern_calloc(
		collection.count,
		sizeof(*collection.entries));
	if (collection.entries == NULL)
		return ENOMEM;

	/* Resolves the same coverage for canonical-range publication. */
	collection.capacity = collection.count;
	collection.count = 0;
	collection.next_block = 0;
	error = fat_file_extents(file, file_format_collect_extent, &collection);
	if (error == 0 && collection.next_block != collection.blocks)
		error = EIO;

	/* Checks overlap with existing claims before making the extents active. */
	if (error == 0) {
		error = backing_claim_finalize(
			claim,
			collection.entries,
			collection.count);
	}

	/* Releases temporary extent storage on every outcome. */
	kern_free(collection.entries);
	return error;
}

/* Validates and publishes one lease while descriptor and inode I/O are held. */
static int
file_format_reserve_locked(
	struct file *file,
	uint64_t size)
{
	struct inode *inode;
	struct mount *mountp;
	struct backing_claim *claim;
	int flags;
	int error;

	/* Requires an explicit non-symlink, read/write description. */
	flags = file_status_flags_get(file);
	if ((flags & O_ACCMODE) != O_RDWR)
		return EBADF;

	/* Rejects append and truncation descriptions before claiming the file. */
	if ((flags & O_NOFOLLOW) == 0 ||
	    (flags & (O_APPEND | O_TRUNC)) != 0)
		return EINVAL;

	/* Limits the first implementation to canonical FAT-backed files. */
	inode = file->f_inode;
	mountp = inode->i_mount;
	if (mountp == NULL || mountp->m_type != &fat_filesystem_type ||
	    mountp->m_disk == NULL || file_vm_inode(file) != inode)
		return EOPNOTSUPP;

	/* Requires the sector unit used by the production FAT extent interface. */
	if (mountp->m_disk->d_block_size != 512U)
		return EOPNOTSUPP;

	/* Refuses read-only media and mounts even for a previously writable FD. */
	if ((mountp->m_flags & MOUNT_READ_ONLY) != 0 ||
	    (mountp->m_disk->d_flags & DISK_READ_ONLY) != 0)
		return EROFS;

	/* Refuses active backing objects and a second reservation on this FD. */
	if (file->f_format_claim != NULL || file->f_backing_claim != NULL ||
	    (inode->i_flags & (INODE_ROOT | INODE_DEAD |
	    INODE_SWAPFILE | INODE_LOOPFILE)) != 0)
		return EBUSY;

	/* Requires complete sectors at exactly the caller's existing size. */
	if (size == 0 || size > (uint64_t)OFF_T_MAX || size % 512U != 0)
		return EINVAL;

	/* Refuses changed identity metadata before any formatting write. */
	if (inode->i_size < 0 || (uint64_t)inode->i_size != size)
		return ESTALE;

	/* Requires VM admission support rather than granting an incomplete lease. */
	if (vm_object_backing_busy == NULL)
		return EOPNOTSUPP;

	/* Excludes competing I/O, raw aliases, and swap or loop activation. */
	error = backing_claim_prepare_inode(inode, BACKING_CLAIM_FORMAT, &claim);
	if (error != 0)
		return error;

	/* Rejects published shared mappings and retained dirty cache aliases. */
	error = vm_object_backing_busy(claim);
	if (error != 0) {
		backing_claim_release(claim);
		return error;
	}

	/* Freezes the exact physical extent set while the claim still prepares. */
	error = file_format_finalize(file, claim, size);
	if (error != 0) {
		backing_claim_release(claim);
		return error;
	}

	/* Revalidates the caller's size before publishing owner write capability. */
	if (inode->i_size < 0 || (uint64_t)inode->i_size != size) {
		backing_claim_release(claim);
		return ESTALE;
	}

	/* Refuses an inode removed before its canonical claim was acquired. */
	if ((inode->i_flags & INODE_DEAD) != 0) {
		backing_claim_release(claim);
		return ESTALE;
	}

	/* Publishes a lease whose lifetime follows the open file description. */
	file->f_format_size = size;
	file->f_format_claim = claim;
	return 0;
}

/* Drains only backend/open-file state while the description lock is held. */
static int
file_fsync_backend_locked(
	struct file *file)
{
	int error;

	/* Preserves explicit directory durability capability and backend behavior. */
	if (file->f_ops != NULL && file->f_ops->fsync != NULL)
		error = file->f_ops->fsync(file);
	else if (file->f_inode != NULL && file->f_inode->i_type == INODE_DIR)
		error = EOPNOTSUPP;
	else if (file->f_inode != NULL)
		error = inode_sync(file->f_inode);
	else
		error = 0;
	return error;
}

/* Prepares optional delayed ownership only for an ordinary direct-content write. */
static void
file_io_writeback_prepare(
	struct file_io *io,
	int flags)
{
	struct file *file;
	int error;

	/* Requires complete modules and excludes every mandatory-through operation. */
	file = io->file;
	if (!file_io_is_write(io->kind) || io->internal_flags != 0 ||
	    io->content_inode == NULL || io->content_inode != file->f_inode ||
	    (flags & (O_APPEND | O_SYNC | O_DSYNC)) != 0 ||
	    file->f_format_claim != NULL || file->f_backing_claim != NULL ||
	    writeback_mount_admit == NULL || writeback_ticket_release == NULL ||
	    vm_object_writeback_prepare == NULL || vm_object_writeback_release == NULL ||
	    vm_object_content_prepare_delayed == NULL)
		return;
	error = writeback_mount_admit(file->f_inode->i_mount, &io->writeback_ticket);
	if (error != 0)
		return;
	error = vm_object_writeback_prepare(file, &io->writeback_object);
	if (error != 0)
		writeback_ticket_release(&io->writeback_ticket);
}

/* Selects an existing cache while shared leases exclude content replacement. */
static void
file_io_cache_read_prepare(
	struct file_io *io)
{
	struct file *file;
	int error;

	/* Leaves internal and claimed backend operations on their serialized path. */
	file = io->file;
	if (file_io_is_write(io->kind) || io->internal_flags != 0 ||
	    io->content_inode == NULL || file->f_backing_claim != NULL ||
	    file->f_format_claim != NULL || vm_object_cache_pin == NULL ||
	    vm_object_cache_unpin == NULL || vm_object_read_coherent == NULL ||
	    vm_object_cache_published == NULL ||
	    vm_object_content_read_begin == NULL || vm_object_content_read_end == NULL)
		return;

	/* Protects stacked identity before taking the final inode's shared lease. */
	if (io->content_inode != file->f_inode) {
		error = vm_object_content_read_begin(file->f_inode);
		if (error != 0)
			return;
		io->held_visible_gate = 1;
	}
	error = vm_object_content_read_begin(io->content_inode);
	if (error == 0) {
		error = vm_object_cache_pin(io->content_inode, &io->read_object);
		if (error == 0) {
			io->held_content_read = 1;
			io->coherent_read = 1;
			return;
		}
		vm_object_content_read_end(io->content_inode);
	}

	/* Releases our gates before falling back to any blocking inode acquisition. */
	if (io->held_visible_gate) {
		vm_object_content_read_end(file->f_inode);
		io->held_visible_gate = 0;
	}
}

/* Returns optional ownership only after the caller has released every I/O lease. */
static void
file_io_resources_release(
	struct file_io *io)
{
	/* Drops the read identity only after the outer transaction releases its locks. */
	if (io->read_object != NULL) {
		vm_object_cache_unpin(io->read_object);
		io->read_object = NULL;
	}

	/* Ends VM ownership while the live ticket still protects its physical budget. */
	if (io->writeback_object != NULL) {
		vm_object_writeback_release(io->writeback_object);
		io->writeback_object = NULL;
	}
	if (io->writeback_ticket.budget != NULL) {
		if (writeback_pressure != NULL)
			writeback_pressure(io->writeback_ticket.budget);
		writeback_ticket_release(&io->writeback_ticket);
	}

	/* Balances priority on successful completion and every failed begin path. */
	if (io->readahead_demand) {
		readahead_demand_end();
		io->readahead_demand = 0;
	}
}

/* Resets under the description lock, or under exclusive final-reference ownership. */
static void
file_readahead_reset_owned(
	struct file *file)
{
	/* Cancels identities before final pool reuse without waiting for running I/O. */
	if (readahead_reset != NULL)
		readahead_reset(&file->f_readahead);
	if (readahead_cancel != NULL)
		readahead_cancel(file);
}

/* Predicts after demand completion without allowing a prior seek/close to restart work. */
static void
file_readahead_completed(
	struct file *file,
	struct inode *inode,
	off_t start,
	ssize_t result,
	off_t eof,
	uint64_t generation,
	uint64_t useful)
{
	struct readahead_request request;
	struct cache_memory_stats memory;
	uint64_t previous;
	int pressure;
	int error;

	/* Keeps optional services absent from reduced kernels semantically harmless. */
	if (readahead_observe == NULL || readahead_submit == NULL)
		return;
	pressure = 0;
	if (cache_memory_get_stats != NULL) {
		cache_memory_get_stats(&memory);
		pressure = memory.resizing || memory.free_bytes <= memory.reserve_bytes ||
		    memory.resident_bytes >= memory.target_bytes;
	}

	/* Skips contended optional state without waiting behind a newer transaction. */
	if (!mutex_trylock(&file->f_lock))
		return;

	/* Rejects an observation invalidated after its transaction began. */
	if (file->f_readahead.generation != generation) {
		mutex_unlock(&file->f_lock);
		return;
	}
	if (result <= 0 || start < 0 || eof < 0) {
		file_readahead_reset_owned(file);
		mutex_unlock(&file->f_lock);
		return;
	}
	previous = file->f_readahead.generation;
	error = readahead_observe(&file->f_readahead, (uint64_t)start,
	    (size_t)result, (uint64_t)eof, useful, pressure, &request);
	if (file->f_readahead.generation != previous && readahead_cancel != NULL)
		readahead_cancel(file);
	mutex_unlock(&file->f_lock);

	/* Queue refusal never changes the already completed demand result. */
	if (error == 0 && request.length != 0)
		(void)readahead_submit(file, inode, &request);
}
