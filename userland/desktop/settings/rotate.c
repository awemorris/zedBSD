/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Display page's angles (ws217-p004; the design is
 * plan/ws217/phase001/phase.md, section 7.3): the pictures in their order,
 * which can be pressed, and the reason said below them.
 */

#include "rotate.h"

#include <keiland/keiland.h>

static unsigned rotate_bit(unsigned angle);

/*
 * Gives the angle a picture of a display's row stands for, clockwise in
 * degrees (KL_DISPLAY_ROTATE_*), by its place from the left; 0 past the
 * last.
 */
unsigned
se_rotate_angle(
	unsigned slot)
{
	/* Upright, on its left side, upside down, on its right side. */
	switch (slot) {
	case 1U:
		return KL_DISPLAY_ROTATE_90;
	case 2U:
		return KL_DISPLAY_ROTATE_180;
	case 3U:
		return KL_DISPLAY_ROTATE_270;
	default:
		break;
	}

	/* Upright. */
	return KL_DISPLAY_ROTATE_0;
}

/*
 * Tells whether the picture of an angle can be pressed: an angle the
 * display may take now (mask, KL_DISPLAY_ROTATION_*) that is not the one
 * chosen already (pressing that one would change nothing).
 */
int
se_rotate_pressable(
	unsigned angle,
	unsigned selected,
	unsigned mask)
{
	unsigned bit;

	/* The angle chosen already. */
	if (angle == selected)
		return 0;

	/* An angle the display may not take now. */
	bit = rotate_bit(angle);
	if ((mask & bit) == 0U)
		return 0;

	/* Succeeded: pressing it turns the display. */
	return 1;
}

/*
 * Gives the reason the page says below a display's pictures
 * (SE_ROTATE_REASON_*) from why it may not take some angles or does not
 * show the one chosen (KL_DISPLAY_ROTATE_WHY_*): the first that applies,
 * the one that leaves the user the least choice first.
 */
unsigned
se_rotate_reason(
	unsigned why)
{
	/* Nothing turns the picture of this display. */
	if ((why & KL_DISPLAY_ROTATE_WHY_SCANOUT) != 0U)
		return SE_ROTATE_REASON_FIXED;

	/* Another display is shown beside it. */
	if ((why & KL_DISPLAY_ROTATE_WHY_ALONE) != 0U)
		return SE_ROTATE_REASON_ALONE;

	/* The angle chosen is not shown: the turn's images could not be made. */
	if ((why & KL_DISPLAY_ROTATE_WHY_RESOURCE) != 0U)
		return SE_ROTATE_REASON_RESOURCE;

	/* The angle chosen is not shown while a game is. */
	if ((why & KL_DISPLAY_ROTATE_WHY_GAME) != 0U)
		return SE_ROTATE_REASON_GAME;

	/* Only 0 and 180 on this device. */
	if ((why & KL_DISPLAY_ROTATE_WHY_TURN) != 0U)
		return SE_ROTATE_REASON_QUARTER;

	/* Succeeded: nothing to say. */
	return SE_ROTATE_REASON_NONE;
}

/* Gives an angle's bit of a display's mask (KL_DISPLAY_ROTATION_*), or 0 for a number that is not an angle. */
static unsigned
rotate_bit(
	unsigned angle)
{
	/* Each angle has its own bit. */
	switch (angle) {
	case KL_DISPLAY_ROTATE_0:
		return KL_DISPLAY_ROTATION_0;
	case KL_DISPLAY_ROTATE_90:
		return KL_DISPLAY_ROTATION_90;
	case KL_DISPLAY_ROTATE_180:
		return KL_DISPLAY_ROTATION_180;
	case KL_DISPLAY_ROTATE_270:
		return KL_DISPLAY_ROTATION_270;
	default:
		break;
	}

	/* Not an angle. */
	return 0U;
}
