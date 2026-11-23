/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A display's scale: how many of its pixels make one unit of the desktop's
 * logical plane, and the steps the user may choose from (scale.c).
 *
 * The plane, the windows and the compositor's own measures are in logical
 * units; a display shows them at its scale, so a scale of 150 % draws a
 * unit with one and a half pixels.  A scale is kept as a whole number of
 * 120ths (wp_fractional_scale_v1's denominator): 120 is 100 %, 180 is
 * 150 %.  libkeiland's API takes 96ths (KL_SCALE_BASE) and the user sees
 * percents; every step of the table is a whole number in all three.
 *
 * The functions declared static inline here map between logical units
 * and a display's pixels (they need no other file, so any part of the
 * compositor and the host tests take them from this header alone).  The drawing, the input and the cursor take the
 * same ones, so a point is put on the same pixel by each of them, and two
 * rectangles that touch in units touch in pixels, with neither a gap nor
 * an overlap between them.
 *
 * Nothing here knows the server or Vulkan, so the host tests run it alone.
 */

#ifndef KWL_SCALE_H
#define KWL_SCALE_H

#include <math.h>
#include <stdint.h>

/* The scale's denominator: a scale of 120 shows one unit with one pixel (100 %). */
#define KWL_SCALE_ONE			120U

/* libkeiland's denominator (KL_SCALE_BASE), and the percent's. */
#define KWL_SCALE_ONE_96		96U
#define KWL_SCALE_ONE_PERCENT		100U

/* How many steps the table holds (scale.c), the most a display offers. */
#define KWL_SCALE_STEPS			8U

/* The smallest and the largest step (100 % and 300 %). */
#define KWL_SCALE_MIN			120U
#define KWL_SCALE_MAX			360U

/*
 * The smallest logical size a step may leave a display, its short side and
 * its long side: a step that leaves less is not offered, so that a window
 * of the usual size still fits and the Settings window stays usable.
 */
#define KWL_SCALE_LEAST_SHORT		600U
#define KWL_SCALE_LEAST_LONG		1024U

/*
 * The default: the density (dots per inch) shown at 100 %, and the largest
 * step the default may choose.  Larger steps are only the user's choice.
 */
#define KWL_SCALE_AUTO_DPI		110U
#define KWL_SCALE_AUTO_MAX		240U

/*
 * Whether a display the user left to the default takes the scale its
 * density asks for (1), or is shown at 100 % (0) while the default is only
 * offered as the recommended step.
 */
#define KWL_SCALE_AUTO_TAKEN		0

/*
 * The densities a display's physical size may give: one outside them is
 * taken for a size its EDID got wrong, and the display is shown at 100 %.
 */
#define KWL_SCALE_DPI_LEAST		50U
#define KWL_SCALE_DPI_MOST		600U

/*
 * How far (in percent) the physical size's aspect may be from the mode's
 * before the physical size is taken for wrong.
 */
#define KWL_SCALE_ASPECT_SLACK		10U

/*
 * Why a display's scale cannot be chosen now (the same bits as
 * libkeiland's KL_DISPLAY_SCALE_WHY_*): it offers 100 % alone, another
 * display is shown beside it (or the same desktop on several), it is not
 * the display the desktop is shown on, or a change of its mode or a game
 * is under way.
 */
#define KWL_SCALE_WHY_FIXED		0x1U
#define KWL_SCALE_WHY_ALONE		0x2U
#define KWL_SCALE_WHY_ANCHOR		0x4U
#define KWL_SCALE_WHY_BUSY		0x8U

/*
 * A rectangle of the plane or of a display's pixels: its left top corner
 * and its size.  One instance is either in logical units or in pixels; the
 * caller knows which, as kwl_scale_snap_rect takes one and makes the
 * other.
 */
struct kwl_scale_rect {
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
};

unsigned kwl_scale_step(unsigned index);
int kwl_scale_is_step(unsigned scale120);
unsigned kwl_scale_offered(uint32_t width, uint32_t height, unsigned *offered, unsigned capacity);
int kwl_scale_offers(uint32_t width, uint32_t height, unsigned scale120);
unsigned kwl_scale_largest(uint32_t width, uint32_t height);
unsigned kwl_scale_density(uint32_t width, uint32_t height, uint32_t mm_width, uint32_t mm_height);
unsigned kwl_scale_recommended(uint32_t width, uint32_t height, uint32_t mm_width, uint32_t mm_height);
unsigned kwl_scale_shown(unsigned chosen, unsigned recommended, uint32_t width, uint32_t height);
unsigned kwl_scale_from_percent(unsigned percent);
unsigned kwl_scale_percent(unsigned scale120);
unsigned kwl_scale_to_96(unsigned scale120);
unsigned kwl_scale_from_96(unsigned scale96);

