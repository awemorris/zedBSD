/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The host test's stand-in for the kernel's open file: the device's data and the status flags. */

#ifndef KERN_KERN_FILE_H
#define KERN_KERN_FILE_H

/* An open file as the device sees it. */
struct file {
	void *f_data;
	int f_flags;
};

int
file_status_flags_get(
	const struct file *file);

#endif
