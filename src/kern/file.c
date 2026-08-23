/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/file.h"
#include "kern/namei.h"
#include "kern/cred.h"
#include "kern/vm-object.h"

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

extern int vm_object_inode_io_wait(struct inode *) __attribute__((weak));
extern int vm_object_inode_resize_active(struct inode *)
    __attribute__((weak));
extern int vm_object_resize_begin(struct inode *, off_t,
    struct vm_object_resize *) __attribute__((weak));
extern int vm_object_resize_prepare(struct vm_object_resize *)
    __attribute__((weak));
extern void vm_object_resize_commit(struct vm_object_resize *, off_t)
    __attribute__((weak));
extern void vm_object_resize_abort(struct vm_object_resize *)
    __attribute__((weak));
extern int vm_object_content_begin(struct file *, off_t, size_t,
    struct vm_object_resize *, struct vm_object_content *)
    __attribute__((weak));
extern int vm_object_content_prepare(struct vm_object_content *)
    __attribute__((weak));
extern void vm_object_content_commit(struct vm_object_content *, const void *,
    size_t) __attribute__((weak));
extern void vm_object_content_abort(struct vm_object_content *)
    __attribute__((weak));
extern int vm_object_read_coherent(struct inode *, off_t, void *, size_t,
    ssize_t *) __attribute__((weak));
