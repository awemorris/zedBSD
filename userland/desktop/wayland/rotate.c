/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arithmetic of a rotated display (WS217; rotate.h says what and why):
 * the logical and physical points, rectangles and vectors, the angles a
 * display may take, and how an angle is shown.  The sizes and the steps of
 * a change are rotate.h's inline functions.
 */

#include "rotate.h"

#include <errno.h>
#include <stddef.h>

/* The 24.8 fixed-point coordinates' sub-pixels in a pixel. */
#define ROTATE_FIXED_ONE	256U

/* Tells whether a number is one of the four angles. */
int
kwl_rotate_valid(
	unsigned rotation)
{
	unsigned bit;

	/* An angle has its bit of the mask. */
	bit = kwl_rotate_bit(rotation);
	if (bit == 0U)
		return 0;

	/* Succeeded: one of the four. */
	return 1;
}

/* Gives an angle's bit of a display's mask (KWL_ROTATION_*), or 0 for a number that is not an angle. */
unsigned
kwl_rotate_bit(
	unsigned rotation)
{
	/* Each angle has its own bit. */
	switch (rotation) {
	case KWL_ROTATE_0:
		return KWL_ROTATION_0;
	case KWL_ROTATE_90:
		return KWL_ROTATION_90;
	case KWL_ROTATE_180:
		return KWL_ROTATION_180;
	case KWL_ROTATE_270:
		return KWL_ROTATION_270;
	default:
		break;
	}

	/* Not an angle. */
	return 0U;
}

/* Gives the physical pixel a logical pixel is shown at, on a panel of a physical size. */
void
kwl_rotate_point(
	unsigned rotation,
	uint32_t physical_width,
	uint32_t physical_height,
	int32_t x,
	int32_t y,
	int32_t *physical_x,
	int32_t *physical_y)
{
	/* The pixel's place for each angle (rotate.h). */
	switch (rotation) {
	case KWL_ROTATE_90:
		*physical_x = (int32_t)physical_width - 1 - y;
		*physical_y = x;
		break;
	case KWL_ROTATE_180:
		*physical_x = (int32_t)physical_width - 1 - x;
		*physical_y = (int32_t)physical_height - 1 - y;
		break;
	case KWL_ROTATE_270:
		*physical_x = y;
		*physical_y = (int32_t)physical_height - 1 - x;
		break;
	default:
		*physical_x = x;
		*physical_y = y;
		break;
	}
}

/* Gives the logical pixel a physical pixel shows (kwl_rotate_point undone). */
void
kwl_rotate_point_back(
	unsigned rotation,
	uint32_t physical_width,
	uint32_t physical_height,
	int32_t physical_x,
	int32_t physical_y,
	int32_t *x,
	int32_t *y)
{
	/* The pixel's place for each angle, the other way. */
	switch (rotation) {
	case KWL_ROTATE_90:
		*x = physical_y;
		*y = (int32_t)physical_width - 1 - physical_x;
		break;
	case KWL_ROTATE_180:
		*x = (int32_t)physical_width - 1 - physical_x;
		*y = (int32_t)physical_height - 1 - physical_y;
		break;
	case KWL_ROTATE_270:
		*x = (int32_t)physical_height - 1 - physical_y;
		*y = physical_x;
		break;
	default:
		*x = physical_x;
		*y = physical_y;
		break;
	}
}

/*
 * Gives the physical place of a logical 24.8 fixed-point point: the pixels'
 * rule on a grid of 256 sub-pixels to the pixel, so that the place's pixel
 * is the one kwl_rotate_point gives and the way back is exact.
 */
void
kwl_rotate_fixed(
	unsigned rotation,
	uint32_t physical_width,
	uint32_t physical_height,
	int32_t x,
	int32_t y,
	int32_t *physical_x,
	int32_t *physical_y)
{
	/* The panel's size in sub-pixels. */
	kwl_rotate_point(rotation, physical_width * ROTATE_FIXED_ONE, physical_height * ROTATE_FIXED_ONE, x, y, physical_x, physical_y);
}

/* Gives the logical place of a physical 24.8 fixed-point point (kwl_rotate_fixed undone). */
void
kwl_rotate_fixed_back(
	unsigned rotation,
	uint32_t physical_width,
	uint32_t physical_height,
	int32_t physical_x,
	int32_t physical_y,
	int32_t *x,
	int32_t *y)
{
	/* The panel's size in sub-pixels. */
	kwl_rotate_point_back(rotation, physical_width * ROTATE_FIXED_ONE, physical_height * ROTATE_FIXED_ONE, physical_x, physical_y, x, y);
}

