/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pacing of terminal's frames on Wayland (pace.h): each frame shown
 * asks for the compositor's frame callback on the window's surface, and
 * the next waits for its answer (or for the watchdog).  The window shows
 * its frames with its own Vulkan, so the callback is asked for here
 * rather than by libkeiland.
 */

#include "terminal.h"

#include <wayland-client.h>

#include <stddef.h>

static void frame_paced(void *data, struct wl_callback *callback, uint32_t time);

/* The compositor's answer to the frame callback of a frame shown (the pacing, pace.h). */
static const struct wl_callback_listener frame_pace_listener = {
	frame_paced
};

/*
 * Tells when the next frame may be drawn (pace.h), changing nothing: 0
 * now, or the milliseconds after which it is drawn without the
 * compositor's answer to the last frame (which wakes the wait sooner).
 */
int
terminal_window_frame_wait(
	const struct terminal_window *window,
	uint64_t now)
{
	int wait;

	/* As the pace stands now. */
	wait = terminal_pace_wait(&window->pace, now);

	/* Reports when the frame may be drawn. */
	return wait;
}

/*
 * Asks for the frame callback of the frame about to be shown, unless one
 * is still awaited (a callback unanswered past the watchdog is given up
 * first); returns 1 when it was asked for now.
 */
int
terminal_window_frame_ask(
	struct terminal_window *window,
	uint64_t now)
{
	struct wl_callback *callback;
	struct wl_surface *surface;
	int stale;
	int status;

	/* A callback unanswered for long rode on a commit that may never be shown: the new frame asks for its own. */
	stale = terminal_pace_stale(&window->pace, now);
	if (stale)
		terminal_window_frame_forget(window);

	/* A callback still awaited: the frame is shown ahead of it. */
	if (window->pace_callback != NULL)
		return 0;

	/* The callback, on the surface's next commit (the window system commits the frame after this call). */
	surface = kl_window_surface(window->kui);
	if (surface == NULL)
		return 0;
	callback = wl_surface_frame(surface);
	if (callback == NULL)
		return 0;
	status = wl_callback_add_listener(callback, &frame_pace_listener, window);
	if (status != 0) {
		wl_callback_destroy(callback);
		return 0;
	}

	/* Succeeded: the frame is awaited from now. */
	window->pace_callback = callback;
	terminal_pace_ask(&window->pace, now);
	return 1;
}

/*
 * Follows a frame's showing: asked says whether its callback was asked for
 * with it (terminal_window_frame_ask), error whether it was shown (0).  A
 * frame not shown awaits no callback asked for with it.
 */
void
terminal_window_frame_shown(
	struct terminal_window *window,
	int asked,
	int error)
{
	int forget;

	/* The pace after the frame; a frame not shown forgets the callback asked for with it. */
	forget = terminal_pace_shown(&window->pace, asked, error);
	if (forget)
		terminal_window_frame_forget(window);
}

/*
 * Stops awaiting the frame callback: the next frame may be drawn at once.
 */
void
terminal_window_frame_forget(
	struct terminal_window *window)
{
	/* The callback, when one is awaited. */
	if (window->pace_callback != NULL)
		wl_callback_destroy(window->pace_callback);
	window->pace_callback = NULL;

	/* Nothing awaited. */
	terminal_pace_clear(&window->pace);
}

/* The compositor took the frame shown with the callback: the next may be drawn. */
static void
frame_paced(
	void *data,
	struct wl_callback *callback,
	uint32_t time)
{
	struct terminal_window *window;

	(void)time;

	/* The callback is used up; nothing is awaited now. */
	window = data;
	wl_callback_destroy(callback);
	window->pace_callback = NULL;
	terminal_pace_clear(&window->pace);
}
