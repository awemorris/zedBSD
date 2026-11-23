/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A display's scale: the steps, the steps a display offers, the default
 * from its density, and the units the scale is told in (scale.h says what
 * and why).
 */

#include "scale.h"

#include <math.h>
#include <stddef.h>

/* Millimetres in an inch, in tenths of a millimetre (the density's arithmetic stays whole). */
#define SCALE_INCH_TENTHS_MM		254U

/*
 * The steps the user may choose from, the smallest first: 100 % to 250 %
 * by a quarter, then 300 %.  These are the steps other desktops offer at
 * these sizes; each is a whole number of 120ths, 96ths and percents.
 */
static const unsigned scale_steps[KWL_SCALE_STEPS] = {
	120U,
	150U,
	180U,
	210U,
	240U,
	270U,
	300U,
	360U
};

static int scale_aspect_agrees(uint32_t width, uint32_t height, uint32_t mm_width, uint32_t mm_height);

/* Reports the step at an index of the table (the smallest first), or 0 past its end. */
unsigned
kwl_scale_step(
	unsigned index)
{
	/* No step past the table. */
	if (index >= KWL_SCALE_STEPS)
		return 0U;

	/* Succeeded: the step. */
	return scale_steps[index];
}

/* Tells whether a scale (120ths) is one of the steps: 1 or 0. */
int
kwl_scale_is_step(
	unsigned scale120)
{
	unsigned index;

	/* Each step of the table. */
	for (index = 0U; index < KWL_SCALE_STEPS; index++) {
		if (scale_steps[index] == scale120)
			return 1;
	}

	/* Succeeded: not a step. */
	return 0;
}

/*
 * Tells whether a display of a size in pixels (either way round) offers a
 * step: 100 % always, another step when the logical size it leaves is at
 * least KWL_SCALE_LEAST_SHORT by KWL_SCALE_LEAST_LONG.  Returns 1 or 0
 * (0 for a scale that is not a step).
 */
int
kwl_scale_offers(
	uint32_t width,
	uint32_t height,
	unsigned scale120)
{
	uint32_t logical_width;
	uint32_t logical_height;
	uint32_t shorter;
	uint32_t longer;
	int step;

	/* Only a step of the table. */
	step = kwl_scale_is_step(scale120);
	if (!step)
		return 0;

	/* 100 % shows any display. */
	if (scale120 == KWL_SCALE_ONE)
		return 1;

	/* The logical size the step leaves, its short side and its long side. */
	logical_width = kwl_scale_logical(width, scale120);
	logical_height = kwl_scale_logical(height, scale120);
	shorter = logical_width;
	longer = logical_height;
	if (logical_height < logical_width) {
		shorter = logical_height;
		longer = logical_width;
	}

	/* Too small a desktop for the windows the user has. */
	if (shorter < KWL_SCALE_LEAST_SHORT)
		return 0;
	if (longer < KWL_SCALE_LEAST_LONG)
		return 0;

	/* Succeeded: the step leaves room enough. */
	return 1;
}

/*
 * Lists the steps a display of a size in pixels offers, the smallest
 * first (100 % is always one).  Returns how many were written to offered
 * (at most capacity).
 */
unsigned
kwl_scale_offered(
	uint32_t width,
	uint32_t height,
	unsigned *offered,
	unsigned capacity)
{
	unsigned count;
	unsigned index;
	int offers;

	/* Each step the size leaves room for, while there is room in the list. */
	count = 0U;
	for (index = 0U; index < KWL_SCALE_STEPS && count < capacity; index++) {
		offers = kwl_scale_offers(width, height, scale_steps[index]);
		if (!offers)
			continue;
		offered[count] = scale_steps[index];
		count++;
	}

	/* Succeeded: the steps offered. */
	return count;
}

