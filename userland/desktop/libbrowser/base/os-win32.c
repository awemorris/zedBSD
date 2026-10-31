/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Windows helpers of the operating-system layer: errors and UTF-16.
 */

#include "base/os-win32.h"
#include "base/base.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
int
wb_os_error_from_system(
	unsigned long code)
{
	UNUSED_PARAMETER(code);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_error_from_socket(
	int code)
{
	UNUSED_PARAMETER(code);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_wide_from_utf8(
	const char *text,
	wchar_t **wide,
	size_t *length)
{
	UNUSED_PARAMETER(text);
	UNUSED_PARAMETER(wide);
	UNUSED_PARAMETER(length);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_utf8_from_wide(
	const wchar_t *wide,
	size_t count,
	char **text,
	size_t *length)
{
	UNUSED_PARAMETER(wide);
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(text);
	UNUSED_PARAMETER(length);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_wide_path(
	const char *path,
	wchar_t **wide)
{
	UNUSED_PARAMETER(path);
	UNUSED_PARAMETER(wide);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}