/*
 * Gives the physical rectangle a logical one is shown at: both half-open,
 * (left, top, right, bottom).
 */
void
kwl_rotate_rect(
	unsigned rotation,
	uint32_t physical_width,
	uint32_t physical_height,
	const int32_t *box,
	int32_t *physical_box)
{
	int32_t width;
	int32_t height;

	/* The panel's edges. */
	width = (int32_t)physical_width;
	height = (int32_t)physical_height;

	/* The rectangle's edges for each angle (the far edge of one is the near edge of the other). */
	switch (rotation) {
	case KWL_ROTATE_90:
		physical_box[0] = width - box[3];
		physical_box[1] = box[0];
		physical_box[2] = width - box[1];
		physical_box[3] = box[2];
		break;
	case KWL_ROTATE_180:
		physical_box[0] = width - box[2];
		physical_box[1] = height - box[3];
		physical_box[2] = width - box[0];
		physical_box[3] = height - box[1];
		break;
	case KWL_ROTATE_270:
		physical_box[0] = box[1];
		physical_box[1] = height - box[2];
		physical_box[2] = box[3];
		physical_box[3] = height - box[0];
		break;
	default:
		physical_box[0] = box[0];
		physical_box[1] = box[1];
		physical_box[2] = box[2];
		physical_box[3] = box[3];
		break;
	}
}

/* Gives the logical rectangle a physical one shows (kwl_rotate_rect undone). */
void
kwl_rotate_rect_back(
	unsigned rotation,
	uint32_t physical_width,
	uint32_t physical_height,
	const int32_t *physical_box,
	int32_t *box)
{
	int32_t width;
	int32_t height;

	/* The panel's edges. */
	width = (int32_t)physical_width;
	height = (int32_t)physical_height;

	/* The rectangle's edges for each angle, the other way. */
	switch (rotation) {
	case KWL_ROTATE_90:
		box[0] = physical_box[1];
		box[1] = width - physical_box[2];
		box[2] = physical_box[3];
		box[3] = width - physical_box[0];
		break;
	case KWL_ROTATE_180:
		box[0] = width - physical_box[2];
		box[1] = height - physical_box[3];
		box[2] = width - physical_box[0];
		box[3] = height - physical_box[1];
		break;
	case KWL_ROTATE_270:
		box[0] = height - physical_box[3];
		box[1] = physical_box[0];
		box[2] = height - physical_box[1];
		box[3] = physical_box[2];
		break;
	default:
		box[0] = physical_box[0];
		box[1] = physical_box[1];
		box[2] = physical_box[2];
		box[3] = physical_box[3];
		break;
	}
}

/*
 * Gives the logical direction of a physical one (a pen's tilt): turned
 * back by the angle, with no origin to move.
 */
void
kwl_rotate_vector(
	unsigned rotation,
	int32_t physical_x,
	int32_t physical_y,
	int32_t *x,
	int32_t *y)
{
	/* The direction's parts for each angle. */
	switch (rotation) {
	case KWL_ROTATE_90:
		*x = physical_y;
		*y = -physical_x;
		break;
	case KWL_ROTATE_180:
		*x = -physical_x;
		*y = -physical_y;
		break;
	case KWL_ROTATE_270:
		*x = -physical_y;
		*y = physical_x;
		break;
	default:
		*x = physical_x;
		*y = physical_y;
		break;
	}
}

/*
 * Gives the angles a display may take (KWL_ROTATION_*), and why it cannot
 * take the others (KWL_ROTATE_WHY_*): 0 always, 180 when its hardware turns
 * the picture, and every angle when the turn pass is offered on this
 * device (turn_default) and built (turn_build), 180 then by the turn where
 * the hardware cannot (ws217-p004: Settings turns the displays with the
 * turn pass before the hardware's 180 is read, ws217-p010).  The test
 * policy allows all four where the build has the turn pass, even where it
 * is not offered by default.
 */
unsigned
kwl_rotate_mask(
	unsigned hw180,
	unsigned turn_default,
	unsigned turn_build,
	unsigned policy,
	unsigned *why)
{
	unsigned turn_allowed;
	unsigned mask;

	/* The turn pass, where it is offered and built. */
	turn_allowed = 0U;
	if (turn_default && turn_build)
		turn_allowed = 1U;

	/* A test's policy allows the turn on any display, where the build has it. */
	if (policy == KWL_ROTATE_POLICY_ANY && turn_build)
		turn_allowed = 1U;

	/* The angles: 0 always, 180 by the hardware, every one by the turn. */
	mask = KWL_ROTATION_0;
	if (hw180)
		mask |= KWL_ROTATION_180;
	if (turn_allowed)
		mask |= KWL_ROTATION_90 | KWL_ROTATION_180 | KWL_ROTATION_270;

	/* Why not: nothing turns the picture at all, or only the quarter turns are missing. */
	*why = 0U;
	if ((mask & KWL_ROTATION_180) == 0U) {
		*why |= KWL_ROTATE_WHY_SCANOUT;
	} else if ((mask & KWL_ROTATION_90) == 0U) {
		*why |= KWL_ROTATE_WHY_TURN;
	}

	/* Succeeded: the angles the display may take. */
	return mask;
}

