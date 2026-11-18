/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The installer's program: one window of libkeiland's widgets, its words
 * in the desktop's language (domain installer), the helper's processes
 * among the application's events, and the log the tests read on standard
 * error (the session's log): "INSTALLER READY", each screen shown, and
 * the place of every control of a screen when it changes.
 *
 *   installer [--display=NAME] [--width=N] [--height=N]
 */

#include "installer.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The fonts. */
#define MAIN_FONT		KEILAND_DATADIR "/fonts/keiland.ttf"
#define MAIN_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"

/* The window's size until the compositor gives one. */
#define MAIN_WIDTH		980U
#define MAIN_HEIGHT		700U

/* The wait for the compositor while something moves, and while nothing does, in milliseconds. */
#define MAIN_FRAME_MS		16
#define MAIN_IDLE_MS		1000

/* How many times a stale swapchain is remade for one frame. */
#define MAIN_STALE_LIMIT	8U

/* How many times, and how far apart, the presenter is asked for again when the device refuses it. */
#define MAIN_RESIZE_TRIES	20U
#define MAIN_RESIZE_WAIT_US	250000U

static int main_parse(int argc, char **argv, const char **display, unsigned *width, unsigned *height);
static int main_number(const char *text, unsigned *value);
static int main_loop(struct inst_app *app);
static void main_event(struct inst_app *app, const struct kl_window_event *event);
static void main_key(struct inst_app *app, uint32_t code, unsigned modifiers);
static int main_frame(struct inst_app *app);
static int main_resize(struct inst_app *app);
static int main_canvas_make(struct inst_app *app);
static void main_log_controls(struct inst_app *app);
static void main_language_changed(void *data, const char *language);

/*
 * The installer, for the program's run.  Its zero value is a window not
 * yet open; main fills it once and the loop works on it until the end.
 */
static struct inst_app main_app;

/*
 * Runs the installer.
 */
int
main(
	int argc,
	char **argv)
{
	struct kl_window_options window_options;
	struct kl_app_options app_options;
	struct inst_app *app;
	const char *display;
	unsigned width;
	unsigned height;
	int status;
	int error;

	/* The command line. */
	status = main_parse(argc, argv, &display, &width, &height);
	if (status != 0) {
		fprintf(stderr, "usage: installer [--display=NAME] [--width=N] [--height=N]\n");
		return 2;
	}

	/* A write to a closed pipe or socket does not end the installer. */
	(void)signal(SIGPIPE, SIG_IGN);

	/* The font; without it the window shows no words. */
	app = &main_app;
	app->pending_item = -1;
	error = kl_text_open(&app->text, MAIN_FONT, MAIN_FALLBACK_FONT);
	if (error != 0)
		inst_log("FONT missing path=%s error=%d", MAIN_FONT, error);

	/* The application: the connection and its globals. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.display = display;
	app_options.application = "installer";
	app->app = kl_app_open(&app_options);
	if (app->app == NULL) {
		fprintf(stderr, "INSTALLER FAILED operation=app error=%d\n", errno);
		kl_text_close(&app->text);
		return 1;
	}

	/* The words in the desktop's language, as it changes. */
	app->language = kl_settings_open(kl_app_display(app->app), NULL);
	if (app->language != NULL) {
		error = kl_tr_follow(app->language, "installer", main_language_changed, app);
		if (error != 0)
			inst_log("LANGUAGE follow errno=%d", error);
	}

	/* Its window. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = kl_tr("Installer");
	window_options.width = width;
	window_options.height = height;
	window_options.present = KL_PRESENT_VULKAN;
	app->window = kl_app_window_create(app->app, &window_options);
	if (app->window == NULL) {
		fprintf(stderr, "INSTALLER FAILED operation=window error=%d\n", errno);
		kl_app_close(app->app);
		kl_text_close(&app->text);
		return 1;
	}

	/* The input and the content's scroll. */
	app->ui = kl_ui_create();
	error = kl_scroll_init(&app->scroll, KL_SCROLL_Y);
	if (app->ui == NULL || error != 0) {
		fprintf(stderr, "INSTALLER FAILED operation=memory\n");
		kl_ui_destroy(app->ui);
		kl_app_close(app->app);
		kl_text_close(&app->text);
		return 1;
	}

	/* The widgets' style, and the wizard at its start. */
	app->style.text = &app->text;
	app->style.theme = kl_theme_default();
	app->style.glass = 0;
	inst_flow_start(app);

	/* The loop, until the window closes. */
	status = main_loop(app);

	/* Everything goes; the helper's session ends with its input. */
	inst_helper_end_session(app);
	inst_license_close(app);
	kl_scroll_release(&app->scroll);
	kl_ui_destroy(app->ui);
	if (app->canvas_made)
		kl_canvas_release(&app->canvas);
	free(app->pixels);
	if (app->language != NULL)
		kl_settings_close(app->language);
	kl_app_close(app->app);
	kl_text_close(&app->text);

	/* Reports how the run ended. */
	if (status != 0)
		return 1;
	return 0;
}

/* Writes one line of the log: "INSTALLER " and the words, on standard error. */
void
inst_log(
	const char *format,
	...)
{
	va_list arguments;

	/* The line. */
	va_start(arguments, format);
	fputs("INSTALLER ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

/*
 * Records a control of the frame being drawn for the log (its name, its
 * rectangle in the window and whether it takes input).
 */
void
inst_control(
	struct inst_app *app,
	const char *name,
	const struct kl_rect *rect,
	int enabled)
{
	struct inst_logged *control;

	/* While the frame's list has room. */
	if (app->drawn_count >= INST_LOGGED_MAX)
		return;
	control = &app->drawn[app->drawn_count];
	memset(control, 0, sizeof(*control));
	(void)snprintf(control->name, sizeof(control->name), "%s", name);
	control->rect = *rect;
	control->enabled = enabled;
	app->drawn_count++;
}

/* Asks for a frame. */
void
inst_redraw(
	struct inst_app *app)
{
	/* The next round draws. */
	app->dirty = 1;
}

/* Reads the command line; nonzero for a malformed one. */
static int
main_parse(
	int argc,
	char **argv,
	const char **display,
	unsigned *width,
	unsigned *height)
{
	int status;
	int index;
	int same;

	/* The defaults. */
	*display = NULL;
	*width = MAIN_WIDTH;
	*height = MAIN_HEIGHT;

	/* Each argument. */
	for (index = 1; index < argc; index++) {
		/* The display to connect to. */
		same = strncmp(argv[index], "--display=", 10U);
		if (same == 0) {
			*display = argv[index] + 10;
			continue;
		}

		/* The window's width. */
		same = strncmp(argv[index], "--width=", 8U);
		if (same == 0) {
			status = main_number(argv[index] + 8, width);
			if (status != 0)
				return status;
			continue;
		}

		/* The window's height. */
		same = strncmp(argv[index], "--height=", 9U);
		if (same == 0) {
			status = main_number(argv[index] + 9, height);
			if (status != 0)
				return status;
			continue;
		}

		/* Anything else refuses the command line. */
		return -1;
	}

	/* A window has some size. */
	if (*width < 640U || *height < 480U)
		return -1;

	/* Succeeded: the options are read. */
	return 0;
}

/* Reads a decimal number of at most 8192; nonzero for a malformed one. */
static int
main_number(
	const char *text,
	unsigned *value)
{
	unsigned long number;
	char *end;

	/* The digits, all of them. */
	errno = 0;
	number = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || number > 8192UL)
		return -1;

	/* Succeeded: the number. */
	*value = (unsigned)number;
	return 0;
}

/* Runs the window until it closes; nonzero when something failed. */
static int
main_loop(
	struct inst_app *app)
{
	struct kl_app_event event;
	int timeout;
	int taken;
	int status;

	/* The presenter's size, a canvas of it, and the first frame. */
	status = main_resize(app);
	if (status != 0)
		return -1;
	status = main_frame(app);
	if (status != 0)
		return -1;
	inst_log("READY width=%u height=%u", app->width, app->height);

	/* Each round: input and the helper's lines, and a frame when something changed or moves. */
	for (;;) {
		/* Waits for the compositor or the helper, or a frame's time while something moves. */
		timeout = MAIN_IDLE_MS;
		if (app->moving || app->screen == INST_S9)
			timeout = MAIN_FRAME_MS;
		if (app->dirty)
			timeout = 0;
		status = kl_app_dispatch(app->app, timeout);
		if (status != 0) {
			inst_log("DONE reason=disconnected");
			return 0;
		}

		/* Lets a language change reach the catalogs. */
		if (app->language != NULL)
			(void)kl_settings_dispatch(app->language);

		/* Every event: the window's inputs, the helper's processes, the theme. */
		for (;;) {
			taken = kl_app_take(app->app, &event);
			if (!taken)
				break;
			if (event.kind == KL_APP_THEME) {
				app->dirty = 1;
				continue;
			}

			/* A helper's process told its lines or its end. */
			if (event.kind == KL_APP_PROCESS) {
				inst_helper_ready(app, event.process, event.ready);
				continue;
			}

			/* Only this window's inputs are taken. */
			if (event.kind != KL_APP_WINDOW || event.window != app->window)
				continue;
			main_event(app, &event.input);
		}

		/* The close button or the last screen's Close ends the run. */
		if (app->closed || app->quit) {
			inst_log("DONE reason=close screen=%s", inst_screen_word(app->screen));
			return 0;
		}

		/* A new size: a new swapchain and canvas. */
		if (app->resized) {
			app->resized = 0;
			status = main_resize(app);
			if (status != 0)
				return -1;
		}

		/* A frame when something changed or moves (S9's time and bar always move). */
		if (app->dirty || app->moving || app->screen == INST_S9) {
			status = main_frame(app);
			if (status != 0)
				return -1;
		}
	}
}

/* Gives one input of the window to the widgets. */
static void
main_event(
	struct inst_app *app,
	const struct kl_window_event *event)
{
	/* Any input may change the screen. */
	app->dirty = 1;

	/* What it is. */
	switch (event->kind) {
	case KL_WINDOW_RESIZE:
		app->resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		app->closed = 1;
		break;
	default:
		(void)kl_ui_window_input(app->ui, event);
		break;
	}
}

/*
 * Takes a key no widget took: Enter goes Next, Alt+Left and Alt+Right go
 * Back and Next, Esc cancels (each confirmed where the screen asks).
 */
static void
main_key(
	struct inst_app *app,
	uint32_t code,
	unsigned modifiers)
{
	/* A dialog takes its own keys. */
	if (app->dialog != INST_DIALOG_NONE)
		return;

	/* Alt with the arrows walks the screens. */
	if ((modifiers & KL_MOD_ALT) != 0U) {
		if (code == KL_KEY_LEFT)
			inst_back(app);
		if (code == KL_KEY_RIGHT)
			inst_next(app);
		return;
	}

	/* Enter goes Next; Esc asks to cancel. */
	if (code == KL_KEY_ENTER || code == KL_KEY_KPENTER)
		inst_next(app);
	else if (code == KL_KEY_ESC)
		inst_cancel_asked(app);
}

/* Draws and shows a frame, remaking a stale swapchain; nonzero when it cannot be shown. */
static int
main_frame(
	struct inst_app *app)
{
	struct kl_event event;
	uint64_t now;
	unsigned stale;
	int taken;
	int status;

	/* Tries until the frame is shown. */
	for (stale = 0; stale < MAIN_STALE_LIMIT; stale++) {
		/* The frame: the widgets record their places as they are drawn. */
		now = kl_clock_us();
		app->dirty = 0;
		app->drawn_count = 0;
		app->style.theme = kl_theme_default();
		kl_ui_begin(app->ui, now);
		inst_draw(app, now);
		app->moving = kl_ui_end(app->ui, now);
		kl_ui_window_text(app->ui, app->window);

		/* The keys no widget took. */
		for (;;) {
			taken = kl_ui_take(app->ui, &event);
			if (!taken)
				break;
			if (event.kind == KL_EVENT_KEY)
				main_key(app, event.code, event.modifiers);
		}

		/* The controls' places, when they changed. */
		main_log_controls(app);

		/* The frame shown. */
		status = kl_window_present(app->window, app->pixels, (size_t)app->width);
		if (status == 0)
			return 0;

		/*
		 * A stale swapchain is remade at the window's size; so is one that
		 * failed otherwise (a device that came back), for an installation
		 * the window shows must not end with a frame that was not shown.
		 */
		if (status != EAGAIN)
			inst_log("PRESENT error=%d attempt=%u", status, stale);
		status = main_resize(app);
		if (status != 0)
			return -1;
	}

	/* The swapchain stayed out of date. */
	fprintf(stderr, "INSTALLER FAILED operation=stale-swapchain\n");
	return -1;
}

/* Remakes the presenter at the window's size, with a canvas to match; nonzero when it cannot. */
static int
main_resize(
	struct inst_app *app)
{
	unsigned attempt;
	int status;

	/*
	 * The presenter at the window's size; a device that refuses it for a
	 * moment (EIO while the GPU is busy) is asked again a few times.
	 */
	status = kl_window_present_resize(app->window, &app->width, &app->height);
	for (attempt = 1U; status != 0 && attempt < MAIN_RESIZE_TRIES; attempt++) {
		inst_log("PRESENT resize error=%d attempt=%u", status, attempt);
		(void)usleep(MAIN_RESIZE_WAIT_US);
		status = kl_window_present_resize(app->window, &app->width, &app->height);
	}

	/* Gives up when the presenter never accepted the size. */
	if (status != 0) {
		fprintf(stderr, "INSTALLER FAILED operation=present error=%d\n", status);
		return -1;
	}

	/* A canvas of its size. */
	status = main_canvas_make(app);
	if (status != 0) {
		fprintf(stderr, "INSTALLER FAILED operation=canvas\n");
		return -1;
	}

	/* Succeeded: frames are drawn at the new size. */
	app->dirty = 1;
	return 0;
}

/* Makes the frame's memory and canvas at the presenter's size; nonzero when memory runs out. */
static int
main_canvas_make(
	struct inst_app *app)
{
	uint32_t *pixels;
	size_t count;
	int status;

	/* The frame's memory. */
	count = (size_t)app->width * (size_t)app->height;
	pixels = malloc(count * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;

	/* The canvas over it, in place of the old one. */
	if (app->canvas_made)
		kl_canvas_release(&app->canvas);
	app->canvas_made = 0;
	free(app->pixels);
	app->pixels = pixels;
	status = kl_canvas_init(&app->canvas, app->pixels, (size_t)app->width, (int)app->width, (int)app->height);
	if (status != 0)
		return -1;
	app->canvas_made = 1;
	app->style.canvas = &app->canvas;

	/* Succeeded. */
	return 0;
}

/*
 * Logs the controls of the frame when they differ from those logged last
 * (another screen, a dialog, a control that moved, came or went), for the
 * tests that press them: "LAYOUT", then a "CONTROL" line each.
 */
static void
main_log_controls(
	struct inst_app *app)
{
	const struct inst_logged *control;
	unsigned i;
	int differs;
	int same;

	/* The same list on the same screen and dialog is not logged again. */
	differs = 0;
	if (!app->logged_once)
		differs = 1;
	if (app->logged_screen != app->screen)
		differs = 1;
	if (app->logged_dialog != app->dialog)
		differs = 1;
	if (app->drawn_count != app->logged_count)
		differs = 1;
	for (i = 0; i < app->drawn_count && !differs; i++) {
		same = memcmp(&app->drawn[i], &app->logged[i], sizeof(app->drawn[i]));
		if (same != 0)
			differs = 1;
	}

	/* Nothing changed since the last log. */
	if (!differs)
		return;

	/* The new list, kept and logged. */
	memcpy(app->logged, app->drawn, sizeof(app->drawn[0]) * app->drawn_count);
	app->logged_count = app->drawn_count;
	app->logged_screen = app->screen;
	app->logged_dialog = app->dialog;
	app->logged_once = 1;
	inst_log("LAYOUT screen=%s dialog=%d controls=%u", inst_screen_word(app->screen), (int)app->dialog, app->drawn_count);
	for (i = 0; i < app->drawn_count; i++) {
		control = &app->drawn[i];
		inst_log("CONTROL name=%s x=%d y=%d width=%d height=%d enabled=%d", control->name, control->rect.x, control->rect.y,
			 control->rect.width, control->rect.height, control->enabled);
	}
}

/* Draws the window again in the language its catalogs were read in. */
static void
main_language_changed(
	void *data,
	const char *language)
{
	struct inst_app *app;

	/* Every word is drawn again; the log line the tests read. */
	app = data;
	app->dirty = 1;
	inst_log("LANGUAGE language=%s", language);

	/* The window's title, given when the window was made, follows the language too. */
	if (app->window != NULL)
		kl_window_set_title(app->window, kl_tr("Installer"));
}
