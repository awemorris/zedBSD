/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window's titlebar in the compositor (WS070's CONTROLS presentation): back,
 * forward, reload, and the location of the page (the parts of its path,
 * which the compositor turns into a field for the whole URL when it is edited).
 *
 * The compositor draws the controls and edits the field; this file declares them
 * on the window (libkeiland's controls, WS131 p025; the compositor's titlebar
 * object in transactions before) with the browser's state, and queues what
 * the window hears from them (a control's KL_WINDOW_ACTION, the field's
 * KL_WINDOW_CONTROL_DONE) for the main loop.  A compositor without the
 * titlebar leaves the window with the compositor's plain titlebar and the
 * keyboard's shortcuts.
 *
 * WS169 p005: while a sign-in code that came by mail is offered, a fifth
 * control at the front, "Code 482913", types it into the page.
 */

#include "shell/internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The most parts of a path the location shows. */
#define TITLEBAR_PARTS		32U

/* The scheme put before the location's text when it is a file's path (a URL keeps its own). */
#define TITLEBAR_FILE_SCHEME	"file://"

/* The actions of the controls: this base and the control's ID. */
#define TITLEBAR_ACTION		0x100U

/* The controls, in their order: ID, role, priority, group, label and action. */
static const struct kl_control_entry titlebar_controls[] = {
	{ SHELL_CONTROL_BACK, KL_CONTROL_BACK, KL_PRIORITY_PRIMARY, 0U, "Back", TITLEBAR_ACTION + SHELL_CONTROL_BACK },
	{ SHELL_CONTROL_FORWARD, KL_CONTROL_FORWARD, KL_PRIORITY_PRIMARY, 0U, "Forward", TITLEBAR_ACTION + SHELL_CONTROL_FORWARD },
	{ SHELL_CONTROL_RELOAD, KL_CONTROL_GENERIC, KL_PRIORITY_NORMAL, 0U, "Reload", TITLEBAR_ACTION + SHELL_CONTROL_RELOAD },
	{ SHELL_CONTROL_LOCATION, KL_CONTROL_BREADCRUMB, KL_PRIORITY_NORMAL, 0U, "Location", TITLEBAR_ACTION + SHELL_CONTROL_LOCATION }
};

/* The control of a sign-in code offered, put before the others while it is (its label is the code's). */
#define TITLEBAR_CONTROLS_MAX	5U

static void titlebar_queue(struct shell_titlebar *titlebar, int kind, uint32_t id, uint32_t detail, const char *text);
static int titlebar_state(struct shell_titlebar *titlebar, int can_back, int can_forward, const char *path);
static size_t titlebar_scheme_length(const char *location);

/*
 * Declares the window's titlebar controls; the window gives the titlebar
 * what is done with them.
 *
 * Returns 0, or an errno value (ENOTSUP for a compositor without the
 * titlebar).
 */
int
shell_titlebar_open(
	struct shell_titlebar *titlebar,
	struct shell_window *window)
{
	int error;

	/* Nothing yet. */
	memset(titlebar, 0, sizeof(*titlebar));

	/* The controls, in their order. */
	error = kl_window_set_controls(window->kui, titlebar_controls, sizeof(titlebar_controls) / sizeof(titlebar_controls[0]));
	if (error != 0)
		return error;

	/* Succeeded: the titlebar is the compositor's to show, and the window's inputs from it come here. */
	titlebar->kui = window->kui;
	window->titlebar = titlebar;
	return 0;
}

/*
 * Shows the browser's state: whether the history goes back and forward,
 * and the path of the page shown.
 */
int
shell_titlebar_show(
	struct shell_titlebar *titlebar,
	int can_back,
	int can_forward,
	const char *path)
{
	int error;

	/* Without a titlebar nothing is shown. */
	if (titlebar->kui == NULL)
		return 0;

	/* The state. */
	error = titlebar_state(titlebar, can_back, can_forward, path);
	if (error != 0)
		return error;

	/* The line the tests read. */
	printf("ZBROWSER TITLEBAR back=%d forward=%d path=%s\n", can_back, can_forward, path);
	fflush(stdout);

	/* Succeeded: the titlebar shows the state. */
	return 0;
}