extern int vm_object_content_read_begin(struct inode *) __attribute__((weak));
extern void vm_object_content_read_end(struct inode *) __attribute__((weak));
extern int vm_object_cache_published(struct inode *) __attribute__((weak));
extern void file_regular_io_lock_checkpoint(struct inode *)
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
		       O_DIRECTORY | O_NONBLOCK | O_NOCTTY | O_NOFOLLOW)) != 0 ||
	    (flags & O_ACCMODE) > O_RDWR ||
	    ((flags & O_EXCL) != 0 && (flags & O_CREAT) == 0))
		return EINVAL;
	/* Reserve the system-wide open-file object before pathname operations
	 * which may create or truncate an inode.  ENFILE must not leave a
	 * namespace or data side effect behind. */
	file = file_alloc();
	if (file == NULL)
		return ENFILE;
	error = namei_path_flags_at(context, path,
	    (flags & O_NOFOLLOW) != 0 ? NAMEI_NOFOLLOW_FINAL : 0, &found);
	if (error == ENOENT &&
	    ((flags & O_ACCMODE) != O_RDONLY ||
	     (flags & (O_CREAT | O_TRUNC | O_APPEND)) != 0)) {
		struct path parent;
		struct inode *collision;
		struct componentname last;
		char storage[NAME_MAX + 1U];
		error = namei_parent_path_at(context, path, &parent, &last, storage);
		if (error != 0)
			goto fail_file;
		mount_vfs_transaction_enter(parent.p_mount);
		if (cred != NULL &&
		    (error = vfs_access(parent.p_inode, cred, W_OK | X_OK)) != 0) {
			mount_vfs_transaction_leave(parent.p_mount);
			path_release(&parent);
			goto fail_file;
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
		error = inode_create(parent.p_inode, &last, mode, &inode);
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
	inode = found.p_inode;
	if ((flags & O_NOFOLLOW) != 0 && inode->i_type == INODE_SYMLINK) {
		path_release(&found);
		error = ELOOP;
		goto fail_file;
	}
	if (cred != NULL) {
		int requested = 0;
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
	if ((flags & O_TRUNC) && inode->i_type == INODE_REG &&
	    (flags & O_ACCMODE) != O_RDONLY) {
		/* Truncation and set-id removal are one inode content transaction.
		 * A metadata failure must fail open before the backend is changed. */
		error = inode_truncate_limited_cred(inode, 0, UINT64_MAX, cred,
		    NULL);
		if (error != 0) {
			path_release(&found);
			goto fail_file;
		}
	}
	file->f_path = found;
	file->f_inode = inode;
	file->f_vm_inode = inode;
	file->f_ops = inode->i_fop;
	atomic_store_release(&file->f_flags, (unsigned)flags);
	/* O_APPEND controls each write operation; it does not change the
	 * initial open-file-description offset. */
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
	    (flags & ~(O_ACCMODE | O_APPEND | O_DIRECTORY | O_NONBLOCK |
	    O_NOCTTY | O_NOFOLLOW)) != 0 ||
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
	atomic_store_release(&file->f_flags, (unsigned)flags);
	file->f_data = data;
	*result = file;
	return 0;
}

int
file_ioctl(struct file *file, unsigned long request, uintptr_t argument)
{
	if (file == NULL)
		return EBADF;
	if (file->f_ops == NULL || file->f_ops->ioctl == NULL)
		return EOPNOTSUPP;
	/* ioctl backends synchronize their own state.  A blocking ioctl must not
	 * exclude read/write on a full-duplex descriptor. */
	return file->f_ops->ioctl(file, request, argument);
}

void
file_status_flags_update(struct file *file, int mask, int value)
{
	unsigned old, updated;

	if (file == NULL)
		return;
	old = atomic_load_acquire(&file->f_flags);
	do {
		updated = (old & ~(unsigned)mask) |
		    ((unsigned)value & (unsigned)mask);
	} while (!atomic_compare_exchange(&file->f_flags, &old, updated));
}

static int
file_io_is_positional(enum file_io_kind kind)
{
	return kind == FILE_IO_PREAD || kind == FILE_IO_PWRITE;
}

static int
file_io_is_write(enum file_io_kind kind)
{
	return kind == FILE_IO_WRITE || kind == FILE_IO_PWRITE;
}

static int
file_vm_resize_available(void)
{
	return vm_object_inode_io_wait != NULL &&
	    vm_object_inode_resize_active != NULL &&
	    vm_object_resize_begin != NULL &&
	    vm_object_resize_prepare != NULL &&
	    vm_object_resize_commit != NULL &&
	    vm_object_resize_abort != NULL;
}

static int
file_vm_content_available(void)
{
	return file_vm_resize_available() &&
	    vm_object_content_begin != NULL &&
	    vm_object_content_prepare != NULL &&
	    vm_object_content_commit != NULL &&
	    vm_object_content_abort != NULL &&
	    vm_object_content_read_begin != NULL &&
	    vm_object_content_read_end != NULL;
}

/* Take i_io only after an older EOF transaction has left its publication
 * gate, then recheck the gate to close the wait/lock race. */
static int
file_regular_io_lock(struct inode *inode, unsigned internal_flags)
{
	int error;

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

/* Stacked writes use visible -> final ordering.  If final is busy, drop the
 * visible mutex before waiting and retry, so a direct lower alias never forms
 * a final -> visible cycle.  held_visible_gate, when present, reserves the
 * visible domain while its mutex is temporarily dropped. */
static int
file_io_regular_locks_reacquire(struct file_io *io)
{
	for (;;) {
		/* The caller either just drained an older gate or owns the gate being
		 * prepared.  Take the mutex directly, then let the surrounding retry
		 * revalidate publication state. */
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
		/* Drain the current lower owner without retaining visible.  Do not use
		 * vm_object_inode_io_wait here: this may be our own published CONTENT
		 * gate while prepare temporarily released the two mutexes. */
		mutex_lock(&io->content_inode->i_io_lock);
		mutex_unlock(&io->content_inode->i_io_lock);
	}
}

static void
file_io_regular_locks_drop(struct file_io *io)
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

int
file_content_lease_begin(struct file *file, struct file_content_lease *lease)
{
	struct inode *content_inode;
	uint64_t content_size;
	size_t content_length;
	int error, flags, visible_gate = 0;

	if (file == NULL || lease == NULL)
		return EINVAL;
	memset(lease, 0, sizeof(*lease));
	content_inode = file_vm_inode(file);
	if (file->f_inode == NULL || file->f_inode->i_type != INODE_REG ||
	    content_inode == NULL || content_inode->i_type != INODE_REG ||
	    file->f_ops == NULL || file->f_ops->pread == NULL ||
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
	/* Even an empty image needs a published gate: otherwise a concurrent
	 * grow could turn the sequence of short reads into a different image. */
	content_length = content_size == 0 ? 1U : (size_t)content_size;
	file_ref(file);

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
			/* Stacked I/O normally takes outer then lower.  Never sleep on
			 * the lower mutex while retaining outer: drain the current lower
			 * owner without outer, then restart both gate checks. */
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

	/* Prepare can perform old-dirty writeback.  CONTENT remains published
	 * while i_io is dropped, preventing a normal reader/writer from entering
	 * the backend and preventing new object faults from publishing PTEs. */
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

ssize_t
file_content_lease_pread(struct file_content_lease *lease, void *buffer,
	size_t length, off_t offset)
{
	ssize_t count;

	if (lease == NULL || !lease->active || lease->file == NULL ||
	    offset < 0 || (buffer == NULL && length != 0))
		return -EINVAL;
	if (offset >= lease->size || length == 0)
		return 0;
	if ((uint64_t)length > (uint64_t)(lease->size - offset))
		length = (size_t)(lease->size - offset);
	count = file_pread_internal(lease->file, buffer, length, offset,
	    FILE_IO_VM_OBJECT | FILE_IO_INODE_IO_OWNED);
	if (count > (ssize_t)length)
		return -EIO;
	if (count > 0)
		lease->transferred = 1;
	return count;
}

void
file_content_lease_end(struct file_content_lease *lease)
{
	struct file *file;

	if (lease == NULL || !lease->active || lease->file == NULL)
		return;
	file = lease->file;
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

int
file_io_begin_cred(struct file *file, enum file_io_kind kind, off_t offset,
	unsigned internal_flags, const struct ucred *credential,
	struct file_io *io)
{
	int flags, writing, positional;

	if (file == NULL || io == NULL || kind < FILE_IO_READ ||
	    kind > FILE_IO_PWRITE ||
	    (internal_flags & ~(FILE_IO_LOOP_BACKING | FILE_IO_VM_OBJECT |
	    FILE_IO_INODE_IO_OWNED | FILE_IO_CONTENT_CHANGE)) != 0 ||
	    ((internal_flags & FILE_IO_INODE_IO_OWNED) != 0 &&
	    (internal_flags & FILE_IO_VM_OBJECT) == 0))
		return EINVAL;
	positional = file_io_is_positional(kind);
	writing = file_io_is_write(kind);
	if (!writing && (internal_flags & FILE_IO_CONTENT_CHANGE) != 0)
		return EINVAL;
	if (positional && offset < 0)
		return EINVAL;
	flags = file_status_flags_get(file);
	if (writing ? ((flags & O_ACCMODE) == O_RDONLY) :
	    ((flags & O_ACCMODE) == O_WRONLY))
		return EBADF;
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

	memset(io, 0, sizeof(*io));
	io->file = file;
	io->content_inode = file->f_inode != NULL &&
	    file->f_inode->i_type == INODE_REG ? file_vm_inode(file) : NULL;
	if (file->f_inode != NULL && file->f_inode->i_type == INODE_REG &&
	    (io->content_inode == NULL ||
	     io->content_inode->i_type != INODE_REG)) {
		memset(io, 0, sizeof(*io));
		return EIO;
	}
	io->credential = credential;
	io->kind = kind;
	io->offset = offset;
	io->internal_flags = internal_flags;
	io->append_requested = !positional && writing &&
	    (flags & O_APPEND) != 0;
	/* Only regular files and block devices use the generic shared position.
	 * Stream and device backends synchronize their queues independently. */
	if (!positional && file->f_inode != NULL &&
	    (file->f_inode->i_type == INODE_REG ||
	     file->f_inode->i_type == INODE_BLOCK)) {
		mutex_lock(&file->f_lock);
		io->held_position = 1;
	}
	if (file->f_inode != NULL && file->f_inode->i_type == INODE_REG &&
	    (internal_flags & FILE_IO_INODE_IO_OWNED) == 0) {
		int error = file_regular_io_lock(file->f_inode, internal_flags);

		if (error != 0) {
			if (io->held_position)
				mutex_unlock(&file->f_lock);
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
					memset(io, 0, sizeof(*io));
					return error;
				}
			}
		}
	}
	if (!positional) {
		io->offset = io->held_position ? file->f_offset : 0;
		if (io->append_requested && io->content_inode != NULL)
			io->offset = io->content_inode->i_size;
	}
	return 0;
}

int
file_io_begin(struct file *file, enum file_io_kind kind, off_t offset,
	unsigned internal_flags, struct file_io *io)
{
	return file_io_begin_cred(file, kind, offset, internal_flags, NULL, io);
}

void
file_io_set_growth_limit(struct file_io *io, uint64_t limit)
{
	if (io == NULL || io->file == NULL)
		return;
	io->growth_limit = limit;
	io->growth_limit_enabled = limit != UINT64_MAX;
}

int
file_io_take_growth_limit_hit(struct file_io *io)
{
	int hit;

	if (io == NULL)
		return 0;
	hit = io->growth_limit_hit != 0;
	io->growth_limit_hit = 0;
	return hit;
}

ssize_t
file_io_transfer(struct file_io *io, void *buffer, size_t length)
{
	struct file *file;
	struct vm_object_resize resize;
	struct vm_object_content content;
	off_t write_start = 0;
	uint64_t limit_existing = 0;
	size_t requested_length = length;
	int resize_error, content_error;
	ssize_t result;

	if (io == NULL || io->file == NULL || (buffer == NULL && length != 0))
		return -EINVAL;
	file = io->file;
	memset(&resize, 0, sizeof(resize));
	memset(&content, 0, sizeof(content));

	/* A published MAP_SHARED object is the read source of truth.  Once selected,
	 * its inode read lease stays in struct file_io until file_io_end(), covering
	 * every syscall copy/iovec chunk.  Faulting a cache miss performs internal
	 * backend I/O, so the outer i_io mutex is dropped only while the lease keeps
	 * normal writers and EOF changes excluded. */
	if (length != 0 && !file_io_is_write(io->kind) && io->held_inode_io &&
	    (io->internal_flags & FILE_IO_VM_OBJECT) == 0 &&
	    vm_object_read_coherent != NULL &&
	    vm_object_content_read_begin != NULL &&
	    vm_object_content_read_end != NULL &&
	    vm_object_cache_published != NULL) {
		ssize_t cached = 0;
		int cache_published;

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
			/* With no published cache, the same final-inode read lease still
			 * excludes direct lower-layer writers across every backend chunk. */
			io->coherent_read = cache_published != 0;
		}
		if (!io->coherent_read)
			goto backend_transfer;
		mutex_unlock(&file->f_inode->i_io_lock);
		io->held_inode_io = 0;
		content_error = vm_object_read_coherent(io->content_inode, io->offset,
		    buffer, length, &cached);
		mutex_lock(&file->f_inode->i_io_lock);
		io->held_inode_io = 1;
		if (content_error == ENOENT) {
			/* Final-mapping teardown flushed the old cache before removing it.
			 * Continue the same leased read through the stable backend. */
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
	length = requested_length;
	io->growth_limit_hit = 0;
	memset(&resize, 0, sizeof(resize));
	memset(&content, 0, sizeof(content));
	/* This is a tentative EOF until resize/content begin publishes the final
	 * inode gate.  Any older generic writer makes begin return BUSY, and the
	 * retry samples its committed EOF before choosing the append range again. */
	if (io->append_requested && !io->append_positioned &&
	    io->content_inode != NULL)
		io->offset = io->content_inode->i_size;
	if (length != 0 && file_io_is_write(io->kind) &&
	    io->growth_limit_enabled && io->content_inode != NULL) {
		uint64_t maximum_end, remaining;

		limit_existing = io->content_inode->i_size > 0 ?
		    (uint64_t)io->content_inode->i_size : 0;
		maximum_end = limit_existing > io->growth_limit ?
		    limit_existing : io->growth_limit;
		if (io->offset < 0 || (uint64_t)io->offset >= maximum_end) {
			io->growth_limit_hit = 1;
			return -EFBIG;
		}
		remaining = maximum_end - (uint64_t)io->offset;
		if (remaining < length) {
			length = remaining > SIZE_MAX ? SIZE_MAX : (size_t)remaining;
			io->growth_limit_hit = 1;
		}
		if (length == 0)
			return -EFBIG;
	} else if (io->content_inode != NULL) {
		limit_existing = io->content_inode->i_size > 0 ?
		    (uint64_t)io->content_inode->i_size : 0;
	}
	if (length != 0 && file_io_is_write(io->kind) &&
	    io->held_inode_io &&
	    (io->internal_flags & FILE_IO_VM_OBJECT) == 0 &&
	    file_vm_resize_available()) {
		uint64_t end;

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
	/* Same-EOF writes need the same gate as extending writes.  Prepare first
	 * revokes writable PTEs and flushes the old dirty image.  Resident pages
	 * stay BUSY (pins are not orphaned) until the backend result is known. */
	if (length != 0 && file_io_is_write(io->kind) &&
	    io->held_inode_io &&
	    (io->internal_flags & FILE_IO_VM_OBJECT) == 0 &&
	    file_vm_content_available()) {
		content_error = vm_object_content_begin(file, io->offset, length,
		    resize.active ? &resize : NULL, &content);
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
		/* The published gate closes the sample/begin window.  An older writer
		 * can finish between those operations; retry rather than committing a
		 * stale append range or RLIMIT_FSIZE decision. */
		if ((io->growth_limit_enabled ||
		    (io->append_requested && !io->append_positioned)) &&
		    (uint64_t)(io->content_inode->i_size > 0 ?
		    io->content_inode->i_size : 0) != limit_existing) {
			vm_object_content_abort(&content);
			if (resize.active)
				vm_object_resize_abort(&resize);
			goto transaction_retry;
		}
		file_io_regular_locks_drop(io);
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
	/* Clear set-user-ID/set-group-ID before the first externally requested
	 * regular-file backend mutation.  This is inside the same i_io/content
	 * transaction as the data write, so exec cannot observe new bytes with
	 * stale privilege metadata.  Failure leaves the backend untouched. */
	if (length != 0 && file_io_is_write(io->kind) &&
	    file->f_inode != NULL && file->f_inode->i_type == INODE_REG &&
	    !io->setid_prepared &&
	    file->f_inode == file_vm_inode(file) &&
	    (io->credential != NULL ||
	    (io->internal_flags & FILE_IO_CONTENT_CHANGE) != 0)) {
		content_error = io->credential != NULL ?
		    vfs_clear_setid_on_write(file->f_inode, io->credential) :
		    vfs_clear_setid_on_content_change(file->f_inode);
		if (content_error != 0) {
			if (content.active)
				vm_object_content_abort(&content);
			if (resize.active)
				vm_object_resize_abort(&resize);
			return -content_error;
		}
		io->setid_prepared = 1;
	}
	switch (io->kind) {
	case FILE_IO_READ:
		if (io->held_position)
			file->f_offset = io->offset;
		result = file->f_ops->read(file, buffer, length);
		if (result > 0 && io->held_position)
			io->offset = file->f_offset;
		break;
	case FILE_IO_WRITE: {
		unsigned forward_flags = io->internal_flags;

		if (io->held_content_inode_io ||
		    (io->internal_flags & FILE_IO_INODE_IO_OWNED) != 0)
			forward_flags |= FILE_IO_VM_OBJECT | FILE_IO_INODE_IO_OWNED;
		/* A stacking backend must receive the originating credential and
		 * content-change marker.  The outer file_io owns f_offset/O_APPEND,
		 * so its positional internal callback is also the canonical forwarding
		 * path for ordinary write/writev. */
		if ((io->credential != NULL || forward_flags != 0) &&
		    file->f_ops->pwrite_internal != NULL) {
			result = file->f_ops->pwrite_internal(file, buffer, length,
			    io->offset, forward_flags, io->credential);
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
	}
	case FILE_IO_PREAD:
		result = io->internal_flags != 0 &&
		    file->f_ops->pread_internal != NULL ?
		    file->f_ops->pread_internal(file, buffer, length, io->offset,
		    io->internal_flags) :
		    file->f_ops->pread(file, buffer, length, io->offset);
		if (result > 0)
			io->offset += result;
		break;
	case FILE_IO_PWRITE: {
		unsigned forward_flags = io->internal_flags;

		if (io->held_content_inode_io ||
		    (io->internal_flags & FILE_IO_INODE_IO_OWNED) != 0)
			forward_flags |= FILE_IO_VM_OBJECT | FILE_IO_INODE_IO_OWNED;
		result = (forward_flags != 0 || io->credential != NULL) &&
		    file->f_ops->pwrite_internal != NULL ?
		    file->f_ops->pwrite_internal(file, buffer, length, io->offset,
		    forward_flags, io->credential) :
		    file->f_ops->pwrite(file, buffer, length, io->offset);
		if (result > 0)
			io->offset += result;
		break;
	}
	default:
		result = -EINVAL;
		break;
	}
	if (content.active) {
		if (result > 0)
			vm_object_content_commit(&content, buffer, (size_t)result);
		else
			vm_object_content_abort(&content);
	}
	if (resize.active) {
		if (result > 0) {
			off_t actual_end = write_start + result;

			if (actual_end > io->content_inode->i_size)
				io->content_inode->i_size = actual_end;
			vm_object_resize_commit(&resize, io->content_inode->i_size);
		} else {
			io->content_inode->i_size = resize.old_size;
			vm_object_resize_abort(&resize);
		}
	}
	if (result > 0)
		io->transferred = 1;
	return result;
}

void
file_io_end(struct file_io *io)
{
	struct file *file;

	if (io == NULL || io->file == NULL)
		return;
	file = io->file;
	if (!file_io_is_positional(io->kind) && io->held_position &&
	    (!io->append_requested || io->transferred))
		file->f_offset = io->offset;
	if (io->transferred && file->f_inode != NULL)
		inode_touch(file->f_inode, file_io_is_write(io->kind) ?
		    INODE_ATTR_MTIME | INODE_ATTR_CTIME : INODE_ATTR_ATIME);
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
	memset(io, 0, sizeof(*io));
}

static ssize_t
file_io_once(struct file *file, enum file_io_kind kind, void *buffer,
	size_t length, off_t offset, unsigned internal_flags)
{
	struct file_io io;
	ssize_t result;
	int error = file_io_begin(file, kind, offset, internal_flags, &io);
	if (error != 0)
		return -error;
	result = file_io_transfer(&io, buffer, length);
	file_io_end(&io);
	return result;
}

ssize_t
file_read(struct file *file, void *buffer, size_t length)
{
	return file_io_once(file, FILE_IO_READ, buffer, length, 0, 0);
}

ssize_t
file_pread(struct file *file, void *buffer, size_t length, off_t offset)
{
	return file_pread_internal(file, buffer, length, offset, 0);
}

ssize_t
file_pread_internal(struct file *file, void *buffer, size_t length,
	off_t offset, unsigned internal_flags)
{
	return file_io_once(file, FILE_IO_PREAD, buffer, length, offset,
	    internal_flags);
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
	return file_pwrite_internal_cred(file, buffer, length, offset,
	    internal_flags, NULL);
}

ssize_t
file_pwrite_internal_cred(struct file *file, const void *buffer, size_t length,
			  off_t offset, unsigned internal_flags,
			  const struct ucred *credential)
{
	struct file_io io;
	ssize_t result;
	int error = file_io_begin_cred(file, FILE_IO_PWRITE, offset,
	    internal_flags, credential, &io);

	if (error != 0)
		return -error;
	result = file_io_transfer(&io, (void *)buffer, length);
	file_io_end(&io);
	return result;
}

ssize_t
file_write(struct file *file, const void *buffer, size_t length)
{
	return file_io_once(file, FILE_IO_WRITE, (void *)buffer, length, 0, 0);
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
