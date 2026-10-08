/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PDF Viewer (ws079-p006): a PDF document in a Wayland window, drawn on
 * the CPU with libpdf and shown with Vulkan.
 *
 *   pdfviewer [--display=NAME] [--font=PATH] [--width=N] [--height=N]
 *             [--mode=scroll|page] [--timeout-s=N] [FILE]
 *
 * The file is opened from the command line (Files' Open With runs this
 * with the file), or with File > Open (Ctrl+O).  "Annotate in Notes"
 * (Ctrl+E) starts /bin/notes on the file.  The outcome is one line on
 * standard error: PDFVIEWER DONE with the reason, or PDFVIEWER FAILED
 * naming what failed; PDFVIEWER READY says the first frame is shown.
 * ws081-p012: the touch screen scrolls with inertia, zooms with two
 * fingers and swipes pages (touch.c).  ws090-p008: the window, its input
 * and the frames shown with Vulkan are libkeiland's (kl_window).
 */

#include "window.h"
#include "../picture/png-write.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* The font used unless told otherwise. */
#define MAIN_FONT		KEILAND_DATADIR "/fonts/keiland.ttf"

/* The program "Annotate in Notes" starts. */
#define MAIN_NOTES		KEILAND_BINDIR "/notes"

/* How many frames in a row may find the swapchain out of date before the program gives up. */
#define MAIN_STALE_LIMIT	8U

/* The longest the loop sleeps when nothing is due, in milliseconds. */
#define MAIN_IDLE_MS		1000

/* The application's identity in the compositor and the recent files. */
#define MAIN_APPLICATION	"pdfviewer"

/* An image dragged out of the window: drawn at twice the page's points, its longer side at most this (ws189-p003). */
#define MAIN_DRAG_SCALE		2.0
#define MAIN_DRAG_SIDE		2048.0

/*
 * What the command line asked for.
 */
struct main_options {
	const char *display;
	const char *font;
	const char *file;
	unsigned width;
	unsigned height;
	unsigned timeout;
	int page_mode;
};

/*
 * The program's parts, for the whole run.  They are file-scope because
 * the viewer is too large for the stack.
 *
 * The window: libkeiland's window (the Wayland connection, the surface, the
 * input queue and the Vulkan presenter), from the start of the run to its
 * end.
 */
static struct pv_window main_window;

/* The size of the presenter, which the frames are drawn at; set with the window and at each new size. */
static uint32_t main_width;
static uint32_t main_height;

/* Whether the compositor gave a new size, or asked to close, since the loop last looked. */
static int main_resized;
static int main_closed;

/* The viewer: the document and the view, made once the swapchain's size is known. */
static struct pv_app main_app;

/* The font the frame's words are drawn in, open for the whole run (without it the frame has no words). */
static struct pv_text main_text;

/*
 * The window's menus in the compositor, opened with the window and closed
 * before it; absent with a compositor without them.
 */
static struct pv_menu main_menu;

/* The window's titlebar controls in the compositor, with the same life as the menus. */
static struct pv_titlebar main_titlebar;

/* The touch screen's gestures and scroller, made with the viewer (without them fingers do nothing). */
static struct pv_touch main_touch;

/* The find field inside the window where the compositor shows no titlebar (ws177-p043), made with the window. */
static struct pv_bar main_bar;

/*
 * libkeiland's file chooser while the viewer waits for it (File > Open), a
 * window of its own over the viewer's; NULL otherwise.
 */
static struct kl_file_chooser *main_chooser;

/*
 * The application (WS131 p017, libkeiland's kl_app): the connection, its
 * one queue of inputs (the window's, the actions of its menus and
 * controls), and the desktop's appearance, which the viewer draws in
 * (draw.c, ws089-p017).
 */
static struct kl_app *main_kl;

/* The print under way (ws145-p007): its request until it is answered, then its job until it ends (0 for none). */
static uint32_t main_print_request;
static uint32_t main_print_job;

/* The font the chooser draws with: the viewer's own. */
static const char *main_font;

/* The files the chooser offers: PDF documents first, or every file. */
static const struct kl_file_filter main_filters[] = {
	{ "PDF Documents", "pdf" },
	{ "All Files", NULL }
};

/*
 * The frame being drawn: ordinary memory the size of the swapchain, remade
 * (and the canvas with it) when the window changes size.
 */
static uint32_t *main_pixels;

/* The canvas over main_pixels, which the viewer draws each frame into. */
static struct pv_canvas main_canvas;

/* The environment a started program inherits. */
extern char **environ;

