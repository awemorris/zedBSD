/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Text Editor (WS092): plain UTF-8 text in a Wayland window, drawn on the
 * CPU and shown with Vulkan (plan/ws092/design.md).
 *
 *   textedit [--display=NAME] [--font=PATH] [--fallback-font=PATH]
 *            [--ui-font=PATH] [--width=N] [--height=N] [--timeout-s=N]
 *            [FILE]
 *
 * The file is opened from the command line (a path that does not exist is
 * made by the first save), or with File > Open.  The outcome is one line
 * on standard error: TEXTEDIT DONE with the reason, or TEXTEDIT FAILED
 * naming what failed; TEXTEDIT READY says the first frame is shown and the
 * window has the keyboard (or MAIN_READY_WAIT_MS passed without it), and
 * TEXTEDIT OPEN and TEXTEDIT SAVE name the files read and written.
 */

#include "window.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The fonts used unless told otherwise: the text's (monospaced), the characters it lacks, and the interface's. */
#define MAIN_FONT		KEILAND_DATADIR "/fonts/keiland-mono.ttf"
#define MAIN_FALLBACK_FONT	KEILAND_DATADIR "/fonts/keiland-fallback.ttf"
#define MAIN_UI_FONT		KEILAND_DATADIR "/fonts/keiland.ttf"

/* How often frames are drawn while the fingers or the view's scroll move, in milliseconds. */
#define MAIN_FRAME_MS		16

/* The text view's id among the parts the fingers' input records, and the dialog's (libkeiland's kl_dialog). */
#define MAIN_TEXT_REGION	1U
#define MAIN_DIALOG		2U

/* The Replace panel's widgets (ws128-p003): its two fields and its three buttons. */
#define MAIN_REPLACE_FIND	3U
#define MAIN_REPLACE_WITH	4U
#define MAIN_REPLACE_ONE	5U
#define MAIN_REPLACE_ALL	6U
#define MAIN_REPLACE_DONE	7U

/* The Find panel's widgets (BUG-248): its field and its three buttons. */
#define MAIN_FIND_TEXT		8U
#define MAIN_FIND_NEXT		9U
#define MAIN_FIND_PREVIOUS	10U
#define MAIN_FIND_DONE		11U

/* The bar of editing buttons over the fingers' selection (ws190-p003). */
#define MAIN_TEXT_BAR		12U

/* The Replace panel's size, its distance from the card's top, a field's and a button's height, and the space between rows. */
#define MAIN_REPLACE_WIDTH	560
#define MAIN_REPLACE_HEIGHT	232
#define MAIN_REPLACE_TOP	28
#define MAIN_REPLACE_ROW	36
#define MAIN_REPLACE_GAP	12

/* The Find panel's height (one row of a field fewer than the Replace panel's). */
#define MAIN_FIND_HEIGHT	(MAIN_REPLACE_HEIGHT - MAIN_REPLACE_ROW - MAIN_REPLACE_GAP)

/* The evdev code of A: Edit > Select All (Ctrl+A, the menu's) selects the Find or Replace panel's field instead of the text. */
#define MAIN_KEY_A		30U

/* How far above the card's bottom a message's chip stands. */
#define MAIN_CHIP_BOTTOM	42

/* How many frames in a row may find the swapchain out of date before the program gives up. */
#define MAIN_STALE_LIMIT	8U

/* The longest the loop sleeps when nothing is due, in milliseconds. */
#define MAIN_IDLE_MS		1000

/*
 * How long READY waits for the keyboard after the first frame
 * (q807-i02): a key typed before the compositor gives the window the keyboard
 * goes nowhere, so READY says the window takes keys, or that this passed.
 */
#define MAIN_READY_WAIT_MS	2000U

/* The application's identity in the compositor and the recent files. */
#define MAIN_APPLICATION	"textedit"

/* The longest window title. */
#define MAIN_TITLE_MAX		(TE_PATH_MAX + 64)

/*
 * What the command line asked for.
 */
struct main_options {
	const char *display;
	const char *font;
	const char *fallback;
	const char *ui_font;
	const char *file;
	unsigned width;
	unsigned height;
	unsigned timeout;
};

/*
 * The program's parts, for the whole run.  They are file-scope because
 * the window's input queue and the editor are too large for the stack.
 *
 * The window: libkeiland's window (the Wayland connection and surface, the
 * Vulkan presenter, the clipboard and the primary selection) and the
 * editor's queue of inputs, from the start of the run to its end.
 */
static struct te_window main_window;

/* The size frames are drawn at (the presenter's), set when the window opens and when it changes size. */
static uint32_t main_width;
static uint32_t main_height;

/*
 * The fingers' input (libkeiland): which part of the window a finger meant,
 * the text view's touch (one finger selects, two scroll) and the scroll.
 * Made with the window, destroyed before it.
 */
static struct kl_ui *main_input;

/* Whether the fingers or the view's scroll still move (the loop draws the next frame soon). */
static int main_moving;

/*
 * The Find and Replace panels' fields (BUG-248, ws128-p003): the text to
 * find (both panels') and its replacement, filled from the editor each
 * time a panel opens (the editor's panel_fresh) and kept while it is open.
 */
static struct kl_field main_find_field;
static struct kl_field main_with_field;

/* Whether the compositor asked to close the window or gave it a new size, since the loop last looked. */
static int main_closed;
static int main_resized;

/* Whether the window has had the keyboard since it opened (READY waits for it, MAIN_READY_WAIT_MS). */
static int main_focus_came;

/* The editor: the document and the view, made once the swapchain's size is known. */
static struct te_app main_app;

/* The text's font (monospaced) and its fallback, open for the whole run (without them the text has no words). */
static struct te_text main_body;

/* The interface's font and its fallback, open for the whole run (the chips, dialogs and messages). */
static struct te_text main_ui;

/*
 * The window's menus in the compositor, opened with the window and closed
 * before it; absent with a compositor without them.
 */
static struct te_menu main_menu;

/* The window's glass, when the compositor has glass and the swapchain is see-through. */
static struct te_glass main_glass;

/*
 * The frame being drawn: ordinary memory the size of the swapchain, remade
 * (and the canvas with it) when the window changes size.
 */
static uint32_t *main_pixels;

/* The canvas over main_pixels, which the editor draws each frame into. */
static struct te_canvas main_canvas;

/* libkeiland's canvas over the same pixels, which the text view's handles, a message's chip and a dialog are drawn with (made with main_canvas). */
static struct kl_canvas main_handles;
static int main_handles_made;

/* The interface's font as libkeiland's text, for the chip and the dialog (open when main_widgets_text is 1). */
static struct kl_text main_widgets;
static int main_widgets_text;

/* The title the window shows now, to set it again only when it changes. */
static char main_title[MAIN_TITLE_MAX];

/*
 * The file chooser open for Open or Save As (libkeiland's), or NULL.  It
 * is destroyed when it answers, and by the main loop when the editor stops
 * waiting for it (Quit while it is open).
 */
static struct kl_file_chooser *main_chooser;

/*
 * The application (WS131 p016, libkeiland's kl_app): the connection, its
 * one queue of inputs (the window's, the actions of its menus and
 * controls), and the desktop's appearance, which the editor draws in
 * (draw.c, ws089-p017).
 */
static struct kl_app *main_kl;

/* The interface's font, which the chooser draws its words with too. */
static const char *main_ui_font;

/*
 * The filters the chooser offers: the kinds of files that are plain text,
 * and every file.
 */
static const struct kl_file_filter main_filters[] = {
	{ "Text Files", "txt text md markdown rst c h cc cpp hpp py sh mk conf cfg ini json xml html css js log csv tsv yaml yml toml" },
	{ "All Files", NULL }
};

