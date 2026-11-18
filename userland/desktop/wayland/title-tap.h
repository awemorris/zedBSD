/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The double click on a floating title bar, decided at its release
 * (BUG-265): the second press of a double click (a mouse's, a touch pad's
 * tap, a finger's) starts a move as any press does, and only its release
 * tells what it was: still within TITLE_TAP_SLOP of the press, a double
 * click (the window docks); moved further, a move (the tap and drag of a
 * touch pad or a finger).  The header needs nothing of the compositor, so
 * the host's tests build title-tap.c alone.
 */

#ifndef KWL_TITLE_TAP_H
#define KWL_TITLE_TAP_H

#include <stdint.h>

/* How far a double click's second press may move before its release and still be a double click (pixels). */
#define TITLE_TAP_SLOP		8

/* What a release of a press on a title bar was. */
#define TITLE_TAP_NONE		0	/* no double click waited for it: a move's end */
#define TITLE_TAP_DOCK		1	/* the release of a double click */
#define TITLE_TAP_MOVED		2	/* a double click's second press that moved: a move's end */

/*
 * A double click waiting for its release: the window (by any pointer the
 * caller keeps; NULL for none) and where its second press was.
 */
struct kwl_title_tap {
	const void *window;
	int32_t x;
	int32_t y;
};

void kwl_title_tap_press(struct kwl_title_tap *tap, const void *window, int32_t x, int32_t y);
int kwl_title_tap_release(struct kwl_title_tap *tap, int32_t x, int32_t y, const void **window);
void kwl_title_tap_forget(struct kwl_title_tap *tap, const void *window);
int kwl_title_tap_still(const struct kwl_title_tap *tap, const void *window, int32_t x, int32_t y);

#endif