static int main_parse(int argc, char **argv, struct main_options *options);
static const char *main_value(const char *argument, const char *name);
static int main_number(const char *text, unsigned maximum, unsigned *value);
static int main_loop(const struct main_options *options);
static int main_frame(void);
static int main_resize(void);
static void main_window_event(const struct kl_window_event *event);
static void main_event(enum pv_event_type type, const struct kl_window_event *event, struct pv_event *input);
static int main_keyboard_inset(void *data, int right, int bottom, unsigned reason);
static void main_choose(void);
static void main_chosen(void *data, struct kl_file_chooser *chooser, unsigned result, const char *path, size_t filter);
static void main_turn_frame(uint64_t started, uint64_t shown);
static int main_canvas_make(void);
static void main_state(struct pv_state *state);
static void main_opened(void);
static void main_annotate(void);
static void main_print(void);
static void main_print_follow(void);
static void main_appearance_changed(void);
static void main_accent(void);
static void main_drag(void);
static int main_drag_image_pixels(uint32_t **pixels, int *width, int *height);

/*
 * Runs PDF Viewer.
 */
int
main(
	int argc,
	char **argv)
{
	struct main_options options;
	struct kl_app_options app_options;
	struct kl_window_options window_options;
	struct pv_state state;
	int status;
	int error;

	/* The command line. */
	status = main_parse(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: pdfviewer [--display=NAME] [--font=PATH] [--width=N] [--height=N] [--mode=scroll|page] [--timeout-s=N] [FILE]\n");
		return 2;
	}

	/* The font; without it the viewer shows the pages and no text of its own. */
	error = pv_text_open(&main_text, options.font);
	if (error != 0)
		pv_log("FONT missing path=%s error=%d", options.font, error);
	main_font = options.font;

	/* The application: the connection to the compositor. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.display = options.display;
	app_options.application = MAIN_APPLICATION;
	main_kl = kl_app_open(&app_options);
	if (main_kl == NULL) {
		fprintf(stderr, "PDFVIEWER FAILED operation=app error=%d\n", errno);
		pv_text_close(&main_text);
		return 1;
	}

	/* The window, its frames shown with Vulkan. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "PDF Viewer";
	window_options.width = options.width;
	window_options.height = options.height;
	window_options.present = KL_PRESENT_VULKAN;
	main_window.kui = kl_app_window_create(main_kl, &window_options);
	if (main_window.kui == NULL) {
		fprintf(stderr, "PDFVIEWER FAILED operation=window error=%d\n", errno);
		kl_app_close(main_kl);
		pv_text_close(&main_text);
		return 1;
	}

	/* The presenter's size, which the frames are drawn at. */
	error = kl_window_present_resize(main_window.kui, &main_width, &main_height);
	if (error != 0) {
		fprintf(stderr, "PDFVIEWER FAILED operation=present error=%d\n", error);
		kl_window_close(main_window.kui);
		kl_app_close(main_kl);
		pv_text_close(&main_text);
		return 1;
	}

	/* The viewer at the presenter's size, in the mode asked for, with the file when one was given. */
	pv_app_init(&main_app, &main_text, (int)main_width, (int)main_height);
	if (options.page_mode)
		pv_app_action(&main_app, PV_ACTION_MODE_PAGE);
	if (options.file != NULL)
		(void)pv_app_open(&main_app, options.file);

	/* The desktop's appearance (the application's): the viewer's colours follow it, the pages stay white. */
	pv_draw_set_dark(kl_appearance_get(NULL) == KL_APPEARANCE_DARK);
	main_accent();

	/* The on-screen keyboard's inset keeps the password card in the part it leaves. */
	kl_window_on_keyboard_inset(main_window.kui, main_keyboard_inset, &main_app);

	/* The touch screen; without memory for it the fingers do nothing. */
	error = pv_touch_open(&main_touch);
	if (error != 0)
		pv_log("TOUCH failed errno=%d", error);

	/* The find field inside the window (ws177-p043); without memory for it Ctrl+F does nothing without a titlebar. */
	error = pv_bar_open(&main_bar, options.font);
	if (error != 0)
		pv_log("BAR failed errno=%d", error);

	/* The menus and the titlebar; a window without them goes on with its keys. */
	main_state(&state);
	error = pv_menu_open(&main_menu, &main_window, &state);
	if (error != 0) {
		pv_log("MENU failed errno=%d", error);
		pv_menu_close(&main_menu);
	}

	/* The titlebar's controls. */
	error = pv_titlebar_open(&main_titlebar, &main_window, &state);
	if (error != 0) {
		pv_log("TITLEBAR failed errno=%d", error);
		pv_titlebar_close(&main_titlebar);
	}

	/* The loop, until the window closes. */
	status = main_loop(&options);

	/* Everything goes, the chooser, the titlebar, the menus and the viewer before the window they belong to. */
	kl_file_chooser_destroy(main_chooser);
	main_chooser = NULL;
	pv_titlebar_close(&main_titlebar);
	pv_menu_close(&main_menu);
	pv_touch_close(&main_touch);
	pv_bar_close(&main_bar);
	pv_app_release(&main_app);
	free(main_pixels);
	kl_window_close(main_window.kui);
	kl_app_close(main_kl);
	pv_text_close(&main_text);

	/* Reports how the run ended. */
	if (status != 0)
		return 1;

	/* Succeeded: the window was closed. */
	return 0;
}

/* Reads the command line into the options; returns nonzero for a malformed one. */
static int
main_parse(
	int argc,
	char **argv,
	struct main_options *options)
{
	const char *value;
	int status;
	int index;
	int match;

