/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rules of the session's layout mode (ws142-p008, BUG-217; layout.h
 * says what the mode is).  Each rule answers one question the shell asks
 * at one moment: a switch to a window, a new window's first configure, a
 * window leaving fullscreen, a window to draw.
 */

#include "layout.h"

#include <stddef.h>

/*
 * Names a layout mode for the log (KWL LAYOUT).
 */
const char *
kwl_layout_name(
	unsigned mode)
{
	/* The docked mode is the tablet's. */
	if (mode == KWL_LAYOUT_DOCKED)
		return "docked";

	/* Every other value is the windowed mode, the session's first. */
	return "windowed";
}

/*
 * Says what a window switched to does to follow the mode (an application
 * switch: Alt+Tab, Wiseview, the bar, an activation): in the docked mode
 * it is docked, in the windowed mode it floats again at its place before.
 * A fullscreen window stays fullscreen (it comes back to the mode when it
 * leaves fullscreen), and a window with a parent floats over its parent,
 * which the caller makes follow the mode instead.
 */
unsigned
kwl_layout_switch_action(
	unsigned mode,
	const struct kwl_layout_window *window)
{
	/* A fullscreen window keeps covering the output. */
	if (window->fullscreen)
		return KWL_LAYOUT_KEEP;

	/* A dialog or a sheet is never docked; its parent follows the mode. */
	if (window->child)
		return KWL_LAYOUT_KEEP;

	/* The docked mode docks a floating window. */
	if (mode == KWL_LAYOUT_DOCKED && !window->docked)
		return KWL_LAYOUT_DOCK;

	/* The windowed mode brings a docked window back to floating. */
	if (mode != KWL_LAYOUT_DOCKED && window->docked)
		return KWL_LAYOUT_FLOAT;

	/* The window already is as the mode wants it. */
	return KWL_LAYOUT_KEEP;
}

/*
 * Tells whether a new window opens docked (its first configure is the
 * docked space): in the docked mode every window but a dialog, a sheet, a
 * fullscreen one and one docked already, a window of one size too (the
 * 2026-10-06 user decision: there is no window that cannot be docked).
 * Returns 1 when it opens docked.
 */
int
kwl_layout_opens_docked(
	unsigned mode,
	const struct kwl_layout_window *window)
{
	/* The windowed mode opens windows floating. */
	if (mode != KWL_LAYOUT_DOCKED)
		return 0;

	/* A dialog or a sheet floats over its parent. */
	if (window->child)
		return 0;

	/* A fullscreen window covers the output, and a docked one is docked already. */
	if (window->fullscreen)
		return 0;
	if (window->docked)
		return 0;

	/* Succeeded: the window opens docked. */
	return 1;
}

/*
 * Tells whether a window leaving fullscreen is docked (it comes back to
 * the session's mode, whatever it was when it went fullscreen; this
 * replaces BUG-208's own memory of it).  A dialog or a sheet floats.
 * Returns 1 when it is docked.
 */
int
kwl_layout_unfullscreen_docked(
	unsigned mode,
	const struct kwl_layout_window *window)
{
	/* The windowed mode brings it back floating. */
	if (mode != KWL_LAYOUT_DOCKED)
		return 0;

	/* A dialog or a sheet floats over its parent. */
	if (window->child)
		return 0;

	/* Succeeded: the window is docked. */
	return 1;
}

/*
 * Tells whether a window is drawn at its own size in the middle of the
 * docked space: a docked window of one size (its client does not draw the
 * docked size; the rest of the space is dark), and in the docked mode a
 * dialog or a sheet (the File Chooser among them, the 2026-10-06 user
 * decision).  Returns 1 when it is centred.
 */
int
kwl_layout_centred(
	unsigned mode,
	const struct kwl_layout_window *window)
{
	/* A docked window of one size keeps its size in the middle. */
	if (window->docked && window->fixed)
		return 1;

	/* In the docked mode, a dialog or a sheet shows in the middle of the screen. */
	if (mode == KWL_LAYOUT_DOCKED && window->child)
		return 1;

	/* Any other window is drawn where its place is. */
	return 0;
}

/*
 * Tells whether a window is left out of the scene: in the docked mode only
 * the current application's windows show (the 2026-10-06 user decision:
 * no other application is stacked under a docked one), except while an
 * overview shows them all (App Home, Wiseview, the switcher).  Returns 1
 * when the window is not drawn and takes no press.
 */
int
kwl_layout_hidden(
	unsigned mode,
	int same_application,
	int overview)
{
	/* The windowed mode shows every window. */
	if (mode != KWL_LAYOUT_DOCKED)
		return 0;

	/* The current application's windows show. */
	if (same_application)
		return 0;

	/* An overview shows every application. */
	if (overview)
		return 0;

	/* Succeeded: another application's window is hidden. */
	return 1;
}

/*
 * Tells the state of a window without a parent (WS181): minimized by the
 * person, fullscreen, hidden by the docked mode behind another
 * application (docking's hiding, kept apart from a minimize, so that the
 * docked mode's end shows it again and leaves a minimized one minimized),
 * docked, or floating.  front_application says whether the window is of
 * the application in front of the desktop shown.
 */
