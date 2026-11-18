/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mode list of one output.  Every timing the sink names is checked
 * against the sink's range limits, the link, the controller and the
 * driver's own limits; what passes is listed once per size and rate, and
 * the default is chosen and put first.
 *
 * The default is the best the output can show now: the sink's preferred
 * timing when it is listed and refreshes at 50 Hz or more; otherwise the
 * fastest listed timing of the preferred size at 50 Hz or more; otherwise
 * the largest listed timing at 50 Hz or more; otherwise the largest listed
 * timing.  A preferred timing below 50 Hz (a dummy plug's or a television's
 * 30 Hz) is listed but not chosen.
 */

#include "drivers/gpu/display/modes.h"

#include <stddef.h>

#include <uapi/errno.h>

/* Half a hertz and half a kHz in their smaller units, for rounding to whole ones. */
#define MODES_HALF_HZ_MHZ		500U
#define MODES_MHZ_PER_HZ		1000U

static int modes_detailed(const struct gpu_display_timing *timing);
static uint32_t modes_range(const struct gpu_edid *edid, const struct gpu_display_timing *timing);
static int modes_repeat(const struct gpu_mode_list *list, const struct gpu_display_timing *timing);
static uint32_t modes_default(const struct gpu_edid *edid, const struct gpu_mode_list *list);
static int modes_larger(const struct gpu_display_timing *first, const struct gpu_display_timing *second);
static void modes_order(struct gpu_mode_list *list, uint32_t chosen);

/*
 * Checks whether an output lists one timing: its refresh, the sink's range
 * limits (for the timings the sink did not give in detail), the link
 * (gpu_link_check), the controller's pixel clock and size, and the
 * driver's own check when fits is not NULL.  edid may be NULL (no EDID:
 * no range limits, a DVI sink).
 *
 * Returns GPU_MODES_FITS, or the GPU_MODES_ code of the first check failed.
 */
uint32_t
gpu_modes_check(
	const struct gpu_edid *edid,
	const struct gpu_link_caps *caps,
	gpu_modes_fits_fn fits,
	void *context,
	const struct gpu_display_timing *timing)
{
	uint32_t answer;
	int refused;

	/* A refresh below film's is no desktop mode. */
	if (timing->refresh_mhz < GPU_MODES_RATE_MIN_MHZ)
		return GPU_MODES_SLOW;

	/* The sink's range limits. */
	if (edid != NULL) {
		answer = modes_range(edid, timing);
		if (answer != GPU_MODES_FITS)
			return answer;
	}

	/* The link to the sink. */
	answer = gpu_link_check(caps, edid, timing);
	if (answer != GPU_LINK_FITS)
		return GPU_MODES_LINK + answer;

	/* The controller's pixel clock and size at its present clocks. */
	if (caps->pixel_max_khz != 0U && timing->pixel_khz > caps->pixel_max_khz)
		return GPU_MODES_CLOCK;
	if (caps->width_max != 0U && timing->h_active > caps->width_max)
		return GPU_MODES_SIZE;
	if (caps->height_max != 0U && timing->v_active > caps->height_max)
		return GPU_MODES_SIZE;

	/* The driver's own limits. */
	if (fits != NULL) {
		refused = fits(context, timing);
		if (refused != 0)
			return GPU_MODES_DRIVER;
	}

	/* The output lists the timing. */
	return GPU_MODES_FITS;
}

/*
 * Builds the mode list of an output from its sink's decoded EDID (see
 * gpu_modes_check for what a mode must pass): one mode per size and rate
 * (the first in the EDID's order), the default first.
 *
 * Returns 0 with list filled, or ENOENT with an empty list when there is no
 * EDID (edid NULL) or nothing passes; the driver then keeps the output's
 * present mode as its only one.
 */
int
gpu_modes_build(
	const struct gpu_edid *edid,
	const struct gpu_link_caps *caps,
	gpu_modes_fits_fn fits,
	void *context,
	struct gpu_mode_list *list)
{
	const struct gpu_display_timing *timing;
	uint32_t index;
	uint32_t answer;
	uint32_t chosen;
	int repeat;

	/* Nothing is listed yet; with no EDID nothing will be. */
	list->count = 0U;
	if (edid == NULL)
		return ENOENT;

	/* Lists each timing that passes, once per size and rate. */
	for (index = 0U; index < edid->timing_count; index++) {
		timing = &edid->timings[index];

		/* A timing the output cannot show is left out. */
		answer = gpu_modes_check(edid, caps, fits, context, timing);
		if (answer != GPU_MODES_FITS)
			continue;

		/* A size and rate already listed keeps its first timing. */
		repeat = modes_repeat(list, timing);
		if (repeat)
			continue;

		/* Lists the timing; the decode keeps no more than a list holds. */
		if (list->count >= GPU_MODES_MAX)
			break;
		list->modes[list->count] = *timing;
		list->count++;
	}

