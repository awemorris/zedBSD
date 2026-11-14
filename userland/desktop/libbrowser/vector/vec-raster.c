/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The coverage of a filled shape in sparse tiles of 8-bit alphas.
 */

#include "vector/vec-raster.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
void
vec_coverage_init(
	struct vec_coverage *coverage)
{
	(void)coverage;
}

/*
 * Not implemented yet.
 */
void
vec_coverage_release(
	struct vec_coverage *coverage)
{
	(void)coverage;
}

/*
 * Not implemented yet.
 */
void
vec_coverage_reset(
	struct vec_coverage *coverage)
{
	(void)coverage;
}

/*
 * Not implemented yet.
 */
int
vec_rasterize(
	const struct vec_lines *lines,
	enum vec_fill_rule rule,
	float offset_x,
	float offset_y,
	const struct vec_irect *clip,
	struct vec_coverage *out)
{
	(void)lines;
	(void)rule;
	(void)offset_x;
	(void)offset_y;
	(void)clip;
	(void)out;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
unsigned
vec_coverage_at(
	const struct vec_coverage *coverage,
	int32_t x,
	int32_t y)
{
	(void)coverage;
	(void)x;
	(void)y;

	/* Not implemented yet. */
	return 0;
}

/*
 * Not implemented yet.
 */
int
vec_coverage_intersect(
	struct vec_coverage *out,
	const struct vec_coverage *a,
	const struct vec_coverage *b)
{
	(void)out;
	(void)a;
	(void)b;

	/* Not implemented yet. */
	return ENOSYS;
}

/*
 * Not implemented yet.
 */
int
vec_coverage_unite(
	struct vec_coverage *out,
	const struct vec_coverage *a,
	const struct vec_coverage *b)
{
	(void)out;
	(void)a;
	(void)b;

	/* Not implemented yet. */
	return ENOSYS;
}
