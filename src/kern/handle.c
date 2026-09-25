/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Typed non-VFS objects retained by kernel work and process descriptors.
 */

#include <kern/handle.h>
#include <kern/fd-object.h>
#include <kern/filedesc.h>
#include <kern/kmem.h>
#include <kern/process.h>
#include <kern/thread.h>
#include <uapi/fcntl.h>
#include <uapi/errno.h>
#include <stddef.h>

/*
 * Allocates a standalone handle and takes payload ownership on success.
 */
int
handle_create(
	uint32_t type,
	const struct kernel_handle_ops *ops,
	void *object,
	struct kernel_handle **result)
{
	struct kernel_handle *handle;

	/* A failed creation never publishes an uninitialized wrapper. */
	if (result == NULL)
		return EINVAL;

	/* Every accepted payload has an explicit type and final destructor. */
	*result = NULL;
	if (type == 0 || ops == NULL)
		return EINVAL;

	/* Payload destruction is mandatory even when the payload pointer is NULL. */
	if (ops->release == NULL)
		return EINVAL;

	/* The wrapper has no file, inode, path or global file-pool allocation. */
	handle = kern_malloc(sizeof(*handle));
	if (handle == NULL)
		return ENOMEM;

	/* The initial reference becomes the caller's ownership of the payload. */
	refcount_init(&handle->refcnt, 1);
	handle->type = type;
	handle->ops = ops;
	handle->object = object;
	*result = handle;

	/* Succeeded: the caller owns one independent handle reference. */
	return 0;
}

/*
 * Retains a handle protected by an existing reference or descriptor-table lock.
 */
void
handle_get(
	struct kernel_handle *handle)
{
	/* A missing handle contributes no ownership. */
	if (handle == NULL)
		return;

	/* The kernel's reference policy rejects overflow and resurrection from zero. */
	refcount_get(&handle->refcnt);

	/* Succeeded: the caller owns an additional handle reference. */
	return;
}

/*
 * Releases a handle and destroys its payload after the last reference.
 */
void
handle_put(
	struct kernel_handle *handle)
{
	int last;

	/* A missing handle needs no cleanup. */
	if (handle == NULL)
		return;

	/* Only the final owner may invoke the subsystem destructor. */
	last = refcount_put(&handle->refcnt);
	if (!last)
		return;

	/* Callers drop descriptor and socket locks before reaching this callback. */
	handle->ops->release(handle->object);
	kern_free(handle);

	/* Succeeded: the final payload and its standalone wrapper are retired. */
	return;
}

/*
 * Installs an additional handle reference in the current process descriptor table.
 */
int
handle_fd_create(
	struct kernel_handle *handle,
	int flags)
{
	struct thread *thread;
	struct fd_object object;
	unsigned descriptor_flags;
	int descriptor;
	int error;

	/* Only lifetime flags apply to this non-I/O descriptor. */
	if (handle == NULL || (flags & ~(O_CLOEXEC | O_CLOFORK)) != 0)
		return -EINVAL;

	/* Kernel threads without a process cannot publish a user descriptor. */
	thread = thread_current();
	if (thread == NULL || thread->proc == NULL)
		return -EBADF;

	/* Translate public close flags into the descriptor table's representation. */
	descriptor_flags = 0;
	if ((flags & O_CLOEXEC) != 0)
		descriptor_flags |= FILEDESC_CLOEXEC;

	/* Close-on-fork is independent from close-on-exec. */
	if ((flags & O_CLOFORK) != 0)
		descriptor_flags |= FILEDESC_CLOFORK;

	/* The new slot owns a separate reference; the caller always keeps its own. */
	object.type = FD_OBJECT_HANDLE;
	object.data.handle = handle;
	handle_get(handle);
	error = filedesc_install_object_from(thread->proc->fd, &object, descriptor_flags, 0, &descriptor);
	if (error != 0) {
		handle_put(handle);
		return -error;
	}

	/* Succeeded: the descriptor carries the newly retained reference. */
	return descriptor;
}

/*
 * Retains the exact handle type carried by a current-process descriptor.
 */
struct kernel_handle *
handle_fd_get(
	int descriptor,
	uint32_t expected_type)
{
	struct thread *thread;
	struct fd_object object;
	int error;

	/* A kernel thread without a process owns no user descriptor namespace. */
	thread = thread_current();
	if (thread == NULL || thread->proc == NULL)
		return NULL;

	/* Lookup takes a strong reference before concurrent close can detach it. */
	error = filedesc_get_object_ref(thread->proc->fd, descriptor, &object);
	if (error != 0)
		return NULL;

	/* A file descriptor cannot be reinterpreted as an arbitrary kernel pointer. */
	if (object.type != FD_OBJECT_HANDLE) {
		(void)fd_object_put(&object);
		return NULL;
	}

	/* Immutable type identity remains stable throughout the acquired reference. */
	if (object.data.handle->type != expected_type) {
		(void)fd_object_put(&object);
		return NULL;
	}

	/* Succeeded: the caller owns the acquired handle reference. */
	return object.data.handle;
}
