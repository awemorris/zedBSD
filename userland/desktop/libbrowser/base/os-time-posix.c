/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The clocks and the local time zone of the operating-system layer on POSIX systems.
 */

#include "base/os-time.h"
#include "base/base.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
uint64_t
wb_os_monotonic_ns(
	void)
{
	/* A skeleton: the function is not written yet. */
	return 0;
}

/* Not implemented yet. */
int64_t
wb_os_wall_ns(
	void)
{
	/* A skeleton: the function is not written yet. */
	return 0;
}

/* Not implemented yet. */
void
wb_os_time_zone_start(
	void)
{
}

/* Not implemented yet. */
int
wb_os_local_offset(
	int64_t seconds,
	long *offset)
{
	UNUSED_PARAMETER(seconds);
	UNUSED_PARAMETER(offset);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
wb_os_zone_name(
	int64_t seconds,
	char *buffer,
	size_t size)
{
	UNUSED_PARAMETER(seconds);
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(size);

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}
