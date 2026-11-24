/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The drawing of terminal on the processor (render-cpu.c) behind
 * display.h: each frame is painted into memory and libkeiland shows the
 * parts that changed (a window of KL_PRESENT_SHM, or the system's own way
 * of showing a frame of memory).  What failed last is a call of the
 * library or the memory, and its errno value.
 */

#include "render-cpu.h"
#include "display.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * The drawing of one window: the painting, the window it shows on,
 * whether the frame painted last has not been shown yet, and the call
 * that failed last with its errno value (0 until one fails).
 */
struct terminal_display {
	struct terminal_cpu cpu;
	struct kl_window *window;
	int pending;
	const char *operation;
	int result;
};

static int display_fit(struct terminal_display *display);

/*
 * Makes the drawing of the window: the atlas (given to the font), the
 * build and a frame at the size libkeiland shows.
 *
 * Returns 0, ENOMEM when the drawing has no memory (no display is made),
 * or an errno value with the display made in part (terminal_display_close
 * releases it).
 */
int
terminal_display_open(
	struct terminal_display **display,
	struct terminal_window *window,
	struct terminal_font *font)
{
	struct terminal_display *made;
	int error;

	/* The display's memory. */
	*display = NULL;
	made = malloc(sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* Nothing is owned yet; the caller holds what is made from now on. */
	memset(made, 0, sizeof(*made));
	made->window = window->kui;
	made->operation = "none";
	*display = made;

	/* The atlas, the build and the copy of what is painted. */
	made->operation = "terminal_cpu_open";
	error = terminal_cpu_open(&made->cpu, font);
	if (error != 0) {
		made->result = error;
		return error;
	}

	/* The frame at the window's size. */
	error = display_fit(made);
	if (error != 0)
		return error;

	/* Succeeded: frames can be painted. */
	return 0;
}

/*
 * Makes the frame the window's new size, as libkeiland shows it (the size
 * asked for is the window's, which libkeiland already follows).
 *
 * Returns 0 or an errno value.
 */
int
terminal_display_resize(
	struct terminal_display *display,
	uint32_t width,
	uint32_t height)
{
	int error;

	(void)width;
	(void)height;

	/* The frame at the size libkeiland shows. */
	error = display_fit(display);
	if (error != 0)
		return error;

	/* Succeeded: the next frame is painted at the new size. */
	return 0;
}

/*
 * Paints the grid, and over it the composed text, and shows the parts
 * that changed.
 *
 * Returns 0, EBUSY when the window has no free buffer to show the frame
 * yet, EAGAIN when the window's size changed (the caller resizes and
 * draws again), or an errno value.
 */
int
terminal_display_draw(
	struct terminal_display *display,
	struct terminal_screen *screen,
	struct terminal_font *font,
	const struct terminal_window *window)
{
	struct kl_rect parts[TERMINAL_CPU_BANDS];
	const struct kl_rect *shown;
	unsigned count;
	int changed;
	int error;

	/* The frame's quads painted where they changed. */
	changed = terminal_cpu_draw(&display->cpu, screen, font, window, parts, &count);
	if (!changed && !display->pending)
		return 0;

	/* A frame whose last showing did not go out is shown whole. */
	shown = parts;
	if (display->pending) {
		shown = NULL;
		count = 0U;
	}

	/* The parts shown (all of it for none); a window with no free buffer asks to try again later. */
	display->operation = "kl_window_present_parts";
	error = kl_window_present_parts(display->window, display->cpu.pixels, (size_t)display->cpu.width, shown, count);
	display->result = error;
	if (error == EAGAIN) {
		display->pending = 1;
		return EBUSY;
	}
	if (error != 0) {
		display->pending = 1;
		return error;
	}

	/* Succeeded: the grid is on the window. */
	display->pending = 0;
	return 0;
}

/*
 * Shows a frame painted elsewhere (width by height pixels, a row after
 * another) in the grid's place; the next grid is painted whole, since the
 * window no longer shows the last one.
 *
 * Returns 0, EBUSY when the window has no free buffer to show the frame
 * yet, EAGAIN when the frame is not the size the window is shown at (the
 * caller resizes and paints again), or an errno value.
 */
int
terminal_display_page(
	struct terminal_display *display,
	const uint32_t *pixels,
	uint32_t width,
	uint32_t height)
{
	int error;

	/* A frame of another size than the window's. */
	if (width != display->cpu.width || height != display->cpu.height)
		return EAGAIN;

	/* The whole frame shown; a window with no free buffer asks to try again later. */
	display->operation = "kl_window_present";
	error = kl_window_present(display->window, pixels, (size_t)width);
	display->result = error;
	if (error == EAGAIN)
		return EBUSY;
	if (error != 0)
		return error;

	/* Succeeded: the next grid covers the page whole. */
	display->cpu.whole = 1;
	display->pending = 0;
	return 0;
}

/*
 * Waits for the frame shown last: nothing, the processor painted it
 * before it was shown.
 */
int
terminal_display_idle(
	struct terminal_display *display)
{
	(void)display;

	/* Reports nothing to wait for. */
	return 0;
}

/*
 * Gives the size the frames are painted at.
 */
void
terminal_display_size(
	const struct terminal_display *display,
	uint32_t *width,
	uint32_t *height)
{
	/* The frame's size. */
	*width = display->cpu.width;
	*height = display->cpu.height;
}

/*
 * Names the call that failed last ("none" before one ran) and gives its
 * errno value, for the error line.
 */
const char *
terminal_display_operation(
	const struct terminal_display *display,
	int *result)
{
	/* No display was made: its memory failed. */
	if (display == NULL) {
		*result = ENOMEM;
		return "malloc";
	}

	/* The call and its result. */
	*result = display->result;
	return display->operation;
}

/*
 * Releases the painting and the display.
 */
void
terminal_display_close(
	struct terminal_display *display)
{
	/* No display was made. */
	if (display == NULL)
		return;

	/* The frame, the build and the atlas, then the display. */
	terminal_cpu_close(&display->cpu);
	free(display);
}

/* Makes the frame the size libkeiland shows the window at; returns 0 or an errno value. */
static int
display_fit(
	struct terminal_display *display)
{
	uint32_t width;
	uint32_t height;
	int error;

	/* The size a frame is shown at. */
	display->operation = "kl_window_present_resize";
	error = kl_window_present_resize(display->window, &width, &height);
	display->result = error;
	if (error != 0)
		return error;

	/* The frame at that size. */
	display->operation = "terminal_cpu_resize";
	error = terminal_cpu_resize(&display->cpu, width, height);
	display->result = error;
	if (error != 0)
		return error;

	/* Succeeded: the frame fits the window. */
	return 0;
}