/* Reports the largest step a display of a size in pixels offers (100 % at least). */
unsigned
kwl_scale_largest(
	uint32_t width,
	uint32_t height)
{
	unsigned largest;
	unsigned index;
	int offers;

	/* Each step, the largest offered kept. */
	largest = KWL_SCALE_ONE;
	for (index = 0U; index < KWL_SCALE_STEPS; index++) {
		offers = kwl_scale_offers(width, height, scale_steps[index]);
		if (offers)
			largest = scale_steps[index];
	}

	/* Succeeded: the largest step. */
	return largest;
}

/*
 * Reports a display's density in dots per inch, from its size in pixels
 * and its physical size in millimetres, both the same way round (the
 * mode's and the EDID's).  Returns 0 for a physical size that cannot be
 * the display's: none, one whose aspect is not the mode's, or a density
 * outside KWL_SCALE_DPI_LEAST to KWL_SCALE_DPI_MOST.
 */
unsigned
kwl_scale_density(
	uint32_t width,
	uint32_t height,
	uint32_t mm_width,
	uint32_t mm_height)
{
	double pixels;
	double inches;
	double dpi;
	int agrees;

	/* No size, either way. */
	if (width == 0U || height == 0U)
		return 0U;
	if (mm_width == 0U || mm_height == 0U)
		return 0U;

	/* A physical size of another shape is not the display's (an aspect ratio given as a size, or a size in centimetres). */
	agrees = scale_aspect_agrees(width, height, mm_width, mm_height);
	if (!agrees)
		return 0U;

	/* The diagonal in pixels over the diagonal in inches. */
	pixels = sqrt((double)width * (double)width + (double)height * (double)height);
	inches = sqrt((double)mm_width * (double)mm_width + (double)mm_height * (double)mm_height) * 10.0 / (double)SCALE_INCH_TENTHS_MM;
	dpi = floor(pixels / inches + 0.5);

	/* A density no display has is a size the EDID got wrong. */
	if (dpi < (double)KWL_SCALE_DPI_LEAST)
		return 0U;
	if (dpi > (double)KWL_SCALE_DPI_MOST)
		return 0U;

	/* Succeeded: the density. */
	return (unsigned)dpi;
}

/*
 * Reports the scale a display's density asks for (120ths): the density
 * over KWL_SCALE_AUTO_DPI rounded to the nearest quarter, from 100 % up to
 * KWL_SCALE_AUTO_MAX and the largest step the display offers.  A display
 * whose physical size is not known, or cannot be its own, is 100 %.
 */
unsigned
kwl_scale_recommended(
	uint32_t width,
	uint32_t height,
	uint32_t mm_width,
	uint32_t mm_height)
{
	unsigned dpi;
	unsigned quarters;
	unsigned scale120;
	unsigned largest;

	/* The density, when the physical size is the display's. */
	dpi = kwl_scale_density(width, height, mm_width, mm_height);
	if (dpi == 0U)
		return KWL_SCALE_ONE;

	/* dpi / KWL_SCALE_AUTO_DPI in quarters, rounded to the nearest (30 120ths each). */
	quarters = (dpi * 4U * 2U + KWL_SCALE_AUTO_DPI) / (2U * KWL_SCALE_AUTO_DPI);
	scale120 = quarters * (KWL_SCALE_ONE / 4U);

	/* Not below 100 %, nor above the default's largest or the display's. */
	if (scale120 < KWL_SCALE_ONE)
		scale120 = KWL_SCALE_ONE;
	if (scale120 > KWL_SCALE_AUTO_MAX)
		scale120 = KWL_SCALE_AUTO_MAX;
	largest = kwl_scale_largest(width, height);
	if (scale120 > largest)
		scale120 = largest;

	/* Succeeded: the recommended step. */
	return scale120;
}

/*
 * Reports the scale a display of a size in pixels is shown at (120ths):
 * the step the user chose (chosen; 0 leaves it to the default), or the
 * largest step the display offers below it when the display cannot offer
 * it now (another mode); for the default, the recommended step while
 * KWL_SCALE_AUTO_TAKEN, else 100 %.
 */
