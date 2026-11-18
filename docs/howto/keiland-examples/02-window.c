/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A window that the program draws itself: the theme's ground, and a dot
 * that follows the pointer.  It ends when its window is closed.
 */

#include <keiland/keiland.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The dot's radius, in pixels. */
#define DOT_RADIUS	12.0f

/*
 * Everything the program keeps: its connection and window, the frame it
 * draws into, where the pointer is, and what the main loop has to do next.
 */
struct hello {
	struct kl_app *app;
	struct kl_window *window;
	struct kl_canvas canvas;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	double pointer_x;
	double pointer_y;
	int pointer_in;
	int dirty;
	int resized;
	int closed;
};

static int hello_resize(struct hello *hello);
static void hello_draw(struct hello *hello);
static int hello_frame(struct hello *hello);
static void hello_input(struct hello *hello, const struct kl_window_event *input);

int
main(void)
{
	static struct hello hello;
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	struct kl_app_event event;
	int timeout;
	int status;
	int taken;

	/* Connects to the desktop. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "org.example.Window";
	hello.app = kl_app_open(&app_options);
	if (hello.app == NULL) {
		fprintf(stderr, "window: no desktop (%s)\n", strerror(errno));
		return 1;
	}

	/* Opens the window; it is configured when the call returns. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Window";
	window_options.width = 480;
	window_options.height = 320;
	window_options.present = KL_PRESENT_VULKAN;
	hello.window = kl_app_window_create(hello.app, &window_options);
	if (hello.window == NULL) {
		fprintf(stderr, "window: no window (%s)\n", strerror(errno));
		return 1;
	}

	/* Makes the first frame at the size the compositor gave. */
	status = hello_resize(&hello);
	if (status != 0)
		return 1;

	/* Runs until the window is closed: waits, takes the events, draws when something changed. */
	while (!hello.closed) {
		/* Waits for the desktop; not at all while a frame is due. */
		timeout = -1;
		if (hello.dirty || hello.resized)
			timeout = 0;
		status = kl_app_dispatch(hello.app, timeout);
		if (status != 0)
			break;

		/* Takes every event queued, in order. */
		for (;;) {
			taken = kl_app_take(hello.app, &event);
			if (!taken)
				break;

			/* The appearance changed: the theme's colours are new. */
			if (event.kind == KL_APP_THEME)
				hello.dirty = 1;

			/* An input of the window. */
			if (event.kind == KL_APP_WINDOW && event.window == hello.window)
				hello_input(&hello, &event.input);
		}

		/* A new size: a new frame of that size. */
		if (hello.resized) {
			hello.resized = 0;
			status = hello_resize(&hello);
			if (status != 0)
				break;
		}

		/* Something changed: draws and shows a frame. */
		if (hello.dirty) {
			status = hello_frame(&hello);
			if (status != 0)
				break;
		}
	}

	/* Closes the window and the connection, and frees the frame. */
	kl_canvas_release(&hello.canvas);
	free(hello.pixels);
	kl_app_close(hello.app);

	/* Succeeded: the window was closed. */
	return 0;
}

/*
 * Makes the frame at the window's size; nonzero when it cannot.
 */
static int
hello_resize(
	struct hello *hello)
{
	uint32_t *pixels;
	int status;

	/* Sets the presenter to the window's size, which it reports. */
	status = kl_window_present_resize(hello->window, &hello->width, &hello->height);
	if (status != 0)
		return -1;

	/* Allocates the frame: one premultiplied 0xAARRGGBB word per pixel. */
	pixels = malloc((size_t)hello->width * hello->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* Puts the canvas on the new pixels in place of the old ones. */
	if (hello->pixels != NULL)
		kl_canvas_release(&hello->canvas);
	free(hello->pixels);
	hello->pixels = pixels;
	status = kl_canvas_init(&hello->canvas, hello->pixels, hello->width, (int)hello->width, (int)hello->height);
	if (status != 0)
		return -1;

	/* Succeeded: the new frame is drawn next. */
	hello->dirty = 1;
	return 0;
}

/*
 * Draws one frame: the ground, and the dot where the pointer is.
 */
static void
hello_draw(
	struct hello *hello)
{
	const struct kl_theme *theme;
	struct kl_rect whole;

	/* Paints the theme's ground over the whole frame. */
	theme = kl_theme_default();
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)hello->width;
	whole.height = (int)hello->height;
	kl_canvas_gradient(&hello->canvas, &whole, theme->ground_top, theme->ground_bottom);

	/* Puts the dot under the pointer while the pointer is over the window. */
	if (hello->pointer_in)
		kl_canvas_circle(&hello->canvas, (float)hello->pointer_x, (float)hello->pointer_y, DOT_RADIUS, theme->accent);
}

/*
 * Draws and shows a frame; nonzero when it cannot be shown.
 */
static int
hello_frame(
	struct hello *hello)
{
	int status;

	/* Draws the frame and hands it to the compositor. */
	hello->dirty = 0;
	hello_draw(hello);
	status = kl_window_present(hello->window, hello->pixels, hello->width);

	/* The window's size changed under the frame: a new frame of the new size. */
	if (status == EAGAIN) {
		hello->resized = 1;
		return 0;
	}

	/* Refuses to go on when the frame could not be shown. */
	if (status != 0)
		return -1;

	/* Succeeded: the frame is shown. */
	return 0;
}

/*
 * Acts on one input of the window.
 */
static void
hello_input(
	struct hello *hello,
	const struct kl_window_event *input)
{
	/* Sorts the input by its kind. */
	switch (input->kind) {
	case KL_WINDOW_MOTION:
		/* The pointer moved: the dot follows it. */
		hello->pointer_x = input->x;
		hello->pointer_y = input->y;
		hello->pointer_in = 1;
		hello->dirty = 1;
		break;
	case KL_WINDOW_LEAVE:
		/* The pointer left the window: the dot goes. */
		hello->pointer_in = 0;
		hello->dirty = 1;
		break;
	case KL_WINDOW_RESIZE:
		/* The compositor gave a new size. */
		hello->resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		/* The close button: the program ends. */
		hello->closed = 1;
		break;
	default:
		/* The keys, the fingers and the rest: not used here. */
		break;
	}
}