/*
 * Rounds a quotient of two signed 64-bit numbers down (toward minus
 * infinity), as C's division rounds toward zero.  The divisor is positive.
 */
static inline int64_t
kwl_scale_floor_div(
	int64_t dividend,
	int64_t divisor)
{
	int64_t quotient;

	/* C rounds toward zero, which is down only for a quotient that is not negative. */
	quotient = dividend / divisor;

	/* Steps a negative quotient with a remainder down to the next integer. */
	if (dividend % divisor != 0 && dividend < 0)
		quotient--;

	/* Succeeded: the quotient rounded down. */
	return quotient;
}

/*
 * Reports the logical size of a length of pixels shown at a scale: the
 * pixels divided by the scale, rounded to the nearest unit, at least one.
 */
static inline uint32_t
kwl_scale_logical(
	uint32_t pixels,
	unsigned scale120)
{
	uint64_t units;

	/* A scale of zero (none known yet) is taken for 100 %. */
	if (scale120 == 0U)
		scale120 = KWL_SCALE_ONE;

	/* Divides with the half added, which rounds a half unit up. */
	units = ((uint64_t)pixels * KWL_SCALE_ONE + scale120 / 2U) / scale120;

	/* A length of some pixels keeps a unit, so that a display is never empty. */
	if (units == 0U && pixels != 0U)
		units = 1U;

	/* Reports the length in units. */
	return (uint32_t)units;
}

/*
 * Reports the pixel a logical coordinate is shown at: the coordinate times
 * the scale, rounded to the nearest pixel (a half up), for a coordinate
 * measured from the display's left top corner.  A coordinate left of or
 * above the display gives a negative pixel, rounded the same way.
 */
static inline int32_t
kwl_scale_snap(
	int32_t logical,
	unsigned scale120)
{
	int64_t pixel;

	/* A scale of zero (none known yet) is taken for 100 %. */
	if (scale120 == 0U)
		scale120 = KWL_SCALE_ONE;

	/* floor(x * scale / 120 + 1/2), with the half added in 240ths. */
	pixel = kwl_scale_floor_div((int64_t)logical * (int64_t)scale120 * 2 + KWL_SCALE_ONE, 2 * (int64_t)KWL_SCALE_ONE);

	/* Keeps the pixel in the coordinate's range; a plane's coordinate is far inside it. */
	if (pixel > INT32_MAX)
		pixel = INT32_MAX;
	if (pixel < INT32_MIN)
		pixel = INT32_MIN;

	/* Reports the pixel. */
	return (int32_t)pixel;
}

/*
 * Maps a logical rectangle to the display's pixels: its left top and its
 * right bottom corners are snapped each, so rectangles that touch in units
 * touch in pixels.  The rectangle is measured from the display's left top
 * corner.
 */
static inline void
kwl_scale_snap_rect(
	const struct kwl_scale_rect *logical,
	unsigned scale120,
	struct kwl_scale_rect *pixels)
{
	int32_t left;
	int32_t top;
	int32_t right;
	int32_t bottom;
	int64_t far_x;
	int64_t far_y;

	/* The right and bottom edges, kept in the coordinate's range. */
	far_x = (int64_t)logical->x + logical->width;
	far_y = (int64_t)logical->y + logical->height;
	if (far_x > INT32_MAX)
		far_x = INT32_MAX;
	if (far_y > INT32_MAX)
		far_y = INT32_MAX;

	/* Snaps the four edges. */
	left = kwl_scale_snap(logical->x, scale120);
	top = kwl_scale_snap(logical->y, scale120);
	right = kwl_scale_snap((int32_t)far_x, scale120);
	bottom = kwl_scale_snap((int32_t)far_y, scale120);

	/* Publishes the rectangle the edges bound. */
	pixels->x = left;
	pixels->y = top;
	pixels->width = (uint32_t)((int64_t)right - left);
	pixels->height = (uint32_t)((int64_t)bottom - top);
}

