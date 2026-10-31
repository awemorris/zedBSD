/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The English long names of the Windows time zones, from the Unicode CLDR.
 */

#include "base/os-zones.h"
#include "base/base.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
const struct wb_os_zone_names *
wb_os_zone_find(
	const char *key)
{
	UNUSED_PARAMETER(key);

	/* A skeleton: the function is not written yet. */
	return NULL;
}

/* Not implemented yet. */
const struct wb_os_zone_names *
wb_os_zone_table(
	size_t *count)
{
	UNUSED_PARAMETER(count);

	/* A skeleton: the function is not written yet. */
	return NULL;
}
