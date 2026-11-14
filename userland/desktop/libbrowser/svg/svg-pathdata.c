/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SVG path data read into a vector path.
 */

#include "svg/svg-pathdata.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
int
svg_path_data_parse(
	const char *text,
	struct vec_path *out,
	int *complete)
{
	(void)text;
	(void)out;
	(void)complete;

	/* Not implemented yet. */
	return ENOSYS;
}