static int main_parse(int argc, char **argv, struct main_options *options);
static const char *main_value(const char *argument, const char *name);
static int main_number(const char *text, unsigned maximum, unsigned *value);
static int main_loop(const struct main_options *options);
static int main_frame(void);
static void main_overlay(uint64_t now_us);
static int main_canvas_make(void);
static void main_state(struct te_state *state);
static void main_title_refresh(void);
static void main_edit_state(const struct te_state *state);
static void main_opened(void);
static void main_recent_refresh(void);
static void main_recent_follow(void);
static void main_replace_panel(uint64_t now_us, const struct kl_style *style, const struct kl_rect *area);
static void main_find_panel(uint64_t now_us, const struct kl_style *style, const struct kl_rect *area);
static void main_panel_find_text(char *text, size_t size);
static void main_host(struct te_app *app);
static void main_copy(void *data, const char *text, size_t length);
static size_t main_paste(void *data, char *text, size_t size);
static void main_select(void *data, const char *text, size_t length);
static size_t main_paste_primary(void *data, char *text, size_t size);
static void main_context_menu(void *data, int x, int y);
static int main_choose(void *data, int saving, const char *folder, const char *name);
static int main_drag_text(void *data, const char *text, size_t length);
static void main_drop_event(const struct kl_window_event *event);
static void main_window_event(const struct kl_window_event *event);
static void main_fingers(uint64_t now_us);
static unsigned main_bar(void);
static void main_bar_press(unsigned button);
static void main_bar_log(void);
static int main_dialog_event(const struct kl_window_event *event);
static void main_text_caret(void);
static int main_resize(void);
static void main_chosen(void *data, struct kl_file_chooser *chooser, unsigned result, const char *path, size_t filter);
static void main_appearance_changed(void);
static void main_action(const struct kl_window_event *event);

/*
 * Runs Text Editor.
 */
int
main(
	int argc,
	char **argv)
{
	struct main_options options;
	struct kl_app_options app_options;
	struct kl_window_options window_options;
	struct te_state state;
	int status;
	int error;

	/* The command line. */
	status = main_parse(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: textedit [--display=NAME] [--font=PATH] [--fallback-font=PATH] [--ui-font=PATH] [--width=N] [--height=N] [--timeout-s=N] [FILE]\n");
		return 2;
	}

	/* The fonts; without them the editor shows no words. */
	error = te_text_open(&main_body, options.font, options.fallback);
	if (error != 0)
		te_log("FONT missing path=%s error=%d", options.font, error);
	error = te_text_open(&main_ui, options.ui_font, options.fallback);
	if (error != 0)
		te_log("FONT missing path=%s error=%d", options.ui_font, error);

	/* The chip and the dialog draw with it too, as libkeiland's text. */
	error = kl_text_open(&main_widgets, options.ui_font, options.fallback);
	if (error == 0)
		main_widgets_text = 1;

	/* The chooser draws with the interface's font. */
	main_ui_font = options.ui_font;

	/* The application: the connection to the compositor. */
	memset(&app_options, 0, sizeof(app_options));
	app_options.display = options.display;
	app_options.application = MAIN_APPLICATION;
	main_kl = kl_app_open(&app_options);
	if (main_kl == NULL) {
		fprintf(stderr, "TEXTEDIT FAILED operation=app error=%d\n", errno);
		te_text_close(&main_ui);
		te_text_close(&main_body);
		return 1;
	}

	/* The window, its frames shown with Vulkan. */
	memset(&window_options, 0, sizeof(window_options));
	window_options.title = "Text Editor";
	window_options.width = options.width;
	window_options.height = options.height;
	window_options.present = KL_PRESENT_VULKAN;
	main_window.kui = kl_app_window_create(main_kl, &window_options);
	if (main_window.kui == NULL) {
		fprintf(stderr, "TEXTEDIT FAILED operation=window error=%d\n", errno);
		kl_app_close(main_kl);
		te_text_close(&main_ui);
		te_text_close(&main_body);
		return 1;
	}

	/* The presenter's size, which the frames are drawn at. */
	error = kl_window_present_resize(main_window.kui, &main_width, &main_height);
	if (error != 0) {
		fprintf(stderr, "TEXTEDIT FAILED operation=present error=%d\n", error);
		kl_window_close(main_window.kui);
		kl_app_close(main_kl);
		te_text_close(&main_ui);
		te_text_close(&main_body);
		return 1;
	}

	/* Text dragged from other windows is taken (ws189-p003). */
	error = kl_window_accept_drops(main_window.kui, KL_DROP_TEXT);
	if (error != 0)
		te_log("DND none errno=%d", error);

	/* The editor at that size, with the file when one was given, and the window's services. */
	te_app_init(&main_app, &main_body, &main_ui, (int)main_width, (int)main_height);
	main_host(&main_app);
	main_app.glass = te_glass_open(&main_glass, &main_window, &main_app, kl_window_see_through(main_window.kui));

	/* The file given on the command line. */
	if (options.file != NULL)
		(void)te_app_open(&main_app, options.file);

	/* The fingers' input; without memory for it the fingers do nothing. */
	main_input = kl_ui_create();
	if (main_input == NULL)
		te_log("TOUCH failed errno=%d", ENOMEM);

	/*
	 * The menus, shown in the titlebar's menu bar (BUG-248: the window
	 * gives no titlebar controls, so the compositor shows its menu); a window
	 * without them goes on with its keys.
	 */
	main_state(&state);
	error = te_menu_open(&main_menu, &main_window, &state);
	if (error != 0) {
		te_log("MENU failed errno=%d", error);
		te_menu_close(&main_menu);
	}

	/* File > Open Recent's files. */
	main_recent_refresh();

	/* The loop, until the window closes. */
	status = main_loop(&options);

	/* Everything goes, the chooser, the menus and the editor before the window they belong to. */
	kl_file_chooser_destroy(main_chooser);
	main_chooser = NULL;
	te_menu_close(&main_menu);
	kl_ui_destroy(main_input);
	te_glass_close(&main_glass);
	te_app_release(&main_app);
	if (main_handles_made)
		kl_canvas_release(&main_handles);
	free(main_pixels);
	kl_window_close(main_window.kui);
	kl_app_close(main_kl);
	te_text_close(&main_ui);
	te_text_close(&main_body);
	if (main_widgets_text)
		kl_text_close(&main_widgets);

	/* Reports how the run ended. */
	if (status != 0)
		return 1;

	/* Succeeded: the window was closed. */
	return 0;
}

/*
 * Writes one line of the log: "TEXTEDIT " and the words, on standard error.
 */
void
te_log(
	const char *format,
	...)
{
	va_list arguments;

	/* The line. */
	va_start(arguments, format);
	fputs("TEXTEDIT ", stderr);
	vfprintf(stderr, format, arguments);
	fputc('\n', stderr);
	va_end(arguments);
}

/*
 * Reports the monotonic clock in milliseconds.
 */
