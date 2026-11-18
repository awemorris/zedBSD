/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Decoding the first OSD plane's words, and the address word of a flip.
 */

#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/mali/display/osd.h"

/* The highest address the 32-bit address word reaches in 16-byte units. */
#define ADDRESS_LIMIT		(0xffffffffULL << MALI_OSD_UNIT_SHIFT)

/*
 * Decodes the words of a plane into its buffer, pitch and window.
 *
 * ENODEV when the plane is off; ENOTSUP when it reads a canvas rather
 * than a linear buffer, or when its window does not start at the
 * buffer's top left corner (the node only flips whole buffers).
 */
int
mali_osd_decode(
	struct mali_osd_plane *plane)
{
	uint32_t first;
	uint32_t last;

	/* The plane and its first block must both be on. */
	if ((plane->control & MALI_OSD_CONTROL_ENABLE) == 0U)
		return ENODEV;
	if ((plane->control & MALI_OSD_CONTROL_BLOCK) == 0U)
		return ENODEV;

	/* Only a linear buffer has an address the node can read and change. */
	if ((plane->control & MALI_OSD_CONTROL_LINEAR) == 0U)
		return ENOTSUP;

	/* The buffer's address and row pitch, both in 16-byte units. */
	plane->buffer = (uint64_t)plane->address << MALI_OSD_UNIT_SHIFT;
	plane->pitch = (plane->stride & MALI_OSD_STRIDE_MASK) << MALI_OSD_UNIT_SHIFT;

	/* The window's columns, from the first. */
	first = plane->window_x & MALI_OSD_WINDOW_FIRST_MASK;
	last = (plane->window_x >> MALI_OSD_WINDOW_LAST_SHIFT) & MALI_OSD_WINDOW_FIRST_MASK;
	if (first != 0U || last < first)
		return ENOTSUP;
	plane->width = last - first + 1U;

	/* The window's rows, from the first. */
	first = plane->window_y & MALI_OSD_WINDOW_FIRST_MASK;
	last = (plane->window_y >> MALI_OSD_WINDOW_LAST_SHIFT) & MALI_OSD_WINDOW_FIRST_MASK;
	if (first != 0U || last < first)
		return ENOTSUP;
	plane->height = last - first + 1U;

	/* Succeeded: the plane shows a linear buffer from its corner. */
	return 0;
}

/*
 * Reports whether a decoded plane shows a given buffer.
 */
int
mali_osd_shows(
	const struct mali_osd_plane *plane,
	uint64_t address,
	uint32_t width,
	uint32_t height,
	uint32_t pitch)
{
	/* The same memory. */
	if (plane->buffer != address)
		return 0;

	/* The same extent and the same distance between rows. */
	if (plane->width != width || plane->height != height)
		return 0;
	if (plane->pitch != pitch)
		return 0;

	/* Succeeded: the plane shows the buffer. */
	return 1;
}

/*
 * Builds the address word that points the plane at a buffer.
 */
int
mali_osd_address_word(
	uint64_t address,
	uint32_t *word)
{
	/* The word names 16-byte units, below 64 GiB. */
	if ((address & ((1ULL << MALI_OSD_UNIT_SHIFT) - 1U)) != 0U)
		return EINVAL;
	if (address > ADDRESS_LIMIT)
		return ERANGE;

	/* Succeeded: the word for the address. */
	*word = (uint32_t)(address >> MALI_OSD_UNIT_SHIFT);
	return 0;
}
