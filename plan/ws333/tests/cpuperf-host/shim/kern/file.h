/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The host test's stand-in for the kernel's open file: the status flags the CPU requests read. */

#ifndef KERN_KERN_FILE_H
#define KERN_KERN_FILE_H

/* An open of /dev/thermal as the framework sees it. */
struct file {
	int f_flags;
};

int
file_status_flags_get(
	const struct file *file);

#endif
