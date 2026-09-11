/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The file answers this fixture links instead of the file layer.
 *
 * Reclaim reaches the file write-back path, which this fixture never takes.
 */

#include <errno.h>

/*
 * The file I/O the object write-back path calls; this fixture writes no file.
 */
struct file;
struct inode;
struct file_io_guard;

int
file_io_begin(
	struct inode *inode,
	int writing,
	struct file_io_guard *guard)
{
	(void)inode;
	(void)writing;
	(void)guard;

	/* Reports that no file I/O can start here. */
	return ENOSYS;
}

int
file_io_transfer(
	struct file_io_guard *guard,
	void *data,
	unsigned long length,
	long long offset,
	unsigned long *done)
{
	(void)guard;
	(void)data;
	(void)length;
	(void)offset;
	(void)done;

	/* Reports that no file I/O can run here. */
	return ENOSYS;
}

void
file_io_end(
	struct file_io_guard *guard)
{
	(void)guard;
}

/*
 * Releases a file reference; this fixture holds no file.
 */
int
file_close(
	struct file *file)
{
	(void)file;

	/* Reports nothing to release. */
	return 0;
}
