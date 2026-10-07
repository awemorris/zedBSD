/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The terminal's tabs in the compositor's titlebar (the Titlebar Presentation's
 * TABS mode, plan/ws070/titlebar-design.md, ws035-p086).
 *
 * Each tab is a shell of its own (main.c).  With one tab the titlebar
 * shows the menus (MENU mode); with two or more it shows the tabs, with
 * "+" for a new one and the menus under "..." (TABS mode), given to
 * libkeiland's window as a table (kl_window_set_tabs, WS131 p018).
 * The compositor draws them and tells the terminal a tab chosen, a tab's close
 * button and "+" as the window's KL_WINDOW_TAB inputs; those are queued
 * here for the main loop.  The tabs are sent only when they changed.
 * Without the compositor's titlebar the terminal still has its tabs, switched
 * from the Shell menu's keys.
 */

#include "terminal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void tabs_queue(struct terminal_window *window, unsigned kind, uint32_t id);
static int tabs_same(const struct terminal_window *window, const struct terminal_tab_view *tabs, unsigned count, uint32_t active);

/*
 * Shows the tabs and the active one: the menus while there is one tab, the
 * tabs (each closable, "+" after them) from two.  Only a change is sent.
 */
void
terminal_tabs_show(
	struct terminal_window *window,
	const struct terminal_tab_view *tabs,
	unsigned count,
	uint32_t active)
{
	struct kl_tab_entry entries[TERMINAL_TABS];
	unsigned shown;
	unsigned index;
	unsigned mode;
	int same;
	int error;

	/* Only a change. */
	same = tabs_same(window, tabs, count, active);
	if (same)
		return;

	/* The tabs from two (each closable, the active one marked); none with one, which shows the menus. */
	memset(entries, 0, sizeof(entries));
	shown = 0U;
	mode = KL_TITLEBAR_MENU;
	if (count >= 2U) {
		shown = count;
		mode = KL_TITLEBAR_TABS;
	}
	if (shown > TERMINAL_TABS)
		shown = TERMINAL_TABS;
	for (index = 0; index < shown; index++) {
		entries[index].id = tabs[index].id;
		entries[index].title = tabs[index].title;
		entries[index].flags = KL_TAB_CLOSABLE;
		if (tabs[index].id == active)
			entries[index].flags |= KL_TAB_ACTIVE;
	}

	/* To libkeiland's window, with "+"; without the compositor's titlebar nothing is shown. */
	error = kl_window_set_tabs(window->kui, entries, shown, KL_TABS_NEW_BUTTON);
	if (error != 0 && error != ENOTSUP)
		printf("ZTERM TABS failed errno=%d\n", error);

	/* What was shown, to send only changes. */
	for (index = 0; index < count && index < TERMINAL_TABS; index++)
		window->tabs_shown[index] = tabs[index];
	window->tabs_shown_count = count;
	window->tabs_shown_active = active;
	window->tabs_sent = 1;
	printf("ZTERM TABS count=%u active=%u mode=%u\n", count, active, mode);
	fflush(stdout);
}

/*
 * Queues what the titlebar asked of the tabs (a KL_WINDOW_TAB input of the
 * window): a tab chosen, a tab to close, a new one.
 */
void
terminal_tabs_input(
	struct terminal_window *window,
	const struct kl_window_event *event)
{
	/* Which request. */
	switch (event->code) {
	case KL_WINDOW_TAB_CHOSEN:
		tabs_queue(window, TERMINAL_TAB_ACTIVATE, (uint32_t)event->id);
		break;
	case KL_WINDOW_TAB_CLOSE:
		tabs_queue(window, TERMINAL_TAB_CLOSE, (uint32_t)event->id);
		break;
	case KL_WINDOW_TAB_NEW:
		tabs_queue(window, TERMINAL_TAB_NEW, 0U);
		break;
	default:
		break;
	}
}

/*
 * Takes the oldest thing the titlebar asked of the tabs.  Returns 1 with
 * it, or 0 when nothing waits.
 */
int
terminal_tabs_take(
	struct terminal_window *window,
	struct terminal_tab_request *request)
{
	/* Nothing waits. */
	if (window->tab_request_count == 0U)
		return 0;

	/* The oldest, and the others move up. */
	*request = window->tab_requests[0];
	window->tab_request_count--;
	memmove(&window->tab_requests[0], &window->tab_requests[1], window->tab_request_count * sizeof(window->tab_requests[0]));

	/* Succeeded: one request. */
	return 1;
}

/* Queues one request for the main loop (a full queue drops it). */
static void
tabs_queue(
	struct terminal_window *window,
	unsigned kind,
	uint32_t id)
{
	/* A full queue: the user is far ahead. */
	if (window->tab_request_count == TERMINAL_ACTIONS)
		return;

	/* The request at the end. */
	window->tab_requests[window->tab_request_count].kind = kind;
	window->tab_requests[window->tab_request_count].id = id;
	window->tab_request_count++;
}

/* Tells whether tabs are the ones last shown (IDs, titles and the active one). */
static int
tabs_same(
	const struct terminal_window *window,
	const struct terminal_tab_view *tabs,
	unsigned count,
	uint32_t active)
{
	unsigned index;
	int same;

	/* Never shown, another count or another active tab. */
	if (!window->tabs_sent ||
	    count != window->tabs_shown_count ||
	    active != window->tabs_shown_active)
		return 0;

	/* Each tab. */
	for (index = 0; index < count; index++) {
		/* Another ID. */
		if (tabs[index].id != window->tabs_shown[index].id)
			return 0;

		/* Another title. */
		same = strcmp(tabs[index].title, window->tabs_shown[index].title);
		if (same != 0)
			return 0;
	}

	/* The same. */
	return 1;
}
