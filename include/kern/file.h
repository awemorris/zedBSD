/*
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_KERN_FILE_H
#define BOOTS_KERN_FILE_H

#include "kern/inode.h"
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>

#ifndef NAME_MAX
#define NAME_MAX 255U
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x0100
#endif

struct cwdinfo;
struct file;

struct dirent {
	ino_t d_ino;
	enum inode_type d_type;
	char d_name[NAME_MAX + 1U];
};

struct file_ops {
	ssize_t (*read)(struct file *, void *, size_t);
	ssize_t (*write)(struct file *, const void *, size_t);
	int (*readdir)(struct file *, struct dirent *, int *);
	off_t (*seek)(struct file *, off_t, int);
	int (*ioctl)(struct file *, unsigned long, void *);
	int (*fsync)(struct file *);
	int (*close)(struct file *);
};

struct file {
	struct inode *f_inode;
	const struct file_ops *f_ops;
	off_t f_offset;
	int f_flags;
	unsigned f_usecount;
	void *f_data;
};

int file_openat(struct cwdinfo *, const char *, int, mode_t,
		struct file **);
ssize_t file_read(struct file *, void *, size_t);
ssize_t file_write(struct file *, const void *, size_t);
int file_readdir(struct file *, struct dirent *, int *);
off_t file_seek(struct file *, off_t, int);
int file_fsync(struct file *);
int file_close(struct file *);
void file_pool_reset(void);

#endif
