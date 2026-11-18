/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The counter with menus: the window's menu (File, Edit, View) with
 * shortcuts, and a context menu on a right click or a long press.  Copy
 * and Paste carry the number through the clipboard; View chooses the
 * number's size.  The menus, the keys and the buttons share one set of
 * actions.
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

/* The widgets' ids. */
#define ID_LESS		1U
#define ID_RESET	2U
#define ID_MORE		3U

/* The actions: what a menu item, a key or a button asks the program to do. */
#define ACTION_QUIT	1U
#define ACTION_COPY	2U
#define ACTION_PASTE	3U
#define ACTION_LESS	4U
#define ACTION_MORE	5U
#define ACTION_RESET	6U
#define ACTION_SMALL	7U
#define ACTION_LARGE	8U

/* The buttons' width and the room around them, in pixels. */
#define BUTTON_WIDTH	96
#define MARGIN		24

/* The longest number the clipboard gives. */
#define NUMBER_MAX	32U

/*
 * Everything the program keeps: its connection and window, the widgets'
 * record, the font, the frame, the state the window shows (the count and
 * whether the number is large), and what the main loop has to do next.
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
	int large;
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
static void counter_act(struct counter *counter, uint32_t action);
static void counter_states(struct counter *counter);

/*
 * The window's menu.  Each row is an item: its ID, its parent (KL_MENU_ROOT
 * for the menu bar's), its type, label and action, the role that tells the
 * desktop what it means, and its shortcut (modifiers and an XKB keysym).
 * A submenu's items follow it.
 */
