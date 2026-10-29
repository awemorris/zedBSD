/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A greeter: a text field for a name, a list of greetings, a switch, a
 * slider, colour swatches the program draws itself, and a button.  Greet
 * (or Enter in the field, or a double click in the list) writes the
 * greeting at the bottom of the window; Escape clears it.
 */

#include <keiland/keiland.h>
#include <ctype.h>
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

/* The widgets' ids; the swatches and the list's rows share one id each and differ by their index. */
#define ID_NAME		1U
#define ID_GREETINGS	2U
#define ID_SHOUT	3U
#define ID_SIZE		4U
#define ID_SWATCH	5U
#define ID_GREET	6U

/* The layout, in pixels. */
#define MARGIN		24
#define COLUMN_WIDTH	240
#define SWATCH_SIZE	32
#define GREETING_MAX	600

/*
 * Everything the program keeps.  The widgets' state is here, in the
 * program's own variables: the field's text (name), the list's selection
 * and scroll (greetings), the switch (shout), the slider (size) and the
 * swatch chosen (swatch).  The widgets read and change them in each frame.
 */
struct greeter {
	struct kl_app *app;
	struct kl_window *window;
	struct kl_ui *ui;
	struct kl_text text;
	struct kl_style style;
	struct kl_canvas canvas;
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	struct kl_field name;
	struct kl_list greetings;
	int shout;
	double size;
	size_t swatch;
	char greeting[GREETING_MAX];
	int dirty;
	int moving;
	int resized;
	int closed;
};

static int greeter_resize(struct greeter *greeter);
static void greeter_draw(struct greeter *greeter, uint64_t now_us);
static void greeter_controls(struct greeter *greeter, const struct kl_theme *theme);
static void greeter_list(struct greeter *greeter, const struct kl_theme *theme);
static void greeter_swatches(struct greeter *greeter, const struct kl_theme *theme, int y);
static void greeter_greet(struct greeter *greeter);
static int greeter_frame(struct greeter *greeter);
static void greeter_input(struct greeter *greeter, const struct kl_window_event *input);
static void greeter_unclaimed(struct greeter *greeter);

/* The greetings the list offers. */
static const char *const greeting_table[] = {
	"Hello", "Good morning", "Good afternoon", "Good evening", "Welcome", "Hi", "Greetings", "Howdy", "Cheers", "Good night"
};

#define GREETING_COUNT	(sizeof(greeting_table) / sizeof(greeting_table[0]))

/* The colours the swatches offer for the greeting. */
static const kl_color swatch_table[] = {
	KL_RGB(0x1c1c1e), KL_RGB(0x0a84ff), KL_RGB(0x30b94d), KL_RGB(0xff375f)
};

#define SWATCH_COUNT	(sizeof(swatch_table) / sizeof(swatch_table[0]))