	/* An output with nothing to list keeps its present mode. */
	if (list->count == 0U)
		return ENOENT;

	/* Chooses the default and orders the list around it. */
	chosen = modes_default(edid, list);
	modes_order(list, chosen);

	/* Succeeded: modes[0] is the default. */
	return 0;
}

/*
 * Finds the listed mode of a size and rate: the first of that size whose
 * refresh is the same rate (gpu_display_timing_same_rate), or with a
 * refresh of 0 the first of that size (the default, or else the fastest).
 *
 * Returns 0 with *index set, or ENOENT when no listed mode matches.
 */
int
gpu_modes_find(
	const struct gpu_mode_list *list,
	uint32_t width,
	uint32_t height,
	uint32_t refresh_mhz,
	uint32_t *index)
{
	const struct gpu_display_timing *mode;
	uint32_t position;
	int same;

	/* Walks the list in its order, so the default and the fastest come first. */
	for (position = 0U; position < list->count; position++) {
		mode = &list->modes[position];

		/* Another size is another mode. */
		if (mode->h_active != width || mode->v_active != height)
			continue;

		/* Any rate is taken when none is asked for. */
		if (refresh_mhz != 0U) {
			same = gpu_display_timing_same_rate(mode->refresh_mhz, refresh_mhz);
			if (!same)
				continue;
		}

		/* Reports the mode. */
		*index = position;
		return 0;
	}

	/* No listed mode has the size and rate. */
	return ENOENT;
}

/*
 * Tells whether the sink gave a timing in detail (a detailed timing of the
 * base block or of a CTA-861 extension).
 *
 * Returns 1 when it did, 0 when it named the timing only by size and rate.
 */
static int
modes_detailed(
	const struct gpu_display_timing *timing)
{
	/* The two detailed sources. */
	if (timing->source == GPU_DISPLAY_TIMING_DETAILED)
		return 1;
	if (timing->source == GPU_DISPLAY_TIMING_CTA_DETAILED)
		return 1;

	/* A timing named by size and rate. */
	return 0;
}

/*
 * Checks a timing against the sink's range limits descriptor: the vertical
 * and horizontal rates rounded to whole hertz and kHz, and the highest
 * pixel clock.  A timing the sink gave in detail is the sink's own word and
 * is not checked (some sinks state limits narrower than their own detailed
 * timings).
 *
 * Returns GPU_MODES_FITS or GPU_MODES_RANGE.
 */
static uint32_t
modes_range(
	const struct gpu_edid *edid,
	const struct gpu_display_timing *timing)
{
	uint32_t v_hz;
	uint32_t h_khz;
	uint32_t h_total;
	int detailed;

	/* No limits, or a detailed timing, passes. */
	if (edid->has_range == 0U)
		return GPU_MODES_FITS;
	detailed = modes_detailed(timing);
	if (detailed)
		return GPU_MODES_FITS;

	/* The vertical rate, in whole hertz. */
	v_hz = (timing->refresh_mhz + MODES_HALF_HZ_MHZ) / MODES_MHZ_PER_HZ;
	if (v_hz < edid->v_min_hz)
		return GPU_MODES_RANGE;
	if (edid->v_max_hz != 0U && v_hz > edid->v_max_hz)
		return GPU_MODES_RANGE;

	/* The horizontal rate, in whole kHz. */
	h_total = timing->h_active + timing->h_front + timing->h_sync + timing->h_back;
	h_khz = (timing->pixel_khz + h_total / 2U) / h_total;
	if (h_khz < edid->h_min_khz)
		return GPU_MODES_RANGE;
	if (edid->h_max_khz != 0U && h_khz > edid->h_max_khz)
		return GPU_MODES_RANGE;

	/* The highest pixel clock. */
	if (edid->range_pixel_max_khz != 0U && timing->pixel_khz > edid->range_pixel_max_khz)
		return GPU_MODES_RANGE;

	/* The timing is inside the limits. */
	return GPU_MODES_FITS;
}

/*
 * Tells whether the list already has a mode of a timing's size and rate.
 *
 * Returns 1 when it has, 0 when it has not.
 */
static int
modes_repeat(
	const struct gpu_mode_list *list,
	const struct gpu_display_timing *timing)
{
	const struct gpu_display_timing *mode;
	uint32_t index;
	int same;

	/* Compares the size and rate of each listed mode. */
	for (index = 0U; index < list->count; index++) {
		mode = &list->modes[index];
		if (mode->h_active != timing->h_active || mode->v_active != timing->v_active)
			continue;
		same = gpu_display_timing_same_rate(mode->refresh_mhz, timing->refresh_mhz);
		if (same)
			return 1;
	}

	/* The size and rate are new. */
	return 0;
}

