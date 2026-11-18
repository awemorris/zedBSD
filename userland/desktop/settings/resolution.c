/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Display page's resolutions: the sizes in the order they are shown,
 * the refreshes of a size, the refresh a size is asked at (the one shown
 * now when the size has it within 0.05 Hz, else its fastest), and the
 * words beside them.
 */

#include "resolution.h"

#include <stdio.h>

/* How close a refresh must be to the one shown to count as the same (mHz). */
#define RESOLUTION_SAME_MHZ	50U

/*
 * The shapes said by their usual names: the ratio of the width to the
 * height and how far from it (in tenths of a per cent) a size may be and
 * still be called so: 1366x768 is 16:9 but 720x400 is not, and 2560x1080
 * and 3440x1440 are sold as 21:9.
 */
struct resolution_shape {
	uint32_t across;
	uint32_t down;
	uint32_t slack;
	const char *name;
};

/* The shapes, the usual ones of displays; a size of none of them is named by its own ratio when that is short. */
static const struct resolution_shape resolution_shapes[] = {
	{ 16U, 9U, 10U, "16:9" },
	{ 16U, 10U, 10U, "16:10" },
	{ 4U, 3U, 10U, "4:3" },
	{ 5U, 4U, 10U, "5:4" },
	{ 3U, 2U, 10U, "3:2" },
	{ 21U, 9U, 30U, "21:9" },
	{ 32U, 9U, 10U, "32:9" }
};

static int resolution_has_size(const struct se_resolution_size *sizes, unsigned count, uint32_t width, uint32_t height, unsigned *slot);
static uint32_t resolution_divisor(uint32_t first, uint32_t second);

/*
 * Makes the sizes a display offers from its modes (the compositor's,
 * larger first): each size once, in the modes' order, marked native or
 * current when one of its refreshes is.  Returns how many were written
 * (at most capacity).
 */
unsigned
se_resolution_sizes(
	const struct kl_display_mode *modes,
	size_t count,
	struct se_resolution_size *sizes,
	unsigned capacity)
{
	const struct kl_display_mode *mode;
	unsigned made;
	unsigned slot;
	size_t index;
	int known;

	/* Each mode: a new size, or the marks of one already made. */
	made = 0U;
	for (index = 0U; index < count; index++) {
		mode = &modes[index];
		known = resolution_has_size(sizes, made, mode->width, mode->height, &slot);
		if (!known) {
			/* No room for another size. */
			if (made == capacity)
				continue;

			/* The size, without marks yet. */
			slot = made;
			sizes[slot].width = mode->width;
			sizes[slot].height = mode->height;
			sizes[slot].flags = 0U;
			made++;
		}

		/* The marks of its refreshes. */
		if ((mode->flags & KL_DISPLAY_MODE_NATIVE) != 0U)
			sizes[slot].flags |= SE_RESOLUTION_NATIVE;
		if ((mode->flags & KL_DISPLAY_MODE_CURRENT) != 0U)
			sizes[slot].flags |= SE_RESOLUTION_CURRENT;
	}

	/* Succeeded: the sizes made. */
	return made;
}

/*
 * Gives the refreshes a display offers at a size, the fastest first (in
 * mHz, each once).  Returns how many were written (at most capacity).
 */
unsigned
se_resolution_refreshes(
	const struct kl_display_mode *modes,
	size_t count,
	uint32_t width,
	uint32_t height,
	uint32_t *refreshes,
	unsigned capacity)
{
	uint32_t refresh;
	unsigned made;
	unsigned place;
	unsigned later;
	size_t index;
	int seen;

	/* Each mode of the size, put in its place among the faster and the slower ones. */
	made = 0U;
	for (index = 0U; index < count; index++) {
		if (modes[index].width != width || modes[index].height != height)
			continue;
		refresh = modes[index].refresh_mhz;

		/* A refresh written already is not written again. */
		seen = 0;
		for (place = 0U; place < made; place++) {
			if (refreshes[place] == refresh)
				seen = 1;
		}
		if (seen || made == capacity)
			continue;

		/* Its place: after every faster one; the slower ones move down. */
		for (place = 0U; place < made && refreshes[place] > refresh; place++)
			continue;
		for (later = made; later > place; later--)
			refreshes[later] = refreshes[later - 1U];
		refreshes[place] = refresh;
		made++;
	}

	/* Succeeded: the refreshes written. */
	return made;
}

/*
 * Gives the refresh a size is asked at: the one shown now when the size
 * has one within RESOLUTION_SAME_MHZ of it, else its fastest; 0 for a size
 * the display does not offer.
 */
uint32_t
se_resolution_refresh_for(
	const struct kl_display_mode *modes,
	size_t count,
	uint32_t width,
	uint32_t height,
	uint32_t shown_mhz)
{
	uint32_t fastest;
	uint32_t apart;
	size_t index;

	/* The size's refreshes: the one close to the refresh shown, and the fastest. */
	fastest = 0U;
	for (index = 0U; index < count; index++) {
		if (modes[index].width != width || modes[index].height != height)
			continue;

		/* How far it is from the refresh shown now. */
		apart = shown_mhz - modes[index].refresh_mhz;
		if (modes[index].refresh_mhz > shown_mhz)
			apart = modes[index].refresh_mhz - shown_mhz;
		if (apart < RESOLUTION_SAME_MHZ)
			return modes[index].refresh_mhz;

		/* The fastest so far. */
		if (modes[index].refresh_mhz > fastest)
			fastest = modes[index].refresh_mhz;
	}

	/* Succeeded: the fastest of the size (0 for none). */
	return fastest;
}