static const struct kl_menu_entry menu_table[] = {
	/* id, parent, type, label, action, role, modifiers, keysym */
	{ 1U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "File", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 2U, 1U, KL_MENU_ITEM_NORMAL, "Quit", ACTION_QUIT, KL_MENU_ROLE_QUIT, KL_MENU_CTRL, 'q' },
	{ 3U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Edit", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 4U, 3U, KL_MENU_ITEM_NORMAL, "Copy", ACTION_COPY, KL_MENU_ROLE_COPY, KL_MENU_CTRL, 'c' },
	{ 5U, 3U, KL_MENU_ITEM_NORMAL, "Paste", ACTION_PASTE, KL_MENU_ROLE_PASTE, KL_MENU_CTRL, 'v' },
	{ 6U, 3U, KL_MENU_ITEM_SEPARATOR, "", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 7U, 3U, KL_MENU_ITEM_NORMAL, "Less", ACTION_LESS, KL_MENU_ROLE_NONE, KL_MENU_CTRL, '-' },
	{ 8U, 3U, KL_MENU_ITEM_NORMAL, "More", ACTION_MORE, KL_MENU_ROLE_NONE, KL_MENU_CTRL, '=' },
	{ 9U, 3U, KL_MENU_ITEM_NORMAL, "Reset", ACTION_RESET, KL_MENU_ROLE_NONE, KL_MENU_CTRL, '0' },
	{ 10U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "View", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 11U, 10U, KL_MENU_ITEM_RADIO, "Small Number", ACTION_SMALL, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 12U, 10U, KL_MENU_ITEM_RADIO, "Large Number", ACTION_LARGE, KL_MENU_ROLE_NONE, 0U, 0U }
};

/* The context menu: top-level items only, shown once where the press was. */
static const struct kl_menu_entry context_table[] = {
	{ 1U, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Copy", ACTION_COPY, KL_MENU_ROLE_COPY, 0U, 0U },
	{ 2U, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Paste", ACTION_PASTE, KL_MENU_ROLE_PASTE, 0U, 0U },
	{ 3U, KL_MENU_ROOT, KL_MENU_ITEM_NORMAL, "Reset", ACTION_RESET, KL_MENU_ROLE_NONE, 0U, 0U }
};

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
	app_options.application = "org.example.MenuCounter";
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

	/* Gives the window its menu; without the desktop's menus the keys and buttons still work. */
	status = kl_window_set_menu(counter.window, menu_table, sizeof(menu_table) / sizeof(menu_table[0]));
	if (status != 0)
		fprintf(stderr, "counter: no menu (%s)\n", strerror(status));

	/* Ticks the size shown and disables what cannot be done now. */
	counter_states(&counter);

	/* Makes the widgets' record and opens the font. */
	counter.ui = kl_ui_create();
	if (counter.ui == NULL)
		return 1;
	status = kl_text_open(&counter.text, FONT, FALLBACK_FONT);
	if (status != 0) {
		fprintf(stderr, "counter: no font %s (%s)\n", FONT, strerror(status));
		return 1;
	}

	/* Gives the widgets the font. */
	counter.style.text = &counter.text;
	counter.style.glass = 0;

	/* Makes the first frame. */
	status = counter_resize(&counter);
	if (status != 0)
		return 1;

	/* Runs until the window is closed or Quit is chosen. */
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

		/* Takes every event queued, in order: the menus' choices come among the keys and the clicks. */
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
 * Draws one frame: the ground, the number at the size chosen, and the
 * three buttons, which ask for the same actions as the menu.
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
	unsigned pixels;
	char number[32];
	int baseline;
	int clicked;

	/* Starts the frame. */
	theme = kl_theme_default();
	counter->style.theme = theme;
	kl_ui_begin(counter->ui, now_us);

	/* Paints the ground. */
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)counter->width;
	whole.height = (int)counter->height;
	kl_canvas_gradient(&counter->canvas, &whole, theme->ground_top, theme->ground_bottom);

	/* Draws the number at the size View chose. */
	pixels = theme->text_title;
	if (counter->large)
		pixels = theme->text_title * 2U;
	snprintf(number, sizeof(number), "%d", counter->count);
	baseline = kl_text_center(pixels, MARGIN, 64);
	(void)kl_text_draw(&counter->text, &counter->canvas, MARGIN, baseline, number, strlen(number), pixels, 1, theme->text);

	/* Lays out the three buttons in a row under it. */
	less.x = MARGIN;
	less.y = MARGIN + 80;
	less.width = BUTTON_WIDTH;
	less.height = theme->control_height;
	reset = less;
	reset.x = less.x + BUTTON_WIDTH + 12;
	more = reset;
	more.x = reset.x + BUTTON_WIDTH + 12;

	/* The buttons ask for the menu's actions. */
	clicked = kl_button(counter->ui, &counter->style, ID_LESS, &less, "Less", 0U);
	if (clicked)
		counter_act(counter, ACTION_LESS);
	reset_flags = 0U;
	if (counter->count == 0)
		reset_flags = KL_BUTTON_DISABLED;
	clicked = kl_button(counter->ui, &counter->style, ID_RESET, &reset, "Reset", reset_flags);
	if (clicked)
		counter_act(counter, ACTION_RESET);
	clicked = kl_button(counter->ui, &counter->style, ID_MORE, &more, "More", KL_BUTTON_PRIMARY);
	if (clicked)
		counter_act(counter, ACTION_MORE);

	/* Ends the frame. */
	counter->moving = kl_ui_end(counter->ui, now_us);
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

	/* Draws the frame. */
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

	/* Takes the long presses and the keys that met no widget. */
	counter_unclaimed(counter);

	/* Succeeded: the frame is shown. */
	return 0;
}

/*
 * Acts on one input of the window: a choice of a menu, a right click, or
 * the widgets' input.
 */
static void
counter_input(
	struct counter *counter,
	const struct kl_window_event *input)
{
	int status;
	int taken;

	/* Sorts the input by its kind. */
	switch (input->kind) {
	case KL_WINDOW_RESIZE:
		/* The compositor gave a new size. */
		counter->resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		/* The close button: the program ends. */
		counter->closed = 1;
		break;
	case KL_WINDOW_ACTION:
		/* An item of the window's menu or the context menu, chosen by the pointer or its shortcut. */
		counter_act(counter, input->code);
		break;
	case KL_WINDOW_BUTTON:
		/* A right press opens the context menu there; the widgets take the main button. */
		if (input->code == KL_BUTTON_RIGHT && input->pressed) {
			status = kl_window_popup_menu(counter->window, context_table, sizeof(context_table) / sizeof(context_table[0]), (int)input->x, (int)input->y);
			if (status != 0)
				fprintf(stderr, "counter: no context menu (%s)\n", strerror(status));
			break;
		}

		/* Offers any other input to the widgets. */
		taken = kl_ui_window_input(counter->ui, input);
		if (taken)
			counter->dirty = 1;
		break;
	default:
		/* The pointer, the keys and the fingers: the widgets'. */
		taken = kl_ui_window_input(counter->ui, input);
		if (taken)
			counter->dirty = 1;
		break;
	}
}

/*
 * Acts on the input no widget took: a finger's long press opens the
 * context menu, Up and Down change the number.
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

		/* A long press is the fingers' right click. */
		if (event.kind == KL_EVENT_LONG_PRESS) {
			(void)kl_window_popup_menu(counter->window, context_table, sizeof(context_table) / sizeof(context_table[0]), (int)event.x, (int)event.y);
			continue;
		}

		/* Up is More, Down is Less. */
		if (event.kind == KL_EVENT_KEY && event.code == KL_KEY_UP)
			counter_act(counter, ACTION_MORE);
		else if (event.kind == KL_EVENT_KEY && event.code == KL_KEY_DOWN)
			counter_act(counter, ACTION_LESS);
	}
}

