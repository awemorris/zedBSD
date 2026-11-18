/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Display page's angles (rotate.c, ws217-p004): the four pictures of a
 * display's angle in the order the page shows them, which of them can be
 * pressed, and which reason the page says below them, from what the
 * compositor tells of the display (libkeiland's kl_system_displays_rotation
 * and _rotations).  It knows nothing of the page, so the host tests run it
 * alone.
 */

#ifndef SE_ROTATE_H
#define SE_ROTATE_H

/* The pictures a display's row shows: upright, on its left side, upside down, on its right side. */
#define SE_ROTATE_ANGLES	4U

/*
 * The reasons the page says below a display's pictures (none, or one of
 * them, the first that applies in this order): the display cannot be
 * turned at all, another display is shown beside it, the turn's images
 * could not be made, a game pauses the turn, or the quarter turns are not
 * offered on this device.
 */
#define SE_ROTATE_REASON_NONE		0U
#define SE_ROTATE_REASON_FIXED		1U
#define SE_ROTATE_REASON_ALONE		2U
#define SE_ROTATE_REASON_RESOURCE	3U
#define SE_ROTATE_REASON_GAME		4U
#define SE_ROTATE_REASON_QUARTER	5U

unsigned se_rotate_angle(unsigned slot);
int se_rotate_pressable(unsigned angle, unsigned selected, unsigned mask);
unsigned se_rotate_reason(unsigned why);

#endif
