/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Rounded rectangles, their border rings and their blurred shadows drawn by closed formulas.
 */

#include "vector/vec-rrect.h"

#include <errno.h>

/*
 * Not implemented yet.
 */
void
vec_rrect_normalize(
	struct vec_rrect *rrect)
{
	(void)rrect;
}

/*
 * Not implemented yet.
 */
void
vec_rrect_inset(
	const struct vec_rrect *outer,
	const float widths[4],
	struct vec_rrect *inner)
{
	(void)outer;
	(void)widths;
	(void)inner;
}

/*
 * Not implemented yet.
 */
void
vec_rrect_spread(
	const struct vec_rrect *border,
	float amount,
	struct vec_rrect *grown)
{
	(void)border;
	(void)amount;
	(void)grown;
}

/*
 * Not implemented yet.
 */
float
vec_rrect_coverage(
	const struct vec_rrect *rrect,
	float px,
	float py)
{
	(void)rrect;
	(void)px;
	(void)py;

	/* Not implemented yet. */
	return 0.0f;
}

/*
 * Not implemented yet.
 */
float
vec_rrect_ring_coverage(
	const struct vec_rrect *outer,
	const struct vec_rrect *inner,
	float px,
	float py)
{
	(void)outer;
	(void)inner;
	(void)px;
	(void)py;

	/* Not implemented yet. */
	return 0.0f;
}

/*
 * Not implemented yet.
 */
float
vec_rrect_side_share(
	const struct vec_rrect *outer,
	const float widths[4],
	int side,
	float px,
	float py)
{
	(void)outer;
	(void)widths;
	(void)side;
	(void)px;
	(void)py;

	/* Not implemented yet. */
	return 0.0f;
}

/*
 * Not implemented yet.
 */
float
vec_erf(
	float x)
{
	(void)x;

	/* Not implemented yet. */
	return 0.0f;
}

/*
 * Not implemented yet.
 */
float
vec_rrect_shadow(
	const struct vec_rrect *rrect,
	float sigma,
	float px,
	float py)
{
	(void)rrect;
	(void)sigma;
	(void)px;
	(void)py;

	/* Not implemented yet. */
	return 0.0f;
}
