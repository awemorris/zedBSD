/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The titlebar of Image Viewer in the compositor (WS070's CONTROLS
 * presentation): the previous and the next image, where the image is in
 * its folder ("3 / 12"), the zoom, the fit, a turn and the full screen.
 *
 * The controls are a table given to libkeiland (WS131 p017:
 * kl_window_set_controls), the image's place its label; their state is
 * their actions' (menu.c).  The compositor draws the controls and makes them give
 * way when the room runs short (into its "..." popup, which also holds the
 * menus); a control chosen comes back as a KL_WINDOW_ACTION input among
 * the window's.  A compositor without the titlebar leaves the viewer with
 * its menus and its keys.
 */

#include "window.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The controls. */
#define CONTROL_PREVIOUS	1U
#define CONTROL_NEXT		2U
#define CONTROL_PLACE		3U
#define CONTROL_ZOOM_OUT	4U
#define CONTROL_ZOOM_IN		5U
#define CONTROL_FIT		6U
#define CONTROL_ROTATE		7U
#define CONTROL_FULLSCREEN	8U

/*
 * The controls, in their order.  The compositor draws a generic control outside a
 * segmented group as a pill with its label.  The place's control is the
 * one whose label changes (titlebar_send).
 */
static const struct kl_control_entry titlebar_controls[] = {
	{ CONTROL_PREVIOUS, KL_CONTROL_BACK, KL_PRIORITY_PRIMARY, 0U, "Previous Image", IV_ACTION_PREVIOUS },
	{ CONTROL_NEXT, KL_CONTROL_FORWARD, KL_PRIORITY_PRIMARY, 0U, "Next Image", IV_ACTION_NEXT },
	{ CONTROL_PLACE, KL_CONTROL_GENERIC, KL_PRIORITY_NORMAL, 0U, "No image", IV_ACTION_PLACE_INFO },
	{ CONTROL_ZOOM_OUT, KL_CONTROL_GENERIC, KL_PRIORITY_SECONDARY, 0U, "\xe2\x88\x92", IV_ACTION_ZOOM_OUT },
	{ CONTROL_ZOOM_IN, KL_CONTROL_GENERIC, KL_PRIORITY_SECONDARY, 0U, "+", IV_ACTION_ZOOM_IN },
	{ CONTROL_FIT, KL_CONTROL_GENERIC, KL_PRIORITY_SECONDARY, 0U, "Fit", IV_ACTION_FIT },
	{ CONTROL_ROTATE, KL_CONTROL_GENERIC, KL_PRIORITY_SECONDARY, 0U, "Rotate", IV_ACTION_ROTATE_RIGHT },
	{ CONTROL_FULLSCREEN, KL_CONTROL_GENERIC, KL_PRIORITY_PRIMARY, 0U, "Full Screen", IV_ACTION_FULLSCREEN }
};

/* The place of the place's control in the table. */
#define TITLEBAR_PLACE_INDEX	2U

static int titlebar_send(struct iv_titlebar *titlebar, const char *place);

/*
 * Gives the compositor the window's titlebar controls, showing a state.
 *
 * Returns 0, also when the compositor has no titlebar presentation, or an
 * errno value when the controls could not be made.
 */
int
iv_titlebar_open(
	struct iv_titlebar *titlebar,
	struct iv_window *window,
	const struct iv_state *state)
{
	int error;

	/* Nothing yet but the window. */
	memset(titlebar, 0, sizeof(*titlebar));
	titlebar->window = window;

	/* The controls; a compositor without the titlebar leaves the menus and the keys. */
	error = titlebar_send(titlebar, "No image");
	if (error == ENOTSUP) {
		iv_log("TITLEBAR none errno=%d", error);
		return 0;
	}
	if (error != 0)
		return error;
	titlebar->shown = 1;

	/* The place for the state. */
	iv_titlebar_refresh(titlebar, state);

	/* Logs the controls for the tests. */
	iv_log("TITLEBAR ready controls=%u", (unsigned)(sizeof(titlebar_controls) / sizeof(titlebar_controls[0])));

	/* Succeeded: the titlebar is the compositor's to show. */
	return 0;
}

/*
 * Shows where the image is in its folder ("3 / 12") in the place's
 * control; the controls' states are their actions' (iv_menu_refresh).
 */
void
iv_titlebar_refresh(
	struct iv_titlebar *titlebar,
	const struct iv_state *state)
{
	char label[64];
	int error;

	/* Without a titlebar nothing is sent. */
	if (!titlebar->shown)
		return;

	/* The image's place in its folder, or that there is none. */
	if (state->has_image) {
		snprintf(label, sizeof(label), "%lu / %lu", (unsigned long)(state->index + 1), (unsigned long)state->count);
	} else {
		snprintf(label, sizeof(label), "No image");
	}

	/* Sent when it changed (libkeiland compares the table). */
	error = titlebar_send(titlebar, label);
	if (error != 0)
		iv_log("TITLEBAR update-failed errno=%d", error);
}

/*
 * Takes the titlebar away from the compositor (before the window goes).
 */
void
iv_titlebar_close(
	struct iv_titlebar *titlebar)
{
	/* No controls on the window, and nothing of the titlebar is left. */
	if (titlebar->window != NULL && titlebar->shown)
		(void)kl_window_set_controls(titlebar->window->kui, NULL, 0U);
	memset(titlebar, 0, sizeof(*titlebar));
}

/* Gives libkeiland the controls with the place's text as its control's label; 0 or an errno value. */
static int
titlebar_send(
	struct iv_titlebar *titlebar,
	const char *place)
{
	struct kl_control_entry controls[sizeof(titlebar_controls) / sizeof(titlebar_controls[0])];
	int error;

	/* The table, with the place's text. */
	memcpy(controls, titlebar_controls, sizeof(controls));
	controls[TITLEBAR_PLACE_INDEX].label = place;

	/* libkeiland sends what changed. */
	error = kl_window_set_controls(titlebar->window->kui, controls, sizeof(controls) / sizeof(controls[0]));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
