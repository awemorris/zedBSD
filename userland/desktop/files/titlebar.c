/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window's titlebar in the compositor (WS070's CONTROLS presentation,
 * plan/ws070/titlebar-design.md §11): back, forward, home, the path, the
 * search field, the view (icons or list), the preview and, while
 * operations run, their progress.
 *
 * The compositor draws the controls in the window's floating titlebar (in the
 * system bar while the window is maximized), makes them give way when the
 * room runs short (into its "..." popup, which also holds the menus), and
 * edits the text fields.  This file gives libkeiland's window the table of
 * the controls (kl_window_set_controls, WS131 p020; the progress in it
 * while operations run) and the window's state (made by
 * fm_ui_titlebar_state), sending the state only when it changed, and
 * queues what the compositor tells the window (a control chosen and a field's
 * text, among the window's inputs, window.c) for the main loop
 * (fm_ui_titlebar).  The file manager needs the compositor's titlebar: without
 * it the window does not start.
 */

#include "window.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The controls always there, in their order (the progress is added while operations run): each one's action is FM_TITLEBAR_ACTION plus its ID. */
static const struct kl_control_entry titlebar_controls[] = {
	{ FM_CONTROL_BACK, KL_CONTROL_BACK, KL_PRIORITY_PRIMARY, 0U, "Back", FM_TITLEBAR_ACTION + FM_CONTROL_BACK },
	{ FM_CONTROL_FORWARD, KL_CONTROL_FORWARD, KL_PRIORITY_PRIMARY, 0U, "Forward", FM_TITLEBAR_ACTION + FM_CONTROL_FORWARD },
	{ FM_CONTROL_HOME, KL_CONTROL_HOME, KL_PRIORITY_PRIMARY, 0U, "Home", FM_TITLEBAR_ACTION + FM_CONTROL_HOME },
	{ FM_CONTROL_PATH, KL_CONTROL_BREADCRUMB, KL_PRIORITY_NORMAL, 0U, "Location", FM_TITLEBAR_ACTION + FM_CONTROL_PATH },
	{ FM_CONTROL_SEARCH, KL_CONTROL_SEARCH, KL_PRIORITY_NORMAL, 0U, "Search", FM_TITLEBAR_ACTION + FM_CONTROL_SEARCH },
	{ FM_CONTROL_ICONS, KL_CONTROL_VIEW_GRID, KL_PRIORITY_SECONDARY, 1U, "Icons", FM_TITLEBAR_ACTION + FM_CONTROL_ICONS },
	{ FM_CONTROL_LIST, KL_CONTROL_VIEW_LIST, KL_PRIORITY_SECONDARY, 1U, "List", FM_TITLEBAR_ACTION + FM_CONTROL_LIST },
	{ FM_CONTROL_PREVIEW, KL_CONTROL_PREVIEW, KL_PRIORITY_SECONDARY, 0U, "Preview", FM_TITLEBAR_ACTION + FM_CONTROL_PREVIEW }
};

/* The progress, added while operations run. */
static const struct kl_control_entry titlebar_progress = {
	FM_CONTROL_PROGRESS, KL_CONTROL_PROGRESS, KL_PRIORITY_NORMAL, 0U, "Operations", FM_TITLEBAR_ACTION + FM_CONTROL_PROGRESS
};

/* How many controls there are at most: the ones always there and the progress. */
#define TITLEBAR_CONTROLS	(sizeof(titlebar_controls) / sizeof(titlebar_controls[0]) + 1U)

static void titlebar_queue(struct fm_titlebar *titlebar, unsigned kind, uint32_t id, uint32_t detail, const char *text);
static int titlebar_controls_send(struct fm_titlebar *titlebar, int progress);
static int titlebar_state(struct fm_titlebar *titlebar, const struct fm_titlebar_state *state);
static void titlebar_action_state(struct fm_titlebar *titlebar, uint32_t control, int enabled, int checked);
static void titlebar_suggest(struct fm_titlebar *titlebar, const struct fm_titlebar_state *state);
static int titlebar_same(unsigned value, unsigned named);

/*
 * Gives the compositor the window's titlebar, showing a state; the window's
 * controls' inputs come here from then on.
 *
 * Returns 0, or an errno value (ENOTSUP for a compositor without the
 * titlebar) when the titlebar could not be made.
 */
int
fm_titlebar_open(
	struct fm_titlebar *titlebar,
	struct fm_window *window,
	const struct fm_titlebar_state *state)
{
	int error;

	/* Nothing yet but the window. */
	memset(titlebar, 0, sizeof(*titlebar));
	titlebar->window = window;

	/* The controls always there. */
	error = titlebar_controls_send(titlebar, 0);
	if (error != 0)
		return error;
	titlebar->controls = 1;
	window->titlebar = titlebar;

	/* The state it shows. */
	error = titlebar_state(titlebar, state);
	if (error != 0)
		return error;

	/* Succeeded: the titlebar is the compositor's to show. */
	fm_log("TITLEBAR ready controls=%u", (unsigned)(sizeof(titlebar_controls) / sizeof(titlebar_controls[0])));
	return 0;
}

/*
 * Tells the titlebar the window's state when it differs from what it
 * shows, and gives a text field the keyboard when the window asked for it
 * since.
 */
void
fm_titlebar_refresh(
	struct fm_titlebar *titlebar,
	const struct fm_titlebar_state *state)
{
	int same;
	int error;

	/* Without a titlebar nothing is sent; nor when the state is the one shown. */
	if (!titlebar->controls)
		return;
	same = memcmp(state, &titlebar->shown, sizeof(*state));
	if (same == 0)
		return;

	/* The new state; a refusal is reported and the titlebar stays as it was. */
	error = titlebar_state(titlebar, state);
	if (error != 0)
		fm_log("TITLEBAR update-failed errno=%d", error);
}

/*
 * Queues what was done with a control (a KL_WINDOW_ACTION input of one,
 * with the breadcrumb's part in begin) or a field's text
 * (KL_WINDOW_CONTROL_TEXT, _DONE) for the main loop.
 */
void
fm_titlebar_input(
	struct fm_titlebar *titlebar,
	const struct kl_window_event *event)
{
	/* A control chosen: its ID from its action. */
	if (event->kind == KL_WINDOW_ACTION) {
		titlebar_queue(titlebar, FM_TITLEBAR_ACTIVATED, event->code - FM_TITLEBAR_ACTION, (uint32_t)event->begin, "");
		return;
	}

	/* A field's text as typed, and when its editing ended (and how). */
	if (event->kind == KL_WINDOW_CONTROL_TEXT) {
		titlebar_queue(titlebar, FM_TITLEBAR_CHANGED, (uint32_t)event->id, 0U, event->text);
		return;
	}
	if (event->kind == KL_WINDOW_CONTROL_DONE)
		titlebar_queue(titlebar, FM_TITLEBAR_DONE, (uint32_t)event->id, event->code, event->text);
}

/*
 * Takes the oldest thing done with the titlebar and not yet carried out;
 * returns 1, or 0 when nothing waits.
 */
int
fm_titlebar_take(
	struct fm_titlebar *titlebar,
	struct fm_titlebar_event *event)
{
	/* Nothing waits. */
	if (titlebar->event_count == 0U)
		return 0;

	/* The oldest leaves the queue. */
	*event = titlebar->events[0];
	titlebar->event_count--;
	memmove(titlebar->events, titlebar->events + 1, titlebar->event_count * sizeof(titlebar->events[0]));

	/* Succeeded: an event to carry out. */
	return 1;
}

/*
 * Takes the titlebar away from the compositor (before the window goes).
 */
void
fm_titlebar_close(
	struct fm_titlebar *titlebar)
{
	/* The controls, and the window's inputs go nowhere here any more. */
	if (titlebar->controls &&
	    titlebar->window != NULL &&
	    titlebar->window->kui != NULL)
		(void)kl_window_set_controls(titlebar->window->kui, NULL, 0U);
	if (titlebar->window != NULL)
		titlebar->window->titlebar = NULL;

	/* Nothing is left. */
	memset(titlebar, 0, sizeof(*titlebar));
}

/* Puts an event at the end of the queue; a full queue keeps only the newest text of a field. */
static void
titlebar_queue(
	struct fm_titlebar *titlebar,
	unsigned kind,
	uint32_t id,
	uint32_t detail,
	const char *text)
{
	struct fm_titlebar_event *event;

	/* A text typed replaces the text of the same field waiting at the end. */
	if (kind == FM_TITLEBAR_CHANGED && titlebar->event_count > 0U) {
		event = &titlebar->events[titlebar->event_count - 1U];
		if (event->kind == FM_TITLEBAR_CHANGED && event->id == id) {
			(void)snprintf(event->text, sizeof(event->text), "%s", text);
			return;
		}
	}

	/* A full queue drops the event (sixteen in one round). */
	if (titlebar->event_count == FM_TITLEBAR_EVENTS)
		return;

	/* The event. */
	event = &titlebar->events[titlebar->event_count];
	event->kind = kind;
	event->id = id;
	event->detail = detail;
	(void)snprintf(event->text, sizeof(event->text), "%s", text);
	titlebar->event_count++;
}

/* Gives libkeiland's window the table of the controls, with the progress while operations run; 0 or an errno value. */
static int
titlebar_controls_send(
	struct fm_titlebar *titlebar,
	int progress)
{
	struct kl_control_entry controls[TITLEBAR_CONTROLS];
	size_t count;
	int error;

	/* The controls always there, and the progress. */
	count = sizeof(titlebar_controls) / sizeof(titlebar_controls[0]);
	memcpy(controls, titlebar_controls, sizeof(titlebar_controls));
	if (progress)
		controls[count++] = titlebar_progress;

	/* The table (libkeiland sends only what changed). */
	error = kl_window_set_controls(titlebar->window->kui, controls, count);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Shows a state in the titlebar, then gives a text field the keyboard when asked; returns 0 or an errno value. */
static int
titlebar_state(
	struct fm_titlebar *titlebar,
	const struct fm_titlebar_state *state)
{
	const char *parts[FM_CRUMBS];
	struct kl_window *window;
	const char *last;
	unsigned focused;
	unsigned mode;
	int progress;
	int index;
	int asked;
	int error;

	/* The history's steps (Home always works); the view shown and the preview, checked. */
	window = titlebar->window->kui;
	titlebar_action_state(titlebar, FM_CONTROL_BACK, state->can_back, 0);
	titlebar_action_state(titlebar, FM_CONTROL_FORWARD, state->can_forward, 0);
	titlebar_action_state(titlebar, FM_CONTROL_ICONS, 1, titlebar_same(state->view, FM_VIEW_ICONS));
	titlebar_action_state(titlebar, FM_CONTROL_LIST, 1, titlebar_same(state->view, FM_VIEW_LIST));
	titlebar_action_state(titlebar, FM_CONTROL_PREVIEW, 1, state->preview);

	/* The path's parts, and the folder its field starts from. */
	for (index = 0; index < state->part_count; index++)
		parts[index] = state->parts[index];
	error = kl_window_set_control_parts(window, FM_CONTROL_PATH, parts, (size_t)state->part_count);
	if (error == 0)
		error = kl_window_set_control_text(window, FM_CONTROL_PATH, state->path, kl_tr("Go to folder"));

	/* The search's query. */
	if (error == 0)
		error = kl_window_set_control_text(window, FM_CONTROL_SEARCH, state->query, kl_tr("Search"));
	if (error != 0)
		return error;

	/* The progress: there while operations run, with their share done. */
	progress = 0;
	if (state->progress != FM_TITLEBAR_NO_PROGRESS)
		progress = 1;
	error = titlebar_controls_send(titlebar, progress);
	if (error != 0)
		return error;
	if (progress) {
		error = kl_window_set_control_value(window, FM_CONTROL_PROGRESS, (unsigned)state->progress);
		if (error != 0)
			return error;
	}

	/* A text field the window asked the keyboard for since the last state (the path is edited as text). */
	asked = 0;
	if (state->focus != FM_CONTROL_NONE && state->focus_serial != titlebar->shown.focus_serial)
		asked = 1;
	if (asked != 0) {
		mode = KL_FOCUS_FIELD;
		if (state->focus == FM_CONTROL_PATH)
			mode = KL_FOCUS_EDIT;
		error = kl_window_focus_control_mode(window, state->focus, mode);
		if (error != 0)
			fm_log("TITLEBAR focus-failed id=%u errno=%d", state->focus, error);
	}

	/* The path's field's suggestions, once for each list made (ws127-p010). */
	if (state->suggest_serial != titlebar->shown.suggest_serial)
		titlebar_suggest(titlebar, state);

	/* The log line the tests read: the last part, and the field given the keyboard now (0 for none). */
	last = "-";
	if (state->part_count > 0)
		last = state->parts[state->part_count - 1];
	focused = FM_CONTROL_NONE;
	if (asked != 0)
		focused = state->focus;
	fm_log("TITLEBAR state back=%d forward=%d parts=%d last=%s view=%u preview=%d progress=%d query=%s focus=%u",
	       state->can_back, state->can_forward, state->part_count, last, state->view, state->preview, state->progress, state->query, focused);

	/* Succeeded: the titlebar shows the state. */
	titlebar->shown = *state;
	titlebar->sent = 1;
	return 0;
}

/* Sets a control's action's state (enabled, checked). */
static void
titlebar_action_state(
	struct fm_titlebar *titlebar,
	uint32_t control,
	int enabled,
	int checked)
{
	unsigned state;
	int error;

	/* The bits. */
	state = 0U;
	if (!enabled)
		state |= KL_ACTION_DISABLED;
	if (checked)
		state |= KL_ACTION_CHECKED;

	/* Kept by the window for its controls. */
	error = kl_window_set_action_state(titlebar->window->kui, FM_TITLEBAR_ACTION + control, state);
	if (error != 0 && error != ENOTSUP)
		fm_log("TITLEBAR update-failed errno=%d", error);
}

/* Gives the path's field the folders it suggests (none takes the list away); a refusal is only logged. */
static void
titlebar_suggest(
	struct fm_titlebar *titlebar,
	const struct fm_titlebar_state *state)
{
	const char *labels[FM_SUGGESTIONS];
	const char *texts[FM_SUGGESTIONS];
	int index;
	int error;

	/* The labels and the texts as the library takes them. */
	for (index = 0; index < state->suggest_count; index++) {
		labels[index] = state->suggest_labels[index];
		texts[index] = state->suggest_texts[index];
	}

	/* The request; a compositor without suggestions (ENOTSUP) leaves the field as it is. */
	error = kl_window_set_control_suggestions(titlebar->window->kui, FM_CONTROL_PATH, labels, texts, (size_t)state->suggest_count);
	if (error != 0)
		fm_log("TITLEBAR suggest-failed errno=%d", error);
}

/* Tells whether a value is the one a control names (1) or not (0). */
static int
titlebar_same(
	unsigned value,
	unsigned named)
{
	/* The control's value. */
	if (value == named)
		return 1;

	/* Another value. */
	return 0;
}
