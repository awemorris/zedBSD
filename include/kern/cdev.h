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

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define CDEV_MAX	16U

struct file;
struct file_ops;

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
};

void
cdev_reset(void);

int
cdev_register(
	const char *,
	dev_t,
	const struct cdev_ops *,
	void *);

const struct cdev *
cdev_find(
	const char *);

const struct cdev *
cdev_at(
	unsigned);

unsigned
cdev_count(void);

extern const struct file_ops cdev_file_ops;

#endif
