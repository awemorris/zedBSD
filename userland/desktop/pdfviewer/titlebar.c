/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The titlebar of PDF Viewer in the compositor (WS070's CONTROLS presentation):
 * the sidebar of page thumbnails (ws079-p015), the previous and the next
 * page, where the view is ("Page 3 of 10"), the two modes, the zoom, the
 * two fits, "Annotate in Notes", and (ws128-p004) the find field, whose
 * text comes as KL_WINDOW_CONTROL_TEXT and KL_WINDOW_CONTROL_DONE inputs.
 *
 * The controls are a table given to libkeiland (WS131 p017:
 * kl_window_set_controls), the page's text its label; their state is their
 * actions' (menu.c).  The compositor draws the controls and makes them give way
 * when the room runs short (into its "..." popup, which also holds the
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
#define CONTROL_PAGE		3U
#define CONTROL_SCROLL		4U
#define CONTROL_PAGES		5U
#define CONTROL_ZOOM_OUT	6U
#define CONTROL_ZOOM_IN		7U
#define CONTROL_FIT_WIDTH	8U
#define CONTROL_FIT_PAGE	9U
#define CONTROL_ANNOTATE	10U
#define CONTROL_THUMBNAILS	11U
#define CONTROL_FIND		12U

/*
 * The controls, in their order.  The compositor draws a generic control outside a
 * segmented group as a pill with its label, so the zoom, the fits and
 * Annotate are ungrouped generic controls; the modes are the view pair.
 * The page's control is the one whose label changes (titlebar_send).
 */
static const struct kl_control_entry titlebar_controls[] = {
	{ CONTROL_THUMBNAILS, KL_CONTROL_SIDEBAR, KL_PRIORITY_PRIMARY, 0U, "Page Thumbnails", PV_ACTION_THUMBNAILS },
	{ CONTROL_PREVIOUS, KL_CONTROL_BACK, KL_PRIORITY_PRIMARY, 0U, "Previous Page", PV_ACTION_PREVIOUS },
	{ CONTROL_NEXT, KL_CONTROL_FORWARD, KL_PRIORITY_PRIMARY, 0U, "Next Page", PV_ACTION_NEXT },
	{ CONTROL_PAGE, KL_CONTROL_GENERIC, KL_PRIORITY_NORMAL, 0U, "No document", PV_ACTION_PAGE_INFO },
	{ CONTROL_SCROLL, KL_CONTROL_VIEW_LIST, KL_PRIORITY_NORMAL, 1U, "Continuous Scroll", PV_ACTION_MODE_SCROLL },
	{ CONTROL_PAGES, KL_CONTROL_VIEW_COLUMNS, KL_PRIORITY_NORMAL, 1U, "Single Page", PV_ACTION_MODE_PAGE },
	{ CONTROL_ZOOM_OUT, KL_CONTROL_GENERIC, KL_PRIORITY_SECONDARY, 0U, "\xe2\x88\x92", PV_ACTION_ZOOM_OUT },
	{ CONTROL_ZOOM_IN, KL_CONTROL_GENERIC, KL_PRIORITY_SECONDARY, 0U, "+", PV_ACTION_ZOOM_IN },
	{ CONTROL_FIT_WIDTH, KL_CONTROL_GENERIC, KL_PRIORITY_SECONDARY, 0U, "Fit Width", PV_ACTION_FIT_WIDTH },
	{ CONTROL_FIT_PAGE, KL_CONTROL_GENERIC, KL_PRIORITY_SECONDARY, 0U, "Fit Page", PV_ACTION_FIT_PAGE },
	{ CONTROL_ANNOTATE, KL_CONTROL_GENERIC, KL_PRIORITY_PRIMARY, 0U, "Annotate in Notes", PV_ACTION_ANNOTATE },
	{ CONTROL_FIND, KL_CONTROL_SEARCH, KL_PRIORITY_NORMAL, 0U, "Find", PV_ACTION_NONE }
};

/* The place of the page's control in the table. */
#define TITLEBAR_PAGE_INDEX	3U

static int titlebar_send(struct pv_titlebar *titlebar, const char *page);

/*
 * Gives the compositor the window's titlebar controls, showing a state.
 *
 * Returns 0, also when the compositor has no titlebar presentation, or an
 * errno value when the controls could not be made.
 */
int
pv_titlebar_open(
	struct pv_titlebar *titlebar,
	struct pv_window *window,
	const struct pv_state *state)
{
	int error;

	/* Nothing yet but the window. */
	memset(titlebar, 0, sizeof(*titlebar));
	titlebar->window = window;

	/* The controls; a compositor without the titlebar leaves the menus and the keys. */
	error = titlebar_send(titlebar, "No document");
	if (error == ENOTSUP) {
		pv_log("TITLEBAR none errno=%d", error);
		return 0;
	}

	/* Another failure is told; the controls are shown. */
	if (error != 0)
		return error;
	titlebar->shown = 1;

	/* The find field's placeholder (ws128-p004). */
	error = kl_window_set_control_text(window->kui, CONTROL_FIND, "", "Find");
	if (error != 0)
		return error;

	/* The page's text for the state. */
	pv_titlebar_refresh(titlebar, state);

	/* Succeeded: the titlebar is the compositor's to show. */
	pv_log("TITLEBAR ready controls=%u", (unsigned)(sizeof(titlebar_controls) / sizeof(titlebar_controls[0])));
	return 0;
}

/*
 * Shows where the view is ("Page 3 of 10") in the page's control; the
 * controls' states are their actions' (pv_menu_refresh).
 */
void
pv_titlebar_refresh(
	struct pv_titlebar *titlebar,
	const struct pv_state *state)
{
	char label[64];
	int error;

	/* Without a titlebar nothing is sent. */
	if (!titlebar->shown)
		return;

	/* Where the view is. */
	snprintf(label, sizeof(label), "No document");
	if (state->has_document)
		snprintf(label, sizeof(label), "Page %lu of %lu", (unsigned long)(state->page + 1), (unsigned long)state->count);

	/* Sent when it changed (libkeiland compares the table). */
	error = titlebar_send(titlebar, label);
	if (error != 0)
		pv_log("TITLEBAR update-failed errno=%d", error);
}

/*
 * Takes the titlebar away from the compositor (before the window goes).
 */
/*
 * Gives the find field the keyboard (Ctrl+F, Edit > Find, ws128-p004).
 */
void
pv_titlebar_focus_find(
	struct pv_titlebar *titlebar)
{
	int error;

	/* Only with the titlebar. */
	if (!titlebar->shown)
		return;

	/* The field takes the keyboard. */
	error = kl_window_focus_control(titlebar->window->kui, CONTROL_FIND);
	if (error != 0)
		pv_log("TITLEBAR focus-failed errno=%d", error);
}

/*
 * Takes the find field's text: as it is typed, Find looks for it from the
 * page in view; Enter in it shows the next place and leaves the field with
 * the keyboard and its text, so that Enter again goes on (ws128-p004,
 * q826), and what is typed next is added to its text (ws177-p043).
 */
void
pv_titlebar_input(
	struct pv_titlebar *titlebar,
	struct pv_app *app,
	const struct kl_window_event *input)
{
	int error;

	/* Only the find field's. */
	if (input->id != (int32_t)CONTROL_FIND)
		return;

	/* As it is typed. */
	if (input->kind == KL_WINDOW_CONTROL_TEXT) {
		pv_find_text(app, input->text);
		return;
	}

	/* Only Enter ends it otherwise than by leaving it. */
	if (input->kind != KL_WINDOW_CONTROL_DONE || input->code != KL_TEXT_SUBMITTED)
		return;

	/* The next place. */
	pv_find_next(app, 1);

	/*
	 * The field keeps the query (the compositor's field starts from the
	 * control's text) and takes the keyboard again to go on editing it:
	 * the caret at its end, nothing selected, so that what is typed next
	 * is added to the words (ws177-p043).
	 */
	if (!titlebar->shown)
		return;
	error = kl_window_set_control_text(titlebar->window->kui, CONTROL_FIND, input->text, "Find");
	if (error == 0)
		error = kl_window_focus_control_mode(titlebar->window->kui, CONTROL_FIND, KL_FOCUS_EDIT);
	pv_log("FIND keep error=%d", error);
}

void
pv_titlebar_close(
	struct pv_titlebar *titlebar)
{
	/* No controls on the window, and nothing kept. */
	if (titlebar->window != NULL && titlebar->shown)
		(void)kl_window_set_controls(titlebar->window->kui, NULL, 0U);
	memset(titlebar, 0, sizeof(*titlebar));
}

/* Gives libkeiland the controls with the page's text as its control's label; 0 or an errno value. */
static int
titlebar_send(
	struct pv_titlebar *titlebar,
	const char *page)
{
	struct kl_control_entry controls[sizeof(titlebar_controls) / sizeof(titlebar_controls[0])];
	int error;

	/* The table, with the page's text. */
	memcpy(controls, titlebar_controls, sizeof(controls));
	controls[TITLEBAR_PAGE_INDEX].label = page;

	/* libkeiland sends what changed. */
	error = kl_window_set_controls(titlebar->window->kui, controls, sizeof(controls) / sizeof(controls[0]));
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