int
main(void)
{
	static struct greeter greeter;
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	struct kl_app_event event;
	int timeout;
	int status;
	int taken;

	/* Connects to the desktop. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.application = "org.example.Greeter";
	greeter.app = kl_app_open(&app_options);
	if (greeter.app == NULL) {
		fprintf(stderr, "greeter: no desktop (%s)\n", strerror(errno));
		return 1;
	}

	/* Opens the window. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Greeter";
	window_options.width = 560;
	window_options.height = 400;
	window_options.present = KL_PRESENT_VULKAN;
	greeter.window = kl_app_window_create(greeter.app, &window_options);
	if (greeter.window == NULL) {
		fprintf(stderr, "greeter: no window (%s)\n", strerror(errno));
		return 1;
	}

	/* Makes the widgets' record and opens the font. */
	greeter.ui = kl_ui_create();
	if (greeter.ui == NULL)
		return 1;
	status = kl_text_open(&greeter.text, FONT, FALLBACK_FONT);
	if (status != 0) {
		fprintf(stderr, "greeter: no font %s (%s)\n", FONT, strerror(status));
		return 1;
	}

	/* Gives the widgets the font. */
	greeter.style.text = &greeter.text;
	greeter.style.glass = 0;

	/* Gives the widgets' state its first values: a name, the first greeting, a size. */
	kl_field_set(&greeter.name, "World");
	status = kl_list_init(&greeter.greetings);
	if (status != 0)
		return 1;
	greeter.greetings.selected = 0;
	greeter.size = 24.0;

	/* Makes the first frame. */
	status = greeter_resize(&greeter);
	if (status != 0)
		return 1;

	/* Runs until the window is closed. */
	while (!greeter.closed) {
		/* Waits for the desktop: not at all while a frame is due, a frame's time while something moves. */
		timeout = -1;
		if (greeter.moving)
			timeout = 16;
		if (greeter.dirty || greeter.resized)
			timeout = 0;
		status = kl_app_dispatch(greeter.app, timeout);
		if (status != 0)
			break;

		/* Takes every event queued, in order. */
		for (;;) {
			taken = kl_app_take(greeter.app, &event);
			if (!taken)
				break;

			/* The appearance changed: the theme's colours are new. */
			if (event.kind == KL_APP_THEME)
				greeter.dirty = 1;

			/* An input of the window. */
			if (event.kind == KL_APP_WINDOW && event.window == greeter.window)
				greeter_input(&greeter, &event.input);
		}

		/* A new size: a new frame of that size. */
		if (greeter.resized) {
			greeter.resized = 0;
			status = greeter_resize(&greeter);
			if (status != 0)
				break;
		}

		/* Something changed or moves: draws and shows a frame. */
		if (greeter.dirty || greeter.moving) {
			status = greeter_frame(&greeter);
			if (status != 0)
				break;
		}
	}

	/* Frees the list's scroll, the widgets' record, the frame and the font, and closes the rest. */
	kl_list_release(&greeter.greetings);
	kl_ui_destroy(greeter.ui);
	kl_canvas_release(&greeter.canvas);
	free(greeter.pixels);
	kl_text_close(&greeter.text);
	kl_app_close(greeter.app);

	/* Succeeded: the window was closed. */
	return 0;
}

/*
 * Makes the frame at the window's size; nonzero when it cannot.
 */
static int
greeter_resize(
	struct greeter *greeter)
{
	uint32_t *pixels;
	int status;

	/* Sets the presenter to the window's size, which it reports. */
	status = kl_window_present_resize(greeter->window, &greeter->width, &greeter->height);
	if (status != 0)
		return -1;

	/* Allocates the frame. */
	pixels = malloc((size_t)greeter->width * greeter->height * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* Puts the canvas on the new pixels in place of the old ones; the widgets draw on it. */
	if (greeter->pixels != NULL)
		kl_canvas_release(&greeter->canvas);
	free(greeter->pixels);
	greeter->pixels = pixels;
	status = kl_canvas_init(&greeter->canvas, greeter->pixels, greeter->width, (int)greeter->width, (int)greeter->height);
	if (status != 0)
		return -1;
	greeter->style.canvas = &greeter->canvas;

	/* Succeeded: the new frame is drawn next. */
	greeter->dirty = 1;
	return 0;
}

/*
 * Draws one frame: the ground, the controls on the left, the list on the
 * right, and the greeting at the bottom.
 */
static void
greeter_draw(
	struct greeter *greeter,
	uint64_t now_us)
{
	const struct kl_theme *theme;
	struct kl_rect whole;
	int baseline;

	/* Starts the frame. */
	theme = kl_theme_default();
	greeter->style.theme = theme;
	kl_ui_begin(greeter->ui, now_us);

	/* Paints the ground. */
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)greeter->width;
	whole.height = (int)greeter->height;
	kl_canvas_gradient(&greeter->canvas, &whole, theme->ground_top, theme->ground_bottom);

	/* Draws the widgets, in the order Tab moves the keyboard's focus through them. */
	greeter_controls(greeter, theme);
	greeter_list(greeter, theme);

	/* Writes the greeting in the colour and the size chosen. */
	baseline = (int)greeter->height - MARGIN;
	(void)kl_text_draw(&greeter->text, &greeter->canvas, MARGIN, baseline, greeter->greeting, strlen(greeter->greeting), (unsigned)greeter->size, 1, swatch_table[greeter->swatch]);

	/* Ends the frame: nonzero while the list's scroll glides or a finger is down. */
	greeter->moving = kl_ui_end(greeter->ui, now_us);

	/* Asks for the input method while the name field has the keyboard, and ties the field to the clipboard. */
	kl_ui_window_text(greeter->ui, greeter->window);
}