/*
 * Reports the pixel range a logical rectangle touches: its left top
 * corner rounded down and its right bottom corner rounded up, so that
 * every pixel the rectangle covers even in part is inside (a damage's, a
 * scissor's).  The rectangle is measured from the display's left top
 * corner.
 */
static inline void
kwl_scale_cover_rect(
	const struct kwl_scale_rect *logical,
	unsigned scale120,
	struct kwl_scale_rect *pixels)
{
	int64_t left;
	int64_t top;
	int64_t right;
	int64_t bottom;

	/* A scale of zero (none known yet) is taken for 100 %. */
	if (scale120 == 0U)
		scale120 = KWL_SCALE_ONE;

	/* The left and top edges, rounded down. */
	left = kwl_scale_floor_div((int64_t)logical->x * scale120, KWL_SCALE_ONE);
	top = kwl_scale_floor_div((int64_t)logical->y * scale120, KWL_SCALE_ONE);

	/* The right and bottom edges, rounded up: minus the floor of the negated edge. */
	right = -kwl_scale_floor_div(-((int64_t)logical->x + logical->width) * scale120, KWL_SCALE_ONE);
	bottom = -kwl_scale_floor_div(-((int64_t)logical->y + logical->height) * scale120, KWL_SCALE_ONE);

	/* Keeps the edges in the coordinate's range; a plane's coordinate is far inside it. */
	if (left < INT32_MIN)
		left = INT32_MIN;
	if (top < INT32_MIN)
		top = INT32_MIN;
	if (right > INT32_MAX)
		right = INT32_MAX;
	if (bottom > INT32_MAX)
		bottom = INT32_MAX;

	/* Publishes the rectangle the edges bound. */
	pixels->x = (int32_t)left;
	pixels->y = (int32_t)top;
	pixels->width = (uint32_t)(right - left);
	pixels->height = (uint32_t)(bottom - top);
}

/*
 * Reports the pixel a logical position between units (an animation's) is
 * shown at, as kwl_scale_snap does for a whole unit.  At 100 % the
 * position is given back as it is, not rounded, so an animation at 100 %
 * moves as it did before scales.
 */
static inline float
kwl_scale_snap_float(
	float logical,
	unsigned scale120)
{
	float pixel;

	/* At 100 % (or a scale not known yet) the plane's units are the pixels. */
	if (scale120 == 0U || scale120 == KWL_SCALE_ONE)
		return logical;

	/* floor(x * scale / 120 + 1/2). */
	pixel = floorf(logical * (float)scale120 / (float)KWL_SCALE_ONE + 0.5f);

	/* Reports the pixel. */
	return pixel;
}

/*
 * Reports the logical point a pixel is at, in 256ths of a unit (the
 * Wayland fixed point): the pixel divided by the scale, rounded down.  The
 * pixel is measured from the display's left top corner, and the point
 * from the same corner.
 */
static inline int64_t
kwl_scale_unsnap_fixed(
	int32_t pixel,
	unsigned scale120)
{
	int64_t point;

	/* A scale of zero (none known yet) is taken for 100 %. */
	if (scale120 == 0U)
		scale120 = KWL_SCALE_ONE;

	/* pixel * 120 / scale, in 256ths. */
	point = kwl_scale_floor_div((int64_t)pixel * KWL_SCALE_ONE * 256, (int64_t)scale120);

	/* Reports the point. */
	return point;
}

/*
 * Reports how many pixels a client's buffer has for a logical length at a
 * scale: the length times the scale, a half rounded away from zero
 * (wp_fractional_scale_v1's rule for a toplevel).
 */
static inline uint32_t
kwl_scale_buffer(
	uint32_t logical,
	unsigned scale120)
{
	uint64_t pixels;

	/* A scale of zero (none known yet) is taken for 100 %. */
	if (scale120 == 0U)
		scale120 = KWL_SCALE_ONE;

	/* floor(length * scale / 120 + 1/2), with the half added in 240ths. */
	pixels = ((uint64_t)logical * scale120 * 2U + KWL_SCALE_ONE) / (2U * KWL_SCALE_ONE);

	/* Keeps the size in the range a buffer's size takes. */
	if (pixels > UINT32_MAX)
		pixels = UINT32_MAX;

	/* Reports the size in pixels. */
	return (uint32_t)pixels;
}

#endif
