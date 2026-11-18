/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window's titlebar in the compositor (WS070's CONTROLS presentation, the
 * file manager's way, ws071-p014): Back, Forward and Home, the breadcrumb
 * (Settings and the page), the search field (ws089-p008) and the list of
 * pages' switch, drawn by the compositor in the floating titlebar, or in the
 * system bar while the window is docked, from the table given to
 * libkeiland's window (kl_window_set_controls, WS131 p019).  A control
 * chosen and a field's text come among the window's inputs (window.c) and
 * are queued here for the main loop.
 */

#include "window.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The controls, in their order: each one's action is SE_TITLEBAR_ACTION plus its ID. */
static const struct kl_control_entry titlebar_controls[] = {
	{ SE_CONTROL_BACK, KL_CONTROL_BACK, KL_PRIORITY_PRIMARY, 0U, "Back", SE_TITLEBAR_ACTION + SE_CONTROL_BACK },
	{ SE_CONTROL_FORWARD, KL_CONTROL_FORWARD, KL_PRIORITY_PRIMARY, 0U, "Forward", SE_TITLEBAR_ACTION + SE_CONTROL_FORWARD },
	{ SE_CONTROL_HOME, KL_CONTROL_HOME, KL_PRIORITY_PRIMARY, 0U, "Home", SE_TITLEBAR_ACTION + SE_CONTROL_HOME },
	{ SE_CONTROL_PATH, KL_CONTROL_BREADCRUMB, KL_PRIORITY_NORMAL, 0U, "Location", SE_TITLEBAR_ACTION + SE_CONTROL_PATH },
	{ SE_CONTROL_SEARCH, KL_CONTROL_SEARCH, KL_PRIORITY_NORMAL, 0U, "Search", SE_TITLEBAR_ACTION + SE_CONTROL_SEARCH },
	{ SE_CONTROL_SIDEBAR, KL_CONTROL_SIDEBAR, KL_PRIORITY_SECONDARY, 0U, "Sidebar", SE_TITLEBAR_ACTION + SE_CONTROL_SIDEBAR }
};

static void titlebar_queue(struct se_titlebar *titlebar, unsigned kind, uint32_t id, uint32_t detail, const char *text);
static int titlebar_state(struct se_titlebar *titlebar, const struct se_titlebar_state *state);
static void titlebar_action_state(struct se_titlebar *titlebar, uint32_t control, int enabled, int checked);

/*
 * Gives the compositor the window's titlebar, showing a state; the window's
 * controls' inputs come here from then on.
 *
 * Returns 0, or an errno value (ENOTSUP for a compositor without the
 * titlebar) when the titlebar could not be made.
 */
int
se_titlebar_open(
	struct se_titlebar *titlebar,
	struct se_window *window,
	const struct se_titlebar_state *state)
{
	int error;

	/* Nothing yet but the window. */
	memset(titlebar, 0, sizeof(*titlebar));
	titlebar->window = window;

	/* The controls. */
	error = kl_window_set_controls(window->kui, titlebar_controls, sizeof(titlebar_controls) / sizeof(titlebar_controls[0]));
	if (error != 0)
		return error;
	titlebar->controls = 1;
	window->titlebar = titlebar;

	/* The state it shows. */
	error = titlebar_state(titlebar, state);
	if (error != 0)
		return error;

	/* Succeeded: the titlebar is the compositor's to show. */
	se_log("TITLEBAR ready controls=%u", (unsigned)(sizeof(titlebar_controls) / sizeof(titlebar_controls[0])));
	return 0;
}

/*
 * Tells the titlebar the window's state when it differs from what it
 * shows.
 */
void
se_titlebar_refresh(
	struct se_titlebar *titlebar,
	const struct se_titlebar_state *state)
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
		se_log("TITLEBAR update-failed errno=%d", error);
}

/*
 * Queues what was done with a control (a KL_WINDOW_ACTION input of one,
 * with the breadcrumb's part in begin) or a field's text
 * (KL_WINDOW_CONTROL_TEXT, _DONE) for the main loop.
 */
void
se_titlebar_input(
	struct se_titlebar *titlebar,
	const struct kl_window_event *event)
{
	uint32_t id;

	/* A control chosen: its ID from its action. */
	if (event->kind == KL_WINDOW_ACTION) {
		id = event->code - SE_TITLEBAR_ACTION;
		se_log("TITLEBAR activated id=%u detail=%u", id, (unsigned)event->begin);
		titlebar_queue(titlebar, SE_TITLEBAR_ACTIVATED, id, (uint32_t)event->begin, "");
		return;
	}

	/* A field's text as typed, and when its editing ended (and how). */
	if (event->kind == KL_WINDOW_CONTROL_TEXT) {
		titlebar_queue(titlebar, SE_TITLEBAR_CHANGED, (uint32_t)event->id, 0U, event->text);
		return;
	}
	if (event->kind == KL_WINDOW_CONTROL_DONE)
		titlebar_queue(titlebar, SE_TITLEBAR_DONE, (uint32_t)event->id, event->code, event->text);
}

