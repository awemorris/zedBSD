/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arithmetic of a rotated display (rotate.c, WS217; the design is
 * plan/ws217/phase001/phase.md, sections 2, 4.6 and 4.10).
 *
 * A display has a physical size, its panel's rows and columns (the mode,
 * the swapchain, the scanout), and a logical one, the desktop the user sees
 * on it.  The rotation is the angle, clockwise in degrees (0, 90, 180 or
 * 270), by which the picture is turned on the panel; at 90 and 270 the
 * logical width is the physical height.  The display's hardware turns the
 * picture by 180 (the scanout rotation), the compositor's turn pass by 90
 * or 270 (the turn rotation); one of the two at most is not 0.
 *
 * Points are pixels (whole numbers, a pixel's index) or 24.8 fixed-point
 * coordinates; rectangles are half-open (left, top, right, bottom).  The
 * physical width and height are Wp and Hp.  A logical point (x, y) is shown
 * at the physical point:
 *
 *   0:   (x, y)
 *   90:  (Wp - 1 - y, x)
 *   180: (Wp - 1 - x, Hp - 1 - y)
 *   270: (y, Hp - 1 - x)
 *
 * Nothing here knows the server or Vulkan, so the host tests run it alone.
 */

#ifndef KWL_ROTATE_H
#define KWL_ROTATE_H

#include <stdint.h>

/* The angles, clockwise in degrees. */
#define KWL_ROTATE_0		0U
#define KWL_ROTATE_90		90U
#define KWL_ROTATE_180		180U
#define KWL_ROTATE_270		270U

/* The angles a display may take, as a mask (the same bits as libkeiland's KL_DISPLAY_ROTATION_*). */
#define KWL_ROTATION_0		1U
#define KWL_ROTATION_90		2U
#define KWL_ROTATION_180	4U
#define KWL_ROTATION_270	8U

/*
 * Why a display cannot take an angle, or does not show the one chosen (the
 * same bits as libkeiland's KL_DISPLAY_ROTATE_WHY_*): its hardware cannot
 * turn the picture by 180 (nothing can be rotated), this build has no turn
 * pass (90 and 270), a game is shown (the turn is paused), the turn's
 * images could not be made, or another display is shown beside it (a
 * head's angle, and the anchor's beside one, come with ws217-p005).
 */
#define KWL_ROTATE_WHY_SCANOUT	1U
#define KWL_ROTATE_WHY_TURN	2U
#define KWL_ROTATE_WHY_GAME	4U
#define KWL_ROTATE_WHY_RESOURCE	8U
#define KWL_ROTATE_WHY_ALONE	16U

/* The policies: the angles the display's hardware allows, or all of them (a test's --rotate-policy=any). */
#define KWL_ROTATE_POLICY_DEFAULT	0U
#define KWL_ROTATE_POLICY_ANY		1U

/*
 * Whether this build has the compositor's turn pass (90 and 270, and 180
 * where the hardware cannot): every build has its code (ws217-p002b).
 */
#define KWL_TURN_BUILD		1U

/*
 * The GPU whose output the turn pass is not offered on without the test
 * policy (its PCI vendor, VkPhysicalDeviceProperties.vendorID): the
 * Raspberry Pi 4's (Broadcom) until WS141 p015, as it samples images in
 * its own tiled layout and a turn image drawn in the raster layout would
 * be converted on every frame, which the Guardrail forbids.  A test's
 * policy still allows it there, to measure.
 */
#define KWL_TURN_VENDOR_OFF	0x14e4U

/*
 * What an output (or a head) shows, before and after a change: its logical
 * size, the angle the user chose, the angle shown (0 while a game pauses
 * the turn), and how it is shown (the scanout's angle and the turn's).
 */
struct kwl_rotate_state {
	uint32_t width;
	uint32_t height;
	unsigned selected;
	unsigned rotation;
	unsigned scanout;
	unsigned turn;
};

/*
 * A floating window an output's new size moved to fit it (BUG-306,
 * kwl_rotate_fit_window): the place it had before (home_x, home_y) and
 * the place it was moved to (x, y).  While the window stays there, the
 * next new size starts from the place it had before, so that a display
 * turned and turned back puts it where it was.
 */
struct kwl_fit_home {
	unsigned moved;
	int32_t home_x;
	int32_t home_y;
	int32_t x;
	int32_t y;
};

/*
 * The steps a change of an output's state takes (kwl_rotate_steps): the
 * output opened again (the scanout's angle is a property of its surface),
 * the turn's images exchanged, the input's mapping and the clients told
 * of a new angle, the desktop fitted to a new size, and the choice saved.
 */
#define KWL_ROTATE_STEP_REOPEN	1U
#define KWL_ROTATE_STEP_TURN	2U
#define KWL_ROTATE_STEP_ROTATED	4U
#define KWL_ROTATE_STEP_RESIZED	8U
#define KWL_ROTATE_STEP_SAVE	16U

/*
 * How long an output turned to another size shows its old frame at most
 * while its windows draw the new size (ms, BUG-306): a window that has not
 * by then is shown as it draws.
 */
#define KWL_ROTATE_HOLD_MS	1000U

/*
 * Who changes the state (kwl_rotate_steps, review-4 S16 and L49): the user
 * (Settings' set_rotation, whose change of choice is saved), an output's
 * opening (which made the surface and the turn's images itself, so it
 * takes neither of those steps), and a game's pause of the turn and its
 * end (which save nothing, and whose new size fits no window).
 */
#define KWL_ROTATE_MODE_USER		0U
#define KWL_ROTATE_MODE_OPEN		1U
#define KWL_ROTATE_MODE_SESSION_PAUSE	2U
#define KWL_ROTATE_MODE_SESSION_RESUME	3U

/*
 * A test's policy that no longer allows the angle shown (kwl_output_policy_set,
 * WS217): the turn changes as at the user's choice, but nothing is saved.
 */
#define KWL_ROTATE_MODE_POLICY		4U

/*
 * What a user's choice of an angle does (kwl_rotate_select, review-4 S12):
 * refused (an angle the display may not take), nothing (the same choice,
 * already shown), saved only (the output is closed: its next opening shows
 * it), saved with the direct scanout left (the opening after it shows it),
 * or applied now.
 */
#define KWL_ROTATE_SELECT_REFUSE	0U
#define KWL_ROTATE_SELECT_NOTHING	1U
#define KWL_ROTATE_SELECT_SAVE		2U
#define KWL_ROTATE_SELECT_LEAVE		3U
#define KWL_ROTATE_SELECT_APPLY		4U

int kwl_rotate_valid(unsigned rotation);
unsigned kwl_rotate_bit(unsigned rotation);
void kwl_rotate_point(unsigned rotation, uint32_t physical_width, uint32_t physical_height, int32_t x, int32_t y, int32_t *physical_x, int32_t *physical_y);
void kwl_rotate_point_back(unsigned rotation, uint32_t physical_width, uint32_t physical_height, int32_t physical_x, int32_t physical_y, int32_t *x, int32_t *y);
void kwl_rotate_fixed(unsigned rotation, uint32_t physical_width, uint32_t physical_height, int32_t x, int32_t y, int32_t *physical_x, int32_t *physical_y);
void kwl_rotate_fixed_back(unsigned rotation, uint32_t physical_width, uint32_t physical_height, int32_t physical_x, int32_t physical_y, int32_t *x, int32_t *y);
void kwl_rotate_rect(unsigned rotation, uint32_t physical_width, uint32_t physical_height, const int32_t *box, int32_t *physical_box);
void kwl_rotate_rect_back(unsigned rotation, uint32_t physical_width, uint32_t physical_height, const int32_t *physical_box, int32_t *box);
void kwl_rotate_vector(unsigned rotation, int32_t physical_x, int32_t physical_y, int32_t *x, int32_t *y);
unsigned kwl_rotate_mask(unsigned hw180, unsigned turn_default, unsigned turn_build, unsigned policy, unsigned *why);
int kwl_rotate_plan(unsigned rotation, unsigned mask, unsigned hw180, unsigned *scanout, unsigned *turn);
void kwl_rotate_report(unsigned selected, unsigned mask, unsigned why, unsigned alone, unsigned *told_mask, unsigned *told_why);
void kwl_turn_coefficients(unsigned rotation, float *u, float *v);

/*
 * The sizes and the steps are here as inline functions, not in rotate.c:
 * the output's own files (output-switch.c) take them without rotate.c,
 * so that the host tests that build such a file alone keep building.
 */

/* Tells whether an angle swaps the width and the height (90 and 270). */
static __inline int
kwl_rotate_swaps(
	unsigned rotation)
{
	/* A quarter turn either way stands the picture on its side. */
	if (rotation == KWL_ROTATE_90)
		return 1;
	if (rotation == KWL_ROTATE_270)
		return 1;

	/* 0 and 180 keep the sides. */
	return 0;
}

/*
 * Gives the size of a picture turned by an angle: the logical size of a
 * physical one, or the physical of a logical one (the same either way).
 */
static __inline void
kwl_rotate_size(
	unsigned rotation,
	uint32_t width,
	uint32_t height,
	uint32_t *rotated_width,
	uint32_t *rotated_height)
{
	int swaps;

	/* A quarter turn exchanges the sides. */
	swaps = kwl_rotate_swaps(rotation);
	if (swaps) {
		*rotated_width = height;
		*rotated_height = width;
	} else {
		*rotated_width = width;
		*rotated_height = height;
	}
}

/*
 * Gives the steps a change of an output's state takes (KWL_ROTATE_STEP_*):
 * the output opened again when the scanout's angle changes (and the rest
 * follows the opening), else the turn's images exchanged when the turn's
 * angle changes (an opening, KWL_ROTATE_MODE_OPEN, made both already); the
 * input and the clients told whenever the angle shown changes, even with
 * the same size (0 and 180, 90 and 270); the desktop fitted when the
 * logical size changes; and the user's change of choice saved.
 */
static __inline unsigned
kwl_rotate_steps(
	const struct kwl_rotate_state *before,
	const struct kwl_rotate_state *after,
	unsigned mode)
{
	unsigned steps;

	/* How the angle is shown: a surface of another transform, or other turn images; an opening made them. */
	steps = 0U;
	if (mode != KWL_ROTATE_MODE_OPEN) {
		if (before->scanout != after->scanout) {
			steps |= KWL_ROTATE_STEP_REOPEN;
		} else if (before->turn != after->turn) {
			steps |= KWL_ROTATE_STEP_TURN;
		}
	}

	/* Another angle shown moves every point, whatever the size. */
	if (before->rotation != after->rotation)
		steps |= KWL_ROTATE_STEP_ROTATED;

	/* Another logical size fits the desktop again. */
	if (before->width != after->width) {
		steps |= KWL_ROTATE_STEP_RESIZED;
	} else if (before->height != after->height) {
		steps |= KWL_ROTATE_STEP_RESIZED;
	}

	/* The user's change of choice is saved; an opening and a game's pause save nothing. */
	if (mode == KWL_ROTATE_MODE_USER && before->selected != after->selected)
		steps |= KWL_ROTATE_STEP_SAVE;

	/* Succeeded: the steps to take. */
	return steps;
}

/*
 * Decides what a user's choice of an angle does (KWL_ROTATE_SELECT_*): the
 * request against the display's mask, the angle chosen before and the one
 * shown (they differ when the turn's images could not be made, and the
 * same choice then tries again), whether a client's buffer is scanned out
 * directly (the output is closed then, review M1) and whether the output
 * is open.
 */
static __inline unsigned
kwl_rotate_select(
	unsigned request,
	unsigned mask,
	unsigned selected,
	unsigned shown,
	unsigned open,
	unsigned scanning_out)
{
	unsigned bit;

	/* An angle the display may take. */
	bit = 0U;
	if (request == KWL_ROTATE_0)
		bit = KWL_ROTATION_0;
	else if (request == KWL_ROTATE_90)
		bit = KWL_ROTATION_90;
	else if (request == KWL_ROTATE_180)
		bit = KWL_ROTATION_180;
	else if (request == KWL_ROTATE_270)
		bit = KWL_ROTATION_270;
	if ((mask & bit) == 0U)
		return KWL_ROTATE_SELECT_REFUSE;

	/* The same choice already shown changes nothing. */
	if (request == selected && request == shown)
		return KWL_ROTATE_SELECT_NOTHING;

	/*
	 * A client's buffer on the display (the output is closed meanwhile)
	 * gives it back to the desktop first, whose opening shows the choice.
	 */
	if (scanning_out)
		return KWL_ROTATE_SELECT_LEAVE;

	/* A closed output shows the choice when it opens. */
	if (!open)
		return KWL_ROTATE_SELECT_SAVE;

	/* Succeeded: the angle is shown now. */
	return KWL_ROTATE_SELECT_APPLY;
}

/*
 * Gives the logical place of a physical point on a panel of width by
 * height (kwl_rotate_point_back's arithmetic, here for the files linked
 * without rotate.c: the cursor's box).  The sizes and the point may be in
 * pixels, or both in 1/256 pixels for a 24.8 fixed-point point (as
 * kwl_rotate_fixed_back).
 */
static __inline void
kwl_rotate_pixel_back(
	unsigned rotation,
	uint32_t width,
	uint32_t height,
	int32_t *x,
	int32_t *y)
{
	int32_t physical_x;
	int32_t physical_y;
	int32_t wide;
	int32_t high;

	/* The physical point, and the panel's sides. */
	physical_x = *x;
	physical_y = *y;
	wide = (int32_t)width;
	high = (int32_t)height;

	/* The logical point for each angle; 0 keeps it. */
	if (rotation == KWL_ROTATE_90) {
		*x = physical_y;
		*y = wide - 1 - physical_x;
	} else if (rotation == KWL_ROTATE_180) {
		*x = wide - 1 - physical_x;
		*y = high - 1 - physical_y;
	} else if (rotation == KWL_ROTATE_270) {
		*x = high - 1 - physical_y;
		*y = physical_x;
	}
}

/*
 * Moves a pointer's place on an output that turns from one angle to
 * another on the same panel (physical_width by physical_height): kept
 * inside the old logical picture, taken to the physical point it is
 * shown at, and back to the logical place at the new angle, so that it
 * stays on the same point of the panel.  The same arithmetic as
 * kwl_rotate_point and kwl_rotate_point_back (the host test compares them).
 */
static __inline void
kwl_rotate_pointer_place(
	unsigned before,
	unsigned after,
	uint32_t physical_width,
	uint32_t physical_height,
	int32_t *x,
	int32_t *y)
{
	uint32_t width;
	uint32_t height;
	int32_t physical_x;
	int32_t physical_y;
	int32_t wide;
	int32_t high;

	/* Inside the old logical picture. */
	kwl_rotate_size(before, physical_width, physical_height, &width, &height);
	if (*x < 0)
		*x = 0;
	if (*y < 0)
		*y = 0;
	if (*x >= (int32_t)width)
		*x = (int32_t)width - 1;
	if (*y >= (int32_t)height)
		*y = (int32_t)height - 1;

	/* The physical point it is shown at. */
	wide = (int32_t)physical_width;
	high = (int32_t)physical_height;
	physical_x = *x;
	physical_y = *y;
	if (before == KWL_ROTATE_90) {
		physical_x = wide - 1 - *y;
		physical_y = *x;
	} else if (before == KWL_ROTATE_180) {
		physical_x = wide - 1 - *x;
		physical_y = high - 1 - *y;
	} else if (before == KWL_ROTATE_270) {
		physical_x = *y;
		physical_y = high - 1 - *x;
	}

	/* Its logical place at the new angle. */
	*x = physical_x;
	*y = physical_y;
	if (after == KWL_ROTATE_90) {
		*x = physical_y;
		*y = wide - 1 - physical_x;
	} else if (after == KWL_ROTATE_180) {
		*x = wide - 1 - physical_x;
		*y = high - 1 - physical_y;
	} else if (after == KWL_ROTATE_270) {
		*x = high - 1 - physical_y;
		*y = physical_x;
	}
}

/*
 * Places a floating window of a size (its body, width by height) inside an
 * output's space after the output's size changed (BUG-306): from where it
 * is, or from the place it had before the last such change moved it when
 * it is still where that change put it (fit); its right and bottom edges
 * inside the space, never left of it nor above it.  *x and *y are its place
 * now and get the new one; fit remembers the place it had when it moves.
 */
static __inline void
kwl_rotate_fit_window(
	struct kwl_fit_home *fit,
	int32_t *x,
	int32_t *y,
	int32_t width,
	int32_t height,
	int32_t space_x,
	int32_t space_y,
	int32_t space_width,
	int32_t space_height)
{
	int32_t home_x;
	int32_t home_y;
	int32_t right;
	int32_t bottom;

	/* The place to start from: the one before the last fit, while the window stayed where that fit put it. */
	home_x = *x;
	home_y = *y;
	if (fit->moved && *x == fit->x && *y == fit->y) {
		home_x = fit->home_x;
		home_y = fit->home_y;
	}

	/* Its right edge inside the space. */
	*x = home_x;
	*y = home_y;
	right = space_x + space_width;
	if (*x + width > right)
		*x = right - width;

	/* And its bottom edge. */
	bottom = space_y + space_height;
	if (*y + height > bottom)
		*y = bottom - height;

	/* Never left of the space, nor above it. */
	if (*x < space_x)
		*x = space_x;
	if (*y < space_y)
		*y = space_y;

	/* Moved: the place it had is kept with the place it got; else nothing is kept. */
	fit->moved = 0U;
	if (*x != home_x || *y != home_y) {
		fit->moved = 1U;
		fit->home_x = home_x;
		fit->home_y = home_y;
		fit->x = *x;
		fit->y = *y;
	}
}

/*
 * Tells whether a configure's serial a client acknowledged is newer than
 * one sent before it (BUG-306: a window acknowledged a configure sent
 * after an output's turn); the serials wrap.
 */
static __inline int
kwl_rotate_serial_newer(
	uint32_t acked,
	uint32_t sent)
{
	int32_t ahead;

	/* How far past the one sent, counting on over the wrap. */
	ahead = (int32_t)(acked - sent);
	if (ahead > 0)
		return 1;

	/* The same one, or one before it. */
	return 0;
}

/*
 * Tells whether a window must change its size after its output turned to
 * another size (BUG-306): a fullscreen or docked one (fixed) is given the
 * new size; a floating one only when its image of width x height no
 * longer fits the new bounds (bounds_width x bounds_height).
 */
static __inline int
kwl_rotate_size_needed(
	unsigned fixed,
	uint32_t width,
	uint32_t height,
	int32_t bounds_width,
	int32_t bounds_height)
{
	/* A fullscreen or docked window is sent the output's or the docked space's new size. */
	if (fixed)
		return 1;

	/* A floating one wider or taller than the new bounds draws a smaller image. */
	if ((int64_t)width > (int64_t)bounds_width)
		return 1;
	if ((int64_t)height > (int64_t)bounds_height)
		return 1;

	/* Succeeded: the image it has fits, and it may keep its size. */
	return 0;
}

/*
 * Tells whether a window an output's turn waits for has drawn the new size
 * with the image it just committed (BUG-306): it acknowledged a configure
 * newer than the last one sent before the turn (acked against sent), and,
 * when it must change its size (needed), the image is of another size than
 * the one it had at the turn (from_width x from_height against width x
 * height).  An image drawn before the client read the configure but
 * committed after its acknowledgement keeps the old size, and is not the
 * new one.
 */
static __inline int
kwl_rotate_drawn(
	uint32_t acked,
	uint32_t sent,
	unsigned needed,
	uint32_t from_width,
	uint32_t from_height,
	uint32_t width,
	uint32_t height)
{
	int newer;

	/* An image committed before the client acknowledged a configure sent after the turn. */
	newer = kwl_rotate_serial_newer(acked, sent);
	if (!newer)
		return 0;

	/* A window that may keep its size has drawn the new state. */
	if (!needed)
		return 1;

	/* One that must change it has, once its image is of another size. */
	if (width != from_width)
		return 1;
	if (height != from_height)
		return 1;

	/* The image is still of the old size. */
	return 0;
}

/*
 * Tells whether an output turned to another size still shows its old
 * frame (BUG-306): some windows have not drawn the new size yet, and the
 * wait that began at began (ms) has not reached KWL_ROTATE_HOLD_MS by now.
 */
static __inline int
kwl_rotate_holding(
	unsigned waiting,
	uint64_t began,
	uint64_t now)
{
	/* Every window drew the new size. */
	if (waiting == 0U)
		return 0;

	/* The wait is over: the windows late are shown as they draw. */
	if (now - began >= KWL_ROTATE_HOLD_MS)
		return 0;

	/* Succeeded: the old frame stays. */
	return 1;
}

#endif
