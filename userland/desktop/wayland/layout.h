/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The session's layout mode (layout.c, ws142-p008, BUG-217): whether the
 * person uses the desktop with windows, or docked (maximized) as a
 * tablet.  It is one state of the session, not of a window: docking a
 * window makes it docked, bringing one back makes it windowed, and every
 * window switched to, opened or leaving fullscreen follows it.
 *
 * It knows nothing of the server: the caller says what a window is (docked,
 * fullscreen, with a parent, of one size) and acts on what the rules say
 * (shell.c).  So the host tests run it alone.
 */

#ifndef KWL_LAYOUT_H
#define KWL_LAYOUT_H

#include <stdint.h>

/* The session's layout mode: windows float, or every window switched to is docked under the system bar. */
#define KWL_LAYOUT_WINDOWED		0U
#define KWL_LAYOUT_DOCKED		1U

/*
 * What a window switched to is to do: stay as it is, dock, or float again;
 * and, when the docked mode ends, float again without an animation (a
 * window that was not shown, kwl_layout_leave_action).
 */
#define KWL_LAYOUT_KEEP			0U
#define KWL_LAYOUT_DOCK			1U
#define KWL_LAYOUT_FLOAT		2U
#define KWL_LAYOUT_QUIET		3U

/*
 * The state a window without a parent is in (kwl_layout_state, WS181): the
 * person minimized it, it is fullscreen, the docked mode hides it behind
 * another application's docked window (docking's own hiding, not a
 * minimize), it is docked, or it floats.
 */
#define KWL_LAYOUT_STATE_FLOATING	0U
#define KWL_LAYOUT_STATE_DOCKED		1U
#define KWL_LAYOUT_STATE_DOCK_HIDDEN	2U
#define KWL_LAYOUT_STATE_MINIMIZED	3U
#define KWL_LAYOUT_STATE_FULLSCREEN	4U

/*
 * What the check of a desktop's docked owner finds (kwl_layout_owner_check,
 * WS181): it is still the owner, the docked mode ends (the owner went on
 * the desktop shown), the owner is forgotten (it went on a desktop not
 * shown), or the owner moved along to the desktop shown.
 */
#define KWL_LAYOUT_OWNER_KEEP		0U
#define KWL_LAYOUT_OWNER_LEAVE		1U
#define KWL_LAYOUT_OWNER_FORGET		2U
#define KWL_LAYOUT_OWNER_MOVED		3U

/*
 * A window as the layout's rules see it: whether it is docked, fullscreen,
 * has a parent (a dialog or a sheet, which floats over its parent and is
 * never docked), and has one size only (its smallest and largest sizes the
 * same: it is docked all the same, drawn at its size in the middle of the
 * docked space).
 */
struct kwl_layout_window {
	unsigned docked;
	unsigned fullscreen;
	unsigned child;
	unsigned fixed;
};

/*
 * A desktop's docked owner as its check sees it (WS181): whether the
 * window is gone (destroyed), still mapped, minimized, docked, fullscreen,
 * and the desktop it is on now.  The owner is the docked window in front
 * of the desktop that the docked mode keeps; when it goes, the docked mode
 * ends instead of docking the next window.
 */
struct kwl_layout_owner {
	unsigned gone;
	unsigned mapped;
	unsigned minimized;
	unsigned docked;
	unsigned fullscreen;
	unsigned desktop;
};

const char *kwl_layout_name(unsigned mode);
unsigned kwl_layout_switch_action(unsigned mode, const struct kwl_layout_window *window);
int kwl_layout_opens_docked(unsigned mode, const struct kwl_layout_window *window);
int kwl_layout_unfullscreen_docked(unsigned mode, const struct kwl_layout_window *window);
int kwl_layout_centred(unsigned mode, const struct kwl_layout_window *window);
int kwl_layout_hidden(unsigned mode, int same_application, int overview);
unsigned kwl_layout_state(unsigned mode, const struct kwl_layout_window *window, int minimized, int front_application);
const char *kwl_layout_state_name(unsigned state);
unsigned kwl_layout_leave_action(const struct kwl_layout_window *window, int front);
unsigned kwl_layout_owner_check(const struct kwl_layout_owner *owner, unsigned desktop, unsigned shown, const char **reason);
void kwl_layout_centre(int32_t space_x, int32_t space_y, int32_t space_width, int32_t space_height, int32_t width, int32_t height, int32_t *x, int32_t *y);

#endif
