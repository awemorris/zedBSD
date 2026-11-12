/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test's stand-in for the kernel's character device header: the
 * operations a device publishes and the registration, which the test
 * records so that it can call the operations as the file layer would.
 */

#ifndef KERN_KERN_CDEV_H
#define KERN_KERN_CDEV_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct file;

/* The operations one device publishes. */
struct cdev_ops {
	int (*open)(struct file *);
	int (*close)(struct file *);
	ssize_t (*read)(struct file *, void *, size_t);
	ssize_t (*write)(struct file *, const void *, size_t);
	int (*ioctl)(struct file *, unsigned long, uintptr_t);
	int (*poll)(struct file *, short, short *);
};

int
cdev_register(
	const char *name,
	dev_t rdev,
	const struct cdev_ops *ops,
	void *data);

#endif
