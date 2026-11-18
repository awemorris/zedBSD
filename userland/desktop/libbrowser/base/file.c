/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reading and writing whole files, through the files of the
 * operating-system layer.
 */

#include "base/base.h"
#include "base/os-file.h"

/*
 * Appends the whole contents of a file to a buffer.
 */
int
wb_file_read(
	const char *path,
	struct wb_buffer *buffer)
{
	int error;

	/* The system reads the file into the buffer. */
	error = wb_os_file_read(path, buffer);
	if (error != 0)
		return error;

	/* Succeeded: the contents are at the end of the buffer. */
	return 0;
}

/*
 * Replaces a file with length bytes.
 */
int
wb_file_write(
	const char *path,
	const void *bytes,
	size_t length)
{
	int error;

	/* The system creates or empties the file and writes the bytes. */
	error = wb_os_file_write(path, bytes, length);
	if (error != 0)
		return error;

	/* Succeeded: the file holds the bytes. */
	return 0;
}