/*
 * Draws the left column: the name field, the switch, the slider, the
 * swatches and the Greet button, and acts on what each reports.
 */
static void
greeter_controls(
	struct greeter *greeter,
	const struct kl_theme *theme)
{
	struct kl_rect rect;
	unsigned field;
	int changed;
	int clicked;
	int baseline;

	/* The name: Enter in the field greets at once. */
	rect.x = MARGIN;
	rect.y = MARGIN;
	rect.width = COLUMN_WIDTH;
	rect.height = theme->control_height;
	field = kl_field(greeter->ui, &greeter->style, ID_NAME, &rect, &greeter->name, "Your name");
	if ((field & KL_FIELD_SUBMITTED) != 0U)
		greeter_greet(greeter);

	/* Shout: the switch changes the variable it is given. */
	rect.y += rect.height + 16;
	changed = kl_switch(greeter->ui, &greeter->style, ID_SHOUT, MARGIN, rect.y, &greeter->shout, 0U);
	if (changed)
		greeter_greet(greeter);
	baseline = kl_text_center(theme->text_body, rect.y, theme->switch_height);
	(void)kl_text_draw(&greeter->text, &greeter->canvas, MARGIN + theme->switch_width + 12, baseline, "Shout", 5U, theme->text_body, 0, theme->text);

	/* The greeting's size: the slider changes the variable it is given. */
	rect.y += theme->switch_height + 16;
	(void)kl_slider(greeter->ui, &greeter->style, ID_SIZE, &rect, 16.0, 48.0, 1.0, &greeter->size);

	/* The colour, from the program's own swatches. */
	rect.y += rect.height + 16;
	greeter_swatches(greeter, theme, rect.y);

	/* Greet. */
	rect.y += SWATCH_SIZE + 16;
	rect.width = kl_button_width(&greeter->style, "Greet");
	clicked = kl_button(greeter->ui, &greeter->style, ID_GREET, &rect, "Greet", KL_BUTTON_PRIMARY);
	if (clicked)
		greeter_greet(greeter);
}

/*
 * Draws the list of greetings on the right: the list draws each row's
 * ground and takes its clicks; the program draws the row's text.
 */
static void
greeter_list(
	struct greeter *greeter,
	const struct kl_theme *theme)
{
	struct kl_rect rect;
	struct kl_rect row;
	unsigned changes;
	size_t first;
	size_t last;
	size_t index;
	kl_color ink;
	int baseline;

	/* Starts the list: the keys it took, and which rows show. */
	rect.x = MARGIN + COLUMN_WIDTH + 32;
	rect.y = MARGIN;
	rect.width = COLUMN_WIDTH;
	rect.height = 6 * theme->row_height;
	changes = kl_list_begin(greeter->ui, &greeter->style, ID_GREETINGS, &rect, &greeter->greetings, GREETING_COUNT, &first, &last);

	/* Draws the rows that show: the ground by the list, the text by the program in the ink it gives. */
	for (index = first; index < last; index++) {
		changes |= kl_list_row(greeter->ui, &greeter->style, ID_GREETINGS, &rect, &greeter->greetings, index, &row, &ink);
		baseline = kl_text_center(theme->text_body, row.y, row.height);
		(void)kl_text_draw(&greeter->text, &greeter->canvas, row.x + 12, baseline, greeting_table[index], strlen(greeting_table[index]), theme->text_body, 0, ink);
	}

	/* Ends the list. */
	kl_list_end(greeter->ui, &greeter->style, &rect, &greeter->greetings);

	/* A double click or Enter on a greeting greets with it. */
	if ((changes & KL_LIST_ACTIVATED) != 0U)
		greeter_greet(greeter);
}

/*
 * Draws the colour swatches, which are not a widget of the library: the
 * program draws each and records it with kl_ui_hit (one id, an index per
 * swatch) to learn what the input did to it.
 */
