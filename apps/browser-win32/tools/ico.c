/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The writer of Windows icon files.
 */

#include "ico.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
int
ico_write(
	const struct ico_image *images,
	unsigned count,
	unsigned char **out,
	size_t *length)
{
	(void)images;
	(void)count;
	(void)out;
	(void)length;

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}