/*
 * Turns the location into a field for editing the URL, and gives it the
 * keyboard.
 */
int
shell_titlebar_edit_location(
	struct shell_titlebar *titlebar)
{
	int error;

	/* Without a titlebar there is no field. */
	if (titlebar->kui == NULL)
		return ENOTSUP;

	/* The field takes the keyboard. */
	error = kl_window_focus_control_mode(titlebar->kui, SHELL_CONTROL_LOCATION, KL_FOCUS_EDIT);
	if (error != 0)
		return error;

	/* Succeeded: the URL can be edited. */
	return 0;
}

/*
 * Offers a sign-in code as a control at the front of the titlebar with a
 * label, or (NULL) takes the offer away: the controls are declared again,
 * so the caller shows the browser's state after.
 *
 * Returns 0, or an errno value (ENOTSUP without a titlebar).
 */
int
shell_titlebar_offer_code(
	struct shell_titlebar *titlebar,
	const char *label)
{
	struct kl_control_entry entries[TITLEBAR_CONTROLS_MAX];
	size_t count;
	size_t index;
	int error;

	/* Without a titlebar there is nowhere to offer it. */
	if (titlebar->kui == NULL)
		return ENOTSUP;

	/* The code's control first, while there is one. */
	count = 0;
	if (label != NULL) {
		memset(&entries[count], 0, sizeof(entries[count]));
		entries[count].id = SHELL_CONTROL_CODE;
		entries[count].role = KL_CONTROL_GENERIC;
		entries[count].priority = KL_PRIORITY_PRIMARY;
		entries[count].label = label;
		entries[count].action = TITLEBAR_ACTION + SHELL_CONTROL_CODE;
		count++;
	}

	/* The browser's own controls. */
	for (index = 0; index < sizeof(titlebar_controls) / sizeof(titlebar_controls[0]); index++) {
		entries[count] = titlebar_controls[index];
		count++;
	}

	/* Declared again. */
	error = kl_window_set_controls(titlebar->kui, entries, count);
	if (error != 0)
		return error;

	/* The line the tests read (not the code). */
	printf("ZBROWSER TITLEBAR code=%d\n", label != NULL);
	fflush(stdout);
	return 0;
}

/*
 * Takes the oldest thing done with the titlebar and not yet carried out;
 * returns 1, or 0 when nothing waits.
 */
