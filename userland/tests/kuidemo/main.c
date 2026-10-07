/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The widgets' sampler (ws090-p005): a window of libkeiland's widgets on
 * three pages -- buttons, switches, a slider, fields and progress; a list
 * of a hundred rows; a dialog and a chip -- for a person (or a test) to
 * try with the pointer, the keyboard and the fingers.  It is in the test
 * image only.
 *
 * What the widgets report is logged on standard error as "KUIDEMO" lines,
 * which the QEMU tests read through the guest's log.
 *
 * It is also the sample of the application API (WS131 p015, KL_VERSION
 * 26): one kl_app, its window, one event queue, the window's menu (File >
 * Quit, Page > the three pages), its titlebar's controls (the page before
 * and after, disabled at the ends), a context menu of the pages on a right
 * press, and its glass panels, all given as tables.  An action chosen is
 * logged as "KUIDEMO ACTION action= id=".
 *
 *   kuidemo [--display=NAME] [--font=PATH] [--fallback-font=PATH]
 *           [--width=N] [--height=N] [--timeout-s=N] [--shm]
 */

#include <keiland/keiland.h>

#include "userland/desktop/paths.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fonts. */
#define DEMO_FONT		KEILAND_DATADIR "/fonts/keiland.ttf"
#define DEMO_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"

/* The window's size until the compositor gives one. */
#define DEMO_WIDTH		900U
#define DEMO_HEIGHT		640U

/* The wait for the compositor while something moves, and while nothing does, in milliseconds. */
#define DEMO_FRAME_MS		16
#define DEMO_IDLE_MS		1000

/* How many times a stale swapchain is remade for one frame. */
#define DEMO_STALE_LIMIT	8U

/* The panels: the margin around them and the gap between them (on glass, and without), the sidebar's width and the corners. */
#define DEMO_MARGIN		12
#define DEMO_GAP		10
#define DEMO_GLASS_GAP		8
#define DEMO_SIDEBAR		212
#define DEMO_RADIUS		16

/* How many rows the list has, and how long the chip shows, in microseconds. */
#define DEMO_ROWS		100U
#define DEMO_CHIP_US		2000000U

/* The evdev code of Q (Ctrl+Q quits). */
#define DEMO_KEY_Q		16U

/* The actions of the menu, the controls and the context menu. */
#define DEMO_ACTION_QUIT	1U
#define DEMO_ACTION_PAGE	10U	/* + the page */
#define DEMO_ACTION_BEFORE	20U
#define DEMO_ACTION_AFTER	21U

/* The pages. */
#define DEMO_PAGE_CONTROLS	0
#define DEMO_PAGE_LIST		1
#define DEMO_PAGE_DIALOGS	2
#define DEMO_PAGES		3

/* The widgets' ids. */
#define DEMO_ID_PAGES		1U
#define DEMO_ID_SAVE		2U
#define DEMO_ID_CANCEL		3U
#define DEMO_ID_DELETE		4U
#define DEMO_ID_DISABLED	5U
#define DEMO_ID_WIFI		6U
#define DEMO_ID_BLUETOOTH	7U
#define DEMO_ID_VOLUME		8U
#define DEMO_ID_NAME		9U
#define DEMO_ID_PASSWORD	10U
#define DEMO_ID_LIST		11U
#define DEMO_ID_ASK		12U
#define DEMO_ID_CHIP		13U
#define DEMO_ID_WORKING		14U
#define DEMO_ID_DIALOG		15U

/* The command line's options. */
struct demo_options {
	const char *display;
	const char *font;
	const char *fallback;
	unsigned width;
	unsigned height;
	unsigned timeout;
	unsigned present;
};

/* The sampler's window, its frame, and what its widgets remember. */
struct demo {
	/* The application, its window and its input. */
	struct kl_app *app;
	struct kl_window *window;
	struct kl_ui *ui;

	/* The frame: its pixels, its size, the canvas over them, the text and the style. */
	uint32_t *pixels;
	uint32_t width;
	uint32_t height;
	struct kl_canvas canvas;
	int canvas_made;
	struct kl_text text;
	struct kl_style style;

	/* The panels of the last frame. */
	struct kl_rect sidebar;
	struct kl_rect content;

	/* The page shown and the controls' values. */
	int page;
	int wifi;
	int bluetooth;
	int working;
	double volume;
	struct kl_field name;
	struct kl_field password;

	/* The list, and the row activated last (-1 for none). */
	struct kl_list list;
	long activated;

	/* The dialog: shown, and the last answer (-1 for none); the chip until a time. */
	int dialog;
	int answer;
	uint64_t chip_until;

	/* A frame is due, something moves, the window changed size or was closed. */
	int dirty;
	int moving;
	int resized;
	int closed;
	int quit;
};

/* The pages' names and icons in the sidebar. */
static const char *const demo_page_names[DEMO_PAGES] = { "Controls", "List", "Dialogs" };
static const enum kl_icon demo_page_icons[DEMO_PAGES] = { KL_ICON_TILES, KL_ICON_LIST, KL_ICON_BELL };

/* The dialog's buttons: the main one first, Cancel last. */
static const char *const demo_dialog_labels[] = { "Delete", "Cancel" };