/*
 * Decides how an angle is shown: by the display's hardware (180, the
 * scanout's angle) or by the compositor's turn pass (90 and 270, and 180
 * on a display whose hardware cannot when the mask allows it).  Returns 0,
 * EINVAL for a number that is not an angle, or ENOTSUP for an angle the
 * display's mask does not allow (nothing is given then).
 */
int
kwl_rotate_plan(
	unsigned rotation,
	unsigned mask,
	unsigned hw180,
	unsigned *scanout,
	unsigned *turn)
{
	unsigned bit;

	/* An angle, and one the display may take. */
	bit = kwl_rotate_bit(rotation);
	if (bit == 0U)
		return EINVAL;
	if ((mask & bit) == 0U)
		return ENOTSUP;

	/* Nothing turns an unrotated picture. */
	*scanout = KWL_ROTATE_0;
	*turn = KWL_ROTATE_0;
	if (rotation == KWL_ROTATE_0)
		return 0;

	/* The display's hardware turns it by 180 when it can. */
	if (rotation == KWL_ROTATE_180 && hw180) {
		*scanout = KWL_ROTATE_180;
		return 0;
	}

	/* Succeeded: the compositor's turn pass turns it. */
	*turn = rotation;
	return 0;
}

/*
 * Gives what Settings is told of a display's angles (ws217-p004): the
 * angles it may take and why not, as they are while it is shown alone;
 * beside another display shown (alone 0), only the angle chosen, with the
 * reason that the others wait for it to be shown alone (a head's angle and
 * the anchor's beside one come with ws217-p005).
 */
void
kwl_rotate_report(
	unsigned selected,
	unsigned mask,
	unsigned why,
	unsigned alone,
	unsigned *told_mask,
	unsigned *told_why)
{
	unsigned bit;

	/* Shown alone: the display's own angles and reasons. */
	*told_mask = mask;
	*told_why = why;
	if (alone)
		return;

	/* Beside another display: the angle chosen alone (0 for a number that is not an angle), and why. */
	bit = kwl_rotate_bit(selected);
	if (bit == 0U)
		bit = KWL_ROTATION_0;
	*told_mask = bit;
	*told_why = why | KWL_ROTATE_WHY_ALONE;
}

/*
 * Gives the turn pass's texture coordinates for an angle (ws217-p002b):
 * the turn image is sampled at (u[0] * cx + u[1] * cy + u[2],
 * v[0] * cx + v[1] * cy + v[2]) at the physical corner (cx, cy) of the
 * whole output, each 0 or 1 (y downward).  The corners are a permutation
 * of the logical image's, so the middle of each physical pixel falls on
 * the middle of the logical pixel kwl_rotate_point_back gives, whatever
 * the sizes.
 */
void
kwl_turn_coefficients(
	unsigned rotation,
	float *u,
	float *v)
{
	/* Nothing turned: the corners as they are. */
	u[0] = 1.0f;
	u[1] = 0.0f;
	u[2] = 0.0f;
	v[0] = 0.0f;
	v[1] = 1.0f;
	v[2] = 0.0f;

	/* The logical picture for each quarter of a turn. */
	switch (rotation) {
	case KWL_ROTATE_90:
		/* The physical y runs along the logical x, the physical x back along the logical y. */
		u[0] = 0.0f;
		u[1] = 1.0f;
		u[2] = 0.0f;
		v[0] = -1.0f;
		v[1] = 0.0f;
		v[2] = 1.0f;
		break;
	case KWL_ROTATE_180:
		/* Both sides run backward. */
		u[0] = -1.0f;
		u[1] = 0.0f;
		u[2] = 1.0f;
		v[0] = 0.0f;
		v[1] = -1.0f;
		v[2] = 1.0f;
		break;
	case KWL_ROTATE_270:
		/* The physical y runs back along the logical x, the physical x along the logical y. */
		u[0] = 0.0f;
		u[1] = -1.0f;
		u[2] = 1.0f;
		v[0] = 1.0f;
		v[1] = 0.0f;
		v[2] = 0.0f;
		break;
	default:
		break;
	}
}