/*
 * Does what an action asks, whichever menu, key or button asked it.
 */
static void
counter_act(
	struct counter *counter,
	uint32_t action)
{
	char number[NUMBER_MAX];
	size_t length;
	size_t digits;

	/* Sorts the action. */
	switch (action) {
	case ACTION_QUIT:
		/* Ends the program as the close button does. */
		counter->closed = 1;
		break;
	case ACTION_COPY:
		/* Puts the number on the clipboard as text. */
		snprintf(number, sizeof(number), "%d", counter->count);
		kl_window_copy(counter->window, number, strlen(number));
		break;
	case ACTION_PASTE:
		/* Takes a number from the clipboard's text; other text is left alone. */
		length = kl_window_paste(counter->window, number, sizeof(number) - 1U);
		number[length] = '\0';
		digits = strspn(number, "-0123456789");
		if (length > 0U && digits == length)
			counter->count = atoi(number);
		break;
	case ACTION_LESS:
		/* The buttons' and the keys' steps. */
		counter->count--;
		break;
	case ACTION_MORE:
		counter->count++;
		break;
	case ACTION_RESET:
		counter->count = 0;
		break;
	case ACTION_SMALL:
		/* The number's size, from the View menu. */
		counter->large = 0;
		break;
	case ACTION_LARGE:
		counter->large = 1;
		break;
	default:
		/* An action this program does not have. */
		return;
	}

	/* The menus follow the new state, and the frame shows it. */
	counter_states(counter);
	counter->dirty = 1;
}

/*
 * Gives the menus the state of the actions: the size shown ticked, Reset
 * disabled at 0.  The compositor never ticks an item by itself; the state
 * applies to the window's menu and the context menu alike.
 */
static void
counter_states(
	struct counter *counter)
{
	unsigned small;
	unsigned large;
	unsigned reset;

	/* The radio items: the size shown is the one ticked. */
	small = KL_ACTION_CHECKED;
	large = 0U;
	if (counter->large) {
		small = 0U;
		large = KL_ACTION_CHECKED;
	}

	/* Tells the desktop which size is checked. */
	(void)kl_window_set_action_state(counter->window, ACTION_SMALL, small);
	(void)kl_window_set_action_state(counter->window, ACTION_LARGE, large);

	/* Reset is disabled while there is nothing to reset. */
	reset = 0U;
	if (counter->count == 0)
		reset = KL_ACTION_DISABLED;
	(void)kl_window_set_action_state(counter->window, ACTION_RESET, reset);
}