/* The window's menu: File > Quit, and Page > each page. */
static const struct kl_menu_entry demo_menu[] = {
	{ 1U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "File", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 2U, 1U, KL_MENU_ITEM_NORMAL, "Quit Widgets", DEMO_ACTION_QUIT, KL_MENU_ROLE_QUIT, KL_MENU_CTRL, 'q' },
	{ 3U, KL_MENU_ROOT, KL_MENU_ITEM_SUBMENU, "Page", 0U, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 4U, 3U, KL_MENU_ITEM_RADIO, "Controls", DEMO_ACTION_PAGE + DEMO_PAGE_CONTROLS, KL_MENU_ROLE_NONE, KL_MENU_CTRL, '1' },
	{ 5U, 3U, KL_MENU_ITEM_RADIO, "List", DEMO_ACTION_PAGE + DEMO_PAGE_LIST, KL_MENU_ROLE_NONE, KL_MENU_CTRL, '2' },
	{ 6U, 3U, KL_MENU_ITEM_RADIO, "Dialogs", DEMO_ACTION_PAGE + DEMO_PAGE_DIALOGS, KL_MENU_ROLE_NONE, KL_MENU_CTRL, '3' }
};

/* The titlebar's controls: the page before and the page after. */
static const struct kl_control_entry demo_controls[] = {
	{ 1U, KL_CONTROL_BACK, KL_PRIORITY_PRIMARY, 0U, "Page before", DEMO_ACTION_BEFORE },
	{ 2U, KL_CONTROL_FORWARD, KL_PRIORITY_PRIMARY, 0U, "Page after", DEMO_ACTION_AFTER }
};

/* The context menu of a right press: the pages. */
static const struct kl_menu_entry demo_context[] = {
	{ 1U, KL_MENU_ROOT, KL_MENU_ITEM_RADIO, "Controls", DEMO_ACTION_PAGE + DEMO_PAGE_CONTROLS, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 2U, KL_MENU_ROOT, KL_MENU_ITEM_RADIO, "List", DEMO_ACTION_PAGE + DEMO_PAGE_LIST, KL_MENU_ROLE_NONE, 0U, 0U },
	{ 3U, KL_MENU_ROOT, KL_MENU_ITEM_RADIO, "Dialogs", DEMO_ACTION_PAGE + DEMO_PAGE_DIALOGS, KL_MENU_ROLE_NONE, 0U, 0U }
};

static int demo_parse(int argc, char **argv, struct demo_options *options);
static const char *demo_value(const char *argument, const char *name);
static int demo_number(const char *text, unsigned maximum, unsigned *value);
static void demo_log(const char *format, ...);
static int demo_loop(struct demo *demo, const struct demo_options *options);
static void demo_event(struct demo *demo, const struct kl_window_event *event);
static void demo_action(struct demo *demo, uint32_t action, int32_t id);
static void demo_show_page(struct demo *demo, int page);
static void demo_states(struct demo *demo);
static int demo_frame(struct demo *demo);
static int demo_resize(struct demo *demo);
static int demo_canvas_make(struct demo *demo);
static void demo_draw(struct demo *demo, uint64_t now_us);
static void demo_layout(struct demo *demo);
static void demo_draw_sidebar(struct demo *demo);
static void demo_draw_controls(struct demo *demo);
static void demo_draw_list(struct demo *demo);
static void demo_draw_dialogs(struct demo *demo, uint64_t now_us);
static void demo_glass_refresh(struct demo *demo);
static int demo_value_x(const struct kl_rect *card);

/*
 * Runs the sampler.
 */
int
main(
	int argc,
	char **argv)
{
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	struct demo_options options;
	static struct demo demo;
	int status;
	int error;

	/* The command line. */
	status = demo_parse(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: kuidemo [--display=NAME] [--font=PATH] [--fallback-font=PATH] [--width=N] [--height=N] [--timeout-s=N] [--shm]\n");
		return 2;
	}

	/* The font; without it the widgets show no words. */
	error = kl_text_open(&demo.text, options.font, options.fallback);
	if (error != 0)
		demo_log("FONT missing path=%s error=%d", options.font, error);

	/* The application: the connection and its globals. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.display = options.display;
	app_options.application = "kuidemo";
	demo.app = kl_app_open(&app_options);
	if (demo.app == NULL) {
		fprintf(stderr, "KUIDEMO FAILED operation=app error=%d\n", errno);
		kl_text_close(&demo.text);
		return 1;
	}

	/* Its window. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Widgets";
	window_options.width = options.width;
	window_options.height = options.height;
	window_options.present = options.present;
	demo.window = kl_app_window_create(demo.app, &window_options);
	if (demo.window == NULL) {
		fprintf(stderr, "KUIDEMO FAILED operation=window error=%d\n", errno);
		kl_app_close(demo.app);
		kl_text_close(&demo.text);
		return 1;
	}

	/* The input and the list's state. */
	demo.ui = kl_ui_create();
	error = kl_list_init(&demo.list);
	if (demo.ui == NULL || error != 0) {
		fprintf(stderr, "KUIDEMO FAILED operation=memory\n");
		kl_ui_destroy(demo.ui);
		kl_app_close(demo.app);
		kl_text_close(&demo.text);
		return 1;
	}

	/* The menu and the titlebar's controls, as tables (a compositor without them leaves the keys and the sidebar). */
	error = kl_window_set_menu(demo.window, demo_menu, sizeof(demo_menu) / sizeof(demo_menu[0]));
	demo_log("MENU error=%d", error);
	error = kl_window_set_controls(demo.window, demo_controls, sizeof(demo_controls) / sizeof(demo_controls[0]));
	demo_log("CONTROLS error=%d", error);

	/* The widgets' first values: on glass when the frames are see-through (the first panels say whether the compositor has it). */
	demo.style.text = &demo.text;
	demo.style.theme = kl_theme_default();
	demo.style.glass = kl_window_see_through(demo.window);
	demo_states(&demo);
	demo.wifi = 1;
	demo.volume = 40.0;
	demo.activated = -1;
	demo.answer = -1;
	demo.password.secret = 1;
	kl_field_set(&demo.name, "Kei");
	demo_log("GLASS glass=%d", demo.style.glass);

	/* The loop, until the window closes. */
	status = demo_loop(&demo, &options);

	/* Everything goes (the application closes its window). */
	kl_list_release(&demo.list);
	kl_ui_destroy(demo.ui);
	if (demo.canvas_made)
		kl_canvas_release(&demo.canvas);
	free(demo.pixels);
	kl_app_close(demo.app);
	kl_text_close(&demo.text);

	/* Reports how the run ended. */
	if (status != 0)
		return 1;
	return 0;
}

