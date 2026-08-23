/*
 * File operation
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_FILE_H
#define ZEDBSD_KERN_FILE_H

#include "kern/inode.h"
#include "kern/mount.h"
#include "kern/atomic.h"
#include "kern/lock.h"
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#ifndef NAME_MAX
#define NAME_MAX 255U
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x0100
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x40000
#endif

struct cwdinfo;
struct ucred;
struct file;

enum file_io_kind {
	FILE_IO_READ,
	FILE_IO_WRITE,
	FILE_IO_PREAD,
	FILE_IO_PWRITE,
};

struct file_io {
	struct file *file;
	enum file_io_kind kind;
	off_t offset;
	unsigned internal_flags;
	unsigned held_position;
	unsigned held_inode_io;
	unsigned transferred;
};

struct dirent {
	ino_t d_ino;
	enum inode_type d_type;
	char d_name[NAME_MAX + 1U];
};

struct file_ops {
	int (*open)(struct file *);
	ssize_t (*read)(struct file *, void *, size_t);
	ssize_t (*write)(struct file *, const void *, size_t);
	ssize_t (*pread)(struct file *, void *, size_t, off_t);
	ssize_t (*pwrite)(struct file *, const void *, size_t, off_t);
	int (*readdir)(struct file *, struct dirent *, int *);
	off_t (*seek)(struct file *, off_t, int);
	int (*ioctl)(struct file *, unsigned long, uintptr_t);
	int (*poll)(struct file *, short, short *);
	int (*fsync)(struct file *);
	int (*close)(struct file *);
};

struct file {
	struct path f_path;
	struct inode *f_inode;
	struct inode *f_vm_inode;
	const struct file_ops *f_ops;
	off_t f_offset;
	atomic_uint_t f_flags;
	refcount_t f_refs;
	struct mutex f_lock;
	unsigned f_mount_cursor;
	void *f_data;
};

int file_openat(struct cwdinfo *, const char *, int, mode_t,
		struct file **);
int file_openat_cred(struct cwdinfo *, const struct ucred *, const char *,
		     int, mode_t, struct file **);
int file_open_resolved(const struct path *, int, struct file **);
int file_create_pseudo(const struct file_ops *, int, void *, struct file **);
int file_io_begin(struct file *, enum file_io_kind, off_t, unsigned,
		  struct file_io *);
ssize_t file_io_transfer(struct file_io *, void *, size_t);
void file_io_end(struct file_io *);
ssize_t file_read(struct file *, void *, size_t);
ssize_t file_pread(struct file *, void *, size_t, off_t);
ssize_t file_pwrite(struct file *, const void *, size_t, off_t);
#define FILE_IO_LOOP_BACKING 0x00000001U
ssize_t file_pwrite_internal(struct file *, const void *, size_t, off_t,
			     unsigned);
ssize_t file_write(struct file *, const void *, size_t);
int file_readdir(struct file *, struct dirent *, int *);
off_t file_seek(struct file *, off_t, int);
int file_ioctl(struct file *, unsigned long, uintptr_t);
/* Flush this open file/backend; VM pages are synchronized by vm-object. */
int file_fsync(struct file *);
int file_close(struct file *);
void file_ref(struct file *);
static inline int
file_status_flags_get(const struct file *file)
{
	return file != NULL ? (int)atomic_load_acquire(&file->f_flags) : 0;
}
void file_status_flags_update(struct file *, int, int);
struct inode *file_vm_inode(struct file *);
void file_pool_reset(void);
unsigned file_count(void);

#endif
