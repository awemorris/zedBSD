/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pen of Notes (ws079-p003): a pen tablet's tools as libkeiland's
 * window gives them (WS131 p018, the tablet protocol in libkeiland's
 * tablet.c), turned into Notes' input events (window.c's queue): a
 * contact starting is NOTES_INPUT_DOWN, a move in contact
 * NOTES_INPUT_MOTION, a lift NOTES_INPUT_UP; a move over the window
 * without contact is NOTES_INPUT_HOVER and leaving the window
 * NOTES_INPUT_LEAVE, which Notes uses to show where the pen is (the
 * compositor's cursor is hidden while a tool is over the window).  The
 * pressure comes as 0..1 (a tool without it draws with the pointer's); the
 * tilt in degrees.  The eraser end is NOTES_SOURCE_ERASER, and so is the
 * pen's tip while its first barrel button is held (design-input-notes.md
 * D5).
 *
 * Without the compositor's tablet protocol the pen comes as the pointer
 * (window.c), with the pointer's fixed pressure.
 */

#include "app.h"

#include <string.h>

/*
 * Queues a tablet's input of the window (KL_WINDOW_TABLET_*) as Notes'
 * input event.
 */
void
notes_tablet_input(
	struct notes_window *window,
	const struct kl_window_event *event)
{
	struct notes_input input;

	/* The kind of event. */
	memset(&input, 0, sizeof(input));
	switch (event->kind) {
	case KL_WINDOW_TABLET_DOWN:
		input.kind = NOTES_INPUT_DOWN;
		break;
	case KL_WINDOW_TABLET_MOTION:
		input.kind = NOTES_INPUT_MOTION;
		break;
	case KL_WINDOW_TABLET_UP:
		input.kind = NOTES_INPUT_UP;
		break;
	case KL_WINDOW_TABLET_HOVER:
		input.kind = NOTES_INPUT_HOVER;
		break;
	case KL_WINDOW_TABLET_LEAVE:
		input.kind = NOTES_INPUT_LEAVE;
		break;
	default:
		return;
	}

	/* The eraser end, or the tip erasing while its first barrel button is held, or the pen. */
	input.source = NOTES_SOURCE_PEN;
	if (event->tool == KL_TABLET_ERASER || (event->buttons & KL_TABLET_BUTTON_STYLUS) != 0U)
		input.source = NOTES_SOURCE_ERASER;

	/* Its place, pressure (the pointer's without the tool's own) and tilt, at the compositor's time. */
	input.x = (float)event->x;
	input.y = (float)event->y;
	input.pressure = (float)event->pressure;
	if (event->pressure < 0.0)
		input.pressure = NOTES_POINTER_PRESSURE;
	input.tilt_x = (float)event->tilt_x;
	input.tilt_y = (float)event->tilt_y;
	input.time_ms = (uint32_t)(event->time_us / 1000U);

	/* Queued like the pointer's. */
	notes_window_input(window, &input);
}
