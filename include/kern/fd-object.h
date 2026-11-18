/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shared ownership carried by descriptor slots and ancillary socket messages.
 */

#ifndef KERN_KERN_FD_OBJECT_H
#define KERN_KERN_FD_OBJECT_H

struct file;
struct kernel_handle;

/* A zeroed reference owns nothing, including after its ownership is moved. */
enum fd_object_type {
	FD_OBJECT_NONE,
	FD_OBJECT_FILE,
	FD_OBJECT_HANDLE,
};

/*
 * One owned reference to either an open file description or a kernel object.
 * Copying this record alone does not acquire ownership; get does. Successful
 * descriptor installation consumes ownership, and put clears before destruction.
 */
struct fd_object {
	enum fd_object_type type;
	union {
		struct file *file;
		struct kernel_handle *handle;
	} data;
};

int fd_object_valid(const struct fd_object *object);
void fd_object_get(const struct fd_object *object);
int fd_object_put(struct fd_object *object);
void fd_object_clear(struct fd_object *object);

#endif
