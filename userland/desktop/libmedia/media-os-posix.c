/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What libmedia asks of a POSIX system: the Vulkan loader and reading media files.
 */

#include "media-os.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
int
media_os_vulkan_open(
	struct media_os_library **library)
{
	(void)library;

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
media_os_function
media_os_library_symbol(
	struct media_os_library *library,
	const char *name)
{
	(void)library;
	(void)name;

	/* A skeleton: the function is not written yet. */
	return NULL;
}

/* Not implemented yet. */
int
media_os_file_open(
	const char *path,
	struct media_os_file **file,
	uint64_t *size)
{
	(void)path;
	(void)file;
	(void)size;

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
media_os_file_read_at(
	struct media_os_file *file,
	uint64_t offset,
	void *data,
	size_t size)
{
	(void)file;
	(void)offset;
	(void)data;
	(void)size;

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
void
media_os_file_close(
	struct media_os_file *file)
{
	(void)file;
}

/* Not implemented yet. */
void
media_os_close_descriptor(
	int descriptor)
{
	(void)descriptor;
}
