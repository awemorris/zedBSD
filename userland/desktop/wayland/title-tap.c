/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The double click on a floating title bar decided at its release
 * (BUG-265, title-tap.h).
 */

#include "title-tap.h"

#include <stddef.h>

static int title_tap_near(const struct kwl_title_tap *tap, int32_t x, int32_t y);

/*
 * Notes the second press of a double click on a window's title bar: its
 * release decides.
 */
void
kwl_title_tap_press(
	struct kwl_title_tap *tap,
	const void *window,
	int32_t x,
	int32_t y)
{
	/* The window, and where the press was. */
	tap->window = window;
	tap->x = x;
	tap->y = y;
}

/*
 * Takes the release of a press on a title bar: TITLE_TAP_DOCK when it ends
 * a double click near its second press (the window in *window),
 * TITLE_TAP_MOVED when that press moved further, TITLE_TAP_NONE when no
 * double click waited.  Nothing waits after it.
 */
int
kwl_title_tap_release(
	struct kwl_title_tap *tap,
	int32_t x,
	int32_t y,
	const void **window)
{
	int near;

	/* No double click waits. */
	*window = tap->window;
	if (tap->window == NULL)
		return TITLE_TAP_NONE;
	tap->window = NULL;

	/* Moved past the slop: a move. */
	near = title_tap_near(tap, x, y);
	if (!near)
		return TITLE_TAP_MOVED;

	/* Succeeded: a double click. */
	return TITLE_TAP_DOCK;
}

/*
 * Tells whether a window's double click still waits within the slop of its
 * second press (its move does not move the window yet): 1 or 0.
 */
int
kwl_title_tap_still(
	const struct kwl_title_tap *tap,
	const void *window,
	int32_t x,
	int32_t y)
{
	int near;

	/* Not that window's double click. */
	if (tap->window == NULL || tap->window != window)
		return 0;

	/* Within the slop. */
	near = title_tap_near(tap, x, y);
	return near;
}

/* Forgets a double click waiting on a window that goes (or on any, for NULL). */
void
kwl_title_tap_forget(
	struct kwl_title_tap *tap,
	const void *window)
{
	/* Only that window's, or any. */
	if (window != NULL && tap->window != window)
		return;
	tap->window = NULL;
}

/* Tells whether a point is within TITLE_TAP_SLOP of the press (each axis bounded first, so the square cannot overflow). */
static int
title_tap_near(
	const struct kwl_title_tap *tap,
	int32_t x,
	int32_t y)
{
	int64_t dx;
	int64_t dy;

	/* Far along an axis. */
	dx = (int64_t)x - tap->x;
	dy = (int64_t)y - tap->y;
	if (dx > TITLE_TAP_SLOP || dx < -TITLE_TAP_SLOP || dy > TITLE_TAP_SLOP || dy < -TITLE_TAP_SLOP)
		return 0;

	/* Within the circle. */
	if (dx * dx + dy * dy > (int64_t)TITLE_TAP_SLOP * TITLE_TAP_SLOP)
		return 0;
	return 1;
}
