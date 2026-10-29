/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A counter: a window with a number and three buttons, Less, Reset and
 * More.  The Up and Down keys change the number too.  The widgets are
 * Keiland's immediate mode widgets: each is one call in the frame.
 */

#include <keiland/keiland.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Where the desktop's fonts are (/opt/keiland/share/fonts for a Linux build under /opt/keiland). */
#ifndef FONT_DIR
#define FONT_DIR	"/usr/share/fonts"
#endif
#define FONT		FONT_DIR "/keiland.ttf"
#define FALLBACK_FONT	FONT_DIR "/keiland-fallback.ttf"

/* The widgets' ids: a number per widget, unique in the window, the same in every frame. */
#define ID_LESS		1U
#define ID_RESET	2U
#define ID_MORE		3U

/* The buttons' width and the room around them, in pixels. */
#define BUTTON_WIDTH	96
#define MARGIN		24

/*
 * Everything the program keeps: its connection and window, the widgets'
 * record (ui), the font, the frame, the state the widgets show (count),
 * and what the main loop has to do next.
 */
struct counter {
	struct kl_app *app;
	struct kl_window *window;
	struct kl_ui *ui;
	struct kl_text text;
	struct kl_style style;
	struct kl_canvas canvas;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	int count;
	int dirty;
	int moving;
	int resized;
	int closed;
};

static int counter_resize(struct counter *counter);
static void counter_draw(struct counter *counter, uint64_t now_us);
static int counter_frame(struct counter *counter);
static void counter_input(struct counter *counter, const struct kl_window_event *input);
static void counter_unclaimed(struct counter *counter);

int
main(void)
{
	static struct counter counter;
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	struct kl_app_event event;
	int timeout;
	int status;
	int taken;

	/* Connects to the desktop. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "org.example.Counter";
	counter.app = kl_app_open(&app_options);
	if (counter.app == NULL) {
		fprintf(stderr, "counter: no desktop (%s)\n", strerror(errno));
		return 1;
	}

	/* Opens the window. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Counter";
	window_options.width = 400;
	window_options.height = 240;
	window_options.present = KL_PRESENT_VULKAN;
	counter.window = kl_app_window_create(counter.app, &window_options);
	if (counter.window == NULL) {
		fprintf(stderr, "counter: no window (%s)\n", strerror(errno));
		return 1;
	}

	/* Makes the widgets' record of the window. */
	counter.ui = kl_ui_create();
	if (counter.ui == NULL)
		return 1;

	/* Opens the font the widgets and the number are drawn in. */
	status = kl_text_open(&counter.text, FONT, FALLBACK_FONT);
	if (status != 0) {
		fprintf(stderr, "counter: no font %s (%s)\n", FONT, strerror(status));
		return 1;
	}

	/* Sets what every widget draws with: the frame's canvas (set by counter_resize), the font, no glass. */
	counter.style.text = &counter.text;
	counter.style.glass = 0;

	/* Makes the first frame. */
	status = counter_resize(&counter);
	if (status != 0)
		return 1;

	/* Runs until the window is closed: waits, gives each input to the widgets, draws when due. */
	while (!counter.closed) {
		/* Waits for the desktop: not at all while a frame is due, a frame's time while something moves. */
		timeout = -1;
		if (counter.moving)
			timeout = 16;
		if (counter.dirty || counter.resized)
			timeout = 0;
		status = kl_app_dispatch(counter.app, timeout);
		if (status != 0)
			break;

		/* Takes every event queued, in order. */
		for (;;) {
			taken = kl_app_take(counter.app, &event);
			if (!taken)
				break;

			/* The appearance changed: the theme's colours are new. */
			if (event.kind == KL_APP_THEME)
				counter.dirty = 1;

			/* An input of the window. */
			if (event.kind == KL_APP_WINDOW && event.window == counter.window)
				counter_input(&counter, &event.input);
		}

		/* A new size: a new frame of that size. */
		if (counter.resized) {
			counter.resized = 0;
			status = counter_resize(&counter);
			if (status != 0)
				break;
		}

		/* Something changed or moves: draws and shows a frame. */
		if (counter.dirty || counter.moving) {
			status = counter_frame(&counter);
			if (status != 0)
				break;
		}
	}

	/* Frees the widgets' record, the frame and the font, and closes the window and the connection. */
	kl_ui_destroy(counter.ui);
	kl_canvas_release(&counter.canvas);
	free(counter.pixels);
	kl_text_close(&counter.text);
	kl_app_close(counter.app);

	/* Succeeded: the window was closed. */
	return 0;
}

/*
 * Makes the frame at the window's size; nonzero when it cannot.
 */
static int
counter_resize(
	struct counter *counter)
{
	uint32_t *pixels;
	int status;

	/* Sets the presenter to the window's size, which it reports. */
	status = kl_window_present_resize(counter->window, &counter->width, &counter->height);
	if (status != 0)
		return -1;