int
shell_titlebar_take(
	struct shell_titlebar *titlebar,
	struct shell_titlebar_event *event)
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
 * Queues what the window heard from the titlebar: a control chosen (its
 * action, with a breadcrumb's part in begin), or the end of the location's
 * editing (how in code, with its text).
 */
void
shell_titlebar_post(
	struct shell_titlebar *titlebar,
	const struct kl_window_event *event)
{
	/* A control chosen: its ID from its action. */
	if (event->kind == KL_WINDOW_ACTION) {
		if (event->code <= TITLEBAR_ACTION)
			return;
		titlebar_queue(titlebar, SHELL_TITLEBAR_ACTIVATED, event->code - TITLEBAR_ACTION, (uint32_t)event->begin, "");
		return;
	}

	/* The field's editing ended, and how. */
	if (event->kind == KL_WINDOW_CONTROL_DONE)
		titlebar_queue(titlebar, SHELL_TITLEBAR_DONE, (uint32_t)event->id, event->code, event->text);
}

/*
 * Takes the controls away (before the window goes).
 */
void
shell_titlebar_close(
	struct shell_titlebar *titlebar)
{
	/* The controls go with the window; nothing is left here. */
	memset(titlebar, 0, sizeof(*titlebar));
}

/* Puts an event at the end of the queue; a full queue drops it. */
static void
titlebar_queue(
	struct shell_titlebar *titlebar,
	int kind,
	uint32_t id,
	uint32_t detail,
	const char *text)
{
	struct shell_titlebar_event *event;

	/* A full queue drops the event (sixteen in one round). */
	if (titlebar->event_count == SHELL_TITLEBAR_EVENTS)
		return;

	/* The event. */
	event = &titlebar->events[titlebar->event_count];
	event->kind = kind;
	event->id = id;
	event->detail = detail;
	snprintf(event->text, sizeof(event->text), "%s", text);
	titlebar->event_count++;
}

/* Shows the history's steps and the page's location; returns 0 or an errno value. */
static int
titlebar_state(
	struct shell_titlebar *titlebar,
	int can_back,
	int can_forward,
	const char *path)
{
	const char *parts[TITLEBAR_PARTS];
	char copy[SHELL_TITLEBAR_TEXT];
	char url[SHELL_TITLEBAR_TEXT + 8U];
	size_t scheme_length;
	size_t count;
	unsigned back;
	unsigned forward;
	char *part;
	char *next;
	int error;

	/*
	 * A location with a scheme (https:, about:, file:) is a URL, edited as
	 * it is; any other is a file's path, edited as a file: URL (BUG-206: an
	 * https: page's URL got file:// before it).
	 */
	scheme_length = titlebar_scheme_length(path);
	if (scheme_length != 0U) {
		snprintf(url, sizeof(url), "%s", path);
	} else {
		snprintf(url, sizeof(url), "%s%s", TITLEBAR_FILE_SCHEME, path);
	}

	/* The parts after the scheme, between the slashes (the first ones give way when there are too many): a URL's host, then its path's. */
	snprintf(copy, sizeof(copy), "%s", path + scheme_length);
	count = 0;
	part = copy;
	while (part != NULL && *part != '\0') {
		next = strchr(part, '/');
		if (next != NULL) {
			*next = '\0';
			next++;
		}

		/* A part that is not empty is shown, the first one giving way when the list is full. */
		if (*part != '\0') {
			if (count == TITLEBAR_PARTS) {
				memmove(parts, parts + 1, (TITLEBAR_PARTS - 1U) * sizeof(parts[0]));
				count--;
			}

			/* The part goes at the end. */
			parts[count] = part;
			count++;
		}

		/* The part after it. */
		part = next;
	}

	/* The history's steps. */
	back = KL_ACTION_DISABLED;
	if (can_back)
		back = 0U;
	forward = KL_ACTION_DISABLED;
	if (can_forward)
		forward = 0U;
	error = kl_window_set_action_state(titlebar->kui, TITLEBAR_ACTION + SHELL_CONTROL_BACK, back);
	if (error == 0)
		error = kl_window_set_action_state(titlebar->kui, TITLEBAR_ACTION + SHELL_CONTROL_FORWARD, forward);

	/* The location's parts and its URL. */
	if (error == 0)
		error = kl_window_set_control_parts(titlebar->kui, SHELL_CONTROL_LOCATION, parts, count);
	if (error == 0)
		error = kl_window_set_control_text(titlebar->kui, SHELL_CONTROL_LOCATION, url, "URL or file path");
	if (error != 0)
		return error;

	/* Succeeded: the titlebar shows the state. */
	return 0;
}

/*
 * Reports the length of the scheme a location starts with, with its colon
 * ("https:" is 6), or 0 when it has none (a file's path): a letter, then
 * letters, digits, "+", "-" and ".", then a colon (RFC 3986).
 */
static size_t
titlebar_scheme_length(
	const char *location)
{
	size_t length;
	int letter;
	char c;

	/* A scheme starts with a letter. */
	c = location[0];
	letter = 0;
	if (c >= 'a' && c <= 'z')
		letter = 1;
	else if (c >= 'A' && c <= 'Z')
		letter = 1;
	if (!letter)
		return 0;

	/* The scheme's characters, up to the colon that ends it. */
	for (length = 1; location[length] != '\0'; length++) {
		c = location[length];

		/* The colon ends the scheme. */
		if (c == ':')
			return length + 1U;

		/* A letter, a digit, "+", "-" and "." go on with it; anything else means there is no scheme. */
		if (c >= 'a' && c <= 'z')
			continue;
		if (c >= 'A' && c <= 'Z')
			continue;
		if (c >= '0' && c <= '9')
			continue;
		if (c == '+' || c == '-' || c == '.')
			continue;
		return 0;
	}

	/* No colon: a file's path. */
	return 0;
}