	/* The defaults. */
	memset(options, 0, sizeof(*options));
	options->font = MAIN_FONT;
	options->width = PV_WIDTH;
	options->height = PV_HEIGHT;

	/* Each argument. */
	for (index = 1; index < argc; index++) {
		/* The compositor's display. */
		value = main_value(argv[index], "--display=");
		if (value != NULL) {
			options->display = value;
			continue;
		}

		/* The font of the viewer's own text. */
		value = main_value(argv[index], "--font=");
		if (value != NULL) {
			options->font = value;
			continue;
		}

		/* The window's width. */
		value = main_value(argv[index], "--width=");
		if (value != NULL) {
			status = main_number(value, 8192U, &options->width);
			if (status != 0)
				return status;
			continue;
		}

		/* The window's height. */
		value = main_value(argv[index], "--height=");
		if (value != NULL) {
			status = main_number(value, 8192U, &options->height);
			if (status != 0)
				return status;
			continue;
		}

		/* The mode to start in. */
		value = main_value(argv[index], "--mode=");
		if (value != NULL) {
			match = strcmp(value, "page");
			if (match == 0) {
				options->page_mode = 1;
				continue;
			}

			/* The scroll mode is the default; anything else is a usage error. */
			match = strcmp(value, "scroll");
			if (match != 0)
				return -1;
			continue;
		}

		/* How long the program runs at most (0 for ever). */
		value = main_value(argv[index], "--timeout-s=");
		if (value != NULL) {
			status = main_number(value, 86400U, &options->timeout);
			if (status != 0)
				return status;
			continue;
		}

		/* An unknown option refuses the command line. */
		if (argv[index][0] == '-')
			return -1;

		/* The file to open, once. */
		if (options->file != NULL)
			return -1;
		options->file = argv[index];
	}

	/* A window has some size. */
	if (options->width < 320U || options->height < 240U)
		return -1;

	/* Succeeded: the options are read. */
	return 0;
}