/* Reads the command line into the options; nonzero for a malformed one. */
static int
demo_parse(
	int argc,
	char **argv,
	struct demo_options *options)
{
	const char *value;
	int status;
	int index;

	/* The defaults. */
	memset(options, 0, sizeof(*options));
	options->font = DEMO_FONT;
	options->fallback = DEMO_FALLBACK_FONT;
	options->width = DEMO_WIDTH;
	options->height = DEMO_HEIGHT;
	options->present = KL_PRESENT_VULKAN;

	/* Each argument. */
	for (index = 1; index < argc; index++) {
		/* The compositor's display. */
		value = demo_value(argv[index], "--display=");
		if (value != NULL) {
			options->display = value;
			continue;
		}

		/* The font. */
		value = demo_value(argv[index], "--font=");
		if (value != NULL) {
			options->font = value;
			continue;
		}

		/* The font of the characters it lacks. */
		value = demo_value(argv[index], "--fallback-font=");
		if (value != NULL) {
			options->fallback = value;
			continue;
		}

		/* The window's width. */
		value = demo_value(argv[index], "--width=");
		if (value != NULL) {
			status = demo_number(value, 8192U, &options->width);
			if (status != 0)
				return status;
			continue;
		}

		/* The window's height. */
		value = demo_value(argv[index], "--height=");
		if (value != NULL) {
			status = demo_number(value, 8192U, &options->height);
			if (status != 0)
				return status;
			continue;
		}

		/* How long it runs at most (0 for ever). */
		value = demo_value(argv[index], "--timeout-s=");
		if (value != NULL) {
			status = demo_number(value, 86400U, &options->timeout);
			if (status != 0)
				return status;
			continue;
		}

		/* Frames in shared memory instead of Vulkan. */
		status = strcmp(argv[index], "--shm");
		if (status == 0) {
			options->present = KL_PRESENT_SHM;
			continue;
		}

		/* Anything else refuses the command line. */
		return -1;
	}

	/* A window has some size. */
	if (options->width < 480U || options->height < 360U)
		return -1;

	/* Succeeded: the options are read. */
	return 0;
}

/* Returns what follows an option's name in an argument, or NULL when the argument is another option. */
static const char *
demo_value(
	const char *argument,
	const char *name)
{
	size_t length;
	int match;

	/* The name must start the argument. */
	length = strlen(name);
	match = strncmp(argument, name, length);
	if (match != 0)
		return NULL;

	/* Reports the value after it. */
	return argument + length;
}

/* Reads a decimal number no larger than a maximum; nonzero for a malformed one. */
static int
demo_number(
	const char *text,
	unsigned maximum,
	unsigned *value)
{
	unsigned long number;
	char *end;

	/* The digits, all of them. */
	errno = 0;
	number = strtoul(text, &end, 10);
	if (errno != 0 ||
	    end == text ||
	    *end != '\0' ||
	    number > maximum)
		return -1;

	/* Succeeded: the number. */
	*value = (unsigned)number;
	return 0;
}

