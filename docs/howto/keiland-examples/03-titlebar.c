/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A window with titlebar controls: it shows one colour at a time; Back and
 * Forward in the titlebar step through the colours, and the search field
 * finds one by its name.  The window's title is the colour's name.
 */

#include <keiland/keiland.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The titlebar's controls, by ID. */
#define ID_BACK		1U
#define ID_FORWARD	2U
#define ID_SEARCH	3U

/* The actions the controls queue when chosen. */
#define ACTION_BACK	1U
#define ACTION_FORWARD	2U
#define ACTION_SEARCH	3U

/* The radius of the dots that tell which colour is shown, and the room between them. */
#define DOT_RADIUS	6.0f
#define DOT_SPACING	24

/*
 * One colour the program shows: its name and its value.
 */
struct colour {
	const char *name;
	kl_color value;
};

/*
 * Everything the program keeps: its connection and window, the frame,
 * which colour is shown, and what the main loop has to do next.
 */
struct colours {
	struct kl_app *app;
	struct kl_window *window;
	struct kl_canvas canvas;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	size_t current;
	int dirty;
	int resized;
	int closed;
};

static int colours_resize(struct colours *colours);
static void colours_draw(struct colours *colours);
static int colours_frame(struct colours *colours);
static void colours_show(struct colours *colours);
static void colours_input(struct colours *colours, const struct kl_window_event *input);
static long colours_find(const char *text);

/*
 * The colours, in the order Back and Forward step through them.
 */
static const struct colour colour_table[] = {
	{ "Sky", KL_RGB(0x5ac8fa) },
	{ "Leaf", KL_RGB(0x34c759) },
	{ "Sun", KL_RGB(0xffcc00) },
	{ "Rose", KL_RGB(0xff2d55) },
	{ "Slate", KL_RGB(0x8e8e93) }
};

#define COLOUR_COUNT	(sizeof(colour_table) / sizeof(colour_table[0]))

/*
 * The titlebar's controls: what each is (its role), which give way first
 * when the titlebar is short of room (its priority), and the action it
 * queues.  The compositor decides how they look.
 */
static const struct kl_control_entry control_table[] = {
	/* id, role, priority, group, label, action */
	{ ID_BACK, KL_CONTROL_BACK, KL_PRIORITY_PRIMARY, 0U, "Back", ACTION_BACK },
	{ ID_FORWARD, KL_CONTROL_FORWARD, KL_PRIORITY_PRIMARY, 0U, "Forward", ACTION_FORWARD },
	{ ID_SEARCH, KL_CONTROL_SEARCH, KL_PRIORITY_NORMAL, 0U, "Find a colour", ACTION_SEARCH }
};

int
main(void)
{
	static struct colours colours;
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	struct kl_app_event event;
	int timeout;
	int status;
	int taken;

	/* Connects to the desktop. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "org.example.Colours";
	colours.app = kl_app_open(&app_options);
	if (colours.app == NULL) {
		fprintf(stderr, "colours: no desktop (%s)\n", strerror(errno));
		return 1;
	}

	/* Opens the window. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = colour_table[0].name;
	window_options.width = 480;
	window_options.height = 320;
	window_options.present = KL_PRESENT_VULKAN;
	colours.window = kl_app_window_create(colours.app, &window_options);
	if (colours.window == NULL) {
		fprintf(stderr, "colours: no window (%s)\n", strerror(errno));
		return 1;
	}

	/* Gives the titlebar its controls; a compositor without them leaves the title alone. */
	status = kl_window_set_controls(colours.window, control_table, sizeof(control_table) / sizeof(control_table[0]));
	if (status != 0)
		fprintf(stderr, "colours: no titlebar controls (%s)\n", strerror(status));

	/* Shows the search field's hint while it is empty. */
	if (status == 0)
		(void)kl_window_set_control_text(colours.window, ID_SEARCH, NULL, "Colour name");

	/* Sets the title and the state of Back and Forward for the first colour. */
	colours_show(&colours);

	/* Makes the first frame. */
	status = colours_resize(&colours);
	if (status != 0)
		return 1;

	/* Runs until the window is closed. */
	while (!colours.closed) {
		/* Waits for the desktop; not at all while a frame is due. */
		timeout = -1;
		if (colours.dirty || colours.resized)
			timeout = 0;
		status = kl_app_dispatch(colours.app, timeout);
		if (status != 0)
			break;

		/* Takes every event queued, in order. */
		for (;;) {
			taken = kl_app_take(colours.app, &event);
			if (!taken)
				break;

			/* The appearance changed: the dots' colours are the theme's. */
			if (event.kind == KL_APP_THEME)
				colours.dirty = 1;

			/* An input of the window, the titlebar's among them. */
			if (event.kind == KL_APP_WINDOW && event.window == colours.window)
				colours_input(&colours, &event.input);
		}

		/* A new size: a new frame of that size. */
		if (colours.resized) {
			colours.resized = 0;
			status = colours_resize(&colours);
			if (status != 0)
				break;
		}

		/* Something changed: draws and shows a frame. */
		if (colours.dirty) {
			status = colours_frame(&colours);
			if (status != 0)
				break;
		}
	}

	/* Closes the window and the connection, and frees the frame. */
	kl_canvas_release(&colours.canvas);
	free(colours.pixels);
	kl_app_close(colours.app);

	/* Succeeded: the window was closed. */
	return 0;
}

/*
 * Makes the frame at the window's size; nonzero when it cannot.
 */
static int
colours_resize(
	struct colours *colours)
{
	uint32_t *pixels;
	int status;

