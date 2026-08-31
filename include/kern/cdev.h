/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Character device
 */

#ifndef ZEDBSD_KERN_CDEV_H
#define ZEDBSD_KERN_CDEV_H

#include <kern/atomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define CDEV_MAX	16U

struct file;
struct file_ops;

typedef void (*cdev_finalizer_t)(void *);

struct cdev_ops {
	int (*open)(struct file *);
	int (*close)(struct file *);
	ssize_t (*read)(struct file *, void *, size_t);
	ssize_t (*write)(struct file *, const void *,size_t);
	int (*ioctl)(struct file *, unsigned long, uintptr_t);
	int (*poll)(struct file *, short, short *);
};

struct cdev {
	char name[32];
	dev_t rdev;
	const struct cdev_ops *ops;
	void *data;
	cdev_finalizer_t finalizer;
	refcount_t refs;
	uint64_t generation;
	atomic_uint_t published;
};

void
cdev_reset(void);

int
cdev_register(
	const char *name,
	dev_t rdev,
	const struct cdev_ops *ops,
	void *data);

/*
 * Publishes one managed, immutable character-device generation.
 *
 * The returned reference belongs to the caller.  The registry owns a
 * separate reference until cdev_unregister() or cdev_reset() unpublishes the
 * generation.  The finalizer runs after both those references and every
 * devfs inode reference have been released.  A failed registration does not
 * consume data and does not call the finalizer.
 */
int
cdev_register_managed(
	const char *name,
	dev_t rdev,
	const struct cdev_ops *ops,
	void *data,
	cdev_finalizer_t finalizer,
	struct cdev **result);

int
cdev_unregister(
	struct cdev *device);

void
cdev_ref(
	struct cdev *device);

void
cdev_release(
	struct cdev *device);

int
cdev_is_published(
	const struct cdev *device);

uint64_t
cdev_generation(
	const struct cdev *device);

struct cdev *
cdev_find_ref(
	const char *name);

unsigned
cdev_snapshot(
	struct cdev **snapshot,
	unsigned capacity);

unsigned
cdev_count(void);

extern const struct file_ops cdev_file_ops;

#endif
