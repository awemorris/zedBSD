/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * File-backed loop block devices
 */

#ifndef ZEDBSD_KERN_LOOP_H
#define ZEDBSD_KERN_LOOP_H

struct disk;
struct file;
struct path;

#define LOOP_MAX_DEVICES	8U

enum loop_flags {
	LOOP_READ_ONLY = 0x0001U,
	LOOP_READ_WRITE = 0x0002U,
};

int
loop_init(void);

int
loop_attach_file(
	struct file *backing,
	unsigned flags,
	struct disk **disk_out);

int
loop_attach_path(
	const struct path *root,
	const char *path,
	unsigned flags,
	struct disk **disk_out);

int
loop_detach(
	struct disk *disk);

int
loop_get_index(
	const struct disk *disk,
	unsigned *index_out);

#endif
