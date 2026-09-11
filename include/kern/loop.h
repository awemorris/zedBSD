/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * File-backed loop block devices
 */

#ifndef KERN_KERN_LOOP_H
#define KERN_KERN_LOOP_H

struct disk;
struct file;
struct path;

#define LOOP_MAX_DEVICES	8U

enum loop_flags {
	LOOP_READ_ONLY = 0x0001U,
	LOOP_READ_WRITE = 0x0002U,
};

/* Returns a referenced backing disk; EOPNOTSUPP identifies a non-loop disk. */
int drv_loop_backing_disk_ref(struct disk *disk, struct disk **result);
/* Returns an attached loop's referenced backing file and attachment flags. */
int drv_loop_backing_file_ref(struct disk *disk, struct file **result, unsigned *flags);

int
drv_loop_init(void);

int
drv_loop_attach_file(
	struct file *backing,
	unsigned flags,
	struct disk **disk_out);

int
drv_loop_attach_path(
	const struct path *root,
	const char *path,
	unsigned flags,
	struct disk **disk_out);

int
drv_loop_detach(
	struct disk *disk);

int
drv_loop_get_index(
	const struct disk *disk,
	unsigned *index_out);

#endif
