/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arithmetic of a window drawn at the compositor's scale (window.c):
 * the compositor tells the scale in 120ths (wp_fractional_scale_v1's
 * preferred_scale, 120 is 100 %), the surface's size and the input's
 * places are in the compositor's logical units, and a window that follows
 * the scale draws a buffer of the size in pixels and takes the input's
 * places in pixels (KL_SCALE_BASE's contract).  The compositor makes the
 * same sizes from the same rules (its scale.h), so a buffer of the size
 * made here is shown pixel for pixel.
 *
 * Nothing here knows Wayland or the window, so the host tests run it alone.
 */

#ifndef KEIUI_WINDOW_SCALE_H
#define KEIUI_WINDOW_SCALE_H

#include <keiland/keiland_canvas.h>

#include <math.h>
#include <stdint.h>

/* The compositor's denominator: a scale of 120 is 100 %. */
#define KEIUI_SCALE_ONE		120U

/* The largest scale taken (a larger one told is taken as this), 400 %. */
#define KEIUI_SCALE_MOST	480U

/*
 * Reports the scale the compositor told (120ths) as one a window takes: 0
 * (none told) is 100 %, and one past KEIUI_SCALE_MOST is that.
 */
static inline unsigned
keiui_scale_taken(
	uint32_t scale120)
{
	/* None told: 100 %. */
	if (scale120 == 0U)
		return KEIUI_SCALE_ONE;

	/* Too large a scale for the sizes to stay in range. */
	if (scale120 > KEIUI_SCALE_MOST)
		return KEIUI_SCALE_MOST;

	/* Reports the scale as told. */
	return (unsigned)scale120;
}

/*
 * Reports how many pixels a buffer has for a logical length at a scale:
 * the length times the scale, a half rounded away from zero
 * (wp_fractional_scale_v1's rule for a toplevel), at least one for a
 * length of some units.
 */
static inline uint32_t
keiui_scale_pixels(
	uint32_t units,
	unsigned scale120)
{
	uint64_t pixels;

	/* floor(length * scale / 120 + 1/2), with the half added in 240ths. */
	pixels = ((uint64_t)units * scale120 * 2U + KEIUI_SCALE_ONE) / (2U * KEIUI_SCALE_ONE);

	/* A length of some units keeps a pixel, and the size stays in range. */
	if (pixels == 0U && units != 0U)
		pixels = 1U;
	if (pixels > UINT32_MAX)
		pixels = UINT32_MAX;

	/* Reports the size in pixels. */
	return (uint32_t)pixels;
}

/* Reports a place in logical units (the compositor's) in pixels, unrounded. */
static inline double
keiui_scale_to_pixels(
	double units,
	unsigned scale120)
{
	/* units * scale / 120. */
	return units * (double)scale120 / (double)KEIUI_SCALE_ONE;
}

/*
 * Reports a place in pixels in logical units, rounded down: the unit the
 * pixel is in (a rectangle's left or top edge).
 */
static inline int32_t
keiui_scale_units_floor(
	int32_t pixels,
	unsigned scale120)
{
	double units;

	/* pixels * 120 / scale, rounded down. */
	units = floor((double)pixels * (double)KEIUI_SCALE_ONE / (double)scale120);

	/* Reports the unit. */
	return (int32_t)units;
}

/*
 * Reports a place in pixels in logical units, rounded up: the first unit
 * past the pixel (a rectangle's right or bottom edge).
 */
static inline int32_t
keiui_scale_units_ceil(
	int32_t pixels,
	unsigned scale120)
{
	double units;

	/* pixels * 120 / scale, rounded up. */
	units = ceil((double)pixels * (double)KEIUI_SCALE_ONE / (double)scale120);

	/* Reports the unit. */
	return (int32_t)units;
}

/*
 * Reports a place in pixels in logical units, rounded to the nearest (a
 * point: a menu's place).
 */
static inline int32_t
keiui_scale_units_round(
	int32_t pixels,
	unsigned scale120)
{
	double units;

	/* pixels * 120 / scale, a half up. */
	units = floor((double)pixels * (double)KEIUI_SCALE_ONE / (double)scale120 + 0.5);

	/* Reports the unit. */
	return (int32_t)units;
}

/* Reports a scale in 120ths in the library's 96ths (KL_SCALE_BASE), rounded to the nearest. */
static inline unsigned
keiui_scale_96(
	unsigned scale120)
{
	/* scale * 96 / 120, a half up. */
	return (scale120 * KL_SCALE_BASE + KEIUI_SCALE_ONE / 2U) / KEIUI_SCALE_ONE;
}

#endif