/*
 * Takes the oldest thing done with the titlebar and not yet carried out;
 * returns 1, or 0 when nothing waits.
 */
int
se_titlebar_take(
	struct se_titlebar *titlebar,
	struct se_titlebar_event *event)
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
se_titlebar_close(
	struct se_titlebar *titlebar)
{
	/* The controls, and the window's inputs go nowhere here any more. */
	if (titlebar->controls && titlebar->window->kui != NULL)
		(void)kl_window_set_controls(titlebar->window->kui, NULL, 0U);
	if (titlebar->window != NULL)
		titlebar->window->titlebar = NULL;

	/* Nothing is left. */
	memset(titlebar, 0, sizeof(*titlebar));
}

/* Puts an event at the end of the queue; a full queue keeps only the newest text of a field. */
static void
titlebar_queue(
	struct se_titlebar *titlebar,
	unsigned kind,
	uint32_t id,
	uint32_t detail,
	const char *text)
{
	struct se_titlebar_event *event;

	/* A text typed replaces the text of the same field waiting at the end. */
	if (kind == SE_TITLEBAR_CHANGED && titlebar->event_count > 0U) {
		event = &titlebar->events[titlebar->event_count - 1U];
		if (event->kind == SE_TITLEBAR_CHANGED && event->id == id) {
			(void)snprintf(event->text, sizeof(event->text), "%s", text);
			return;
		}
	}

	/* A full queue drops the event (sixteen in one round). */
	if (titlebar->event_count == SE_TITLEBAR_EVENTS)
		return;

	/* The event. */
	event = &titlebar->events[titlebar->event_count];
	event->kind = kind;
	event->id = id;
	event->detail = detail;
	(void)snprintf(event->text, sizeof(event->text), "%s", text);
	titlebar->event_count++;
}

/* Shows a state in the titlebar, then gives the search field the keyboard when asked; returns 0 or an errno value. */
static int
titlebar_state(
	struct se_titlebar *titlebar,
	const struct se_titlebar_state *state)
{
	const char *parts[SE_CRUMBS];
	struct kl_window *window;
	int index;
	int asked;
	int error;

	/* The history's steps (Home always works), and the list of pages, checked while it is shown. */
	window = titlebar->window->kui;
	titlebar_action_state(titlebar, SE_CONTROL_BACK, state->can_back, 0);
	titlebar_action_state(titlebar, SE_CONTROL_FORWARD, state->can_forward, 0);
	titlebar_action_state(titlebar, SE_CONTROL_SIDEBAR, 1, state->sidebar);

	/* The breadcrumb's parts. */
	for (index = 0; index < state->part_count; index++)
		parts[index] = state->parts[index];
	error = kl_window_set_control_parts(window, SE_CONTROL_PATH, parts, (size_t)state->part_count);
	if (error != 0)
		return error;

	/* The search's query, and what the field shows when empty. */
	error = kl_window_set_control_text(window, SE_CONTROL_SEARCH, state->query, kl_tr("Search settings"));
	if (error != 0)
		return error;

	/* The search field, when the window asked for it to have the keyboard since the last state (Ctrl+F). */
	asked = 0;
	if (state->focus_serial != titlebar->shown.focus_serial)
		asked = 1;
	if (asked != 0) {
		error = kl_window_focus_control(window, SE_CONTROL_SEARCH);
		if (error != 0)
			se_log("TITLEBAR focus-failed errno=%d", error);
	}

	/* The log line the tests read: the history's steps, the breadcrumb's last part, the query and the field given the keyboard now. */
	se_log("TITLEBAR state back=%d forward=%d parts=%d last=%s sidebar=%d query=%s focus=%d", state->can_back, state->can_forward, state->part_count, state->parts[state->part_count - 1], state->sidebar, state->query, asked);

	/* Succeeded: the titlebar shows the state. */
	titlebar->shown = *state;
	titlebar->sent = 1;
	return 0;
}

/* Sets a control's action's state (enabled, checked). */
static void
titlebar_action_state(
	struct se_titlebar *titlebar,
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
	error = kl_window_set_action_state(titlebar->window->kui, SE_TITLEBAR_ACTION + control, state);
	if (error != 0 && error != ENOTSUP)
		se_log("TITLEBAR update-failed errno=%d", error);
}