	/* Sets the presenter to the window's size, which it reports. */
	status = kl_window_present_resize(colours->window, &colours->width, &colours->height);
	if (status != 0)
		return -1;

	/* Allocates the frame. */
	pixels = malloc((size_t)colours->width * colours->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* Puts the canvas on the new pixels in place of the old ones. */
	if (colours->pixels != NULL)
		kl_canvas_release(&colours->canvas);
	free(colours->pixels);
	colours->pixels = pixels;
	status = kl_canvas_init(&colours->canvas, colours->pixels, colours->width, (int)colours->width, (int)colours->height);
	if (status != 0)
		return -1;

	/* Succeeded: the new frame is drawn next. */
	colours->dirty = 1;
	return 0;
}

/*
 * Draws one frame: the colour shown, and a row of dots that tells which
 * of the colours it is.
 */
static void
colours_draw(
	struct colours *colours)
{
	const struct kl_theme *theme;
	struct kl_rect whole;
	size_t index;
	float x;
	float y;

	/* Paints the colour shown over the whole frame. */
	theme = kl_theme_default();
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)colours->width;
	whole.height = (int)colours->height;
	kl_canvas_fill(&colours->canvas, &whole, colour_table[colours->current].value);

	/* Draws one dot per colour along the bottom, the one shown filled. */
	x = (float)colours->width / 2.0f - (float)(DOT_SPACING * (int)(COLOUR_COUNT - 1U)) / 2.0f;
	y = (float)colours->height - 32.0f;
	for (index = 0; index < COLOUR_COUNT; index++) {
		if (index == colours->current) {
			kl_canvas_circle(&colours->canvas, x, y, DOT_RADIUS, theme->panel);
		} else {
			kl_canvas_ring(&colours->canvas, x, y, DOT_RADIUS, 2.0f, 1.0f, theme->panel);
		}

		/* Moves on to the next dot. */
		x += (float)DOT_SPACING;
	}
}

/*
 * Draws and shows a frame; nonzero when it cannot be shown.
 */
static int
colours_frame(
	struct colours *colours)
{
	int status;

	/* Draws the frame and hands it to the compositor. */
	colours->dirty = 0;
	colours_draw(colours);
	status = kl_window_present(colours->window, colours->pixels, colours->width);

	/* The window's size changed under the frame: a new frame of the new size. */
	if (status == EAGAIN) {
		colours->resized = 1;
		return 0;
	}

	/* Refuses to go on when the frame could not be shown. */
	if (status != 0)
		return -1;

	/* Succeeded: the frame is shown. */
	return 0;
}

/*
 * Tells the titlebar about the colour now shown: its name as the title,
 * Back disabled at the first colour, Forward disabled at the last.
 */
static void
colours_show(
	struct colours *colours)
{
	unsigned back;
	unsigned forward;

	/* Names the window after the colour. */
	kl_window_set_title(colours->window, colour_table[colours->current].name);

	/* Disables the steps that lead nowhere. */
	back = 0U;
	forward = 0U;
	if (colours->current == 0U)
		back = KL_ACTION_DISABLED;
	if (colours->current + 1U == COLOUR_COUNT)
		forward = KL_ACTION_DISABLED;
	(void)kl_window_set_action_state(colours->window, ACTION_BACK, back);
	(void)kl_window_set_action_state(colours->window, ACTION_FORWARD, forward);

	/* The frame shows the new colour. */
	colours->dirty = 1;
}

/*
 * Acts on one input of the window.
 */
static void
colours_input(
	struct colours *colours,
	const struct kl_window_event *input)
{
	long found;

	/* Sorts the input by its kind. */
	switch (input->kind) {
	case KL_WINDOW_ACTION:
		/* A control was chosen: its action is in code. */
		if (input->code == ACTION_BACK && colours->current > 0U) {
			colours->current--;
			colours_show(colours);
		} else if (input->code == ACTION_FORWARD && colours->current + 1U < COLOUR_COUNT) {
			colours->current++;
			colours_show(colours);
		}

		break;
	case KL_WINDOW_CONTROL_TEXT:
		/* The search field's text as it is typed: the first colour it names. */
		found = colours_find(input->text);
		if (found >= 0 && (size_t)found != colours->current) {
			colours->current = (size_t)found;
			colours_show(colours);
		}

		break;
	case KL_WINDOW_CONTROL_DONE:
		/* The search ended (Enter, Escape or elsewhere): the field is emptied for the next one. */
		(void)kl_window_set_control_text(colours->window, ID_SEARCH, "", NULL);
		break;
	case KL_WINDOW_RESIZE:
		/* The compositor gave a new size. */
		colours->resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		/* The close button: the program ends. */
		colours->closed = 1;
		break;
	default:
		/* The pointer, the keys and the rest: not used here. */
		break;
	}
}

/*
 * Finds the first colour whose name starts with a text, ignoring case;
 * -1 for none (and for an empty text).
 */
static long
colours_find(
	const char *text)
{
	size_t length;
	size_t index;
	size_t at;
	int name_letter;
	int text_letter;
	int same;

	/* An empty search names no colour. */
	length = strlen(text);
	if (length == 0U)
		return -1;

	/* Compares the text with the start of each name. */
	for (index = 0; index < COLOUR_COUNT; index++) {
		same = 1;
		for (at = 0; at < length; at++) {
			name_letter = tolower((unsigned char)colour_table[index].name[at]);
			text_letter = tolower((unsigned char)text[at]);
			if (colour_table[index].name[at] == '\0' || name_letter != text_letter) {
				same = 0;
				break;
			}
		}

		/* Answers with the first colour whose name the text starts. */
		if (same)
			return (long)index;
	}

	/* No colour has such a name. */
	return -1;
}