static void
greeter_swatches(
	struct greeter *greeter,
	const struct kl_theme *theme,
	int y)
{
	struct kl_rect rect;
	unsigned hit;
	size_t index;

	/* One swatch after another along the row at y. */
	rect.x = MARGIN;
	rect.y = y;
	rect.width = SWATCH_SIZE;
	rect.height = SWATCH_SIZE;
	for (index = 0; index < SWATCH_COUNT; index++) {
		/* Records it, and learns whether it was clicked since the last frame or is under the pointer. */
		hit = kl_ui_hit(greeter->ui, ID_SWATCH, (uint32_t)index, &rect);
		if ((hit & KL_HIT_CLICKED) != 0U) {
			greeter->swatch = index;
			greeter->dirty = 1;
		}

		/* Draws it, ringed when chosen, lit under the pointer. */
		kl_canvas_round(&greeter->canvas, (float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, 8.0f, swatch_table[index]);
		if (index == greeter->swatch) {
			kl_canvas_round_border(&greeter->canvas, (float)rect.x - 3.0f, (float)rect.y - 3.0f, (float)rect.width + 6.0f, (float)rect.height + 6.0f, 10.0f, 2.0f, theme->accent);
		} else if ((hit & KL_HIT_HOT) != 0U) {
			kl_canvas_round_border(&greeter->canvas, (float)rect.x - 3.0f, (float)rect.y - 3.0f, (float)rect.width + 6.0f, (float)rect.height + 6.0f, 10.0f, 1.0f, theme->separator);
		}

		/* Moves on to the next swatch. */
		rect.x += SWATCH_SIZE + 12;
	}
}

/*
 * Writes the greeting from the widgets' state: the greeting chosen in the
 * list, the name in the field, in capitals when the switch is on.
 */
static void
greeter_greet(
	struct greeter *greeter)
{
	const char *word;
	size_t index;

	/* The greeting chosen, or the first. */
	word = greeting_table[0];
	if (greeter->greetings.selected >= 0)
		word = greeting_table[greeter->greetings.selected];

	/* The greeting and the name. */
	snprintf(greeter->greeting, sizeof(greeter->greeting), "%s, %s!", word, greeter->name.text);

	/* In capitals when shouting. */
	if (greeter->shout) {
		for (index = 0; greeter->greeting[index] != '\0'; index++)
			greeter->greeting[index] = (char)toupper((unsigned char)greeter->greeting[index]);
	}

	/* The frame shows it. */
	greeter->dirty = 1;
}

/*
 * Draws and shows a frame, then takes the input no widget took; nonzero
 * when the frame cannot be shown.
 */
static int
greeter_frame(
	struct greeter *greeter)
{
	int status;

	/* Draws the frame; what the widgets report there may ask for one more. */
	greeter->dirty = 0;
	greeter_draw(greeter, kl_clock_us());

	/* Hands it to the compositor. */
	status = kl_window_present(greeter->window, greeter->pixels, greeter->width);
	if (status == EAGAIN) {
		/* The window's size changed under the frame: a new frame of the new size. */
		greeter->resized = 1;
	} else if (status != 0) {
		/* Refuses to go on when the frame could not be shown. */
		return -1;
	}

	/* Takes the keys and the presses that met no widget. */
	greeter_unclaimed(greeter);

	/* Succeeded: the frame is shown. */
	return 0;
}

/*
 * Gives one input of the window to the widgets, or acts on it.
 */
static void
greeter_input(
	struct greeter *greeter,
	const struct kl_window_event *input)
{
	int taken;

	/* The window's own inputs: a new size, the close button. */
	if (input->kind == KL_WINDOW_RESIZE) {
		greeter->resized = 1;
		return;
	}

	/* The close button ends the program. */
	if (input->kind == KL_WINDOW_CLOSE) {
		greeter->closed = 1;
		return;
	}

	/* The pointer, the keys, the fingers and an input method's text: the widgets'. */
	taken = kl_ui_window_input(greeter->ui, input);
	if (taken)
		greeter->dirty = 1;
}

/*
 * Acts on the input no widget took: Escape clears the greeting.  The
 * keys the focused field or list takes never come here.
 */
static void
greeter_unclaimed(
	struct greeter *greeter)
{
	struct kl_event event;
	int taken;

	/* Takes each input the frame left for the program. */
	for (;;) {
		taken = kl_ui_take(greeter->ui, &event);
		if (!taken)
			break;

		/* Escape clears the greeting. */
		if (event.kind == KL_EVENT_KEY && event.code == KL_KEY_ESC) {
			greeter->greeting[0] = '\0';
			greeter->dirty = 1;
		}
	}
}