/* Writes one line of the log: "KUIDEMO " and the words, on standard error. */
static void
demo_log(
	const char *format,
	...)
{
	va_list arguments;

	/* The line. */
	va_start(arguments, format);
	fputs("KUIDEMO ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

/* Runs the window until it closes, Ctrl+Q or the timeout; nonzero when something failed. */
static int
demo_loop(
	struct demo *demo,
	const struct demo_options *options)
{
	struct kl_app_event event;
	uint64_t started;
	uint64_t now;
	int timeout;
	int taken;
	int status;

	/* The presenter's size, a canvas of it, and the first frame. */
	status = demo_resize(demo);
	if (status != 0)
		return -1;
	status = demo_frame(demo);
	if (status != 0)
		return -1;
	demo_log("READY width=%u height=%u", demo->width, demo->height);

	/* Each round: input, and a frame when something changed or moves. */
	started = kl_clock_us();
	for (;;) {
		/* Waits for the compositor, or a frame's time while something moves (a held key's repeat is the application's). */
		timeout = DEMO_IDLE_MS;
		if (demo->moving)
			timeout = DEMO_FRAME_MS;
		if (demo->dirty)
			timeout = 0;
		status = kl_app_dispatch(demo->app, timeout);
		if (status != 0) {
			demo_log("DONE reason=disconnected");
			return 0;
		}

		/* Every event: the window's inputs go to the widgets, its actions to the pages. */
		now = kl_clock_us();
		for (;;) {
			taken = kl_app_take(demo->app, &event);
			if (!taken)
				break;

			/* The desktop's appearance changed: the theme's colours are new, the window is drawn again (ws089-p017). */
			if (event.kind == KL_APP_THEME) {
				demo->dirty = 1;
				continue;
			}

			/* Another window's event is not this one's. */
			if (event.kind != KL_APP_WINDOW || event.window != demo->window)
				continue;
			if (event.input.kind == KL_WINDOW_ACTION)
				demo_action(demo, event.input.code, event.input.id);
			else
				demo_event(demo, &event.input);
		}

		/* The close button, Ctrl+Q or the timeout ends the run. */
		if (demo->closed || demo->quit) {
			demo_log("DONE reason=close");
			return 0;
		}

		/* The timeout, when one was given. */
		if (options->timeout != 0U && now - started >= (uint64_t)options->timeout * 1000000U) {
			demo_log("DONE reason=timeout");
			return 0;
		}

		/* A new size: a new swapchain and canvas. */
		if (demo->resized) {
			demo->resized = 0;
			status = demo_resize(demo);
			if (status != 0)
				return -1;
		}

		/* A frame when something changed or moves. */
		if (demo->dirty || demo->moving) {
			status = demo_frame(demo);
			if (status != 0)
				return -1;
		}
	}
}

/* Gives one input of the window to the widgets. */
static void
demo_event(
	struct demo *demo,
	const struct kl_window_event *event)
{
	/* Any input may change the page. */
	demo->dirty = 1;

	/* What it is. */
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		(void)kl_ui_pointer_motion(demo->ui, event->x, event->y);
		break;
	case KL_WINDOW_LEAVE:
		(void)kl_ui_pointer_leave(demo->ui);
		break;
	case KL_WINDOW_BUTTON:
		/* The main button goes to the widgets; a right press opens the context menu of the pages. */
		(void)kl_ui_pointer_motion(demo->ui, event->x, event->y);
		if (event->code == KL_BUTTON_LEFT)
			(void)kl_ui_pointer_button(demo->ui, event->pressed, event->arrival_us);
		if (event->code == KL_BUTTON_RIGHT && event->pressed)
			demo_log("CONTEXT error=%d", kl_window_popup_menu(demo->window, demo_context, sizeof(demo_context) / sizeof(demo_context[0]), (int)event->x, (int)event->y));
		break;
	case KL_WINDOW_AXIS:
		(void)kl_ui_wheel(demo->ui, event->dx, event->dy, event->arrival_us);
		break;
	case KL_WINDOW_KEY:
		/* Ctrl+Q quits; the other keys go to the widgets. */
		if (event->pressed && event->code == DEMO_KEY_Q && (event->modifiers & KL_MOD_CTRL) != 0U) {
			demo->quit = 1;
			break;
		}

		/* Any other key goes to the widgets. */
		(void)kl_ui_key(demo->ui, event->code, event->pressed, event->modifiers);
		break;
	case KL_WINDOW_TOUCH_DOWN:
		demo_log("TOUCH down id=%d x=%.0f y=%.0f", (int)event->id, event->x, event->y);
		(void)kl_ui_touch_down(demo->ui, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		(void)kl_ui_touch_motion(demo->ui, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_UP:
		demo_log("TOUCH up id=%d", (int)event->id);
		(void)kl_ui_touch_up(demo->ui, event->id, event->time_us, event->arrival_us);
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		(void)kl_ui_touch_cancel(demo->ui, event->arrival_us);
		break;
	case KL_WINDOW_RESIZE:
		demo->resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		demo->closed = 1;
		break;
	default:
		break;
	}
}

/* Carries out an action chosen in the menu, a control or the context menu. */
static void
demo_action(
	struct demo *demo,
	uint32_t action,
	int32_t id)
{
	/* The log line the tests read. */
	demo_log("ACTION action=%u id=%d", (unsigned)action, (int)id);

	/* Quit, a page, or the page before or after. */
	if (action == DEMO_ACTION_QUIT)
		demo->quit = 1;
	else if (action >= DEMO_ACTION_PAGE && action < DEMO_ACTION_PAGE + DEMO_PAGES)
		demo_show_page(demo, (int)(action - DEMO_ACTION_PAGE));
	else if (action == DEMO_ACTION_BEFORE && demo->page > 0)
		demo_show_page(demo, demo->page - 1);
	else if (action == DEMO_ACTION_AFTER && demo->page < DEMO_PAGES - 1)
		demo_show_page(demo, demo->page + 1);
}

/* Shows a page (the dialog closes), and the menu's and controls' states follow. */
static void
demo_show_page(
	struct demo *demo,
	int page)
{
	/* The same page changes nothing. */
	if (page == demo->page)
		return;

	/* The page, logged. */
	demo->page = page;
	demo->dialog = 0;
	demo->dirty = 1;
	demo_log("PAGE %s", demo_page_names[page]);
	demo_states(demo);
}

/* Tells the window the actions' states: the page shown is checked, and the controls stop at the ends. */
static void
demo_states(
	struct demo *demo)
{
	unsigned state;
	int page;

	/* Each page's item, checked when it is shown. */
	for (page = 0; page < DEMO_PAGES; page++) {
		state = 0;
		if (page == demo->page)
			state = KL_ACTION_CHECKED;
		(void)kl_window_set_action_state(demo->window, DEMO_ACTION_PAGE + (uint32_t)page, state);
	}

	/* The page before and after, while there is one. */
	state = 0;
	if (demo->page == 0)
		state = KL_ACTION_DISABLED;
	(void)kl_window_set_action_state(demo->window, DEMO_ACTION_BEFORE, state);
	state = 0;
	if (demo->page == DEMO_PAGES - 1)
		state = KL_ACTION_DISABLED;
	(void)kl_window_set_action_state(demo->window, DEMO_ACTION_AFTER, state);
}

/* Draws and shows a frame, remaking a stale swapchain; nonzero when it cannot be shown. */
static int
demo_frame(
	struct demo *demo)
{
	unsigned stale;
	int status;

	/* Tries until the frame is shown. */
	for (stale = 0; stale < DEMO_STALE_LIMIT; stale++) {
		/* The frame, its glass panels, and the frame shown. */
		demo_draw(demo, kl_clock_us());
		demo_glass_refresh(demo);
		status = kl_window_present(demo->window, demo->pixels, (size_t)demo->width);
		if (status == 0)
			return 0;

		/* Anything but a stale swapchain is a failure. */
		if (status != EAGAIN) {
			fprintf(stderr, "KUIDEMO FAILED operation=present error=%d\n", status);
			return -1;
		}

		/* A stale swapchain is remade at the window's size. */
		status = demo_resize(demo);
		if (status != 0)
			return -1;
	}

	/* The swapchain stayed out of date. */
	fprintf(stderr, "KUIDEMO FAILED operation=stale-swapchain\n");
	return -1;
}

/* Remakes the presenter at the window's size, with a canvas to match; nonzero when it cannot. */
static int
demo_resize(
	struct demo *demo)
{
	int status;

	/* The presenter at the window's size. */
	status = kl_window_present_resize(demo->window, &demo->width, &demo->height);
	if (status != 0) {
		fprintf(stderr, "KUIDEMO FAILED operation=present error=%d\n", status);
		return -1;
	}

	/* A canvas of its size. */
	status = demo_canvas_make(demo);
	if (status != 0) {
		fprintf(stderr, "KUIDEMO FAILED operation=canvas\n");
		return -1;
	}

	/* Succeeded: frames are drawn at the new size. */
	demo->dirty = 1;
	return 0;
}

/* Makes the frame's memory and canvas at the presenter's size; nonzero when memory runs out. */
static int
demo_canvas_make(
	struct demo *demo)
{
	uint32_t *pixels;
	size_t count;
	int status;

	/* The frame's memory. */
	count = (size_t)demo->width * (size_t)demo->height;
	pixels = malloc(count * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* The canvas over it, in place of the old one. */
	if (demo->canvas_made)
		kl_canvas_release(&demo->canvas);
	demo->canvas_made = 0;
	free(demo->pixels);
	demo->pixels = pixels;
	status = kl_canvas_init(&demo->canvas, demo->pixels, (size_t)demo->width, (int)demo->width, (int)demo->height);
	if (status != 0)
		return -1;
	demo->canvas_made = 1;
	demo->style.canvas = &demo->canvas;

	/* Succeeded. */
	return 0;
}

/* Draws the frame: the ground, the sidebar and the page, and learns whether something still moves. */
static void
demo_draw(
	struct demo *demo,
	uint64_t now_us)
{
	const struct kl_theme *theme;
	struct kl_event event;
	struct kl_rect whole;
	int moving;
	int taken;

	/* The frame of the input (a widget that changes the page asks for another), and the panels' places. */
	theme = demo->style.theme;
	demo->dirty = 0;
	kl_ui_begin(demo->ui, now_us);
	demo_layout(demo);

	/* The ground: clear on glass (the desktop shows between the panels), else a quiet gradient. */
	whole.x = 0;
	whole.y = 0;
	whole.width = (int)demo->width;
	whole.height = (int)demo->height;
	if (demo->style.glass)
		kl_canvas_clear(&demo->canvas);
	else
		kl_canvas_gradient(&demo->canvas, &whole, theme->ground_top, theme->ground_bottom);

	/* The sidebar and the page shown. */
	demo_draw_sidebar(demo);
	kl_panel(&demo->style, &demo->content, 0);
	switch (demo->page) {
	case DEMO_PAGE_LIST:
		demo_draw_list(demo);
		break;
	case DEMO_PAGE_DIALOGS:
		demo_draw_dialogs(demo, now_us);
		break;
	default:
		demo_draw_controls(demo);
		break;
	}

	/* The frame is drawn; frames go on while the input or a widget moves. */
	moving = kl_ui_end(demo->ui, now_us);
	demo->moving = moving;
	if (demo->chip_until > now_us || (demo->page == DEMO_PAGE_DIALOGS && demo->working))
		demo->moving = 1;

	/* The keys no widget took are the application's. */
	for (;;) {
		taken = kl_ui_take(demo->ui, &event);
		if (!taken)
			break;
		if (event.kind == KL_EVENT_KEY)
			demo_log("KEY code=%u modifiers=%u", (unsigned)event.code, event.modifiers);
	}
}

/* Places the sidebar and the content's panel for the window's size. */
static void
demo_layout(
	struct demo *demo)
{
	int margin;
	int gap;

	/* On glass the panels reach the window's edges, a small gap between them. */
	margin = DEMO_MARGIN;
	gap = DEMO_GAP;
	if (demo->style.glass) {
		margin = 0;
		gap = DEMO_GLASS_GAP;
	}

	/* The sidebar on the left, the content beside it. */
	demo->sidebar.x = margin;
	demo->sidebar.y = margin;
	demo->sidebar.width = DEMO_SIDEBAR;
	demo->sidebar.height = (int)demo->height - 2 * margin;
	demo->content.x = margin + DEMO_SIDEBAR + gap;
	demo->content.y = margin;
	demo->content.width = (int)demo->width - demo->content.x - margin;
	demo->content.height = (int)demo->height - 2 * margin;
}

/* Draws the sidebar: the pages, the one shown lit. */
static void
demo_draw_sidebar(
	struct demo *demo)
{
	struct kl_rect item;
	int pressed;
	int index;
	int top;

	/* The panel and its section. */
	kl_panel(&demo->style, &demo->sidebar, 1);
	top = kl_sidebar_section(&demo->style, demo->sidebar.x + 8, demo->sidebar.y + 8, demo->sidebar.width - 16, "Widgets");

	/* Each page; a click shows it (the dialog closes). */
	for (index = 0; index < DEMO_PAGES; index++) {
		item.x = demo->sidebar.x + 8;
		item.y = top + index * 30;
		item.width = demo->sidebar.width - 16;
		item.height = 30;
		pressed = kl_sidebar_item(demo->ui, &demo->style, DEMO_ID_PAGES, (uint32_t)index, &item, demo_page_icons[index], demo_page_names[index], index == demo->page);
		if (pressed)
			demo_show_page(demo, index);
	}
}

/* Draws the controls' page: buttons, a card of settings, and progress. */
static void
demo_draw_controls(
	struct demo *demo)
{
	const struct kl_theme *theme;
	struct kl_rect card;
	struct kl_rect control;
	char value[32];
	unsigned changes;
	int pressed;
	int changed;
	int value_x;
	int top;
	int x;

	/* The page's header. */
	theme = demo->style.theme;
	top = kl_header(&demo->style, demo->content.x + 24, demo->content.y + 20, demo->content.width - 48, "Controls", "Buttons, switches, a slider, fields and progress");

	/* The buttons' card: one of each kind. */
	card.x = demo->content.x + 16;
	card.y = top + 16;
	card.width = demo->content.width - 32;
	card.height = 104;
	top = kl_card(&demo->style, &card, "Buttons", NULL);
	control.x = card.x + 20;
	control.y = top + 4;
	control.height = theme->control_height;
	control.width = kl_button_width(&demo->style, "Save");
	pressed = kl_button(demo->ui, &demo->style, DEMO_ID_SAVE, &control, "Save", KL_BUTTON_PRIMARY);
	if (pressed)
		demo_log("BUTTON save");
	control.x += control.width + 8;
	control.width = kl_button_width(&demo->style, "Cancel");
	pressed = kl_button(demo->ui, &demo->style, DEMO_ID_CANCEL, &control, "Cancel", 0U);
	if (pressed)
		demo_log("BUTTON cancel");
	control.x += control.width + 8;
	control.width = kl_button_width(&demo->style, "Delete");
	pressed = kl_button(demo->ui, &demo->style, DEMO_ID_DELETE, &control, "Delete", KL_BUTTON_DANGER);
	if (pressed)
		demo_log("BUTTON delete");
	control.x += control.width + 8;
	control.width = kl_button_width(&demo->style, "Disabled");
	(void)kl_button(demo->ui, &demo->style, DEMO_ID_DISABLED, &control, "Disabled", KL_BUTTON_DISABLED);

	/* The settings' card: a row for each control. */
	card.y += card.height + 12;
	card.height = 46 + 5 * 40 + 14;
	top = kl_card(&demo->style, &card, "Settings", NULL);
	value_x = demo_value_x(&card);

	/* Wi-Fi and Bluetooth: switches. */
	(void)kl_row(&demo->style, card.x, top, card.width, "Wi-Fi", "", 0);
	changed = kl_switch(demo->ui, &demo->style, DEMO_ID_WIFI, value_x, top + (40 - theme->switch_height) / 2, &demo->wifi, 0U);
	if (changed)
		demo_log("SWITCH wifi on=%d", demo->wifi);
	top += 40;
	(void)kl_row(&demo->style, card.x, top, card.width, "Bluetooth", "", 0);
	changed = kl_switch(demo->ui, &demo->style, DEMO_ID_BLUETOOTH, value_x, top + (40 - theme->switch_height) / 2, &demo->bluetooth, 0U);
	if (changed)
		demo_log("SWITCH bluetooth on=%d", demo->bluetooth);
	top += 40;

	/* The volume: a slider and its value. */
	snprintf(value, sizeof(value), "%d%%", (int)demo->volume);
	(void)kl_row(&demo->style, card.x, top, card.width, "Volume", "", 0);
	control.x = value_x;
	control.y = top + 8;
	control.width = card.x + card.width - 20 - 56 - value_x;
	control.height = 24;
	changed = kl_slider(demo->ui, &demo->style, DEMO_ID_VOLUME, &control, 0.0, 100.0, 1.0, &demo->volume);
	if (changed)
		demo_log("SLIDER volume=%d", (int)demo->volume);
	x = control.x + control.width + 12;
	(void)kl_text_draw(&demo->text, &demo->canvas, x, kl_text_center(14U, top, 40), value, strlen(value), 14U, 0, theme->text_secondary);
	top += 40;

	/* The name and the password: fields. */
	(void)kl_row(&demo->style, card.x, top, card.width, "Name", "", 0);
	control.x = value_x;
	control.y = top + 4;
	control.width = card.x + card.width - 20 - value_x;
	control.height = theme->control_height;
	changes = kl_field(demo->ui, &demo->style, DEMO_ID_NAME, &control, &demo->name, "Your name");
	if ((changes & KL_FIELD_CHANGED) != 0U)
		demo_log("FIELD name text=%s", demo->name.text);
	if ((changes & KL_FIELD_SUBMITTED) != 0U)
		demo_log("FIELD name submitted text=%s", demo->name.text);
	if ((changes & KL_FIELD_CANCELLED) != 0U)
		demo_log("FIELD name cancelled");
	top += 40;
	(void)kl_row(&demo->style, card.x, top, card.width, "Password", "", 1);
	control.y = top + 4;
	changes = kl_field(demo->ui, &demo->style, DEMO_ID_PASSWORD, &control, &demo->password, "Password");
	if ((changes & KL_FIELD_CHANGED) != 0U)
		demo_log("FIELD password length=%lu", (unsigned long)demo->password.length);

	/* The progress card: the volume's share. */
	card.y += card.height + 12;
	card.height = 46 + 40;
	top = kl_card(&demo->style, &card, "Progress", NULL);
	control.x = card.x + 20;
	control.y = top + 10;
	control.width = card.width - 40;
	control.height = 6;
	kl_progress(&demo->style, &control, demo->volume / 100.0, 0U);
}

/* Draws the list's page: a hundred rows, what is selected and what was activated. */
static void
demo_draw_list(
	struct demo *demo)
{
	const struct kl_theme *theme;
	struct kl_rect card;
	struct kl_rect area;
	struct kl_rect row;
	kl_color ink;
	kl_color quiet;
	char label[64];
	char status[96];
	unsigned changes;
	size_t first;
	size_t last;
	size_t index;
	int width;
	int top;

	/* The page's header, and what the list reports. */
	theme = demo->style.theme;
	top = kl_header(&demo->style, demo->content.x + 24, demo->content.y + 20, demo->content.width - 48, "List", "The arrows, Page Up and Down, Home, End and Enter; a double click");
	snprintf(status, sizeof(status), "Selected: %ld   Activated: %ld", demo->list.selected, demo->activated);
	(void)kl_text_draw(&demo->text, &demo->canvas, demo->content.x + 24, top + 24, status, strlen(status), 13U, 0, theme->text_secondary);

	/* The card the list stands in. */
	card.x = demo->content.x + 16;
	card.y = top + 40;
	card.width = demo->content.width - 32;
	card.height = demo->content.y + demo->content.height - 16 - card.y;
	(void)kl_card(&demo->style, &card, NULL, NULL);
	area.x = card.x + 8;
	area.y = card.y + 8;
	area.width = card.width - 16;
	area.height = card.height - 16;

	/* The rows that show: a name, and a size at the right. */
	changes = kl_list_begin(demo->ui, &demo->style, DEMO_ID_LIST, &area, &demo->list, DEMO_ROWS, &first, &last);
	for (index = first; index < last; index++) {
		changes |= kl_list_row(demo->ui, &demo->style, DEMO_ID_LIST, &area, &demo->list, index, &row, &ink);
		snprintf(label, sizeof(label), "Item %u", (unsigned)index + 1U);
		(void)kl_text_draw(&demo->text, &demo->canvas, row.x + 12, kl_text_center(13U, row.y, row.height), label, strlen(label), 13U, 0, ink);
		snprintf(label, sizeof(label), "%u KB", ((unsigned)index * 37U) % 900U + 4U);
		width = kl_text_width(&demo->text, label, strlen(label), 13U, 0);
		quiet = theme->text_secondary;
		if (ink != theme->text)
			quiet = ink;
		(void)kl_text_draw(&demo->text, &demo->canvas, row.x + row.width - 12 - width, kl_text_center(13U, row.y, row.height), label, strlen(label), 13U, 0, quiet);
	}

	/* The list ends. */
	kl_list_end(demo->ui, &demo->style, &area, &demo->list);

	/* What the list did (the status above it shows it in the next frame). */
	if (changes != 0U)
		demo->dirty = 1;
	if ((changes & KL_LIST_SELECTED) != 0U)
		demo_log("LIST selected=%ld", demo->list.selected);
	if ((changes & KL_LIST_ACTIVATED) != 0U) {
		demo->activated = demo->list.selected;
		demo_log("LIST activated=%ld", demo->activated);
	}
}

/* Draws the dialogs' page: a question, a chip, work of an unknown length, and the dialog over the page when asked. */
static void
demo_draw_dialogs(
	struct demo *demo,
	uint64_t now_us)
{
	const struct kl_theme *theme;
	struct kl_rect card;
	struct kl_rect control;
	const char *answer;
	int pressed;
	int changed;
	int chosen;
	int value_x;
	int top;

	/* The page's header. */
	theme = demo->style.theme;
	top = kl_header(&demo->style, demo->content.x + 24, demo->content.y + 20, demo->content.width - 48, "Dialogs", "A question over the page, a message of a moment, and work going on");

	/* The card: the buttons that ask and tell, and the last answer. */
	card.x = demo->content.x + 16;
	card.y = top + 16;
	card.width = demo->content.width - 32;
	card.height = 46 + 44 + 3 * 40 + 14;
	top = kl_card(&demo->style, &card, "Try them", NULL);
	control.x = card.x + 20;
	control.y = top + 4;
	control.height = theme->control_height;
	control.width = kl_button_width(&demo->style, "Ask a question");
	pressed = kl_button(demo->ui, &demo->style, DEMO_ID_ASK, &control, "Ask a question", KL_BUTTON_PRIMARY);
	if (pressed) {
		demo->dialog = 1;
		demo_log("DIALOG shown");
	}

	/* The next button, to its right. */
	control.x += control.width + 8;
	control.width = kl_button_width(&demo->style, "Show a chip");
	pressed = kl_button(demo->ui, &demo->style, DEMO_ID_CHIP, &control, "Show a chip", 0U);
	if (pressed) {
		demo->chip_until = now_us + DEMO_CHIP_US;
		demo_log("CHIP shown");
	}

	/* Below the buttons. */
	top += 44;

	/* The last answer. */
	answer = "None yet";
	if (demo->answer >= 0)
		answer = demo_dialog_labels[demo->answer];
	top = kl_row(&demo->style, card.x, top, card.width, "Last answer", answer, 0);

	/* Work going on: a switch, and a bar that moves while it is on. */
	value_x = demo_value_x(&card);
	(void)kl_row(&demo->style, card.x, top, card.width, "Working", "", 0);
	changed = kl_switch(demo->ui, &demo->style, DEMO_ID_WORKING, value_x, top + (40 - theme->switch_height) / 2, &demo->working, 0U);
	if (changed)
		demo_log("SWITCH working on=%d", demo->working);
	top += 40;
	(void)kl_row(&demo->style, card.x, top, card.width, "Progress", "", 1);
	control.x = value_x;
	control.y = top + 17;
	control.width = card.x + card.width - 20 - value_x;
	control.height = 6;
	if (demo->working)
		kl_progress(&demo->style, &control, -1.0, now_us);
	else
		kl_progress(&demo->style, &control, 0.0, now_us);

	/* The chip at the bottom of the content, while it shows. */
	if (demo->chip_until > now_us)
		kl_chip(&demo->style, demo->content.x + demo->content.width / 2, demo->content.y + demo->content.height - 20, "Saved to Documents");

	/* The dialog over the content, while it shows. */
	if (!demo->dialog)
		return;
	chosen = kl_dialog(demo->ui, &demo->style, DEMO_ID_DIALOG, &demo->content, "Delete \xe2\x80\x9creport.txt\xe2\x80\x9d?", "The file goes to the Trash; you can put it back from there until the Trash is emptied.", demo_dialog_labels, 2);
	if (chosen >= 0) {
		demo->answer = chosen;
		demo->dialog = 0;
		demo->dirty = 1;
		demo_log("DIALOG answer=%s", demo_dialog_labels[demo->answer]);
	}
}

/* Reports the left of a card's values: its rows' label takes a third of the card, as kl_row draws them. */
static int
demo_value_x(
	const struct kl_rect *card)
{
	int left;
	int right;

	/* The row's text from the card's margin to its other margin. */
	left = card->x + 20;
	right = card->x + card->width - 18;
	return left + (int)((float)(right - left) * 0.34f);
}

/* Gives the window the frame's glass panels (the sidebar and the content; the library sends only a change); without glass the frames are opaque. */
static void
demo_glass_refresh(
	struct demo *demo)
{
	struct kl_glass_panel panels[2];
	int error;

	/* Frames that are not on glass have no panels. */
	if (!demo->style.glass)
		return;

	/* The two panels. */
	memset(panels, 0, sizeof(panels));
	panels[0].x = demo->sidebar.x;
	panels[0].y = demo->sidebar.y;
	panels[0].width = demo->sidebar.width;
	panels[0].height = demo->sidebar.height;
	panels[0].radius = DEMO_RADIUS;
	panels[0].kind = KL_GLASS_CARD;
	panels[1].x = demo->content.x;
	panels[1].y = demo->content.y;
	panels[1].width = demo->content.width;
	panels[1].height = demo->content.height;
	panels[1].radius = DEMO_RADIUS;
	panels[1].kind = KL_GLASS_CARD;

	/* Refused (a compositor without glass): the next frames are opaque. */
	error = kl_window_set_glass(demo->window, panels, 2U);
	if (error != 0) {
		demo_log("GLASS off errno=%d", error);
		demo->style.glass = 0;
		demo->dirty = 1;
	}
}
