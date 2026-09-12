/* -*- mode: c; c-file-style: "bsd"; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Character device
 */

#ifndef KERN_KERN_CDEV_H
#define KERN_KERN_CDEV_H

#include <kern/atomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct file;
struct file_ops;

/* Releases driver data after the last reference on its cdev generation. */
typedef void (*cdev_finalizer_t)(void *);

/* Dispatches open file operations for every device using one driver. */
struct cdev_ops {
	int (*open)(struct file *);
	int (*close)(struct file *);
	ssize_t (*read)(struct file *, void *, size_t);
	ssize_t (*write)(struct file *, const void *,size_t);
	int (*ioctl)(struct file *, unsigned long, uintptr_t);
	int (*poll)(struct file *, short, short *);
};

/*
 * One immutable device generation retained by registry, driver and inodes.
 * registry_next belongs exclusively to the registry and its spinlock.
 */
struct cdev {
	struct cdev *registry_next;
	char name[32];
	dev_t rdev;
	const struct cdev_ops *ops;
	void *data;
	cdev_finalizer_t finalizer;
	refcount_t refs;
	uint64_t generation;
	atomic_uint_t published;
};

/* Explicitly unpublishes all devices; boot initialization does not call it. */
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

/*
 * Allocates a complete coherent snapshot and retains every returned device.
 * The caller releases each device and frees the array with kern_free().
 * An empty snapshot succeeds with a NULL array and zero count.
 */
int
cdev_snapshot_alloc(
	struct cdev ***result,
	unsigned *count_out);

unsigned
cdev_count(void);

extern const struct file_ops cdev_file_ops;

#endif
