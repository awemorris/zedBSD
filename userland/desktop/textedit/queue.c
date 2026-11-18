/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The editor's queue of inputs (ws090-p004): the window's pointer, keys
 * and focus, which the main loop turns from libkeiland's window events into
 * te_event values, and the actions of the menus and the file chooser,
 * carried out in the order they came.
 */

#include "window.h"

#include <string.h>

/*
 * Takes the oldest queued input; zero when there is none.
 */
int
te_window_take(
	struct te_window *window,
	struct te_event *event)
{
	/* An empty queue. */
	if (window->event_count == 0U)
		return 0;

	/* The oldest input, and the queue moves on. */
	*event = window->events[window->event_first];
	window->event_first = (window->event_first + 1U) % TE_WINDOW_EVENTS;
	window->event_count--;

	/* Succeeded: one input taken. */
	return 1;
}

/*
 * Queues a new input of a kind at the pointer's place with the modifiers
 * held; NULL when the queue is full.
 */
struct te_event *
te_window_push(
	struct te_window *window,
	enum te_event_type type)
{
	struct te_event *event;
	unsigned slot;

	/* A full queue drops the input (the user is far ahead of the program). */
	if (window->event_count == TE_WINDOW_EVENTS)
		return NULL;

	/* The slot after the last one queued. */
	slot = (window->event_first + window->event_count) % TE_WINDOW_EVENTS;
	window->event_count++;

	/* The input, with what every input carries. */
	event = &window->events[slot];
	memset(event, 0, sizeof(*event));
	event->type = type;
	event->x = window->pointer_x;
	event->y = window->pointer_y;
	event->modifiers = window->modifiers;
	event->time = te_clock();

	/* Reports the queued input for its details. */
	return event;
}

/*
 * Queues an action among the editor's inputs (the main loop's, when the
 * window's queue hands it over).
 */
void
te_window_act(
	struct te_window *window,
	uint32_t action)
{
	struct te_event *event;

	/* The input; a full queue drops it. */
	event = te_window_push(window, TE_EVENT_ACTION);
	if (event == NULL)
		return;
	event->action = action;
}
