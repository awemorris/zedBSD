/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Typed kernel objects whose lifetime can be carried by ordinary descriptors.
 */

#ifndef KERN_KERN_HANDLE_H
#define KERN_KERN_HANDLE_H

#include <kern/atomic.h>
#include <stdint.h>

/* Driver implementations own payload identity; generic descriptors only classify the wrapper. */
enum kernel_handle_type {
	KERNEL_HANDLE_DRIVER = 1,
};

/* Final destruction belongs to the subsystem that supplied the payload. */
struct kernel_handle_ops {
	void (*release)(void *object);
	int (*poll)(void *object, short events, short *revents);
};

/*
 * One immutable typed payload, retained by descriptors, messages and kernel work.
 * handle_create allocates this standalone wrapper; final put releases the payload
 * before freeing the wrapper. Callers never allocate or embed it themselves.
 */
struct kernel_handle {
	refcount_t refcnt;
	uint32_t type;
	const struct kernel_handle_ops *ops;
	void *object;
};

/* Success owns the payload and returns one reference; failure owns nothing. */
int handle_create(uint32_t type, const struct kernel_handle_ops *ops, void *object, struct kernel_handle **result);
void handle_get(struct kernel_handle *handle);
void handle_put(struct kernel_handle *handle);

/*
 * Create returns a descriptor or negative errno and retains a new reference on
 * success. The caller keeps its input reference on every result. Only O_CLOEXEC
 * and O_CLOFORK are accepted. Get returns a strong reference of the exact type,
 * or NULL. Both operations address the current thread's process descriptor table.
 */
int handle_fd_create(struct kernel_handle *handle, int flags);
struct kernel_handle *handle_fd_get(int descriptor, uint32_t expected_type);

#endif