	/* Allocates the frame. */
	pixels = malloc((size_t)counter->width * counter->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* Puts the canvas on the new pixels in place of the old ones; the widgets draw on it. */
	if (counter->pixels != NULL)
		kl_canvas_release(&counter->canvas);
	free(counter->pixels);
	counter->pixels = pixels;
	status = kl_canvas_init(&counter->canvas, counter->pixels, counter->width, (int)counter->width, (int)counter->height);
	if (status != 0)
		return -1;
	counter->style.canvas = &counter->canvas;

	/* Succeeded: the new frame is drawn next. */
	counter->dirty = 1;
	return 0;
}

/*
 * Draws one frame: the ground, the number and the three buttons.  Each
 * button's call draws it, records where it is for the input to come, and
 * reports a click the input made on it since the last frame.
 */
static void
counter_draw(
	struct counter *counter,
	uint64_t now_us)
{
	const struct kl_theme *theme;
	struct kl_rect whole;
	struct kl_rect less;
	struct kl_rect reset;
	struct kl_rect more;
	unsigned reset_flags;
	char number[32];
	int baseline;
	int clicked;

	/* Starts the frame: the records of the last one are what the input was matched against. */
	theme = kl_theme_default();
	counter->style.theme = theme;
	kl_ui_begin(counter->ui, now_us);

	/* Paints the ground. */
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)counter->width;
	whole.height = (int)counter->height;
	kl_canvas_gradient(&counter->canvas, &whole, theme->ground_top, theme->ground_bottom);

	/* Draws the number as it is now. */
	snprintf(number, sizeof(number), "%d", counter->count);
	baseline = kl_text_center(theme->text_title, MARGIN, 64);
	(void)kl_text_draw(&counter->text, &counter->canvas, MARGIN, baseline, number, strlen(number), theme->text_title, 1, theme->text);

	/* Lays out the three buttons in a row under it. */
	less.x = MARGIN;
	less.y = MARGIN + 80;
	less.width = BUTTON_WIDTH;
	less.height = theme->control_height;
	reset = less;
	reset.x = less.x + BUTTON_WIDTH + 12;
	more = reset;
	more.x = reset.x + BUTTON_WIDTH + 12;

	/* Less: one less. */
	clicked = kl_button(counter->ui, &counter->style, ID_LESS, &less, "Less", 0U);
	if (clicked) {
		counter->count--;
		counter->dirty = 1;
	}

	/* Reset: back to 0; disabled while the number is 0 already. */
	reset_flags = 0U;
	if (counter->count == 0)
		reset_flags = KL_BUTTON_DISABLED;
	clicked = kl_button(counter->ui, &counter->style, ID_RESET, &reset, "Reset", reset_flags);
	if (clicked) {
		counter->count = 0;
		counter->dirty = 1;
	}

	/* More: one more, the window's main action. */
	clicked = kl_button(counter->ui, &counter->style, ID_MORE, &more, "More", KL_BUTTON_PRIMARY);
	if (clicked) {
		counter->count++;
		counter->dirty = 1;
	}

	/* Ends the frame: nonzero while it needs more frames by itself (a finger down, keys waiting, a scroll gliding). */
	counter->moving = kl_ui_end(counter->ui, now_us);

	/* Lets the input method and the clipboard follow the focused widget (none takes text here). */
	kl_ui_window_text(counter->ui, counter->window);
}

/*
 * Draws and shows a frame, then takes the input no widget took; nonzero
 * when the frame cannot be shown.
 */
static int
counter_frame(
	struct counter *counter)
{
	int status;

	/* Draws the frame; a click it reports asks for one more (dirty again). */
	counter->dirty = 0;
	counter_draw(counter, kl_clock_us());

	/* Hands it to the compositor. */
	status = kl_window_present(counter->window, counter->pixels, counter->width);
	if (status == EAGAIN) {
		/* The window's size changed under the frame: a new frame of the new size. */
		counter->resized = 1;
	} else if (status != 0) {
		/* Refuses to go on when the frame could not be shown. */
		return -1;
	}

	/* Takes the keys and the presses that met no widget. */
	counter_unclaimed(counter);

	/* Succeeded: the frame is shown. */
	return 0;
}

/*
 * Gives one input of the window to the widgets, or acts on it.
 */
static void
counter_input(
	struct counter *counter,
	const struct kl_window_event *input)
{
	int taken;

	/* The window's own inputs: a new size, the close button. */
	if (input->kind == KL_WINDOW_RESIZE) {
		counter->resized = 1;
		return;
	}

	/* The close button ends the program. */
	if (input->kind == KL_WINDOW_CLOSE) {
		counter->closed = 1;
		return;
	}

	/* The pointer, the keys and the fingers: the widgets' input, resolved against the frame last drawn. */
	taken = kl_ui_window_input(counter->ui, input);
	if (taken)
		counter->dirty = 1;
}

/*
 * Acts on the input no widget took: Up and Down change the number.
 */
static void
counter_unclaimed(
	struct counter *counter)
{
	struct kl_event event;
	int taken;

	/* Takes each input the frame left for the program. */
	for (;;) {
		taken = kl_ui_take(counter->ui, &event);
		if (!taken)
			break;

		/* Only keys are used here; presses on the ground are left alone. */
		if (event.kind != KL_EVENT_KEY)
			continue;

		/* Up is More, Down is Less. */
		if (event.code == KL_KEY_UP) {
			counter->count++;
			counter->dirty = 1;
		} else if (event.code == KL_KEY_DOWN) {
			counter->count--;
			counter->dirty = 1;
		}
	}
}