uint64_t
te_clock(void)
{
	struct timespec now;

	/* The monotonic clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Reports it in milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
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

	/* The defaults. */
	memset(options, 0, sizeof(*options));
	options->font = MAIN_FONT;
	options->fallback = MAIN_FALLBACK_FONT;
	options->ui_font = MAIN_UI_FONT;
	options->width = TE_WIDTH;
	options->height = TE_HEIGHT;

	/* Each argument. */
	for (index = 1; index < argc; index++) {
		/* The compositor's display. */
		value = main_value(argv[index], "--display=");
		if (value != NULL) {
			options->display = value;
			continue;
		}

		/* The text's font. */
		value = main_value(argv[index], "--font=");
		if (value != NULL) {
			options->font = value;
			continue;
		}

		/* The font of the characters the others lack. */
		value = main_value(argv[index], "--fallback-font=");
		if (value != NULL) {
			options->fallback = value;
			continue;
		}

		/* The interface's font. */
		value = main_value(argv[index], "--ui-font=");
		if (value != NULL) {
			options->ui_font = value;
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
	struct te_event event;
	struct te_state state;
	uint64_t ready_due;
	uint64_t started;
	uint64_t now;
	int ready_told;
	int taken;
	int status;
	int timeout;
	int due;

	/* The first frame's canvas and the first frame. */
	status = main_canvas_make();
	if (status != 0) {
		fprintf(stderr, "TEXTEDIT FAILED operation=canvas\n");
		return -1;
	}

	/*
	 * The input method's text, asked for before the window has the
	 * keyboard, so that the text input is on as soon as it comes (the loop
	 * asks again each round, under a dialog or the chooser not).
	 */
	kl_window_text_input(main_window.kui, 1);

	/* The file given on the command line, the title, and the first frame; READY waits for the keyboard. */
	main_app.now = te_clock();
	main_opened();
	main_title_refresh();
	status = main_frame();
	if (status != 0)
		return -1;
	ready_told = 0;
	ready_due = te_clock() + MAIN_READY_WAIT_MS;

	/* Each round: input, time, and a frame when something changed. */
	started = te_clock();
	for (;;) {
		/* Waits for the compositor, or until something is due (the fingers and the view's scroll soon while they move). */
		now = te_clock();
		timeout = MAIN_IDLE_MS;
		due = te_app_tick(&main_app, now);
		if (due >= 0 && due < timeout)
			timeout = due;
		if (main_moving && timeout > MAIN_FRAME_MS)
			timeout = MAIN_FRAME_MS;
		if (main_app.dirty)
			timeout = 0;

		/* Until READY is told, no later than its time without the keyboard. */
		if (!ready_told) {
			due = 0;
			if (ready_due > now)
				due = (int)(ready_due - now);
			if (due < timeout)
				timeout = due;
		}

		/* Waits (a key held repeats within, after its release if that came, BUG-111); a lost connection ends the run. */
		status = kl_app_dispatch(main_kl, timeout);
		if (status != 0) {
			te_log("DONE reason=disconnected");
			return 0;
		}
		now = te_clock();
		main_app.now = now;

		/* The application's input: the window's (the menus' and the controls' among it), and the desktop's appearance. */
		for (;;) {
			taken = kl_app_take(main_kl, &app_event);
			if (taken == 0)
				break;
			if (app_event.kind == KL_APP_THEME)
				main_appearance_changed();
			if (app_event.kind == KL_APP_WINDOW && app_event.window == main_window.kui)
				main_window_event(&app_event.input);
		}

		/* READY once the window has the keyboard (keys typed from now reach it), or once its time passed without it. */
		if (!ready_told) {
			if (main_focus_came) {
				ready_told = 1;
			} else if (now >= ready_due) {
				ready_told = 1;
			}
			if (ready_told)
				te_log("READY width=%u height=%u lines=%lu focus=%d", main_width, main_height, (unsigned long)main_app.buffer.line_count, main_focus_came);
		}

		/* Every input queued (the menus' among them). */
		for (;;) {
			taken = te_window_take(&main_window, &event);
			if (taken == 0)
				break;
			te_app_event(&main_app, &event);
		}

		/* The text input is asked for where the body's text is edited: not while the chooser is open (a dialog's fields ask after its frame). */
		if (main_app.dialog == TE_DIALOG_NONE)
			kl_window_text_input(main_window.kui, !main_app.choosing);

		/* The fingers at this time: their selection, taps and menus, and the view's scroll. */
		main_fingers(kl_clock_us());

		/* A chooser the editor no longer waits for (Quit came meanwhile) closes. */
		if (main_chooser != NULL && !main_app.choosing) {
			kl_file_chooser_destroy(main_chooser);
			main_chooser = NULL;
		}

		/* The close button asks like File > Close (unsaved changes are asked about). */
		if (main_closed != 0) {
			main_closed = 0;
			te_app_action(&main_app, TE_ACTION_CLOSE);
		}

		/* A selection made becomes the primary one; a file opened joins the recent files; the title follows. */
		te_app_publish_primary(&main_app);
		main_opened();
		main_title_refresh();

		/* Time passes for the editor; the menus show its state. */
		(void)te_app_tick(&main_app, now);
		main_state(&state);
		main_edit_state(&state);
		te_menu_refresh(&main_menu, &state);

		/* Close (with nothing unsaved, or dropped) ends the run. */
		if (main_app.want_close != 0) {
			te_log("DONE reason=close");
			return 0;
		}

		/* So does the timeout, when one was given. */
		if (options->timeout != 0U && now - started >= (uint64_t)options->timeout * 1000U) {
			te_log("DONE reason=timeout");
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
	struct kl_style style;
	struct kl_rect clip;
	struct te_rect text;
	unsigned stale;
	int status;

	/* Tries until the frame is shown, remaking a stale swapchain a few times. */
	for (stale = 0; stale < MAIN_STALE_LIMIT; stale++) {
		/* The frame on the CPU, and the fingers' handles over the text (within it, and a knob's size around it). */
		te_draw(&main_app, &main_canvas);
		te_app_text_rect(&main_app, &text);
		clip.x = text.x - KL_TEXT_HANDLE;
		clip.y = text.y;
		clip.width = text.width + 2 * KL_TEXT_HANDLE;
		clip.height = text.height + KL_TEXT_HANDLE;
		kl_canvas_clip_push(&main_handles, &clip);
		kl_text_touch_draw_handles(&main_app.touch, &main_handles, (double)text.x - main_app.scroll_x, (double)text.y - main_app.scroll_y, kl_theme_default());
		kl_canvas_clip_pop(&main_handles);

		/* The bar of editing buttons over the selection, laid out by the fingers' input (ws190-p003). */
		if (main_app.bar.count != 0U && main_widgets_text) {
			style.canvas = &main_handles;
			style.text = &main_widgets;
			style.theme = kl_theme_default();
			style.glass = main_app.glass;
			kl_text_bar_draw(&main_app.bar, &style, main_app.bar_held);
		}

		/* A message's chip and a dialog over it all. */
		main_overlay(kl_clock_us());

		/* Its glass card, and the frame shown in the window. */
		te_glass_refresh(&main_glass, &main_app);
		status = kl_window_present(main_window.kui, main_pixels, (size_t)main_width);
		if (status == 0) {
			main_text_caret();
			return 0;
		}

		/* Anything but a stale swapchain is a failure. */
		if (status != EAGAIN) {
			fprintf(stderr, "TEXTEDIT FAILED operation=present error=%d\n", status);
			return -1;
		}

		/* A stale swapchain is remade at the window's size, with a canvas to match. */
		status = main_resize();
		if (status != 0)
			return -1;
	}

	/* The swapchain stayed out of date. */
	fprintf(stderr, "TEXTEDIT FAILED operation=stale-swapchain\n");
	return -1;
}

/* Draws a message's chip and the dialog shown (libkeiland's), and carries out the dialog's answer. */
static void
main_overlay(
	uint64_t now_us)
{
	struct kl_style style;
	struct kl_event event;
	struct kl_rect area;
	struct te_rect card;
	const char *const *labels;
	const char *words;
	char title[TE_PATH_MAX + 64];
	int answer;
	int count;
	int taken;

	/* Without the interface's text, the words cannot be drawn. */
	if (!main_widgets_text)
		return;

	/* The widgets draw over the frame, on the window's card. */
	style.canvas = &main_handles;
	style.text = &main_widgets;
	style.theme = kl_theme_default();
	style.glass = main_app.glass;
	te_app_card(&main_app, &card);
	area.x = card.x;
	area.y = card.y;
	area.width = card.width;
	area.height = card.height;

	/* A message, at the bottom middle of the card. */
	if (main_app.message[0] != '\0')
		kl_chip(&style, card.x + card.width / 2, card.y + card.height - MAIN_CHIP_BOTTOM, main_app.message);

	/* The dialog, a frame of the fingers' and the pointer's input of its own. */
	if (main_app.dialog == TE_DIALOG_NONE || main_input == NULL)
		return;

	/* Edit > Replace's panel is a dialog of fields (ws128-p003). */
	if (main_app.dialog == TE_DIALOG_REPLACE) {
		main_replace_panel(now_us, &style, &area);
		return;
	}

	/* Edit > Find's panel too (BUG-248). */
	if (main_app.dialog == TE_DIALOG_FIND) {
		main_find_panel(now_us, &style, &area);
		return;
	}

	/* Any other dialog: its words and buttons (libkeiland's dialog). */
	te_app_dialog_words(&main_app, title, sizeof(title), &words, &labels, &count);
	kl_ui_begin(main_input, now_us);
	answer = kl_dialog(main_input, &style, MAIN_DIALOG, &area, title, words, labels, count);
	main_moving = kl_ui_end(main_input, now_us);

	/* What no part took under a dialog is nothing. */
	for (;;) {
		taken = kl_ui_take(main_input, &event);
		if (taken == 0)
			break;
	}

	/* The answer: the dialog closes and its button is carried out (the next frame shows it). */
	if (answer >= 0)
		te_app_dialog_choose(&main_app, answer);
}

/* Remakes the presenter at the window's size, with a canvas to match; nonzero when it cannot. */
static int
main_resize(void)
{
	int status;

	/* The presenter at the window's size. */
	status = kl_window_present_resize(main_window.kui, &main_width, &main_height);
	if (status != 0) {
		fprintf(stderr, "TEXTEDIT FAILED operation=present error=%d\n", status);
		return -1;
	}

	/* A canvas of the presenter's size. */
	status = main_canvas_make();
	if (status != 0)
		return -1;
	te_app_resize(&main_app, (int)main_width, (int)main_height);

	/* Succeeded: frames are drawn at the new size. */
	return 0;
}

/* Makes the frame's memory and canvas at the swapchain's size; nonzero when memory runs out. */
static int
main_canvas_make(void)
{
	uint32_t *pixels;
	size_t count;
	int status;

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
	te_canvas_unclip(&main_canvas);
	main_app.dirty = 1;

	/* libkeiland's canvas over the same pixels, for the handles. */
	if (main_handles_made)
		kl_canvas_release(&main_handles);
	main_handles_made = 0;
	status = kl_canvas_init(&main_handles, main_pixels, (size_t)main_width, (int)main_width, (int)main_height);
	if (status != 0)
		return -1;
	main_handles_made = 1;

	/* Succeeded: frames can be drawn. */
	return 0;
}

/* Gathers what the menus show. */
static void
main_state(
	struct te_state *state)
{
	size_t start;
	size_t end;

	/* A clean state, so that states compare by their bytes. */
	memset(state, 0, sizeof(*state));
	state->can_undo = te_undo_can_undo(&main_app.undo);
	state->can_redo = te_undo_can_redo(&main_app.undo);
	te_edit_selection(&main_app, &start, &end);
	state->selected = 0;
	if (end > start)
		state->selected = 1;
	state->modified = te_app_modified(&main_app);
	state->line_numbers = main_app.line_numbers;
	state->wrap = main_app.wrap;
}

/*
 * Tells the window's editing state -- a selection, something to paste,
 * something to undo or redo -- which the on-screen keyboard's editing
 * buttons follow (KL_VERSION 18, ws102-p023); the library sends it only
 * when it changed.
 */
static void
main_edit_state(
	const struct te_state *state)
{
	unsigned flags;
	int paste;

	/* The state's bits. */
	flags = 0U;
	if (state->selected)
		flags |= KL_EDIT_HAS_SELECTION;
	if (state->can_undo)
		flags |= KL_EDIT_CAN_UNDO;
	if (state->can_redo)
		flags |= KL_EDIT_CAN_REDO;
	paste = kl_window_can_paste(main_window.kui);
	if (paste)
		flags |= KL_EDIT_CAN_PASTE;

	/* Succeeded: the window tells it before its next wait. */
	kl_window_edit_state(main_window.kui, flags);
}

/* Sets the window's title when it changed: "• " for unsaved changes, the document's name and the application's. */
static void
main_title_refresh(void)
{
	char title[MAIN_TITLE_MAX];
	const char *mark;
	int modified;
	int same;

	/* The title as it should be. */
	modified = te_app_modified(&main_app);
	mark = "";
	if (modified)
		mark = "\xe2\x80\xa2 ";
	snprintf(title, sizeof(title), "%s%s \xe2\x80\x94 Text Editor", mark, te_app_name(&main_app));

	/* Sent only when it differs. */
	same = strcmp(title, main_title);
	if (same == 0)
		return;
	snprintf(main_title, sizeof(main_title), "%s", title);
	kl_window_set_title(main_window.kui, main_title);
	te_log("TITLE %s", main_title);
}

/* After a file was opened or saved: it joins the recent files. */
static void
main_opened(void)
{
	char resolved[PATH_MAX];
	char *absolute;
	int error;

	/* Only once for each file. */
	if (!main_app.opened)
		return;
	main_app.opened = 0;

	/* The recent files, by the absolute path. */
	absolute = realpath(main_app.path, resolved);
	if (absolute == NULL)
		return;
	error = kl_recent_add(resolved, MAIN_APPLICATION);
	if (error != 0)
		te_log("RECENT failed errno=%d", error);

	/* File > Open Recent shows it. */
	main_recent_refresh();
}

/*
 * Reads File > Open Recent again when another program changed the recent
 * list since it was read (ws177-p008: emptied by Files, stopped by
 * Settings), as the window gets the keyboard.
 */
static void
main_recent_follow(void)
{
	uint64_t stamp;
	int error;

	/* The list's stamp now; one that cannot be read changes nothing. */
	error = kl_recent_stamp(&stamp);
	if (error != 0)
		return;

	/* The same list: the menu is current. */
	if (stamp == main_app.recent_stamp)
		return;

	/* Read again (logged for the tests). */
	te_log("RECENT changed: read again");
	main_recent_refresh();
}

/*
 * Reads the files Text Editor used from libkeiland's recent list, newest
 * first and at most TE_RECENT_MAX (ws128-p003), notes which are still
 * there, and shows them in File > Open Recent.
 */
static void
main_recent_refresh(void)
{
	static struct kl_recent_item items[64];
	size_t count;
	size_t index;
	size_t kept;
	int differs;
	int error;
	int status;

	/* The list's stamp, then the list (an unreadable one shows no file). */
	(void)kl_recent_stamp(&main_app.recent_stamp);
	count = 0;
	error = kl_recent_list(items, sizeof(items) / sizeof(items[0]), &count);
	if (error != 0) {
		te_log("RECENT list failed errno=%d", error);
		count = 0;
	}

	/* The editor's own files, newest first. */
	kept = 0;
	for (index = 0; index < count && kept < TE_RECENT_MAX; index++) {
		differs = strcmp(items[index].application, MAIN_APPLICATION);
		if (differs != 0)
			continue;
		snprintf(main_app.recent[kept], sizeof(main_app.recent[kept]), "%s", items[index].path);
		status = access(items[index].path, R_OK);
		main_app.recent_present[kept] = 0;
		if (status == 0)
			main_app.recent_present[kept] = 1;
		kept++;
	}

	/* How many the editor keeps. */
	main_app.recent_count = kept;

	/* The menu shows them. */
	te_menu_recent(&main_menu, &main_app);
}

/*
 * Draws Edit > Replace's panel over the card and carries out what was done
 * with it (ws128-p003): the text to find and its replacement, Replace (or
 * Enter in the replacement) replaces the place found and selects the next,
 * Replace All replaces every place as one undo step, Enter in the text to
 * find finds the next place, and Done or Esc closes the panel.
 */
static void
main_replace_panel(
	uint64_t now_us,
	const struct kl_style *style,
	const struct kl_rect *area)
{
	struct kl_event event;
	struct kl_rect panel;
	struct kl_rect rect;
	char selected[TE_FIND_MAX];
	unsigned find_flags;
	unsigned with_flags;
	int replace_one;
	int replace_all;
	int done;
	int width;
	int top;
	int taken;

	/* Opened just now: the text to find is the selection (a short one on one line) or the last, the replacement the last. */
	if (main_app.panel_fresh) {
		main_app.panel_fresh = 0;
		main_panel_find_text(selected, sizeof(selected));

		/* The fields, and the keyboard for the replacement (for the text to find when there is none yet). */
		kl_field_set(&main_find_field, selected);
		kl_field_set(&main_with_field, main_app.replace_with);
		if (main_find_field.length == 0U) {
			kl_ui_set_focus(main_input, MAIN_REPLACE_FIND, 0);
		} else {
			kl_ui_set_focus(main_input, MAIN_REPLACE_WITH, 0);
		}

		/* The log line the tests read. */
		te_log("REPLACE open find=%s", main_find_field.text);
	}

	/* The panel near the card's top, in its middle, narrower in a narrow window. */
	width = MAIN_REPLACE_WIDTH;
	if (width > area->width - 32)
		width = area->width - 32;
	panel.x = area->x + (area->width - width) / 2;
	panel.y = area->y + MAIN_REPLACE_TOP;
	panel.width = width;
	panel.height = MAIN_REPLACE_HEIGHT;

	/* The frame of input of its own: the panel, the two fields and the buttons. */
	kl_ui_begin(main_input, now_us);
	kl_panel(style, &panel, 0);
	top = kl_card(style, &panel, "Replace", "The next place found is selected; Replace All can be undone at once.");
	rect.x = panel.x + 20;
	rect.width = panel.width - 40;
	rect.height = MAIN_REPLACE_ROW;
	rect.y = top;
	find_flags = kl_field(main_input, style, MAIN_REPLACE_FIND, &rect, &main_find_field, "Find");
	rect.y = top + MAIN_REPLACE_ROW + MAIN_REPLACE_GAP;
	with_flags = kl_field(main_input, style, MAIN_REPLACE_WITH, &rect, &main_with_field, "Replace with");

	/* The buttons at the bottom right: Replace (the default), Replace All, Done. */
	rect.y = top + 2 * (MAIN_REPLACE_ROW + MAIN_REPLACE_GAP) + 4;
	rect.width = kl_button_width(style, "Replace");
	rect.x = panel.x + panel.width - 20 - rect.width;
	replace_one = kl_button(main_input, style, MAIN_REPLACE_ONE, &rect, "Replace", KL_BUTTON_PRIMARY);
	rect.width = kl_button_width(style, "Replace All");
	rect.x -= rect.width + 10;
	replace_all = kl_button(main_input, style, MAIN_REPLACE_ALL, &rect, "Replace All", 0U);
	rect.width = kl_button_width(style, "Done");
	rect.x -= rect.width + 10;
	done = kl_button(main_input, style, MAIN_REPLACE_DONE, &rect, "Done", 0U);
	main_moving = kl_ui_end(main_input, now_us);

	/* A key no widget took: Esc closes the panel, anything else is nothing. */
	for (;;) {
		taken = kl_ui_take(main_input, &event);
		if (taken == 0)
			break;

		/* Esc. */
		if (event.kind == KL_EVENT_KEY && event.code == KL_KEY_ESC)
			done = 1;
	}

	/* Esc in either field closes the panel too. */
	if ((find_flags & KL_FIELD_CANCELLED) != 0U || (with_flags & KL_FIELD_CANCELLED) != 0U)
		done = 1;

	/* Enter in the text to find finds the next place, without replacing. */
	if ((find_flags & KL_FIELD_SUBMITTED) != 0U) {
		snprintf(main_app.find, sizeof(main_app.find), "%s", main_find_field.text);
		main_app.find_length = strlen(main_app.find);
		te_edit_find(&main_app, 1, 0);
	}

	/* Enter in the replacement is Replace. */
	if ((with_flags & KL_FIELD_SUBMITTED) != 0U)
		replace_one = 1;

	/* What the buttons and Enter asked for, then the frame shows it. */
	if (replace_one)
		te_app_replace(&main_app, main_find_field.text, main_with_field.text, 0);
	if (replace_all)
		te_app_replace(&main_app, main_find_field.text, main_with_field.text, 1);
	if (done)
		te_app_replace_close(&main_app);
	main_app.dirty = 1;
}

/*
 * Draws Edit > Find's panel over the card and carries out what was done
 * with it (BUG-248, in place of the titlebar's find field): the text to
 * find is found as it is typed, Next (or Enter) and Previous go on to the
 * next and the one before, and Done or Esc closes the panel with the place
 * found selected.
 */
static void
main_find_panel(
	uint64_t now_us,
	const struct kl_style *style,
	const struct kl_rect *area)
{
	struct kl_event event;
	struct kl_rect panel;
	struct kl_rect rect;
	char selected[TE_FIND_MAX];
	unsigned find_flags;
	int next;
	int previous;
	int done;
	int width;
	int top;
	int taken;

	/* Opened just now: the text to find is the selection (a short one on one line) or the last, with the keyboard. */
	if (main_app.panel_fresh) {
		main_app.panel_fresh = 0;
		main_panel_find_text(selected, sizeof(selected));
		kl_field_set(&main_find_field, selected);
		kl_ui_set_focus(main_input, MAIN_FIND_TEXT, 0);

		/* The log line the tests read. */
		te_log("FIND open find=%s", main_find_field.text);
	}

	/* The panel near the card's top, in its middle, narrower in a narrow window. */
	width = MAIN_REPLACE_WIDTH;
	if (width > area->width - 32)
		width = area->width - 32;
	panel.x = area->x + (area->width - width) / 2;
	panel.y = area->y + MAIN_REPLACE_TOP;
	panel.width = width;
	panel.height = MAIN_FIND_HEIGHT;

	/* The frame of input of its own: the panel, the field and the buttons. */
	kl_ui_begin(main_input, now_us);
	kl_panel(style, &panel, 0);
	top = kl_card(style, &panel, "Find", "The place found is selected as you type; Next and Previous go on.");
	rect.x = panel.x + 20;
	rect.width = panel.width - 40;
	rect.height = MAIN_REPLACE_ROW;
	rect.y = top;
	find_flags = kl_field(main_input, style, MAIN_FIND_TEXT, &rect, &main_find_field, "Find");

	/* The buttons at the bottom right: Next (the default), Previous, Done. */
	rect.y = top + MAIN_REPLACE_ROW + MAIN_REPLACE_GAP + 4;
	rect.width = kl_button_width(style, "Next");
	rect.x = panel.x + panel.width - 20 - rect.width;
	next = kl_button(main_input, style, MAIN_FIND_NEXT, &rect, "Next", KL_BUTTON_PRIMARY);
	rect.width = kl_button_width(style, "Previous");
	rect.x -= rect.width + 10;
	previous = kl_button(main_input, style, MAIN_FIND_PREVIOUS, &rect, "Previous", 0U);
	rect.width = kl_button_width(style, "Done");
	rect.x -= rect.width + 10;
	done = kl_button(main_input, style, MAIN_FIND_DONE, &rect, "Done", 0U);
	main_moving = kl_ui_end(main_input, now_us);

	/* A key no widget took: Esc closes the panel, anything else is nothing. */
	for (;;) {
		taken = kl_ui_take(main_input, &event);
		if (taken == 0)
			break;

		/* Esc. */
		if (event.kind == KL_EVENT_KEY && event.code == KL_KEY_ESC)
			done = 1;
	}

	/* Esc in the field closes the panel too. */
	if ((find_flags & KL_FIELD_CANCELLED) != 0U)
		done = 1;

	/* The text as it is typed is found from where the selection starts. */
	if ((find_flags & KL_FIELD_CHANGED) != 0U)
		te_app_find_text(&main_app, main_find_field.text);

	/* Enter in the field is Next. */
	if ((find_flags & KL_FIELD_SUBMITTED) != 0U)
		next = 1;

	/* Next and Previous go on with the field's text (one filled in when the panel opened is the editor's from then). */
	if (next || previous) {
		snprintf(main_app.find, sizeof(main_app.find), "%s", main_find_field.text);
		main_app.find_length = strlen(main_app.find);
	}

	/* Next finds the place after the one found, Previous the one before. */
	if (next) {
		te_edit_find(&main_app, 1, 0);
	} else if (previous) {
		te_edit_find(&main_app, 0, 0);
	}

	/* Done, then the frame shows what was asked. */
	if (done)
		te_app_find_close(&main_app);
	main_app.dirty = 1;
}

/*
 * Gives the text a Find or Replace panel opens with: the selection when it
 * is short and on one line, else the text looked for last.
 */
static void
main_panel_find_text(
	char *text,
	size_t size)
{
	const char *newline;
	size_t start;
	size_t end;

	/* The text looked for last, unless the selection says otherwise. */
	snprintf(text, size, "%s", main_app.find);

	/* No selection, or one too long for the field: the last text stays. */
	te_edit_selection(&main_app, &start, &end);
	if (end <= start || end - start >= size)
		return;

	/* The selection, unless it runs over a line. */
	te_buffer_copy(&main_app.buffer, start, end, text);
	text[end - start] = '\0';
	newline = strchr(text, '\n');
	if (newline != NULL)
		snprintf(text, size, "%s", main_app.find);
}

/* Gives the editor the window's services. */
static void
main_host(
	struct te_app *app)
{
	/* The clipboard, the primary selection and the context menu. */
	memset(&app->host, 0, sizeof(app->host));
	app->host.data = &main_window;
	app->host.copy = main_copy;
	app->host.paste = main_paste;
	app->host.select = main_select;
	app->host.paste_primary = main_paste_primary;
	app->host.context_menu = main_context_menu;
	app->host.choose = main_choose;
	app->host.drag_text = main_drag_text;
}

/*
 * Starts a drag of the selection's text out of the window, from the press
 * that started it (ws189-p003); while it goes on the window takes no drop,
 * so that the window's own drag over it shows nothing.  Returns 0 when the
 * drag started, or an errno value.
 */
static int
main_drag_text(
	void *data,
	const char *text,
	size_t length)
{
	struct te_window *window;
	uint32_t serial;
	int error;

	/* The drag, from the press. */
	window = data;
	serial = kl_window_press_serial(window->kui);
	error = kl_window_drag_text(window->kui, text, length, serial);
	if (error != 0) {
		te_log("DND drag failed errno=%d", error);
		return error;
	}

	/* The window's own drag is not taken back. */
	(void)kl_window_accept_drops(window->kui, 0U);
	te_log("DND drag start bytes=%lu", (unsigned long)length);

	/* Succeeded: the drag goes on. */
	return 0;
}

/*
 * Takes drag and drop's inputs (ws189-p003): a drag of text over the
 * window shows where it would go and is answered for that place, a drop
 * inserts it there, and the end of the window's own drag lets the window
 * take drops again.
 */
static void
main_drop_event(
	const struct kl_window_event *event)
{
	char *text;
	size_t length;
	unsigned type;
	int taken;
	int error;

	/* What it is. */
	switch (event->kind) {
	case KL_WINDOW_DROP_ENTER:
	case KL_WINDOW_DROP_MOTION:
		/* Over the text it is taken as a copy; elsewhere not. */
		taken = te_app_drop_over(&main_app, (int)event->x, (int)event->y);
		if (taken) {
			kl_window_answer_drop(main_window.kui, KL_DND_COPY, KL_DND_COPY);
		} else {
			kl_window_answer_drop(main_window.kui, 0U, 0U);
		}

		/* Answered. */
		break;
	case KL_WINDOW_DROP_LEAVE:
		te_app_drop_leave(&main_app);
		break;
	case KL_WINDOW_DROP:
		/* The text, inserted where the caret was; the drop finished as a copy, or given up. */
		error = kl_window_receive_drop(main_window.kui, &text, &length, &type);
		if (error != 0 || type != KL_DROP_TEXT || !main_app.drop_over) {
			te_log("DND drop refused errno=%d type=%u", error, type);
			free(text);
			te_app_drop_leave(&main_app);
			kl_window_finish_drop(main_window.kui, 0U);
			break;
		}

		/* Inserted, and finished as a copy. */
		te_app_drop_text(&main_app, text, length);
		free(text);
		kl_window_finish_drop(main_window.kui, KL_DND_COPY);
		te_log("DND drop bytes=%lu", (unsigned long)length);
		break;
	case KL_WINDOW_DRAG_DONE:
		/* The window's own drag ended: it takes drops again. */
		te_app_drag_done(&main_app);
		(void)kl_window_accept_drops(main_window.kui, KL_DROP_TEXT);
		te_log("DND drag done dropped=%u", event->code);
		break;
	default:
		break;
	}
}

/* Copies text to the clipboard. */
static void
main_copy(
	void *data,
	const char *text,
	size_t length)
{
	struct te_window *window;

	/* The window's clipboard. */
	window = data;
	kl_window_copy(window->kui, text, length);
	te_log("CLIPBOARD set bytes=%lu", (unsigned long)length);
}

/* Pastes the clipboard's text into a buffer; reports its length. */
static size_t
main_paste(
	void *data,
	char *text,
	size_t size)
{
	struct te_window *window;
	size_t length;

	/* The window's clipboard. */
	window = data;
	length = kl_window_paste(window->kui, text, size);
	te_log("CLIPBOARD paste received bytes=%lu", (unsigned long)length);

	/* Succeeded: the length received. */
	return length;
}

/* Makes text the primary selection. */
static void
main_select(
	void *data,
	const char *text,
	size_t length)
{
	struct te_window *window;

	/* The window's primary selection. */
	window = data;
	kl_window_select(window->kui, text, length);
	te_log("PRIMARY set bytes=%lu", (unsigned long)length);
}

/* Pastes the primary selection's text into a buffer; reports its length. */
static size_t
main_paste_primary(
	void *data,
	char *text,
	size_t size)
{
	struct te_window *window;
	size_t length;

	/* The window's primary selection. */
	window = data;
	length = kl_window_paste_primary(window->kui, text, size);
	te_log("PRIMARY paste bytes=%lu", (unsigned long)length);

	/* Succeeded: the length received. */
	return length;
}

/* Opens the context menu at a place. */
static void
main_context_menu(
	void *data,
	int x,
	int y)
{
	/* The menus' context menu (the window is the only one). */
	(void)data;
	te_menu_popup(&main_menu, x, y);
}

/*
 * Opens the file chooser to open a file or to save as a name in a folder;
 * its answer comes back as a TE_EVENT_CHOSEN.  Returns 0 or an errno value.
 */
static int
main_choose(
	void *data,
	int saving,
	const char *folder,
	const char *name)
{
	struct kl_file_chooser_options options;
	static const struct kl_file_chooser_listener listener = {
		main_chosen
	};
	struct te_window *window;

	/* A chooser left open goes first (one at a time). */
	window = data;
	kl_file_chooser_destroy(main_chooser);
	main_chooser = NULL;

	/* Open or Save As, at the document's folder, with the text files shown first. */
	memset(&options, 0, sizeof(options));
	options.mode = KL_FILE_CHOOSER_OPEN;
	if (saving) {
		options.mode = KL_FILE_CHOOSER_SAVE;
		options.name = name;
	}

	/* The editor's mark on the chooser, its folder, its filters and the interface's font. */
	options.application = MAIN_APPLICATION;
	options.folder = folder;
	options.filters = main_filters;
	options.filter_count = sizeof(main_filters) / sizeof(main_filters[0]);
	options.filter = 0;
	options.font = main_ui_font;

	/* The chooser's window over the editor's. */
	main_chooser = kl_file_chooser_open(kl_app_display(main_kl), kl_window_toplevel(window->kui), &options, &listener, window);
	if (main_chooser == NULL)
		return errno;

	/* Succeeded: the answer comes while the loop dispatches. */
	return 0;
}

/* The chooser answered: the path (empty when cancelled) goes to the editor, and the chooser goes. */
static void
main_chosen(
	void *data,
	struct kl_file_chooser *chooser,
	unsigned result,
	const char *path,
	size_t filter)
{
	struct te_event *event;

	/* The answer as an input of the editor. */
	(void)filter;
	event = te_window_push(data, TE_EVENT_CHOSEN);
	if (event != NULL) {
		event->text[0] = '\0';
		if (result == KL_FILE_CHOOSER_CHOSEN)
			snprintf(event->text, sizeof(event->text), "%s", path);
	}

	/* The log names the answer. */
	te_log("CHOSEN result=%u path=%s", result, path);

	/* The chooser is spent. */
	kl_file_chooser_destroy(chooser);
	if (chooser == main_chooser)
		main_chooser = NULL;
}

/*
 * Takes one input of the window: the pointer, the keys and the focus
 * become the editor's inputs, the fingers go to the fingers' input, and a
 * new size or the close button is noted for the loop.
 */
static void
main_window_event(
	const struct kl_window_event *event)
{
	struct te_event *input;
	int main_dialog_input;

	/* Every input carries the pointer's place and the modifiers (the same bits as the editor's). */
	main_window.pointer_x = (int)event->x;
	main_window.pointer_y = (int)event->y;
	main_window.modifiers = event->modifiers;

	/* Drag and drop with other windows, also under a dialog (which answers it as not taken, ws189-p003). */
	if (event->kind == KL_WINDOW_DROP_ENTER ||
	    event->kind == KL_WINDOW_DROP_MOTION ||
	    event->kind == KL_WINDOW_DROP_LEAVE ||
	    event->kind == KL_WINDOW_DROP ||
	    event->kind == KL_WINDOW_DRAG_DONE) {
		main_drop_event(event);
		return;
	}

	/* A dialog takes the pointer and the keys (libkeiland's). */
	main_dialog_input = 0;
	if (main_app.dialog != TE_DIALOG_NONE && main_input != NULL)
		main_dialog_input = main_dialog_event(event);
	if (main_dialog_input)
		return;

	/* What it is. */
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		(void)te_window_push(&main_window, TE_EVENT_MOTION);
		break;
	case KL_WINDOW_LEAVE:
		(void)te_window_push(&main_window, TE_EVENT_LEAVE);
		break;
	case KL_WINDOW_BUTTON:
		/* The button and whether it went down. */
		input = te_window_push(&main_window, TE_EVENT_BUTTON);
		if (input == NULL)
			break;
		input->button = event->code;
		input->pressed = event->pressed;
		break;
	case KL_WINDOW_AXIS:
		/* The wheel's distance down and across. */
		input = te_window_push(&main_window, TE_EVENT_AXIS);
		if (input == NULL)
			break;
		input->scroll = (int)event->dy;
		input->scroll_x = (int)event->dx;
		input->axis_dx = event->dx;
		input->axis_dy = event->dy;
		input->axis_source = event->axis_source;
		input->axis_us = event->time_us;
		break;
	case KL_WINDOW_AXIS_STOP:
		/* A touch pad's fingers lifted (ws090-p019), at the compositor's time. */
		input = te_window_push(&main_window, TE_EVENT_AXIS_STOP);
		if (input == NULL)
			break;
		input->axis_us = event->time_us;
		break;
	case KL_WINDOW_KEY:
		/* The key and whether it went down. */
		input = te_window_push(&main_window, TE_EVENT_KEY);
		if (input == NULL)
			break;
		input->key = event->code;
		input->pressed = event->pressed;
		break;
	case KL_WINDOW_TEXT_COMMIT:
		/* Text from an input method or the on-screen keyboard. */
		te_log("TEXT input commit=%s", event->text);
		input = te_window_push(&main_window, TE_EVENT_TEXT);
		if (input != NULL)
			snprintf(input->text, sizeof(input->text), "%s", event->text);
		break;
	case KL_WINDOW_TEXT_DELETE:
		/* Bytes around the caret it replaces. */
		input = te_window_push(&main_window, TE_EVENT_TEXT_DELETE);
		if (input == NULL)
			break;
		input->key = event->before;
		input->button = event->after;
		break;
	case KL_WINDOW_TEXT_PREEDIT:
		/* The text being composed, drawn in the body at the cursor (draw.c), and its segment or caret. */
		te_log("TEXT input preedit=%s begin=%d end=%d", event->text, (int)event->begin, (int)event->end);
		snprintf(main_app.preedit, sizeof(main_app.preedit), "%s", event->text);
		main_app.preedit_begin = event->begin;
		main_app.preedit_end = event->end;
		main_app.dirty = 1;
		break;
	case KL_WINDOW_FOCUS:
		/* The keyboard came (READY waits for the first time) or went; Open Recent follows another program's change of the list. */
		if (event->pressed) {
			main_focus_came = 1;
			main_recent_follow();
		}

		/* The window hears it too. */
		input = te_window_push(&main_window, TE_EVENT_FOCUS);
		if (input != NULL)
			input->pressed = event->pressed;
		break;
	case KL_WINDOW_TOUCH_DOWN:
		te_log("TOUCH down id=%d x=%.0f y=%.0f", (int)event->id, event->x, event->y);
		if (main_input != NULL)
			(void)kl_ui_touch_down(main_input, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_MOTION:
		if (main_input != NULL)
			(void)kl_ui_touch_motion(main_input, event->id, event->time_us, event->arrival_us, event->x, event->y);
		break;
	case KL_WINDOW_TOUCH_UP:
		te_log("TOUCH up id=%d", (int)event->id);
		if (main_input != NULL)
			(void)kl_ui_touch_up(main_input, event->id, event->time_us, event->arrival_us);
		break;
	case KL_WINDOW_TOUCH_CANCEL:
		if (main_input != NULL)
			(void)kl_ui_touch_cancel(main_input, event->arrival_us);
		break;
	case KL_WINDOW_RESIZE:
		main_resized = 1;
		break;
	case KL_WINDOW_CLOSE:
		main_closed = 1;
		break;
	case KL_WINDOW_ACTION:
		/* An item of the menus, in its place among the keys. */
		main_action(event);
		break;
	default:
		break;
	}
}

/*
 * Tells the text input where the caret is after a frame: a dialog's field
 * with the keyboard asks for it there (or a dialog without one turns it
 * off), otherwise the body's caret (ws090-p022).
 */
static void
main_text_caret(
	void)
{
	struct te_rect caret;

	/* A dialog's fields. */
	if (main_app.dialog != TE_DIALOG_NONE && main_input != NULL) {
		kl_ui_window_text(main_input, main_window.kui);
		return;
	}

	/* The body's caret. */
	te_app_caret_rect(&main_app, &caret);
	kl_window_text_cursor(main_window.kui, caret.x, caret.y, caret.width, caret.height);
}

/* Gives the pointer's, the keys' and an input method's input to a dialog shown; 1 when it took it. */
static int
main_dialog_event(
	const struct kl_window_event *event)
{
	/* What it is. */
	switch (event->kind) {
	case KL_WINDOW_MOTION:
		(void)kl_ui_pointer_motion(main_input, event->x, event->y);
		break;
	case KL_WINDOW_LEAVE:
		(void)kl_ui_pointer_leave(main_input);
		break;
	case KL_WINDOW_BUTTON:
		/* The main button only. */
		(void)kl_ui_pointer_motion(main_input, event->x, event->y);
		if (event->code == KL_BUTTON_LEFT)
			(void)kl_ui_pointer_button(main_input, event->pressed, event->arrival_us);
		break;
	case KL_WINDOW_KEY:
		(void)kl_ui_key(main_input, event->code, event->pressed, event->modifiers);
		break;
	case KL_WINDOW_TEXT_COMMIT:
	case KL_WINDOW_TEXT_PREEDIT:
	case KL_WINDOW_TEXT_DELETE:
		/* An input method's text, for the dialog's field with the keyboard (ws090-p022). */
		(void)kl_ui_text(main_input, event);
		break;
	case KL_WINDOW_AXIS:
		break;
	default:
		return 0;
	}

	/* Succeeded: the dialog is drawn again with it. */
	main_app.dirty = 1;
	return 1;
}

/*
 * Moves the fingers' input on to a time: the text view is recorded (unless
 * a dialog or the chooser covers it), the fingers' selection and context
 * menu reach the editor, a tap elsewhere (a dialog's button) is a click,
 * and the view is drawn where its scroll has it.
 */
static void
main_fingers(
	uint64_t now_us)
{
	struct kl_event event;
	struct kl_rect region;
	struct te_rect text;
	unsigned pressed;
	int taken;

	/* Without the fingers' input, only the view's scroll moves. */
	main_moving = 0;
	if (main_input == NULL) {
		main_moving = te_app_sync_scroll(&main_app, now_us);
		return;
	}

	/* A dialog records its own frame of the input (main_overlay); the bar is not drawn under it (ws190-p003). */
	if (main_app.dialog != TE_DIALOG_NONE) {
		main_app.bar.count = 0;
		main_bar_log();
		return;
	}

	/* The frame of the fingers' input: the text view and the bar over it, when nothing covers them. */
	kl_ui_begin(main_input, now_us);
	pressed = 0U;
	main_app.bar.count = 0;
	if (main_app.dialog == TE_DIALOG_NONE && !main_app.choosing) {
		te_app_text_rect(&main_app, &text);
		region.x = text.x;
		region.y = text.y;
		region.width = text.width;
		region.height = text.height;
		kl_ui_text_region(main_input, MAIN_TEXT_REGION, &region, &main_app.scroll, &main_app.touch);
		pressed = main_bar();
	}

	/* The frame is recorded; whether something still moves. */
	main_moving = kl_ui_end(main_input, now_us);

	/* The fingers' selection and context menu, then the bar's button pressed. */
	te_app_touch(&main_app);
	main_bar_press(pressed);
	main_bar_log();

	/* What no part took: a tap is a click (a dialog's button), a long press elsewhere asks for the menu. */
	for (;;) {
		taken = kl_ui_take(main_input, &event);
		if (taken == 0)
			break;
		if (event.kind == KL_EVENT_TAP) {
			te_log("TOUCH tap x=%.0f y=%.0f", event.x, event.y);
			te_app_tap(&main_app, (int)event.x, (int)event.y, 1);
		} else if (event.kind == KL_EVENT_DOUBLE_TAP) {
			te_app_tap(&main_app, (int)event.x, (int)event.y, 2);
		}
	}

	/* The view where its scroll has it (a new place is drawn, the handles with it). */
	(void)te_app_sync_scroll(&main_app, now_us);
}

/* Takes the desktop's new appearance: the editor is drawn again in its colours. */
static void
main_appearance_changed(void)
{
	unsigned appearance;

	/* A new frame. */
	appearance = kl_appearance_get(NULL);
	main_app.dirty = 1;
	te_log("APPEARANCE appearance=%u", appearance);
}

/*
 * Carries out an item of the menus: Select All (the compositor takes Ctrl+A for
 * the menu) selects the focused field of the Find or Replace panel; any
 * other action is the editor's.
 */
static void
main_action(
	const struct kl_window_event *event)
{
	/* The log line of the choice. */
	te_log("ACTION id=%d action=%u", (int)event->id, (unsigned)event->code);

	/* Select All in the Find or Replace panel's field. */
	if (event->code == TE_ACTION_SELECT_ALL &&
	    (main_app.dialog == TE_DIALOG_REPLACE || main_app.dialog == TE_DIALOG_FIND) &&
	    main_input != NULL) {
		(void)kl_ui_key(main_input, MAIN_KEY_A, 1, KL_MOD_CTRL);
		main_app.dirty = 1;
		return;
	}

	/* The editor's action, in its place among the keys. */
	te_window_act(&main_window, event->code);
}

/*
 * Lays the bar of editing buttons out over the fingers' selection and
 * records its buttons in the fingers' input's frame (ws190-p003, plan/ws190/
 * phase001/phase.md section 3): Cut, Copy, Paste and Select All as they
 * apply, above the selection within the window less the on-screen
 * keyboard.  Reports the button pressed since the last frame, 0 for none.
 */
static unsigned
main_bar(void)
{
	struct kl_rect first;
	struct kl_rect second;
	struct kl_rect selection;
	struct kl_rect visible;
	struct kl_rect bounds;
	struct te_rect text;
	unsigned facts;
	unsigned buttons;
	unsigned pressed;
	size_t length;
	size_t start;
	size_t end;
	double origin_x;
	double origin_y;
	int right;
	int bottom;
	int paste;
	int shown;

	/* Only while the fingers' selection shows it, no finger drags it, and the interface's font is there. */
	if (!main_app.touch.bar || main_app.touch.selecting || !main_widgets_text)
		return 0U;

	/* The selection's ends in order, and the text's length. */
	start = main_app.anchor;
	end = main_app.cursor;
	if (start > end) {
		start = main_app.cursor;
		end = main_app.anchor;
	}

	/* The text's length. */
	length = te_buffer_length(&main_app.buffer);

	/* What the text is: the window's clipboard is always there. */
	facts = KL_TEXT_BAR_CLIPBOARD;
	if (start != end)
		facts |= KL_TEXT_BAR_SELECTED;
	if (start == 0U && end == length && length != 0U)
		facts |= KL_TEXT_BAR_WHOLE;
	if (length == 0U)
		facts |= KL_TEXT_BAR_EMPTY;
	paste = kl_window_can_paste(main_window.kui);
	if (paste)
		facts |= KL_TEXT_BAR_CAN_PASTE;
	buttons = kl_text_bar_buttons(facts);

	/* The selection in the window: one line's ends, or its lines across the text. */
	te_app_text_rect(&main_app, &text);
	origin_x = (double)text.x - main_app.scroll_x;
	origin_y = (double)text.y - main_app.scroll_y;
	main_app.touch.view->caret_rect(main_app.touch.data, start, &first);
	main_app.touch.view->caret_rect(main_app.touch.data, end, &second);
	selection.x = (int)(origin_x + (double)first.x);
	selection.y = (int)(origin_y + (double)first.y);
	selection.width = second.x - first.x;
	selection.height = first.height;
	if (second.y != first.y) {
		selection.x = text.x;
		selection.width = text.width;
		selection.height = second.y + second.height - first.y;
	}

	/* What shows of the text, and the window less the on-screen keyboard. */
	visible.x = text.x;
	visible.y = text.y;
	visible.width = text.width;
	visible.height = text.height;
	kl_window_keyboard_inset(main_window.kui, &right, &bottom);
	bounds.x = 0;
	bounds.y = 0;
	bounds.width = (int)main_width - right;
	bounds.height = (int)main_height - bottom;

	/* Laid out and recorded over the text. */
	shown = kl_text_bar_layout(&main_app.bar, &main_widgets, buttons, &selection, &visible, &bounds);
	if (!shown)
		return 0U;
	pressed = kl_text_bar_hit(main_input, MAIN_TEXT_BAR, &main_app.bar, &main_app.bar_held);

	/* Reports the button pressed. */
	return pressed;
}

/*
 * Carries out a button of the bar (ws190-p003): Copy keeps the selection
 * and hides the bar; Cut and Paste leave a caret, the fingers' handles and
 * bar gone; Select All selects the whole text as the fingers' selection.
 */
static void
main_bar_press(
	unsigned button)
{
	size_t length;

	/* Each button: its action, logged. */
	switch (button) {
	case KL_TEXT_BAR_COPY:
		te_log("TOUCH bar press button=copy");
		te_app_action(&main_app, TE_ACTION_COPY);
		kl_text_touch_hide_bar(&main_app.touch);
		break;
	case KL_TEXT_BAR_CUT:
		te_log("TOUCH bar press button=cut");
		te_app_action(&main_app, TE_ACTION_CUT);
		kl_text_touch_set_selection(&main_app.touch, main_app.anchor, main_app.cursor);
		break;
	case KL_TEXT_BAR_PASTE:
		te_log("TOUCH bar press button=paste");
		te_app_action(&main_app, TE_ACTION_PASTE);
		kl_text_touch_set_selection(&main_app.touch, main_app.anchor, main_app.cursor);
		break;
	case KL_TEXT_BAR_SELECT_ALL:
		te_log("TOUCH bar press button=select-all");
		te_app_action(&main_app, TE_ACTION_SELECT_ALL);
		length = te_buffer_length(&main_app.buffer);
		kl_text_touch_select(&main_app.touch, 0, length);
		(void)kl_text_touch_take(&main_app.touch);
		break;
	default:
		return;
	}

	/* The next frame shows what the button did. */
	main_app.dirty = 1;
}

/* Logs the bar's coming and going (for the tests). */
static void
main_bar_log(void)
{
	/* Shown since the last log. */
	if (main_app.bar.count != 0U && !main_app.bar_logged) {
		main_app.bar_logged = 1;
		te_log("TOUCH bar shown buttons=%u rect=%d,%d,%d,%d", main_app.bar.buttons, main_app.bar.rect.x, main_app.bar.rect.y, main_app.bar.rect.width, main_app.bar.rect.height);
		main_app.dirty = 1;
		return;
	}

	/* Gone since the last log. */
	if (main_app.bar.count == 0U && main_app.bar_logged) {
		main_app.bar_logged = 0;
		te_log("TOUCH bar hidden");
		main_app.dirty = 1;
	}
}