/*
 * Finds the mode with a mark (KL_DISPLAY_MODE_NATIVE or _CURRENT).
 * Returns 1 with it, 0 when no mode has it.
 */
int
se_resolution_marked(
	const struct kl_display_mode *modes,
	size_t count,
	unsigned flag,
	struct kl_display_mode *mode)
{
	size_t index;

	/* The first mode with the mark. */
	for (index = 0U; index < count; index++) {
		if ((modes[index].flags & flag) == 0U)
			continue;
		*mode = modes[index];
		return 1;
	}

	/* None has it. */
	return 0;
}

/*
 * Writes the words of a size's shape: its usual name ("16:9"), else its
 * own ratio when both numbers are short ("9:5"), else nothing.
 */
void
se_resolution_aspect(
	uint32_t width,
	uint32_t height,
	char *text,
	size_t size)
{
	const struct resolution_shape *shape;
	uint64_t wide;
	uint64_t tall;
	uint64_t apart;
	uint32_t divisor;
	size_t index;

	/* Nothing for an empty size. */
	text[0] = '\0';
	if (width == 0U || height == 0U)
		return;

	/* A usual shape: the width over the height near its ratio (compared as across * height and down * width). */
	for (index = 0U; index < sizeof(resolution_shapes) / sizeof(resolution_shapes[0]); index++) {
		shape = &resolution_shapes[index];
		wide = (uint64_t)width * shape->down;
		tall = (uint64_t)height * shape->across;
		apart = wide - tall;
		if (tall > wide)
			apart = tall - wide;
		if (apart * 1000U > tall * shape->slack)
			continue;
		(void)snprintf(text, size, "%s", shape->name);
		return;
	}

	/* The size's own ratio, when it is short. */
	divisor = resolution_divisor(width, height);
	if (width / divisor > 32U || height / divisor > 32U)
		return;
	(void)snprintf(text, size, "%lu:%lu", (unsigned long)(width / divisor), (unsigned long)(height / divisor));
}

/*
 * Writes a refresh in hertz with at most two decimals, none for a whole
 * number ("60 Hz", "59.94 Hz", "59.9 Hz").
 */
void
se_resolution_hertz(
	uint32_t refresh_mhz,
	char *text,
	size_t size)
{
	unsigned long hundredths;

	/* The refresh rounded to hundredths of a hertz. */
	hundredths = ((unsigned long)refresh_mhz + 5UL) / 10UL;

	/* A whole number. */
	if (hundredths % 100UL == 0UL) {
		(void)snprintf(text, size, "%lu Hz", hundredths / 100UL);
		return;
	}

	/* One decimal, when the second is 0. */
	if (hundredths % 10UL == 0UL) {
		(void)snprintf(text, size, "%lu.%lu Hz", hundredths / 100UL, (hundredths / 10UL) % 10UL);
		return;
	}

	/* Two decimals. */
	(void)snprintf(text, size, "%lu.%02lu Hz", hundredths / 100UL, hundredths % 100UL);
}

/*
 * Gives the reason the page says below a display's sizes
 * (SE_RESOLUTION_REASON_*) from why none can be chosen now
 * (KL_DISPLAY_RESOLUTION_WHY_*): the first that applies, the one that
 * leaves the user the least choice first.
 */
unsigned
se_resolution_reason(
	unsigned why)
{
	/* The display offers one mode. */
	if ((why & KL_DISPLAY_RESOLUTION_WHY_FIXED) != 0U)
		return SE_RESOLUTION_REASON_FIXED;

	/* The desktop is shown on another display. */
	if ((why & KL_DISPLAY_RESOLUTION_WHY_ANCHOR) != 0U)
		return SE_RESOLUTION_REASON_ANCHOR;

	/* Another display is shown beside it. */
	if ((why & KL_DISPLAY_RESOLUTION_WHY_ALONE) != 0U)
		return SE_RESOLUTION_REASON_ALONE;

	/* A game is shown. */
	if ((why & KL_DISPLAY_RESOLUTION_WHY_GAME) != 0U)
		return SE_RESOLUTION_REASON_GAME;

	/* Succeeded: nothing to say. */
	return SE_RESOLUTION_REASON_NONE;
}

/* Gives the whole seconds a trial has left, counted up (1 ms left is 1 second). */
unsigned
se_resolution_seconds(
	uint32_t remaining_ms)
{
	/* Rounded up. */
	return (unsigned)(((uint64_t)remaining_ms + 999U) / 1000U);
}

/* Tells whether a size is among those made; sets its place when it is. */
static int
resolution_has_size(
	const struct se_resolution_size *sizes,
	unsigned count,
	uint32_t width,
	uint32_t height,
	unsigned *slot)
{
	unsigned index;

	/* Each size made. */
	for (index = 0U; index < count; index++) {
		if (sizes[index].width != width || sizes[index].height != height)
			continue;
		*slot = index;
		return 1;
	}

	/* Not made yet. */
	return 0;
}

/* Gives the greatest common divisor of two numbers that are not 0. */
static uint32_t
resolution_divisor(
	uint32_t first,
	uint32_t second)
{
	uint32_t rest;

	/* Euclid's steps. */
	while (second != 0U) {
		rest = first % second;
		first = second;
		second = rest;
	}

	/* Succeeded: the divisor. */
	return first;
}
