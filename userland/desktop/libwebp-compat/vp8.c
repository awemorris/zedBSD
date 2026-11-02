/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lossy decoder: one key frame into its planes.
 */

#include "vp8.h"

#include <errno.h>

/*
 * Decodes a key frame (data from its frame tag, size bytes) into a new
 * picture.
 */
int
webpc_vp8_decode(
	const uint8_t *data,
	size_t size,
	int bypass_filtering,
	struct webpc_vp8_picture *picture)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(bypass_filtering);
	UNUSED_PARAMETER(picture);

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Frees a picture's planes and clears it; a cleared picture is ignored.
 */
void
webpc_vp8_picture_release(
	struct webpc_vp8_picture *picture)
{
	UNUSED_PARAMETER(picture);

	/* Not implemented yet. */
}
