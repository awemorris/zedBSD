/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The files of the operating-system layer on Windows.
 */

#include "base/os-file.h"
#include "base/os-win32.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
int
wb_os_file_read(
	const char *path,
	struct wb_buffer *buffer)
{
	UNUSED_PARAMETER(path);
	UNUSED_PARAMETER(buffer);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_file_write(
	const char *path,
	const void *bytes,
	size_t length)
{
	UNUSED_PARAMETER(path);
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(length);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_file_replace(
	const char *path,
	const void *bytes,
	size_t length)
{
	UNUSED_PARAMETER(path);
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(length);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_file_remove(
	const char *path)
{
	UNUSED_PARAMETER(path);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_make_directories(
	const char *path)
{
	UNUSED_PARAMETER(path);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_list(
	const char *path,
	wb_os_list_each each,
	void *context)
{
	UNUSED_PARAMETER(path);
	UNUSED_PARAMETER(each);
	UNUSED_PARAMETER(context);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_full_path(
	const char *path,
	struct wb_buffer *buffer)
{
	UNUSED_PARAMETER(path);
	UNUSED_PARAMETER(buffer);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_current_directory(
	struct wb_buffer *buffer)
{
	UNUSED_PARAMETER(buffer);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_path_is_absolute(
	const char *path)
{
	UNUSED_PARAMETER(path);

	/* A skeleton: the function is not written yet. */
	return 0;
}

/* Not implemented yet. */
int
wb_os_path_from_url(
	const char *host,
	const char *path,
	struct wb_buffer *buffer)
{
	UNUSED_PARAMETER(host);
	UNUSED_PARAMETER(path);
	UNUSED_PARAMETER(buffer);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_url_from_path(
	const char *local,
	struct wb_buffer *host,
	struct wb_buffer *path)
{
	UNUSED_PARAMETER(local);
	UNUSED_PARAMETER(host);
	UNUSED_PARAMETER(path);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_lock_open(
	const char *directory,
	struct wb_os_lock **lock)
{
	UNUSED_PARAMETER(directory);
	UNUSED_PARAMETER(lock);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
void
wb_os_lock_close(
	struct wb_os_lock *lock)
{
	UNUSED_PARAMETER(lock);
}