/*
 * Chooses the default among the listed modes (see the head of this file).
 *
 * Returns the index of the chosen mode; the list is not empty.
 */
static uint32_t
modes_default(
	const struct gpu_edid *edid,
	const struct gpu_mode_list *list)
{
	const struct gpu_display_timing *mode;
	const struct gpu_display_timing *preferred;
	uint32_t index;
	uint32_t chosen;
	uint32_t found;
	int larger;

	/* The preferred timing, when it is listed at 50 Hz or more. */
	preferred = NULL;
	if (edid->has_preferred != 0U)
		preferred = &edid->preferred;
	if (preferred != NULL && preferred->refresh_mhz >= GPU_MODES_DEFAULT_MIN_MHZ) {
		for (index = 0U; index < list->count; index++) {
			mode = &list->modes[index];
			if (mode->h_active != preferred->h_active || mode->v_active != preferred->v_active)
				continue;
			if (mode->pixel_khz != preferred->pixel_khz)
				continue;
			if (mode->refresh_mhz != preferred->refresh_mhz)
				continue;
			return index;
		}
	}

	/* The fastest listed mode of the preferred size at 50 Hz or more. */
	found = 0U;
	chosen = 0U;
	if (preferred != NULL) {
		for (index = 0U; index < list->count; index++) {
			mode = &list->modes[index];
			if (mode->h_active != preferred->h_active || mode->v_active != preferred->v_active)
				continue;
			if (mode->refresh_mhz < GPU_MODES_DEFAULT_MIN_MHZ)
				continue;
			if (found != 0U && mode->refresh_mhz <= list->modes[chosen].refresh_mhz)
				continue;
			chosen = index;
			found = 1U;
		}
	}

	/* Reports the preferred size at another rate. */
	if (found != 0U)
		return chosen;

	/* The largest listed mode at 50 Hz or more, the fastest of equal size. */
	for (index = 0U; index < list->count; index++) {
		mode = &list->modes[index];
		if (mode->refresh_mhz < GPU_MODES_DEFAULT_MIN_MHZ)
			continue;
		if (found != 0U) {
			larger = modes_larger(mode, &list->modes[chosen]);
			if (!larger)
				continue;
		}
		chosen = index;
		found = 1U;
	}

	/* Reports the largest mode at 50 Hz or more. */
	if (found != 0U)
		return chosen;

	/* The largest listed mode at any rate, the fastest of equal size. */
	chosen = 0U;
	for (index = 1U; index < list->count; index++) {
		larger = modes_larger(&list->modes[index], &list->modes[chosen]);
		if (larger)
			chosen = index;
	}

	/* Reports the largest mode, slow as it is. */
	return chosen;
}

/*
 * Tells whether a mode has more pixels than another, or as many and a
 * higher refresh: the order of the list after its default.
 *
 * Returns 1 when it has, 0 otherwise.
 */
static int
modes_larger(
	const struct gpu_display_timing *first,
	const struct gpu_display_timing *second)
{
	uint64_t first_pixels;
	uint64_t second_pixels;

	/* The active pixels of each. */
	first_pixels = (uint64_t)first->h_active * first->v_active;
	second_pixels = (uint64_t)second->h_active * second->v_active;

	/* More pixels wins; fewer loses. */
	if (first_pixels > second_pixels)
		return 1;
	if (first_pixels < second_pixels)
		return 0;

	/* Equal pixels: the higher refresh wins. */
	if (first->refresh_mhz > second->refresh_mhz)
		return 1;

	/* Not larger. */
	return 0;
}

/*
 * Puts the chosen default first and sorts the rest, keeping the EDID's
 * order between equal modes.
 */
static void
modes_order(
	struct gpu_mode_list *list,
	uint32_t chosen)
{
	struct gpu_display_timing moving;
	uint32_t index;
	uint32_t position;
	int larger;

	/* Moves the default to the front, shifting the modes before it down by one. */
	moving = list->modes[chosen];
	for (position = chosen; position > 0U; position--)
		list->modes[position] = list->modes[position - 1U];
	list->modes[0] = moving;

	/* Inserts each later mode into its place among the sorted ones after the default. */
	for (index = 2U; index < list->count; index++) {
		moving = list->modes[index];
		position = index;
		while (position > 1U) {
			larger = modes_larger(&moving, &list->modes[position - 1U]);
			if (!larger)
				break;
			list->modes[position] = list->modes[position - 1U];
			position--;
		}
		list->modes[position] = moving;
	}
}