unsigned
kwl_layout_state(
	unsigned mode,
	const struct kwl_layout_window *window,
	int minimized,
	int front_application)
{
	/* A window the person minimized stays so, whatever the mode. */
	if (minimized)
		return KWL_LAYOUT_STATE_MINIMIZED;

	/* A fullscreen window covers the output. */
	if (window->fullscreen)
		return KWL_LAYOUT_STATE_FULLSCREEN;

	/* The docked mode hides another application's windows, docked or not. */
	if (mode == KWL_LAYOUT_DOCKED && !front_application)
		return KWL_LAYOUT_STATE_DOCK_HIDDEN;

	/* A docked window shown. */
	if (window->docked)
		return KWL_LAYOUT_STATE_DOCKED;

	/* Succeeded: the window floats. */
	return KWL_LAYOUT_STATE_FLOATING;
}

/*
 * Names a window's state for the log (KWL LAYOUT).
 */
const char *
kwl_layout_state_name(
	unsigned state)
{
	/* Each state's name, as the tests read it. */
	switch (state) {
	case KWL_LAYOUT_STATE_DOCKED:
		return "docked";
	case KWL_LAYOUT_STATE_DOCK_HIDDEN:
		return "dock-hidden";
	case KWL_LAYOUT_STATE_MINIMIZED:
		return "minimized";
	case KWL_LAYOUT_STATE_FULLSCREEN:
		return "fullscreen";
	default:
		break;
	}

	/* Every other value is a floating window. */
	return "floating";
}

/*
 * Says what the end of the docked mode does to a window (WS181): the
 * window the person brings back floats again with its animation, every
 * other docked window -- on any desktop, minimized or not yet mapped --
 * floats again at once (it was not shown), and a floating window, a
 * fullscreen one and a dialog or a sheet stay.  So no docked window is left
 * in the windowed mode.
 */
unsigned
kwl_layout_leave_action(
	const struct kwl_layout_window *window,
	int front)
{
	/* A dialog or a sheet follows its parent. */
	if (window->child)
		return KWL_LAYOUT_KEEP;

	/* A floating or fullscreen window is not docked. */
	if (!window->docked)
		return KWL_LAYOUT_KEEP;

	/* The window brought back is seen coming back. */
	if (front)
		return KWL_LAYOUT_FLOAT;

	/* Succeeded: a docked window not shown floats again at once. */
	return KWL_LAYOUT_QUIET;
}

/*
 * Checks a desktop's docked owner in the docked mode (WS181, the
 * 2026-10-07 UAT): an owner that is gone, unmapped, minimized or no longer
 * docked ends the docked mode when its desktop is shown (the other windows
 * float again; the next one is not docked), and is only forgotten on a
 * desktop not shown (what is shown does not change behind the person); an
 * owner on another desktop moved along with the desktop shown (it stays
 * docked), or was sent away from the desktop shown (the docked mode ends).
 * Sets *reason to the log's word for a leave.
 */
unsigned
kwl_layout_owner_check(
	const struct kwl_layout_owner *owner,
	unsigned desktop,
	unsigned shown,
	const char **reason)
{
	/* What went wrong with the owner, when something did. */
	*reason = NULL;
	if (owner->gone) {
		/* The window was destroyed (closed, or its client went). */
		*reason = "closed";
	} else if (!owner->mapped) {
		/* The client took its image away. */
		*reason = "closed";
	} else if (owner->minimized) {
		/* The person minimized it. */
		*reason = "minimized";
	} else if (!owner->docked && !owner->fullscreen) {
		/* It floats again without the docked mode ending (it should not, but the mode follows). */
		*reason = "floated";
	}

	/* An owner gone on the desktop shown ends the docked mode. */
	if (*reason != NULL && desktop == shown)
		return KWL_LAYOUT_OWNER_LEAVE;

	/* One gone on a desktop not shown is forgotten. */
	if (*reason != NULL)
		return KWL_LAYOUT_OWNER_FORGET;

	/* An owner still on its desktop stays the owner. */
	if (owner->desktop == desktop)
		return KWL_LAYOUT_OWNER_KEEP;

	/* Carried along to the desktop shown: still docked there. */
	if (owner->desktop == shown)
		return KWL_LAYOUT_OWNER_MOVED;

	/* Sent from the desktop shown to one not shown: the docked mode ends. */
	if (desktop == shown) {
		*reason = "moved";
		return KWL_LAYOUT_OWNER_LEAVE;
	}

	/* Succeeded: an owner gone between desktops not shown is forgotten. */
	return KWL_LAYOUT_OWNER_FORGET;
}

/*
 * Places a body of a size in the middle of a space; one larger than the
 * space starts at the space's top-left corner.
 */
void
kwl_layout_centre(
	int32_t space_x,
	int32_t space_y,
	int32_t space_width,
	int32_t space_height,
	int32_t width,
	int32_t height,
	int32_t *x,
	int32_t *y)
{
	/* Across: the middle, never left of the space. */
	*x = space_x + (space_width - width) / 2;
	if (*x < space_x)
		*x = space_x;

	/* Down: the middle, never above the space. */
	*y = space_y + (space_height - height) / 2;
	if (*y < space_y)
		*y = space_y;
}