unsigned
kwl_scale_shown(
	unsigned chosen,
	unsigned recommended,
	uint32_t width,
	uint32_t height)
{
	unsigned wanted;
	unsigned shown;
	unsigned index;
	int offers;

	/* The user's step, or the default's. */
	wanted = chosen;
	if (chosen == 0U) {
		wanted = KWL_SCALE_ONE;
		if (KWL_SCALE_AUTO_TAKEN)
			wanted = recommended;
	}

	/* The largest step offered up to it (100 % is always offered). */
	shown = KWL_SCALE_ONE;
	for (index = 0U; index < KWL_SCALE_STEPS; index++) {
		if (scale_steps[index] > wanted)
			break;
		offers = kwl_scale_offers(width, height, scale_steps[index]);
		if (offers)
			shown = scale_steps[index];
	}

	/* Succeeded: the scale shown. */
	return shown;
}

/* Reports the step (120ths) of a percent, or 0 when the percent is not a step. */
unsigned
kwl_scale_from_percent(
	unsigned percent)
{
	unsigned scale120;
	int step;

	/* A percent that is not a whole number of 120ths is no step. */
	if (percent > KWL_SCALE_MAX * KWL_SCALE_ONE_PERCENT)
		return 0U;
	if ((percent * KWL_SCALE_ONE) % KWL_SCALE_ONE_PERCENT != 0U)
		return 0U;
	scale120 = percent * KWL_SCALE_ONE / KWL_SCALE_ONE_PERCENT;

	/* Only a step of the table. */
	step = kwl_scale_is_step(scale120);
	if (!step)
		return 0U;

	/* Succeeded: the step. */
	return scale120;
}

/* Reports a scale (120ths) in percent, rounded to the nearest. */
unsigned
kwl_scale_percent(
	unsigned scale120)
{
	/* scale * 100 / 120, a half up. */
	return (scale120 * KWL_SCALE_ONE_PERCENT + KWL_SCALE_ONE / 2U) / KWL_SCALE_ONE;
}

/* Reports a scale (120ths) in libkeiland's 96ths, rounded to the nearest. */
unsigned
kwl_scale_to_96(
	unsigned scale120)
{
	/* scale * 96 / 120, a half up. */
	return (scale120 * KWL_SCALE_ONE_96 + KWL_SCALE_ONE / 2U) / KWL_SCALE_ONE;
}

/* Reports a scale in libkeiland's 96ths in 120ths, rounded to the nearest. */
unsigned
kwl_scale_from_96(
	unsigned scale96)
{
	/* scale * 120 / 96, a half up. */
	return (scale96 * KWL_SCALE_ONE + KWL_SCALE_ONE_96 / 2U) / KWL_SCALE_ONE_96;
}

/*
 * Tells whether a physical size has the shape of a size in pixels: their
 * aspects (the longer side over the shorter, so either way round) differ
 * by at most KWL_SCALE_ASPECT_SLACK percent.  Returns 1 or 0.
 */
static int
scale_aspect_agrees(
	uint32_t width,
	uint32_t height,
	uint32_t mm_width,
	uint32_t mm_height)
{
	uint64_t pixel_long;
	uint64_t pixel_short;
	uint64_t mm_long;
	uint64_t mm_short;
	uint64_t pixel_cross;
	uint64_t mm_cross;
	uint64_t difference;

	/* Each size's longer and shorter side. */
	pixel_long = width;
	pixel_short = height;
	if (height > width) {
		pixel_long = height;
		pixel_short = width;
	}
	mm_long = mm_width;
	mm_short = mm_height;
	if (mm_height > mm_width) {
		mm_long = mm_height;
		mm_short = mm_width;
	}

	/* The two aspects compared crosswise: pixel_long / pixel_short against mm_long / mm_short. */
	pixel_cross = pixel_long * mm_short;
	mm_cross = mm_long * pixel_short;
	difference = pixel_cross - mm_cross;
	if (mm_cross > pixel_cross)
		difference = mm_cross - pixel_cross;

	/* Further apart than the slack, against the pixels' aspect. */
	if (difference * KWL_SCALE_ONE_PERCENT > pixel_cross * KWL_SCALE_ASPECT_SLACK)
		return 0;

	/* Succeeded: the same shape. */
	return 1;
}