/* Returns what follows an option's name in an argument, or NULL when the argument is another option. */
static const char *
main_value(
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
main_number(
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

/* Runs the window until it closes (or the timeout passes); returns nonzero when something failed. */
static int
main_loop(
	const struct main_options *options)
{
	struct kl_app_event app_event;
	struct pv_state state;
	uint64_t started;
	uint64_t now;
	pid_t ended;
	int prefetched;
	int taken;
	int status;
	int timeout;
	int due;

	/* The first frame's canvas and the first frame. */
	status = main_canvas_make();
	if (status != 0) {
		fprintf(stderr, "PDFVIEWER FAILED operation=canvas\n");
		return -1;
	}

	/* The document given on the command line, and the first frame. */
	main_opened();
	status = main_frame();
	if (status != 0)
		return -1;
	pv_log("READY width=%u height=%u pages=%lu", main_width, main_height, (unsigned long)main_app.document.count);

	/* Each round: input, time, and a frame when something changed. */
	started = pv_clock();
	for (;;) {
		/* Waits for the compositor, or until something is due. */
		now = pv_clock();
		timeout = MAIN_IDLE_MS;
		due = pv_app_tick(&main_app, now);
		if (due >= 0 && due < timeout)
			timeout = due;
		due = pv_touch_tick(&main_touch, &main_app, kl_clock_us());
		if (due >= 0 && due < timeout)
			timeout = due;
		if (main_app.dirty)
			timeout = 0;

		/* With time to spare, the pages next to the view are drawn ahead, one a round. */
		if (timeout > 0) {
			prefetched = pv_app_prefetch(&main_app);
			if (prefetched)
				timeout = 0;
		}

		/*
		 * Waits; a lost connection ends the run.  A key held repeats within,
		 * after the compositor's input, so that its release is seen first
		 * (BUG-111).
		 */
		status = kl_app_dispatch(main_kl, timeout);
		if (status != 0) {
			pv_log("DONE reason=disconnected");
			return 0;
		}

		/* The time of this round. */
		now = pv_clock();
		main_app.now = now;

		/* Every input queued, in the order it came (the menus' and the titlebar's choices and the fingers among them), and the desktop's appearance. */
		for (;;) {
			taken = kl_app_take(main_kl, &app_event);
			if (taken == 0)
				break;
			if (app_event.kind == KL_APP_THEME)
				main_appearance_changed();
			if (app_event.kind == KL_APP_WINDOW && app_event.window == main_window.kui)
				main_window_event(&app_event.input);
		}

		/* The chooser the viewer asked for (File > Open), or one it no longer waits for closed. */
		main_choose();

		/* A document opened: its title and the recent files; an annotation asked for: Notes. */
		main_opened();
		if (main_app.want_annotate) {
			main_app.want_annotate = 0;
			main_annotate();
		}

		/* A print asked for (ws145-p007), and the one under way followed. */
		if (main_app.want_print) {
			main_app.want_print = 0;
			main_print();
		}

		/* The print under way followed. */
		main_print_follow();

		/* The find field asked for (Ctrl+F, ws128-p004): the titlebar's, or without it the one inside the window (ws177-p043). */
		if (main_app.want_find_focus) {
			main_app.want_find_focus = 0;
			if (main_titlebar.shown) {
				pv_titlebar_focus_find(&main_titlebar);
			} else {
				pv_find_bar_open(&main_app);
			}
		}

		/* The field inside the window takes the keyboard when it is asked for. */
		if (main_app.want_bar_focus) {
			main_app.want_bar_focus = 0;
			pv_bar_focus(&main_bar, &main_app);
		}

		/* A drag out of the window the view asked for: the selection's words or an image (ws189-p003). */
		if (main_app.drag_request != PV_DRAG_NONE)
			main_drag();

		/* The words copied, on the clipboard. */
		if (main_app.copy_text != NULL) {
			kl_window_copy(main_window.kui, main_app.copy_text, main_app.copy_length);
			free(main_app.copy_text);
			main_app.copy_text = NULL;
		}

		/* Time passes for the viewer and the fingers; the menus and the titlebar show its state. */
		(void)pv_app_tick(&main_app, now);
		(void)pv_touch_tick(&main_touch, &main_app, kl_clock_us());
		main_state(&state);
		pv_menu_refresh(&main_menu, &state);
		pv_titlebar_refresh(&main_titlebar, &state);

		/* Started programs that ended are reaped. */
		for (;;) {
			ended = waitpid(-1, NULL, WNOHANG);
			if (ended <= 0)
				break;
		}

		/* The close button, Quit, or Close on an empty window end the run. */
		if (main_closed != 0 || main_app.want_close != 0) {
			pv_log("DONE reason=close");
			return 0;
		}

		/* So does the timeout, when one was given. */
		if (options->timeout != 0U && now - started >= (uint64_t)options->timeout * 1000U) {
			pv_log("DONE reason=timeout");
			return 0;
		}

		/* A new size: a new swapchain and canvas, and a frame. */
		if (main_resized != 0) {
			main_resized = 0;
			status = main_resize();
			if (status != 0)
				return -1;
		}

		/* A frame when something changed. */
		if (main_app.dirty != 0) {
			status = main_frame();
			if (status != 0)
				return -1;
		}
	}
}

/* Draws and shows a frame, remaking the swapchain when it is out of date; nonzero when it cannot be shown. */
static int
main_frame(void)
{
	uint64_t started;
	uint64_t shown;
	unsigned stale;
	int status;
	int again;

	/* Tries until the frame is shown, remaking a stale swapchain a few times. */
	for (stale = 0; stale < MAIN_STALE_LIMIT; stale++) {
		/* The frame on the CPU, shown in the window. */
		started = pv_clock();
		pv_draw(&main_app, &main_canvas);

		/* The find field inside the window over it (ws177-p043), which may want the next frame. */
		again = pv_bar_draw(&main_bar, &main_app, main_window.kui, main_pixels, (size_t)main_width, (int)main_width, (int)main_height,
				    kl_clock_us());
		if (again)
			main_app.dirty = 1;

		/* Shown. */
		status = kl_window_present(main_window.kui, main_pixels, (size_t)main_width);
		if (status == 0) {
			shown = pv_clock();
			main_turn_frame(started, shown);
			return 0;
		}

		/* Anything but a stale swapchain is a failure. */
		if (status != EAGAIN) {
			fprintf(stderr, "PDFVIEWER FAILED operation=present error=%d\n", status);
			return -1;
		}

		/* A stale swapchain is remade at the window's size, with a canvas to match. */
		status = main_resize();
		if (status != 0)
			return -1;
	}

	/* The swapchain stayed out of date. */
	fprintf(stderr, "PDFVIEWER FAILED operation=stale-swapchain\n");
	return -1;
}

/*
 * Follows a page turn's frames (ws079-p016): the longest frame while the
 * page changes, and once it rests, the time from the action to its frame
 * shown ("TURN done", which the demo's test reads).
 */
static void
main_turn_frame(
	uint64_t started,
	uint64_t shown)
{
	static uint64_t longest;
	uint64_t took;

	/* No turn going on. */
	if (main_app.turn_at == 0U)
		return;

	/* The longest frame of the turn. */
	took = shown - started;
	if (took > longest)
		longest = took;

	/* Still sliding: more frames come. */
	if (main_app.turning)
		return;

	/* The page rests: the whole turn and its longest frame. */
	pv_log("TURN done page=%lu ms=%lu frame_ms=%lu", (unsigned long)pv_app_current_page(&main_app), (unsigned long)(shown - main_app.turn_at), (unsigned long)longest);
	main_app.turn_at = 0U;
	longest = 0U;
}

/* Remakes the presenter at the window's size, with a canvas to match; nonzero when it cannot. */
static int
main_resize(void)
{
	int status;

	/* The presenter at the window's size. */
	status = kl_window_present_resize(main_window.kui, &main_width, &main_height);
	if (status != 0) {
		fprintf(stderr, "PDFVIEWER FAILED operation=present error=%d\n", status);
		return -1;
	}

	/* A canvas of the presenter's size, and the viewer laid out in it. */
	status = main_canvas_make();
	if (status != 0)
		return -1;
	pv_app_resize(&main_app, (int)main_width, (int)main_height);

	/* Succeeded: frames are drawn at the new size. */
	return 0;
}

/*
 * Takes one input of the window: the pointer, the wheel, the keys and the
 * menus' and the titlebar's actions become the viewer's inputs, the
 * fingers go to the touch screen, and a new size or a close request is
 * noted for the loop.
 */
static void
main_window_event(
	const struct kl_window_event *event)
{
	struct pv_event input;
	int taken;

	/* The find field inside the window takes its inputs first (ws177-p043). */
	taken = pv_bar_input(&main_bar, &main_app, event);
	if (taken)
		return;

	/* What it is. */
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		main_event(PV_EVENT_MOTION, event, &input);
		pv_app_event(&main_app, &input);
		break;
	case KL_WINDOW_LEAVE:
		main_event(PV_EVENT_LEAVE, event, &input);
		pv_app_event(&main_app, &input);
		break;
	case KL_WINDOW_BUTTON:
		/* The button and whether it went down. */
		main_event(PV_EVENT_BUTTON, event, &input);
		input.button = event->code;
		input.pressed = event->pressed;
		pv_app_event(&main_app, &input);
		break;
	case KL_WINDOW_AXIS:
		/* A touch pad's fingers scroll the view as fingers do, with libkeiland's scroller (ws090-p019). */
		if (event->axis_source == KL_AXIS_SOURCE_FINGER) {
			pv_touch_pad(&main_touch, &main_app, event);
			break;
		}

		/* Only the vertical wheel scrolls the view. */
		if (event->dy == 0.0)
			break;
		main_event(PV_EVENT_AXIS, event, &input);
		input.scroll = (int)event->dy;
		pv_app_event(&main_app, &input);
		break;
	case KL_WINDOW_AXIS_STOP:
		/* The touch pad's fingers lift: the view flies on. */
		pv_touch_pad_stop(&main_touch, event);
		break;
	case KL_WINDOW_KEY:
		/* The key, whether it went down, and whether it is a held key's repeat. */
		main_event(PV_EVENT_KEY, event, &input);
		input.key = event->code;
		input.pressed = event->pressed;
		input.repeat = event->repeated;
		pv_app_event(&main_app, &input);
		break;
	case KL_WINDOW_ACTION:
		/* An item of the menus or a control of the titlebar, in its place among the keys. */
		pv_log("ACTION id=%d action=%u", (int)event->id, (unsigned)event->code);
		main_event(PV_EVENT_ACTION, event, &input);
		input.action = event->code;
		pv_app_event(&main_app, &input);
		break;
	case KL_WINDOW_TOUCH_DOWN:
	case KL_WINDOW_TOUCH_MOTION:
	case KL_WINDOW_TOUCH_UP:
	case KL_WINDOW_TOUCH_CANCEL:
		pv_touch_event(&main_touch, &main_app, event);
		break;
	case KL_WINDOW_CONTROL_TEXT:
	case KL_WINDOW_CONTROL_DONE:
		/* The titlebar's find field's text (ws128-p004). */
		pv_titlebar_input(&main_titlebar, &main_app, event);
		break;
	case KL_WINDOW_RESIZE:
		main_resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		main_closed = 1;
		break;
	default:
		break;
	}
}

/* Fills a viewer's input of a kind with what every input carries: the pointer's place, the modifiers held and the time it was read. */
static void
main_event(
	enum pv_event_type type,
	const struct kl_window_event *event,
	struct pv_event *input)
{
	/* The modifiers are the same bits as the viewer's; the time is in milliseconds of the same clock. */
	memset(input, 0, sizeof(*input));
	input->type = type;
	input->x = (int)event->x;
	input->y = (int)event->y;
	input->modifiers = event->modifiers;
	input->time = event->arrival_us / 1000U;
}

/*
 * Hears the on-screen keyboard's inset (libkeiland's KL_VERSION 18): the
 * password card stays in the middle of the part of the window the keyboard
 * leaves.  The viewer has no text view whose caret the library would keep
 * in sight, so the default has nothing to do.
 */
static int
main_keyboard_inset(
	void *data,
	int right,
	int bottom,
	unsigned reason)
{
	struct pv_app *app;

	/* The part the keyboard covers, for the card's layout and the next frame. */
	app = data;
	app->keyboard_right = right;
	app->keyboard_bottom = bottom;
	app->dirty = 1;
	pv_log("KEYBOARD inset right=%d bottom=%d reason=%u", right, bottom, reason);

	/* Taken care of: the library's default is left out. */
	return 1;
}

/* Makes the frame's memory and canvas at the presenter's size; nonzero when memory runs out. */
static int
main_canvas_make(void)
{
	uint32_t *pixels;
	size_t count;

	/* The frame's memory. */
	count = (size_t)main_width * (size_t)main_height;
	pixels = malloc(count * sizeof(pixels[0]));
	if (pixels == NULL)
		return -1;
	free(main_pixels);
	main_pixels = pixels;

	/* The canvas over it. */
	main_canvas.pixels = main_pixels;
	main_canvas.stride = main_width;
	main_canvas.width = (int)main_width;
	main_canvas.height = (int)main_height;
	main_app.dirty = 1;

	/* Succeeded: frames can be drawn. */
	return 0;
}

/*
 * Shows libkeiland's file chooser when the viewer asks for it, at the folder
 * it names, and closes one it no longer waits for.
 */
static void
main_choose(void)
{
	struct kl_file_chooser_options options;
	static const struct kl_file_chooser_listener listener = {
		main_chosen
	};

	/* A chooser the viewer no longer waits for goes. */
	if (!main_app.choosing) {
		if (main_chooser != NULL) {
			kl_file_chooser_destroy(main_chooser);
			main_chooser = NULL;
		}

		/* Nothing to show. */
		return;
	}

	/* One already shown answers in its time. */
	if (main_chooser != NULL)
		return;

	/* Open, at the viewer's folder, with the PDF documents shown first, in the viewer's font. */
	memset(&options, 0, sizeof(options));
	options.mode = KL_FILE_CHOOSER_OPEN;
	options.application = MAIN_APPLICATION;
	options.folder = main_app.chooser_folder;
	options.filters = main_filters;
	options.filter_count = sizeof(main_filters) / sizeof(main_filters[0]);
	options.filter = 0;
	options.font = main_font;

	/* The chooser's window over the viewer's; without it the viewer stops waiting. */
	main_chooser = kl_file_chooser_open(kl_app_display(main_kl), kl_window_toplevel(main_window.kui), &options, &listener, &main_app);
	if (main_chooser == NULL) {
		pv_log("CHOOSER failed errno=%d", errno);
		pv_app_message(&main_app, "The file chooser could not be shown.", 4000U);
		pv_app_chosen(&main_app, NULL);
	}
}

/* The chooser answered: the path (empty when cancelled) goes to the viewer, and the chooser goes. */
static void
main_chosen(
	void *data,
	struct kl_file_chooser *chooser,
	unsigned result,
	const char *path,
	size_t filter)
{
	/* The answer, cancelled unless a file was chosen. */
	(void)filter;
	if (result != KL_FILE_CHOOSER_CHOSEN)
		path = NULL;
	pv_app_chosen(data, path);

	/* The chooser is spent. */
	kl_file_chooser_destroy(chooser);
	if (chooser == main_chooser)
		main_chooser = NULL;
}

/* Gathers what the menus and the titlebar show. */
static void
main_state(
	struct pv_state *state)
{
	/* A clean state, so that states compare by their bytes. */
	memset(state, 0, sizeof(*state));
	state->has_document = main_app.has_document;
	state->count = main_app.document.count;
	state->page = pv_app_current_page(&main_app);
	state->mode = (int)main_app.mode;
	state->fit = (int)main_app.fit;
	state->thumbnails = main_app.thumbnails;
	state->has_selection = main_app.has_selection;
}

/* After a document opened: the window's title names it, and it joins the recent files. */
static void
main_opened(void)
{
	char resolved[PATH_MAX];
	char title[PV_PATH_MAX + 32];
	const char *name;
	char *absolute;
	int error;

	/* Only once for each document opened. */
	if (!main_app.opened)
		return;
	main_app.opened = 0;

	/* The title: the file's name and the application's. */
	name = strrchr(main_app.document.path, '/');
	if (name == NULL) {
		name = main_app.document.path;
	} else {
		name++;
	}

	/* The window's title. */
	snprintf(title, sizeof(title), "%s \xe2\x80\x94 PDF Viewer", name);
	kl_window_set_title(main_window.kui, title);

	/* The recent files, by the absolute path. */
	absolute = realpath(main_app.document.path, resolved);
	if (absolute == NULL)
		return;
	error = kl_recent_add(resolved, MAIN_APPLICATION);
	if (error != 0)
		pv_log("RECENT failed errno=%d", error);
}

/* Starts Notes on the open document ("Annotate in Notes"). */
static void
main_annotate(void)
{
	char resolved[PATH_MAX];
	char *arguments[3];
	char *absolute;
	pid_t child;
	int usable;
	int error;

	/* Notes must be on the system. */
	usable = access(MAIN_NOTES, X_OK);
	if (usable != 0) {
		pv_app_message(&main_app, "Notes is not installed on this system.", 4000U);
		pv_log("ANNOTATE missing program=%s", MAIN_NOTES);
		return;
	}

	/* The document's absolute path, so that Notes finds it wherever it starts. */
	absolute = realpath(main_app.document.path, resolved);
	if (absolute == NULL)
		snprintf(resolved, sizeof(resolved), "%s", main_app.document.path);

	/* Starts Notes with the path. */
	arguments[0] = "notes";
	arguments[1] = resolved;
	arguments[2] = NULL;
	error = posix_spawn(&child, MAIN_NOTES, NULL, NULL, arguments, environ);
	if (error != 0) {
		pv_app_message(&main_app, "Notes could not be started.", 4000U);
		pv_log("ANNOTATE failed errno=%d", error);
		return;
	}

	/* Succeeded: Notes opens the document. */
	pv_log("ANNOTATE program=%s path=%s pid=%ld", MAIN_NOTES, resolved, (long)child);
}

/* Takes the desktop's new appearance: the viewer is drawn again in its colours. */
static void
main_appearance_changed(void)
{
	unsigned appearance;

	/* The colours, and a new frame. */
	appearance = kl_appearance_get(NULL);
	pv_draw_set_dark(appearance == KL_APPEARANCE_DARK);
	main_accent();
	main_app.dirty = 1;
	pv_log("APPEARANCE appearance=%u accent=%u", appearance, kl_accent_get());
}

/* Gives the frame the accent the user chose and its ink, as libkeiland's theme has them (ws179-p002). */
static void
main_accent(void)
{
	const struct kl_theme *theme;

	/* The theme's accent colours. */
	theme = kl_theme_default();
	pv_draw_set_accent(theme->accent, theme->accent_ink);
}

/*
 * Prints the document on the default printer (ws145-p007): the desktop is
 * given its file; the answer and the job's end come later as messages.
 */
static void
main_print(void)
{
	struct kl_system *system;
	unsigned capabilities;
	const char *title;
	const char *slash;
	int error;

	/* The desktop's printers. */
	system = kl_app_system(main_kl);
	capabilities = 0U;
	if (system != NULL)
		capabilities = kl_system_capabilities(system);
	if ((capabilities & KL_SYSTEM_HAS_PRINTERS) == 0U) {
		pv_app_message(&main_app, "This desktop cannot print.", 4000U);
		pv_log("PRINT none");
		return;
	}

	/* The document's file, under its name. */
	title = main_app.document.path;
	slash = strrchr(title, '/');
	if (slash != NULL)
		title = slash + 1;
	error = kl_system_printers_print(system, 0U, main_app.document.path, title, &main_print_request);
	pv_log("PRINT asked error=%d", error);
	main_print_job = 0U;
	if (error != 0) {
		main_print_request = 0U;
		pv_app_message(&main_app, "The document could not be printed.", 4000U);
		return;
	}

	/* Answered later. */
	pv_app_message(&main_app, "Sending to the printer...", 0U);
}

/* Follows the print under way: its answer, then its job to its end. */
static void
main_print_follow(void)
{
	struct kl_print_job jobs[KL_PRINT_JOBS_MAX];
	struct kl_system *system;
	uint32_t request;
	unsigned changed;
	size_t count;
	size_t index;
	int taken;
	int error;
	int found;

	/* Nothing under way. */
	if (main_print_request == 0U && main_print_job == 0U)
		return;
	system = kl_app_system(main_kl);
	if (system == NULL)
		return;
	changed = 0U;
	(void)kl_system_dispatch(system, &changed);

	/* The answer: refused (no printer), or the job. */
	for (;;) {
		taken = kl_system_take_result(system, &request, &error);
		if (!taken)
			break;
		if (request != main_print_request)
			continue;
		main_print_request = 0U;
		pv_log("PRINT result error=%d", error);
		if (error != 0) {
			pv_app_message(&main_app, "No printer took it. Add a printer in Settings.", 5000U);
			return;
		}

		/* Its job, followed from here. */
		found = kl_system_print_job_of(system, request, &main_print_job);
		if (!found)
			main_print_job = 0U;
	}

	/* The job's end. */
	if (main_print_job == 0U || (changed & KL_SYSTEM_CHANGED_PRINTERS) == 0U)
		return;
	count = kl_system_print_jobs_get(system, jobs, KL_PRINT_JOBS_MAX);
	for (index = 0; index < count; index++) {
		if (jobs[index].job != main_print_job || jobs[index].state < KL_PRINT_DONE)
			continue;
		pv_log("PRINT job=%u state=%u detail=%s", jobs[index].job, jobs[index].state, jobs[index].detail);
		main_print_job = 0U;
		if (jobs[index].state == KL_PRINT_DONE)
			pv_app_message(&main_app, "Printed.", 4000U);
		else if (jobs[index].state == KL_PRINT_CANCELLED)
			pv_app_message(&main_app, "The printing was cancelled.", 4000U);
		else
			pv_app_message(&main_app, "The printer could not print the document.", 5000U);
	}
}

/*
 * Starts the drag out of the window the view asked for (ws189-p003): the
 * selection's words as text, or the image under a press held still as a
 * picture (the page drawn within its corners, a PNG), with the picture
 * under the pointer.  The press is the drag's from here.
 */
static void
main_drag(void)
{
	struct kl_drag_data data;
	struct kl_drag_icon icon;
	unsigned char *png;
	uint32_t *pixels;
	char *text;
	size_t length;
	size_t size;
	uint32_t serial;
	int request;
	int width;
	int height;
	int error;

	/* What was asked, once. */
	request = main_app.drag_request;
	main_app.drag_request = PV_DRAG_NONE;
	serial = kl_window_press_serial(main_window.kui);

	/* The words: the selection's text, taken from the copy (not the clipboard). */
	if (request == PV_DRAG_TEXT) {
		pv_select_copy(&main_app);
		text = main_app.copy_text;
		length = main_app.copy_length;
		main_app.copy_text = NULL;
		if (text == NULL)
			return;
		error = kl_window_drag_text(main_window.kui, text, length, serial);
		free(text);
		pv_log("DND drag text bytes=%lu errno=%d", (unsigned long)length, error);
		return;
	}

	/* The image: the page drawn within its corners. */
	error = main_drag_image_pixels(&pixels, &width, &height);
	if (error != 0) {
		pv_log("DND drag image failed errno=%d", error);
		return;
	}

	/* The drag, the picture under the pointer at its middle; its PNG filled in after it starts. */
	data.type = "image/png";
	data.data = NULL;
	data.length = 0;
	icon.pixels = pixels;
	icon.width = width;
	icon.height = height;
	icon.hot_x = width / 2;
	icon.hot_y = height / 2;
	error = kl_window_start_drag_icon(main_window.kui, &data, 1U, KL_DND_COPY, serial, &icon);
	if (error != 0) {
		free(pixels);
		pv_log("DND drag image failed errno=%d", error);
		return;
	}

	/* The PNG. */
	png = NULL;
	size = 0;
	error = kl_picture_png(pixels, width, height, (size_t)width, &png, &size);
	if (error == 0)
		error = kl_window_drag_fill(main_window.kui, "image/png", png, size);
	free(png);
	free(pixels);

	/* The log line the tests read. */
	pv_log("DND drag image page=%lu size=%dx%d bytes=%lu errno=%d", (unsigned long)main_app.drag_page, width, height, (unsigned long)size, error);
}

/*
 * Draws the part of the page within the dragged image's corners on white:
 * at MAIN_DRAG_SCALE, its longer side at most MAIN_DRAG_SIDE.  Returns 0
 * with the pixels (the caller frees them), or an errno value.
 */
static int
main_drag_image_pixels(
	uint32_t **pixels,
	int *width,
	int *height)
{
	struct pdf_display_list *list;
	double left;
	double top;
	double right;
	double bottom;
	double scale;
	size_t count;
	size_t index;
	unsigned corner;
	int error;

	/* The corners' bounds on the page. */
	*pixels = NULL;
	left = main_app.drag_quad[0];
	right = left;
	top = main_app.drag_quad[1];
	bottom = top;
	for (corner = 1U; corner < 4U; corner++) {
		if (main_app.drag_quad[corner * 2U] < left)
			left = main_app.drag_quad[corner * 2U];
		if (main_app.drag_quad[corner * 2U] > right)
			right = main_app.drag_quad[corner * 2U];
		if (main_app.drag_quad[corner * 2U + 1U] < top)
			top = main_app.drag_quad[corner * 2U + 1U];
		if (main_app.drag_quad[corner * 2U + 1U] > bottom)
			bottom = main_app.drag_quad[corner * 2U + 1U];
	}

	/* An image too small to draw is none. */
	if (right - left < 1.0 || bottom - top < 1.0)
		return EINVAL;

	/* The scale: twice the points, the longer side within the limit. */
	scale = MAIN_DRAG_SCALE;
	if ((right - left) * scale > MAIN_DRAG_SIDE)
		scale = MAIN_DRAG_SIDE / (right - left);
	if ((bottom - top) * scale > MAIN_DRAG_SIDE)
		scale = MAIN_DRAG_SIDE / (bottom - top);
	*width = (int)((right - left) * scale + 0.5);
	*height = (int)((bottom - top) * scale + 0.5);
	if (*width < 1)
		*width = 1;
	if (*height < 1)
		*height = 1;

	/* White pixels to draw on. */
	count = (size_t)*width * (size_t)*height;
	*pixels = malloc(count * sizeof(**pixels));
	if (*pixels == NULL)
		return ENOMEM;

	/* Every pixel white, the page's paper. */
	for (index = 0; index < count; index++)
		(*pixels)[index] = 0xffffffffU;

	/* The page's drawing, moved so that the corners' top left is the pixels' (a page point p lands on p * scale + offset). */
	error = pdf_page_render(main_app.document.document, main_app.drag_page, &list);
	if (error == 0) {
		error = pdf_display_list_rasterize(list, *pixels, (size_t)*width, (size_t)*width, (size_t)*height, scale, -left * scale, -top * scale);
		pdf_display_list_destroy(list);
	}

	/* A page that cannot be drawn gives no image. */
	if (error != 0) {
		free(*pixels);
		*pixels = NULL;
		return error;
	}

	/* Succeeded: the image's pixels. */
	return 0;
}
