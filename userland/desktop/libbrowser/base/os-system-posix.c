/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system services of the operating-system layer on POSIX systems.
 */

#include "base/os-system.h"
#include "base/base.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
int
wb_os_aligned_alloc(
	size_t alignment,
	size_t size,
	void **memory)
{
	UNUSED_PARAMETER(alignment);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(memory);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
void
wb_os_aligned_free(
	void *memory)
{
	UNUSED_PARAMETER(memory);
}

/* Not implemented yet. */
int
wb_os_random(
	void *bytes,
	size_t length)
{
	UNUSED_PARAMETER(bytes);
	UNUSED_PARAMETER(length);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
void
wb_os_thread_name(
	const char *name)
{
	UNUSED_PARAMETER(name);
}

/* Not implemented yet. */
int
wb_os_library_open(
	const char *const *names,
	struct wb_os_library **library)
{
	UNUSED_PARAMETER(names);
	UNUSED_PARAMETER(library);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
wb_os_function
wb_os_library_symbol(
	struct wb_os_library *library,
	const char *name)
{
	UNUSED_PARAMETER(library);
	UNUSED_PARAMETER(name);

	/* A skeleton: the function is not written yet. */
	return NULL;
}

/* Not implemented yet. */
void
wb_os_library_close(
	struct wb_os_library *library)
{
	UNUSED_PARAMETER(library);
}

/* Not implemented yet. */
const char *
wb_os_platform_name(
	void)
{
	/* A skeleton: the function is not written yet. */
	return "";
}

/* Not implemented yet. */
const char *
wb_os_user_agent_system(
	void)
{
	/* A skeleton: the function is not written yet. */
	return "";
}

/* Not implemented yet. */
int
wb_os_data_directory(
	char *buffer,
	size_t size)
{
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(size);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_trust_roots(
	wb_os_trust_each each,
	void *context)
{
	UNUSED_PARAMETER(each);
	UNUSED_PARAMETER(context);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}
