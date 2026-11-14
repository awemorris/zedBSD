/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The window mode of browser: a view (<browser/browser.h>) shown in a compositor
 * window, recording its drawing into the frames of the window's swapchain
 * on the shell's Vulkan device.  The view holds the page, its history, its
 * scroll, its timers, the network and the renderer, and runs them on
 * threads of its own (BROWSER_THREADING_OWN): the window's thread here
 * only hands the view the window's input and commands and reads the state
 * it published, so that no script, layout or fetch keeps the window from
 * taking its events.  The shell's drawing thread (render.c) draws the
 * frames the view commits, with the GPU or the CPU.  The shell holds the
 * window, the compositor's titlebar (back, forward, reload and the location,
 * whose URL can be edited) and the presenter (the swapchain), and
 * turns the window's input into the view's input (ws074-p056): the
 * pointer's moves, buttons, wheel and leaving, the keys with the DOM's
 * names (keys.c), and the keyboard's focus.  The view does what the input
 * means for the page (scrolling, links, the focus, the history's keys).
 * ws081-p006: the touch screen's fingers (touch.c) scroll the page with
 * inertia and stretch it past its ends (the view's placed scroll and
 * overscroll), and click with taps and long presses.
 * WS169 p005: a sign-in code that comes by mail is offered as a
 * notification, whose click types it into the page (mail.c).
 * The shell keeps its own shortcuts, which the page never sees: Ctrl+Q and
 * Ctrl+W close the window, and Ctrl+L edits the location; the titlebar's
 * controls step through the history and reload.
 *
 * The program writes lines to standard output that the guest tests read
 * (ZBROWSER READY, FRAME, LINK, NAVIGATE, TITLEBAR, CONSOLE, LOADING,
 * STOPPED, ERROR, TOUCH); they are its diagnostic interface.
 */

#include "shell/internal.h"
#include "../../picture/png-write.h"
#include "../../libbrowser/base/os-error.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest data directory the shell keeps. */
#define SHELL_DIRECTORY_MAX	4096U

/* How long a frame the window cannot take yet waits before it is shown again, in milliseconds. */
#define SHELL_RETRY_MS		8

/* The longest location typed into the URL's field that is opened (a longer one is cut). */
#define SHELL_LOCATION_MAX	4096U

/* The evdev codes of the keys of the shell's own shortcuts. */
#define SHELL_KEY_Q		16U
#define SHELL_KEY_W		17U
#define SHELL_KEY_L		38U

/* How far a press moves before the image under it is dragged out of the window, and the picture's longest side (ws189-p003). */
#define SHELL_DRAG_DISTANCE	8
#define SHELL_DRAG_SIDE		2048

/* The longest reason of a failed load the shell keeps for the page that says why the start page did not open (BUG-320). */
#define SHELL_FAILURE_MAX	512U

/* The location of the page that says why the start page did not open: an HTML document in a data: URL. */
#define SHELL_FAILURE_PREFIX	"data:text/html;charset=utf-8,"

/* The page that says why the start page did not open, around the location and the reason, which are escaped (BUG-320). */
#define SHELL_FAILURE_HEAD	"<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n" \
	"<title>Page not opened</title>\n<style>\n" \
	"body { margin: 0; background: #eef1f6; color: #1f2633; font-family: sans-serif; font-size: 15px; line-height: 1.5; }\n" \
	".main { max-width: 640px; margin: 48px auto; background: #ffffff; border: 1px solid #d3dae6; padding: 14px 22px 16px; }\n" \
	"h1 { margin: 0 0 8px; font-size: 22px; color: #1d2b45; }\n" \
	"p { margin: 6px 0; word-wrap: break-word; }\n" \
	".reason { color: #8a2b2b; }\n" \
	"a { color: #2463b3; }\n" \
	"</style>\n</head>\n<body>\n<div class=\"main\">\n<h1>The page could not be opened</h1>\n<p>"
#define SHELL_FAILURE_REASON	"</p>\n<p class=\"reason\">"
#define SHELL_FAILURE_LINK	"</p>\n<p><a href=\""
#define SHELL_FAILURE_TAIL	"\">Try again</a></p>\n</div>\n</body>\n</html>\n"

/*
 * What the window mode holds while it runs: the view, the window with its
 * titlebar and presenter (the Vulkan one, or with use_cpu the CPU one,
 * cpu), the size frames are drawn at (width and height), and whether the
 * window is up (the view's first page is asked for before it).
 *
 * The drawing thread (render.c) draws the view's committed frames:
 * frames_seen is how many of its presents the window's thread has
 * written a FRAME line for, and cpu_waiting says the CPU's frame taken
 * last has not been shown yet (the window could not take it).  latency is
 * the diagnostic clock of the window's thread (latency.c).
 *
 * The fingers (touch.c, ws081-p006): their gestures and scroller (without
 * them fingers do nothing), in how many milliseconds they want the next
 * round (-1: none), and the number of the page shown, which each page
 * committed moves on (a new page's scroll is taken over by the fingers).
 *
 * The sign-in codes of mail (mail.c, WS169 p005).
 *
 * The question about a page that does not respond (prompt.c): its card
 * over the page and the window's input to it while it shows.
 *
 * A left press that may drag a picture out of the window (ws189-p003):
 * whether one is held, and where it went down; drag_asked says the view
 * was asked for the image there and the press is still held (its answer,
 * the image callback, starts the drag).
 *
 * text_session is the view's text input's session when the window's text
 * input was last asked for (browser_view_text_session, ws177-p019).
 *
 * load_failure is the reason the view gave for the last load that failed
 * (empty when it gave none), which the page that says why the start page
 * did not open shows (BUG-320).  start is the start page (the options'
 * string) and started says a page was shown: a load that fails before
 * opens that page.  failure_page is that page's data: URL as the view
 * keeps it (NULL until it is shown), owned until the view is gone, while
 * failure_wanted says it was asked for and is not shown yet, and
 * failure_location the start page it stands for: while the page is shown
 * the titlebar shows that location, and reload tries it again.
 *
 * instance is the engine's instance the view belongs to, which keeps the
 * cookies and localStorage in the user's data directory (shell_instance;
 * nothing is kept when the directory cannot be used); it outlives the view.
 *
 * loading is the location of the load under way (empty when none), which
 * the titlebar shows from the click on, before its page is shown, while
 * its box fills as the load goes on (browser_view_load_progress).
 *
 * The view's wake runs on the view's threads: wake_app is the
 * application it posts to (NULL until the window is up; written and read
 * with atomic operations), and wake_missed is set when a wake could not
 * be posted (the main loop then runs the view without waiting).
 */
struct shell_state {
	struct browser_instance *instance;
	struct browser_view *view;
	struct shell_window window;
	struct shell_titlebar titlebar;
	struct shell_present present;
	struct shell_cpu cpu;
	int use_cpu;
	uint32_t width;
	uint32_t height;
	int ready;
	struct shell_render render;
	unsigned long frames_seen;
	int cpu_waiting;
	struct shell_latency latency;
	struct shell_touch touch;
	int touch_due;
	unsigned long page_number;
	struct shell_mail mail;
	struct shell_prompt prompt;
	int drag_armed;
	int drag_press_x;
	int drag_press_y;
	int drag_asked;
	uint64_t text_session;
	char load_failure[SHELL_FAILURE_MAX];
	char loading[SHELL_LOCATION_MAX + 1U];
	const char *start;
	int started;
	char *failure_page;
	int failure_wanted;
	const char *failure_location;
	struct kl_app *wake_app;
	int wake_missed;
};

/*
 * A text being built for the page that says why the start page did not
 * open: its bytes, always ended by a NUL, how many are used and how many
 * the allocation holds.  The builder owns the bytes until it frees them.
 */
struct shell_text {
	char *bytes;
	size_t length;
	size_t size;
};

static void shell_show_state(struct shell_state *state);
static void shell_show_titlebar(struct shell_state *state);
static void shell_input(struct shell_state *state, const struct shell_event *event);
static void shell_button(struct shell_state *state, const struct shell_event *event);
static void shell_image(void *context, struct browser_view *view, int error, struct browser_image *image);
static void shell_drag_image(struct shell_state *state, const struct browser_image *image);
static void shell_key(struct shell_state *state, const struct shell_event *event);
static int shell_text(struct shell_state *state, const struct shell_event *event);
static void shell_text_input(struct shell_state *state);
static int shell_shortcut(struct shell_state *state, const struct shell_event *event);
static void shell_titlebar_input(struct shell_state *state, const struct shell_titlebar_event *event);
static void shell_go(struct shell_state *state, int steps);
static void shell_reload(struct shell_state *state);
static void shell_follow(struct shell_state *state, const char *target);
static void shell_follow_typed(struct shell_state *state, const char *typed);
static int shell_instance(struct kl_app *app, struct browser_instance **instance);
static int shell_surface_open(struct shell_state *state, const struct shell_options *options);
static int shell_window_up(struct shell_state *state, const struct shell_options *options, unsigned present);
static int shell_surface_resize(struct shell_state *state);
static void shell_resize_live(void *data, uint32_t width, uint32_t height);
static int shell_path_is_absolute(const char *typed);
static int shell_render_up(struct shell_state *state);
static int shell_frames(struct shell_state *state);
static int shell_cpu_frame(struct shell_state *state);
static void shell_frame_line(struct shell_state *state, uint32_t width, uint32_t height);
static void shell_release(struct shell_state *state);
static int shell_resize_view(struct shell_state *state);
static void shell_color_scheme(struct shell_state *state);
static const char *shell_string(const char *string);
static void shell_title(void *context, struct browser_view *view, const char *title);
static void shell_committed(void *context, struct browser_view *view);
static void shell_load(void *context, struct browser_view *view, enum browser_load_state state, const char *url, int error, const char *reason);
static enum browser_policy shell_link(void *context, struct browser_view *view, const char *href);
static void shell_console(void *context, struct browser_view *view, int level, const char *text, size_t length);
static void shell_wake(void *context);
static void shell_script_error(void *context, struct browser_view *view, int error);
static void shell_field(void *context, struct browser_view *view, int error, const float *caret);
static void shell_touch_round(struct shell_state *state);
static void shell_touch_pointer(struct shell_state *state, const struct shell_touch_pointer *made);
static int shell_open_failure(struct shell_state *state, const char *location, int failure);
static int shell_failure_shown(const struct shell_state *state);
static int shell_failure_page(const char *location, const char *reason, struct shell_text *page);
static int shell_text_append(struct shell_text *text, const char *bytes, size_t length);
static int shell_text_escaped(struct shell_text *text, const char *string);
static int shell_text_percent(struct shell_text *text, const char *bytes, size_t length);

/*
 * Runs the browser in a compositor window until it is closed.
 *
 * Returns the program's exit status.
 */
int
shell_run(
	const struct shell_options *options)
{
	struct shell_state state;
	struct browser_callbacks callbacks;
	struct browser_view_options view_options;
	struct shell_event event;
	struct shell_titlebar_event titlebar_event;
	uint64_t serial;
	uint64_t shown_serial;
	int changed;
	int timeout;
	int mail_due;
	int prompt_due;
	int missed;
	int retry;
	int status;
	int taken;
	int error;

	/* A window needs a page to show. */
	memset(&state, 0, sizeof(state));
	if (options->start == NULL) {
		fprintf(stderr, "browser: a page to open is needed (a file or a file: URL)\n");
		return 2;
	}

	/* Keeps the page the window opens on. */
	state.start = options->start;

	/* The connection to the compositor: its application names the program's data directory. */
	status = shell_window_connect(&state.window, options->display);
	if (status != 0) {
		fprintf(stderr, "browser: cannot open a window: %s\n", wb_os_error_text(errno));
		return 1;
	}

	/* The engine's instance, which remembers the cookies and localStorage. */
	error = shell_instance(state.window.app, &state.instance);
	if (error != 0) {
		fprintf(stderr, "browser: cannot make the engine's instance: %s\n", wb_os_error_text(error));
		shell_window_close(&state.window);
		return 1;
	}

	/* The drawing thread's lock, which the view's frame_wake takes. */
	error = shell_render_init(&state.render);
	if (error != 0) {
		fprintf(stderr, "browser: cannot start drawing: %s\n", wb_os_error_text(error));
		(void)browser_instance_destroy(state.instance);
		shell_window_close(&state.window);
		return 1;
	}

	/* The view, on threads of its own, which tells the shell what happens to it. */
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.title = shell_title;
	callbacks.committed = shell_committed;
	callbacks.load = shell_load;
	callbacks.link = shell_link;
	callbacks.console = shell_console;
	callbacks.script_error = shell_script_error;
	callbacks.image = shell_image;
	callbacks.field = shell_field;
	callbacks.context = &state;
	memset(&view_options, 0, sizeof(view_options));
	view_options.version = BROWSER_API_VERSION;
	view_options.fonts = options->fonts;
	view_options.callbacks = &callbacks;
	view_options.stack_base = __builtin_frame_address(0);
	view_options.width = options->width;
	view_options.height = options->height;
	view_options.wake = shell_wake;
	view_options.wake_context = &state;
	view_options.instance = state.instance;
	view_options.threading = BROWSER_THREADING_OWN;
	view_options.frame_wake = shell_render_frame_wake;
	view_options.frame_wake_context = &state.render;
	error = browser_view_create(&view_options, &state.view);
	if (error != 0) {
		fprintf(stderr, "browser: cannot start the network: %s\n", wb_os_error_text(error));
		shell_render_close(&state.render);
		(void)browser_instance_destroy(state.instance);
		shell_window_close(&state.window);
		return 1;
	}

	/* The pages are shown in the desktop's appearance (prefers-color-scheme). */
	shell_color_scheme(&state);

	/* The fingers; without memory for them they do nothing. */
	state.touch_due = -1;
	error = shell_touch_open(&state.touch);
	if (error != 0) {
		printf("ZBROWSER ERROR touch error=%d\n", error);
		fflush(stdout);
	}

	/* The start page, asked for before the window opens; one that cannot be opened leaves a page that says why, so the window still opens (BUG-320). */
	error = browser_view_load(state.view, options->start);
	if (error != 0) {
		error = shell_open_failure(&state, options->start, error);
		if (error != 0) {
			fprintf(stderr, "browser: cannot open %s: %s\n", options->start, wb_os_error_text(error));
			shell_release(&state);
			return 1;
		}
	}

	/* The window, and the presenter that shows the page in it (the GPU's, or the CPU's). */
	status = shell_surface_open(&state, options);
	if (status != 0) {
		shell_release(&state);
		return 1;
	}

	/* The page at the window's size, and the drawing thread that draws it. */
	status = shell_resize_view(&state);
	if (status == 0)
		status = shell_render_up(&state);
	if (status != 0) {
		shell_release(&state);
		return 1;
	}

	/* The question about a page that does not respond, in the desktop's language, placed in the window. */
	shell_prompt_open(&state.prompt, options->fonts, kl_app_language(state.window.app));
	shell_prompt_resize(&state.prompt, &state.render, state.width, state.height, shell_clock());

	/* The line the tests wait for, and the titlebar's state (the page's own once one is shown; the view tells of a later one). */
	printf("ZBROWSER READY width=%u height=%u document=%.0f\n", (unsigned)state.width,
	    (unsigned)state.height, browser_view_document_height(state.view));
	fflush(stdout);
	state.ready = 1;
	if (state.started) {
		shell_show_state(&state);
	} else {
		shell_show_titlebar(&state);
	}

	/*
	 * The diagnostic clock of the window's thread, when asked for: from the
	 * loop's start, so that it times the waits of the loop that takes the
	 * events (the window's start-up before it, such as the question's fonts,
	 * is not an event's wait).
	 */
	error = shell_latency_start(&state.latency, state.window.app);
	if (error != 0) {
		printf("ZBROWSER ERROR latency error=%d\n", error);
		fflush(stdout);
	}

	/* Lets the window report its waits to the latency record, when one started. */
	if (state.latency.thread != NULL)
		state.window.latency = &state.latency;

	/* Takes the window's events and hands them to the view, until the window closes. */
	shown_serial = 0;
	while (!state.window.closed) {
		/* A new size for the drawing thread, and the page laid out again at it. */
		if (state.window.resized) {
			state.window.resized = 0;
			status = shell_surface_resize(&state);
			if (status != 0)
				break;

			/* The page at the new size, and the question's card placed in it. */
			status = shell_resize_view(&state);
			if (status != 0)
				break;
			shell_prompt_resize(&state.prompt, &state.render, state.width, state.height, shell_clock());
		}

		/* The desktop's appearance changed: the page in the new scheme, and the question's card in the new theme. */
		if (state.window.themed) {
			state.window.themed = 0;
			shell_color_scheme(&state);
			shell_prompt_redraw(&state.prompt, &state.render, shell_clock());
		}

		/* The desktop's language changed: the question's words in it. */
		if (state.window.languaged) {
			state.window.languaged = 0;
			shell_prompt_language(&state.prompt, &state.render, state.window.language);
		}

		/* The frames the drawing thread drew: shown (the CPU's way) and told (FRAME); a frame the window cannot take yet is shown again soon. */
		status = shell_frames(&state);
		if (status == 1)
			break;
		retry = 0;
		if (status == 2)
			retry = 1;

		/* Waits for the compositor, the view's and the drawing thread's wakes, the fingers or the end of a code's offer (a held key's repeat waits less, within the dispatch). */
		timeout = -1;
		if (state.touch_due >= 0)
			timeout = state.touch_due;
		mail_due = shell_mail_timeout(&state.mail, shell_clock());
		if (mail_due >= 0 && (timeout < 0 || mail_due < timeout))
			timeout = mail_due;

		/* The page's running task, looked at for the question about a page that does not respond (when to look again). */
		prompt_due = shell_prompt_round(&state.prompt, state.view, &state.render, shell_clock());
		if (prompt_due >= 0 && (timeout < 0 || prompt_due < timeout))
			timeout = prompt_due;

		/* A frame that waits for the window is tried again after a short wait. */
		if (retry && (timeout < 0 || timeout > SHELL_RETRY_MS))
			timeout = SHELL_RETRY_MS;

		/* A wake that could not be posted is not waited for. */
		missed = __atomic_exchange_n(&state.wake_missed, 0, __ATOMIC_ACQ_REL);
		if (missed)
			timeout = 0;

		/* The wait. */
		status = shell_window_dispatch(&state.window, timeout);
		if (status != 0) {
			fprintf(stderr, "browser: the connection to the compositor was lost\n");
			break;
		}

		/* Hands the view the inputs that arrived. */
		for (;;) {
			taken = shell_window_take(&state.window, &event);
			if (!taken)
				break;
			shell_input(&state, &event);
		}

		/* And what was done with the titlebar. */
		for (;;) {
			taken = shell_titlebar_take(&state.titlebar, &titlebar_event);
			if (!taken)
				break;
			shell_titlebar_input(&state, &titlebar_event);
		}

		/* The fingers: the scroll they move and the clicks they make. */
		shell_touch_round(&state);

		/* A sign-in code that came by mail, offered or typed (the titlebar declared again shows the state again). */
		changed = shell_mail_round(&state.mail, state.view, &state.titlebar, shell_clock());
		if (changed)
			shell_show_state(&state);

		/* What the view published since: its state, and its callbacks; then how far a load has come, in the location's box. */
		browser_view_process(state.view);
		shell_titlebar_progress(&state.titlebar, browser_view_load_progress(state.view));

		/* The window's text input follows the page's state: on while the page's focus takes an input method's text. */
		serial = browser_view_snapshot_serial(state.view);
		if (serial != shown_serial) {
			shown_serial = serial;
			shell_text_input(&state);
		}
	}

	/* Closes everything. */
	shell_release(&state);

	/* Succeeded: the window was closed. */
	return 0;
}

/* Gives the view the swapchain's size; nonzero on failure. */
static int
shell_resize_view(
	struct shell_state *state)
{
	int error;

	/* The view at the frames' size. */
	error = browser_view_resize(state->view, state->width, state->height);
	if (error != 0) {
		fprintf(stderr, "browser: cannot lay out the page: %s\n", wb_os_error_text(error));
		return 1;
	}

	/* Succeeded: the page fits the window. */
	return 0;
}

/* Gives the view the desktop's appearance as its color scheme, and writes the SCHEME line. */
static void
shell_color_scheme(
	struct shell_state *state)
{
	enum browser_color_scheme scheme;
	unsigned appearance;
	int dark;

	/* The appearance the program was told last. */
	appearance = kl_appearance_get(NULL);
	scheme = BROWSER_COLOR_SCHEME_LIGHT;
	dark = 0;
	if (appearance == KL_APPEARANCE_DARK) {
		scheme = BROWSER_COLOR_SCHEME_DARK;
		dark = 1;
	}

	/* The view's pages in it, and the line the tests read. */
	browser_view_set_color_scheme(state->view, scheme);
	printf("ZBROWSER SCHEME dark=%d\n", dark);
	fflush(stdout);
}

/* Shows the page's title in the window and the history and location in the titlebar, and writes the NAVIGATE line. */
static void
shell_show_state(
	struct shell_state *state)
{
	const char *title;
	const char *url;

	/* The window's title: the page's, or its location (nothing before the view published its first state). */
	title = shell_string(browser_view_title(state->view));
	url = shell_string(browser_view_url(state->view));
	shell_window_title(&state->window, title);

	/* The line the tests read. */
	printf("ZBROWSER NAVIGATE path=%s title=%s\n", url, title);
	fflush(stdout);

	/* The titlebar. */
	shell_show_titlebar(state);
}

/* Shows the history and the location in the titlebar: the page's, or the location of a load under way. */
static void
shell_show_titlebar(
	struct shell_state *state)
{
	const char *url;
	int can_back;
	int can_forward;
	int shown;
	int error;

	/* The titlebar shows the start page that did not open in the place of the page that says why, and a load under way's location in the place of the page's. */
	shown = shell_failure_shown(state);
	url = shell_string(browser_view_url(state->view));
	if (shown)
		url = state->failure_location;
	if (state->loading[0] != '\0')
		url = state->loading;

	/* The titlebar; a refusal is reported and the titlebar stays as it was. */
	can_back = browser_view_can_go(state->view, -1);
	can_forward = browser_view_can_go(state->view, 1);
	error = shell_titlebar_show(&state->titlebar, can_back, can_forward, url);
	if (error != 0) {
		printf("ZBROWSER ERROR titlebar-state error=%d\n", error);
		fflush(stdout);
	}
}

/* Gives one input of the window to the view: the pointer, the wheel, a key, or the keyboard's focus. */
static void
shell_input(
	struct shell_state *state,
	const struct shell_event *event)
{
	uint32_t modifiers;
	int distance_x;
	int distance_y;
	int taken;
	int error;

	/* While the question about a page that does not respond shows, its card takes the input, and the keys it leaves go to the shortcuts only. */
	if (state->prompt.shown) {
		taken = shell_prompt_input(&state->prompt, state->view, &state->render, event, shell_clock());
		if (taken)
			return;
		if (event->type == SHELL_EVENT_KEY) {
			(void)shell_shortcut(state, event);
			return;
		}
	}

	/* The modifiers held, as the view names them. */
	modifiers = shell_key_modifiers(event->modifiers);

	/* Each kind of input to its call. */
	error = 0;
	switch (event->type) {
	case SHELL_EVENT_BUTTON:
		shell_button(state, event);
		break;
	case SHELL_EVENT_MOTION:
		/* A left press moved far enough asks for the image under it, which is dragged out of the window when there is one (ws189-p003). */
		distance_x = abs(event->x - state->drag_press_x);
		distance_y = abs(event->y - state->drag_press_y);
		if (state->drag_armed &&
		    (distance_x > SHELL_DRAG_DISTANCE ||
		     distance_y > SHELL_DRAG_DISTANCE)) {
			state->drag_armed = 0;
			error = browser_view_request_image(state->view, (float)state->drag_press_x, (float)state->drag_press_y);
			if (error == 0)
				state->drag_asked = 1;
		}

		/* The pointer's move, the page's. */
		error = browser_view_pointer_move(state->view, (float)event->x, (float)event->y, modifiers);
		break;
	case SHELL_EVENT_LEAVE:
		error = browser_view_pointer_leave(state->view);
		break;
	case SHELL_EVENT_SCROLL:
		error = browser_view_wheel(
			state->view,
			(float)event->x,
			(float)event->y,
			(float)event->scroll_x,
			(float)event->scroll,
			modifiers);
		break;
	case SHELL_EVENT_KEY:
		shell_key(state, event);
		break;
	case SHELL_EVENT_FOCUS:
		error = browser_view_focus(state->view, event->pressed);
		break;
	case SHELL_EVENT_TEXT_COMMIT:
	case SHELL_EVENT_TEXT_PREEDIT:
	case SHELL_EVENT_TEXT_DELETE:
		error = shell_text(state, event);
		break;
	case SHELL_EVENT_OPEN:
		/* A later start's first URL opens here, as if typed (the window has one page); the list is freed. */
		if (event->data != NULL && event->data[0] != '\0') {
			printf("ZBROWSER OPEN handed=%s\n", event->data);
			fflush(stdout);
			shell_follow_typed(state, event->data);
		}
		free(event->data);
		break;
	default:
		break;
	}

	/* A failure of the page's scripts or its layout is reported and the window goes on. */
	if (error != 0) {
		printf("ZBROWSER ERROR input error=%s\n", wb_os_error_text(error));
		fflush(stdout);
	}
}

/* Gives the view a pointer button pressed or let go (the view makes the click of a press and a release). */
static void
shell_button(
	struct shell_state *state,
	const struct shell_event *event)
{
	uint32_t modifiers;
	int button;
	int error;

	/* A left press may become a drag of the image under it; any release ends that, and a drag asked for that has not started. */
	state->drag_armed = 0;
	state->drag_asked = 0;
	if (event->pressed && event->button == SHELL_BUTTON_LEFT) {
		state->drag_armed = 1;
		state->drag_press_x = event->x;
		state->drag_press_y = event->y;
	}

	/* The DOM's number of the button; a button it does not number is not given. */
	button = shell_key_button(event->button);
	if (button < 0)
		return;

	/* The button, at the pointer's place. */
	modifiers = shell_key_modifiers(event->modifiers);
	error = browser_view_pointer_button(state->view, (float)event->x, (float)event->y, button, event->pressed, modifiers);
	if (error != 0) {
		printf("ZBROWSER ERROR click error=%s\n", wb_os_error_text(error));
		fflush(stdout);
	}
}

/* Gives the view a key pressed, repeated or let go, unless it is one of the shell's own shortcuts. */
static void
shell_key(
	struct shell_state *state,
	const struct shell_event *event)
{
	struct shell_key_names names;
	uint32_t modifiers;
	int taken;
	int error;

	/* The shell's own shortcuts never reach the page. */
	taken = shell_shortcut(state, event);
	if (taken)
		return;

	/* The key with the DOM's names. */
	shell_key_names(event->key, event->modifiers, event->text, &names);
	modifiers = shell_key_modifiers(event->modifiers);
	error = browser_view_key(state->view, names.key, names.code, names.text, event->pressed, event->repeat, modifiers);
	if (error != 0) {
		printf("ZBROWSER ERROR key error=%s\n", wb_os_error_text(error));
		fflush(stdout);
	}
}

/* Gives the view an input method's text: committed, composed, or the bytes it deletes around the caret (ws090-p025). */
static int
shell_text(
	struct shell_state *state,
	const struct shell_event *event)
{
	int error;

	/* Each kind to its call; what is committed is for the field the window's text input was asked for (another field's text is dropped). */
	switch (event->type) {
	case SHELL_EVENT_TEXT_COMMIT:
		error = browser_view_commit_text_in(state->view, state->text_session, event->text, 0U, 0U);
		break;
	case SHELL_EVENT_TEXT_PREEDIT:
		error = browser_view_compose(state->view, event->text, event->begin, event->end);
		break;
	default:
		error = browser_view_commit_text_in(state->view, state->text_session, "", event->before, event->after);
		break;
	}

	/* A failure of the page's scripts is the caller's to report. */
	if (error != 0)
		return error;

	/* Succeeded: the view took it. */
	return 0;
}

/*
 * Asks for the window's text input while the page's focused element takes
 * an input method's text, and tells where its caret is (each time the view
 * published a new state).
 */
static void
shell_text_input(
	struct shell_state *state)
{
	char text[KL_TEXT_SURROUNDING_MAX];
	float caret[4];
	uint64_t session;
	size_t cursor;
	unsigned hints;
	int purpose;
	int wanted;
	int known;

	/*
	 * Another field, another page, or a composing the page ended itself (a
	 * click put it into the value, ws177-p019): the text input is turned
	 * off first, so that the input method is deactivated and drops what it
	 * was composing, and on again below for the field that has the focus.
	 */
	session = browser_view_text_session(state->view);
	if (session != state->text_session) {
		kl_window_text_input(state->window.kui, 0);
		state->text_session = session;
	}

	/* On while the focus is in a field or a textarea, off otherwise. */
	wanted = browser_view_text_target(state->view, caret);
	kl_window_text_input(state->window.kui, wanted);
	if (!wanted)
		return;

	/* Where its caret is, for the candidates and the on-screen keyboard. */
	kl_window_text_cursor(state->window.kui, (int)caret[0], (int)caret[1], (int)caret[2], (int)caret[3]);

	/*
	 * Its text around the caret and what it is for (ws177-p019): the
	 * browser's purposes and hints are text-input-v3's numbers, as
	 * libkeiland's are.
	 */
	known = browser_view_text_context(state->view, text, sizeof(text), &cursor, &purpose, &hints);
	if (!known)
		return;
	kl_window_text_context(state->window.kui, text, cursor, cursor, hints, (unsigned)purpose);
}

/*
 * Carries out the shell's own shortcuts: Ctrl+Q and Ctrl+W close the
 * window, and Ctrl+L edits the location.  Returns whether the key was one
 * (its press and its release are both the shell's).
 */
static int
shell_shortcut(
	struct shell_state *state,
	const struct shell_event *event)
{
	int error;

	/* Only keys with Control are shortcuts. */
	if ((event->modifiers & SHELL_MOD_CTRL) == 0U)
		return 0;

	/* The keys that close the window, when pressed. */
	if (event->key == SHELL_KEY_Q || event->key == SHELL_KEY_W) {
		if (event->pressed)
			state->window.closed = 1;
		return 1;
	}

	/* The key that edits the location, when pressed. */
	if (event->key == SHELL_KEY_L) {
		if (event->pressed) {
			error = shell_titlebar_edit_location(&state->titlebar);
			if (error != 0)
				printf("ZBROWSER ERROR location-edit error=%d\n", error);
		}

		/* Its release is the shell's too. */
		return 1;
	}

	/* Any other key with Control is the page's. */
	return 0;
}

/* Carries out what was done with the titlebar: back, forward, reload, or the location clicked or edited. */
static void
shell_titlebar_input(
	struct shell_state *state,
	const struct shell_titlebar_event *event)
{
	int error;

	/* The location's editing ended: Enter opens the URL typed; Esc and leaving change nothing. */
	if (event->kind == SHELL_TITLEBAR_DONE) {
		if (event->id != SHELL_CONTROL_LOCATION)
			return;
		if (event->detail != KL_TEXT_SUBMITTED)
			return;
		shell_follow_typed(state, event->text);
		return;
	}

	/* A control chosen. */
	switch (event->id) {
	case SHELL_CONTROL_BACK:
		shell_go(state, -1);
		break;
	case SHELL_CONTROL_FORWARD:
		shell_go(state, 1);
		break;
	case SHELL_CONTROL_RELOAD:
		shell_reload(state);
		break;
	case SHELL_CONTROL_CODE:
		/* The sign-in code offered, typed into the page (WS169 p005); the titlebar shows the state again. */
		shell_mail_fill(&state->mail, state->view, &state->titlebar);
		shell_show_state(state);
		break;
	case SHELL_CONTROL_LOCATION:
		/* A part of the location turns it into the URL's field. */
		error = shell_titlebar_edit_location(&state->titlebar);
		if (error != 0)
			printf("ZBROWSER ERROR location-edit error=%d\n", error);
		break;
	default:
		break;
	}
}

/* Shows the page some steps back or forward, or the same page again (0, a reload). */
static void
shell_go(
	struct shell_state *state,
	int steps)
{
	/* A failure was reported by the load callback; the history stays where it was. */
	(void)browser_view_go(state->view, steps);
}

/*
 * Reloads the page shown; the page that says why the start page did not
 * open tries that start page again instead (BUG-320), resolved as the
 * command line's was.
 */
static void
shell_reload(
	struct shell_state *state)
{
	int shown;

	/* The start page again in the place of the page that says why it did not open (a failure is reported by the load callback). */
	shown = shell_failure_shown(state);
	if (shown) {
		(void)browser_view_load(state->view, state->failure_location);
		return;
	}

	/* Any other page, from the history's step shown. */
	shell_go(state, 0);
}

/* Opens a typed location, resolved against the page shown, as a new step. */
static void
shell_follow(
	struct shell_state *state,
	const char *target)
{
	int error;

	/* The view's navigation; a location that does not resolve is reported here. */
	error = browser_view_follow(state->view, target);
	if (error == EINVAL) {
		printf("ZBROWSER ERROR follow target=%s error=%s\n", target, wb_os_error_text(error));
		fflush(stdout);
	}
}

/*
 * Tells whether a typed location is an absolute path: from the root
 * ("/usr/..."), a drive's ("C:\x" or "C:/x") or a network share's
 * ("\\host\x").  Both forms are paths on every system; nobody types a
 * URL of that shape.
 */
static int
shell_path_is_absolute(
	const char *typed)
{
	/* From the root. */
	if (typed[0] == '/')
		return 1;

	/* A network share: two separators. */
	if (typed[0] == '\\' && (typed[1] == '\\' || typed[1] == '/'))
		return 1;

	/* A drive: a letter, a colon and a separator. */
	if (isalpha((unsigned char)typed[0]) && typed[1] == ':' && (typed[2] == '\\' || typed[2] == '/'))
		return 1;

	/* Anything else is resolved against the page. */
	return 0;
}

/*
 * Opens what was typed into the URL's field (BUG-240): an absolute path
 * (shell_path_is_absolute) is a local file whatever page is shown, so it
 * opens as file://; anything else is resolved against the page as before.
 */
static void
shell_follow_typed(
	struct shell_state *state,
	const char *typed)
{
	static char location[sizeof("file://") + SHELL_LOCATION_MAX + 1U];
	size_t index;
	int written;

	/* Anything but an absolute path: as typed. */
	if (!shell_path_is_absolute(typed)) {
		shell_follow(state, typed);
		return;
	}

	/* An absolute path: a local file, not a path of the page's site.  A drive's path is /C:/x with its separators turned, a network share's is //host/x. */
	if (typed[0] == '/') {
		written = snprintf(location, sizeof(location), "file://%s", typed);
	} else if (typed[0] == '\\') {
		written = snprintf(location, sizeof(location), "file:%s", typed);
	} else {
		written = snprintf(location, sizeof(location), "file:///%s", typed);
	}
	if (written < 0)
		location[0] = '\0';
	if (typed[0] != '/') {
		for (index = 0; location[index] != '\0'; index++) {
			if (location[index] == '\\')
				location[index] = '/';
		}
	}
	printf("ZBROWSER location typed=path url=%s\n", location);
	shell_follow(state, location);
}

/*
 * Starts the drawing thread at the window's size: with the presenter's GPU,
 * whose Wayland configures of a new size it acknowledges itself before the
 * first frame of the size (and on Windows hears every size of a resize's
 * modal loop), or with the CPU.  Returns 0, or 1 after saying why not.
 */
static int
shell_render_up(
	struct shell_state *state)
{
	struct shell_present *present;
	int error;

	/* The size to draw at. */
	shell_render_resize(&state->render, state->width, state->height, 0, 0U);

	/* The GPU's frames are presented by the drawing thread: the window leaves a new size's configure to it. */
	present = NULL;
	if (!state->use_cpu) {
		present = &state->present;
		error = kl_window_set_configure_deferred(state->window.kui, 1);
		if (error != 0 && error != ENOTSUP) {
			printf("ZBROWSER ERROR configure-deferred error=%d\n", error);
			fflush(stdout);
		}
	}

	/* The sizes of a resize the window's thread is held in (Windows' modal loop). */
	kl_window_on_resize(state->window.kui, shell_resize_live, state);

	/* The thread, whose acknowledgements the window carries out. */
	state->window.render = &state->render;
	error = shell_render_start(&state->render, state->view, state->window.kui, present, shell_wake, state);
	if (error != 0) {
		state->window.render = NULL;
		fprintf(stderr, "browser: cannot start drawing: %s\n", wb_os_error_text(error));
		return 1;
	}

	/* Succeeded: the page is drawn from now on. */
	return 0;
}

/*
 * Carries out what the drawing thread did since: the CPU's frame drawn
 * last, shown in the window, and the GPU's frames presented, told in FRAME
 * lines.  Returns 0, 1 when the drawing failed (the window closes), or 2
 * when a frame waits for the window to take it (the caller tries again
 * soon).
 */
static int
shell_frames(
	struct shell_state *state)
{
	unsigned long shown;
	uint32_t width;
	uint32_t height;
	int failed;
	int status;

	/* How the drawing goes. */
	failed = shell_render_status(&state->render, &shown, &width, &height);
	if (failed)
		return 1;

	/* The CPU's frame, shown here (libkeiland's shared memory is the window's thread's). */
	if (state->use_cpu) {
		status = shell_cpu_frame(state);
		if (status != 0)
			return status;
		return 0;
	}

	/* The GPU's frames, presented by the drawing thread: the line the tests read, for the last. */
	if (shown != state->frames_seen) {
		state->frames_seen = shown;
		shell_frame_line(state, width, height);
	}

	/* Succeeded: the window shows what was drawn. */
	return 0;
}

/*
 * Shows the CPU's frame the drawing thread drew last, unless it is of an
 * old size.  Returns 0, 1 on failure, or 2 when the window cannot take the
 * frame yet (the compositor still holds the memory).
 */
static int
shell_cpu_frame(
	struct shell_state *state)
{
	const struct shell_cpu_frame *frame;
	int dropped;
	int fresh;
	int error;

	/* The frame drawn last, or the one that waits still. */
	frame = shell_render_cpu_frame(&state->render, &fresh);
	if (fresh)
		state->cpu_waiting = 1;
	if (!state->cpu_waiting)
		return 0;

	/* Shown; an old size's is dropped (the drawing thread draws the new size). */
	error = shell_cpu_show(&state->cpu, &state->window, frame, &dropped);
	if (error == EAGAIN)
		return 2;
	state->cpu_waiting = 0;
	if (error != 0) {
		fprintf(stderr, "browser: cannot draw a frame: %s\n", wb_os_error_text(error));
		return 1;
	}

	/* Ends the round without a present when the frame was dropped. */
	if (dropped)
		return 0;

	/* The page's animation frames run on the present, and the line the tests read. */
	browser_view_presented(state->view, 0);
	shell_frame_line(state, frame->width, frame->height);

	/* Succeeded: the frame is in the window. */
	return 0;
}

/* Writes the FRAME line the tests read, for a frame shown at a size. */
static void
shell_frame_line(
	struct shell_state *state,
	uint32_t width,
	uint32_t height)
{
	/* The scroll the view published, and the frame's size. */
	printf("ZBROWSER FRAME scroll=%.0f width=%u height=%u\n", browser_view_scroll_y(state->view),
	    (unsigned)width, (unsigned)height);
	fflush(stdout);
}

/*
 * Opens the window and the presenter that shows the page in it: the
 * window's own Vulkan, or the CPU's when the options ask for it or Vulkan
 * cannot show the window (said in one line).  The titlebar's controls, the
 * arrivals of mail and the view's wakes start with the first window.
 * Returns 0, or 1 after saying why not.
 */
static int
shell_surface_open(
	struct shell_state *state,
	const struct shell_options *options)
{
	VkResult result;
	unsigned present;
	int error;
	int status;

	/* The window for the way asked for. */
	present = KL_PRESENT_NONE;
	if (options->no_gpu)
		present = KL_PRESENT_SHM;
	status = shell_window_up(state, options, present);
	if (status != 0)
		return 1;

	/* The arrivals of mail with their sign-in codes (a compositor without them leaves the browser without codes). */
	shell_mail_open(&state->mail, state->window.app);

	/* The view's wakes go to the application from now on; what came before is taken at once. */
	__atomic_store_n(&state->wake_app, state->window.app, __ATOMIC_RELEASE);
	browser_view_process(state->view);

	/* The Vulkan presenter in the window. */
	if (!options->no_gpu) {
		result = shell_present_open(&state->present, &state->window, state->view);
		if (result == VK_SUCCESS) {
			state->width = state->present.extent.width;
			state->height = state->present.extent.height;
			return 0;
		}

		/* Vulkan cannot show this window: its objects go, and a window of the CPU's way replaces it. */
		fprintf(stderr, "browser: cannot draw with the GPU: %s failed (%d): drawing with the CPU\n", state->present.operation, (int)result);
		shell_present_close(&state->present, state->view);
		shell_titlebar_close(&state->titlebar);
		shell_window_drop(&state->window);
		status = shell_window_up(state, options, KL_PRESENT_SHM);
		if (status != 0)
			return 1;
	}

	/* The CPU presenter's memory at the window's size. */
	state->use_cpu = 1;
	error = shell_cpu_open(&state->cpu, &state->window);
	if (error != 0) {
		fprintf(stderr, "browser: cannot draw in the window: %s\n", wb_os_error_text(error));
		return 1;
	}
	state->width = state->cpu.width;
	state->height = state->cpu.height;

	/* Succeeded: pages can be shown. */
	return 0;
}

/* Makes the window for a way of showing frames, with the page's title and the compositor's titlebar. Returns 0, or 1 after saying why not. */
static int
shell_window_up(
	struct shell_state *state,
	const struct shell_options *options,
	unsigned present)
{
	const char *title;
	int status;
	int error;

	/* The page's title, or the program's name before the page has one. */
	title = shell_string(browser_view_title(state->view));
	if (title[0] == '\0')
		title = SHELL_PRODUCT_NAME;

	/* The window, with the title. */
	status = shell_window_open(&state->window, options->width, options->height, title, present);
	if (status != 0) {
		fprintf(stderr, "browser: cannot open a window: %s\n", wb_os_error_text(errno));
		return 1;
	}

	/* The compositor's titlebar with the browser's controls (a compositor without it leaves the plain titlebar). */
	error = shell_titlebar_open(&state->titlebar, &state->window);
	if (error != 0) {
		printf("ZBROWSER ERROR titlebar error=%d\n", error);
		shell_titlebar_close(&state->titlebar);
	}

	/* Succeeded: the window is up. */
	return 0;
}

/*
 * Gives the drawing thread the window's new size: the CPU's frames at the
 * size libkeiland shows, or the swapchain at the window's with the
 * configure that waits for the first frame of it (Wayland).  Says why on
 * failure.  Returns 0, or 1.
 */
static int
shell_surface_resize(
	struct shell_state *state)
{
	struct kl_window_configure configure;
	int taken;
	int error;

	/* The CPU's frames at the new size, shown by this thread. */
	if (state->use_cpu) {
		error = shell_cpu_resize(&state->cpu, &state->window);
		if (error != 0) {
			fprintf(stderr, "browser: cannot resize: %s\n", wb_os_error_text(error));
			return 1;
		}
		state->width = state->cpu.width;
		state->height = state->cpu.height;
		shell_render_resize(&state->render, state->width, state->height, 0, 0U);
		return 0;
	}

	/* The configure of the new size, which the drawing thread acknowledges (none on a system without configures). */
	memset(&configure, 0, sizeof(configure));
	taken = 0;
	error = kl_window_configure_take(state->window.kui, &configure);
	if (error == 0)
		taken = 1;

	/* The swapchain at the window's size, made by the drawing thread. */
	state->width = state->window.width;
	state->height = state->window.height;
	shell_render_resize(&state->render, state->width, state->height, taken, configure.serial);

	/* Succeeded: the next frame is shown at the new size. */
	return 0;
}

/*
 * The window's size during a resize that holds the window's thread
 * (Windows' modal loop, kl_window_on_resize), on the window's thread: the
 * page laid out at it and drawn at it by the drawing thread (the GPU's
 * way; the CPU's frames are shown by the window's thread, after the loop).
 */
static void
shell_resize_live(
	void *data,
	uint32_t width,
	uint32_t height)
{
	struct shell_state *state;

	/* The view lays the page out at the size (a command queued for its thread). */
	state = data;
	(void)browser_view_resize(state->view, width, height);

	/* The drawing thread draws at it, with the question's card placed in it. */
	if (state->use_cpu)
		return;
	state->width = width;
	state->height = height;
	shell_render_resize(&state->render, width, height, 0, 0U);
	shell_prompt_resize(&state->prompt, &state->render, width, height, shell_clock());
}

/*
 * Makes the engine's instance over the files backend in the program's data
 * directory (kl_app_storage_directory: $XDG_DATA_HOME/keiland/browser, or
 * ~/.local/share/keiland/browser, where the localStorage of earlier versions
 * is), wrapped as the system wraps it (shell_storage_wrap).  When the
 * directory cannot be used (no home, or another browser holds it), the
 * instance keeps nothing and one line says why.  Returns 0
 * or the instance's error.
 */
static int
shell_instance(
	struct kl_app *app,
	struct browser_instance **instance)
{
	struct browser_instance_options options;
	const struct browser_storage_calls *calls;
	char directory[SHELL_DIRECTORY_MAX];
	void *context;
	int error;

	/* The data directory of the application. */
	error = kl_app_storage_directory(app, directory, sizeof(directory));

	/* The files backend over it; without it the instance remembers nothing. */
	calls = NULL;
	context = NULL;
	if (error != 0) {
		fprintf(stderr, "browser: no data directory: cookies and localStorage are not kept\n");
	} else {
		error = browser_storage_files_open(directory, &calls, &context);
		if (error != 0) {
			fprintf(stderr, "browser: cannot use %s: %s: cookies and localStorage are not kept\n", directory, wb_os_error_text(error));
			calls = NULL;
			context = NULL;
		}
	}

	/* The system's wrapping of the backend (the cookies encrypted on Windows). */
	if (calls != NULL) {
		error = shell_storage_wrap(&calls, &context);
		if (error != 0)
			fprintf(stderr, "browser: cannot protect the storage: %s: cookies and localStorage are not kept\n", strerror(error));
	}

	/* The instance, which owns the backend from now on. */
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.storage = calls;
	options.storage_context = context;
	error = browser_instance_create(&options, instance);
	if (error != 0)
		return error;

	/* Succeeded: the views may be made with it. */
	return 0;
}

/*
 * Releases everything, in this order: the diagnostic clock, the drawing
 * thread (stopped while the window's events are still taken), the
 * presenter whose surface is the window's, the titlebar, the window
 * itself, the view and its instance, and the application (once no thread
 * of the view posts to it).
 */
static void
shell_release(
	struct shell_state *state)
{
	/* The diagnostic clock, which says what it measured. */
	shell_latency_stop(&state->latency);
	state->window.latency = NULL;

	/* The drawing thread, stopped before what it draws with goes, and the question it drew the card of. */
	shell_render_stop(&state->render, &state->window);
	state->window.render = NULL;
	shell_prompt_close(&state->prompt);

	/* The presenter before the window whose surface it draws. */
	shell_present_close(&state->present, state->view);
	shell_cpu_close(&state->cpu);

	/* The titlebar before the window it belongs to. */
	shell_titlebar_close(&state->titlebar);

	/* The offer of a sign-in code, before the application's system goes with the window. */
	if (state->window.app != NULL)
		shell_mail_close(&state->mail, &state->titlebar);

	/* The window goes from the screen first; the application stays while the view's threads may post to it. */
	shell_window_drop(&state->window);

	/* The view, with its page, its history, its threads and its network, and the fingers that moved it. */
	if (state->view != NULL)
		browser_view_destroy(state->view);
	state->view = NULL;
	shell_touch_close(&state->touch);

	/* What the drawing thread kept, once the view that woke it is gone. */
	shell_render_close(&state->render);

	/* The instance once its view is gone, which writes what is left to save. */
	if (state->instance != NULL)
		(void)browser_instance_destroy(state->instance);
	state->instance = NULL;

	/* The application, once nothing posts to it. */
	if (state->window.app != NULL)
		shell_window_close(&state->window);

	/* The URL of the page that said why the start page did not open, which no view shows any more. */
	free(state->failure_page);
	state->failure_page = NULL;
}

/* The view's callback: a script changed the title, which the window shows. */
static void
shell_title(
	void *context,
	struct browser_view *view,
	const char *title)
{
	struct shell_state *state;

	/* The window's title, once the window is up. */
	UNUSED_PARAMETER(view);
	state = context;
	if (state->ready)
		shell_window_title(&state->window, shell_string(title));
}

/* The view's callback: a page became the one shown (the first page is shown once the window is up). */
static void
shell_committed(
	void *context,
	struct browser_view *view)
{
	struct shell_state *state;

	/* A page is shown: a later failure is the page's, not the start's. */
	UNUSED_PARAMETER(view);
	state = context;
	state->started = 1;

	/* The page that says why the start page did not open, shown: its location as the view keeps it tells later that it is shown. */
	if (state->failure_wanted) {
		state->failure_wanted = 0;
		free(state->failure_page);
		state->failure_page = strdup(shell_string(browser_view_url(state->view)));
	}

	/* The title, the NAVIGATE line and the titlebar (with the page's location: no load is under way now). */
	state->loading[0] = '\0';
	if (state->ready)
		shell_show_state(state);

	/* Another page, whose scroll the fingers take over. */
	state->page_number++;
}

/* The view's callback: a load started (LOADING), was stopped (STOPPED) or failed (ERROR). */
static void
shell_load(
	void *context,
	struct browser_view *view,
	enum browser_load_state state,
	const char *url,
	int error,
	const char *reason)
{
	struct shell_state *shell;
	int failed;

	/* The line the tests read. */
	UNUSED_PARAMETER(view);
	shell = context;
	if (state == BROWSER_LOAD_STARTED)
		printf("ZBROWSER LOADING url=%s\n", url);
	if (state == BROWSER_LOAD_STOPPED)
		printf("ZBROWSER STOPPED url=%s\n", url);
	if (state == BROWSER_LOAD_FAILED)
		printf("ZBROWSER ERROR load path=%s error=%s tls=%s\n", url, wb_os_error_text(error), reason);
	fflush(stdout);

	/* The reason of a failure, kept for the page that says why the start page did not open (cut to what fits). */
	if (state == BROWSER_LOAD_FAILED)
		snprintf(shell->load_failure, sizeof(shell->load_failure), "%s", reason);

	/* The start page failed before any page was shown: the page that says why, once (BUG-320). */
	if (state == BROWSER_LOAD_FAILED && !shell->started && shell->failure_location == NULL) {
		failed = shell_open_failure(shell, shell->start, error);
		if (failed != 0) {
			printf("ZBROWSER ERROR failure-page error=%s\n", wb_os_error_text(failed));
			fflush(stdout);
		}
	}

	/* The titlebar shows the location of a load as it starts, and the page's again when it stops or fails. */
	shell->loading[0] = '\0';
	if (state == BROWSER_LOAD_STARTED)
		snprintf(shell->loading, sizeof(shell->loading), "%s", url);
	if (shell->ready)
		shell_show_titlebar(shell);
}

/* The view's callback: a click opens a link, which the shell allows (and writes the LINK line). */
static enum browser_policy
shell_link(
	void *context,
	struct browser_view *view,
	const char *href)
{
	/* The line the tests read. */
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(view);
	printf("ZBROWSER LINK href=%s\n", href);
	fflush(stdout);

	/* The view follows it. */
	return BROWSER_POLICY_ALLOW;
}

/* The view's callback: a page's console line, as a CONSOLE line of the diagnostic interface. */
static void
shell_console(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	/* The level's number and the text. */
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(view);
	printf("ZBROWSER CONSOLE level=%d %.*s\n", level, (int)length, text);
	fflush(stdout);
}

/*
 * The view's wake, on the view's threads, and the drawing thread's: posts
 * an event to the application, which ends the main loop's wait (the loop
 * takes what the view published and what the drawing thread drew after
 * each dispatch).  Before the window is up, or when the post fails,
 * the main loop is told to run the view without waiting.
 */
static void
shell_wake(
	void *context)
{
	struct shell_state *state;
	struct kl_app *app;
	int error;

	/* The application, once the window is up. */
	state = context;
	app = __atomic_load_n(&state->wake_app, __ATOMIC_ACQUIRE);
	if (app == NULL) {
		__atomic_store_n(&state->wake_missed, 1, __ATOMIC_RELEASE);
		return;
	}

	/* The post that wakes the main loop. */
	error = kl_app_post(app, SHELL_POST_VIEW, NULL);
	if (error != 0)
		__atomic_store_n(&state->wake_missed, 1, __ATOMIC_RELEASE);
}

/* The view's callback: a timer's script failed. */
static void
shell_script_error(
	void *context,
	struct browser_view *view,
	int error)
{
	/* The line the tests read. */
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(view);
	printf("ZBROWSER ERROR script error=%s\n", wb_os_error_text(error));
	fflush(stdout);
}

/* The view's callback: the answer about the page's field for one-time codes, which mail asked for. */
static void
shell_field(
	void *context,
	struct browser_view *view,
	int error,
	const float *caret)
{
	struct shell_state *state;

	/* The code typed into the field, or put on the clipboard; the titlebar shows the state again. */
	UNUSED_PARAMETER(caret);
	state = context;
	shell_mail_field(&state->mail, view, &state->titlebar, error);
	shell_show_state(state);
}

/* Reports a string the view published, or "" before it published one. */
static const char *
shell_string(
	const char *string)
{
	/* Nothing published yet. */
	if (string == NULL)
		return "";

	/* Succeeded: the string. */
	return string;
}

/*
 * Runs the fingers for one round (ws081-p006): gives them the page's scroll
 * and range, their events and the time, places the scroll and the
 * overscroll they set, and gives the view the clicks they made.  With no
 * finger down and nothing gliding there is nothing to do.
 */
static void
shell_touch_round(
	struct shell_state *state)
{
	struct shell_touch_pointer made;
	double largest_x;
	double largest_y;
	double scroll;
	double overscroll;
	unsigned index;
	int moved;
	int taken;
	int error;

	/* Nothing touches and nothing glides. */
	if (state->window.touch_count == 0U && state->touch_due < 0)
		return;

	/* The page as it is: its scroll, how far it scrolls, and the view's height. */
	error = browser_view_scroll_range(state->view, &largest_x, &largest_y);
	if (error != 0)
		largest_y = 0.0;
	shell_touch_layout(&state->touch, state->page_number, browser_view_scroll_y(state->view), largest_y,
	    (double)state->height);

	/* The fingers' events. */
	for (index = 0; index < state->window.touch_count; index++)
		shell_touch_event(&state->touch, &state->window.touches[index]);

	/* The queue is empty again: the window fills it from the next read. */
	state->window.touch_count = 0U;

	/* Time moves on for them, and they say when they want the next round. */
	state->touch_due = shell_touch_tick(&state->touch, shell_touch_clock());

	/* The scroll and the stretch they set (the view asks to be drawn again when they change). */
	moved = shell_touch_scroll(&state->touch, &scroll, &overscroll);
	if (moved) {
		(void)browser_view_scroll_to(state->view, 0.0, scroll);
		(void)browser_view_set_overscroll(state->view, 0.0, overscroll);
	}

	/* The pointer's events they made, in order. */
	for (;;) {
		taken = shell_touch_take_pointer(&state->touch, &made);
		if (!taken)
			break;
		shell_touch_pointer(state, &made);
	}
}

/* Gives the view one pointer event the fingers made: the pointer moves there, or a button is pressed or let go. */
static void
shell_touch_pointer(
	struct shell_state *state,
	const struct shell_touch_pointer *made)
{
	uint32_t modifiers;
	int pressed;
	int error;

	/* The modifiers held on the keyboard. */
	modifiers = shell_key_modifiers(state->window.modifiers);

	/* A motion, or a button. */
	if (made->kind == SHELL_TOUCH_POINTER_MOTION) {
		error = browser_view_pointer_move(state->view, made->x, made->y, modifiers);
	} else {
		pressed = 0;
		if (made->kind == SHELL_TOUCH_POINTER_PRESS)
			pressed = 1;
		error = browser_view_pointer_button(state->view, made->x, made->y, made->button, pressed, modifiers);
	}

	/* A failure of the page's scripts or its layout is reported and the window goes on. */
	if (error != 0) {
		printf("ZBROWSER ERROR touch-click error=%s\n", wb_os_error_text(error));
		fflush(stdout);
	}
}

/*
 * The view's callback, on the window's thread: the answer to the request
 * for the image under a left press moved far enough (ws189-p003).  While
 * the press is still held, an image there is dragged out of the window;
 * no image (or a press let go meanwhile) leaves the motion the page's.
 */
static void
shell_image(
	void *context,
	struct browser_view *view,
	int error,
	struct browser_image *image)
{
	struct shell_state *state;

	/* Only the answer to a press still held. */
	UNUSED_PARAMETER(view);
	state = context;
	if (!state->drag_asked)
		return;
	state->drag_asked = 0;

	/* No image there. */
	if (error != 0 || image == NULL)
		return;

	/* The drag, with the image lent for the call. */
	shell_drag_image(state, image);
}

/*
 * Drags an image out of the window (ws189-p003): its picture
 * ("image/png", at most SHELL_DRAG_SIDE on its longer side) and the URL of
 * its source as text, as a copy, with the picture under the pointer, from
 * the last press.  The drag starts first and its PNG is filled in after.
 * The image stays the caller's.
 */
static void
shell_drag_image(
	struct shell_state *state,
	const struct browser_image *image)
{
	struct kl_drag_data data[3];
	struct kl_drag_icon icon;
	unsigned char *png;
	uint32_t *fitted;
	size_t count;
	size_t size;
	int fitted_width;
	int fitted_height;
	uint32_t serial;
	int error;

	/* The picture, filled in later, and the source's URL as both text types when it has one. */
	data[0].type = "image/png";
	data[0].data = NULL;
	data[0].length = 0;
	count = 1;
	if (image->url != NULL && image->url[0] != '\0') {
		data[1].type = "text/plain;charset=utf-8";
		data[1].data = image->url;
		data[1].length = strlen(image->url);
		data[2].type = "text/plain";
		data[2].data = image->url;
		data[2].length = strlen(image->url);
		count = 3;
	}

	/* The drag from the press, a copy, the picture under the pointer at its middle. */
	icon.pixels = image->pixels;
	icon.width = image->width;
	icon.height = image->height;
	icon.hot_x = image->width / 2;
	icon.hot_y = image->height / 2;
	serial = kl_window_press_serial(state->window.kui);
	error = kl_window_start_drag_icon(state->window.kui, data, count, KL_DND_COPY, serial, &icon);
	if (error != 0) {
		printf("ZBROWSER DND drag failed errno=%d\n", error);
		fflush(stdout);
		return;
	}

	/* The PNG, shrunk, filled in. */
	png = NULL;
	size = 0;
	error = kl_picture_fit(image->pixels, image->width, image->height, (size_t)image->width, SHELL_DRAG_SIDE, &fitted, &fitted_width,
	    &fitted_height);
	if (error == 0) {
		error = kl_picture_png(fitted, fitted_width, fitted_height, (size_t)fitted_width, &png, &size);
		free(fitted);
	}

	/* Given to the drag. */
	if (error == 0)
		error = kl_window_drag_fill(state->window.kui, "image/png", png, size);
	free(png);

	/* The log line the tests read; the drag goes on. */
	printf("ZBROWSER DND drag image size=%dx%d bytes=%lu url=%s errno=%d\n", image->width, image->height, (unsigned long)size,
	    shell_string(image->url), error);
	fflush(stdout);
}

/*
 * Asks the view for a page that says why the start page did not open
 * (BUG-320): the location, the error with the view's reason, and a link
 * that tries the location again.  The page is known as shown when the
 * view commits it (shell_committed).  Returns 0 when it was asked for.
 */
static int
shell_open_failure(
	struct shell_state *state,
	const char *location,
	int failure)
{
	struct shell_text page;
	char reason[SHELL_FAILURE_MAX + 128U];
	int error;

	/* The error in words, with what the view said beyond its number (for example, the certificate's). */
	if (state->load_failure[0] != '\0') {
		snprintf(reason, sizeof(reason), "%s: %s", wb_os_error_text(failure), state->load_failure);
	} else {
		snprintf(reason, sizeof(reason), "%s", wb_os_error_text(failure));
	}

	/* The page, as a data: URL. */
	memset(&page, 0, sizeof(page));
	error = shell_failure_page(location, reason, &page);
	if (error != 0) {
		free(page.bytes);
		return error;
	}

	/* The page asked for in the start page's place. */
	error = browser_view_load(state->view, page.bytes);
	if (error != 0) {
		free(page.bytes);
		return error;
	}

	/* The URL built is the view's now. */
	free(page.bytes);

	/* The start page it stands for, which the titlebar and the reload use; the page is waited for. */
	state->failure_location = location;
	state->failure_wanted = 1;

	/* Succeeded: the window opens on the page that says why. */
	return 0;
}

/* Tells whether the view shows the page that says why the start page did not open. */
static int
shell_failure_shown(
	const struct shell_state *state)
{
	const char *url;
	int differs;

	/* No such page when the start page opened. */
	if (state->failure_page == NULL)
		return 0;

	/* The page shown, compared with it. */
	url = browser_view_url(state->view);
	if (url == NULL)
		return 0;
	differs = strcmp(url, state->failure_page);
	if (differs != 0)
		return 0;

	/* Succeeded: that page is the one shown. */
	return 1;
}

/* Builds the data: URL of the page that says why the location did not open, with the reason; nonzero on failure. */
static int
shell_failure_page(
	const char *location,
	const char *reason,
	struct shell_text *page)
{
	struct shell_text html;
	int error;

	/* The page's head and its heading. */
	memset(&html, 0, sizeof(html));
	error = shell_text_append(&html, SHELL_FAILURE_HEAD, strlen(SHELL_FAILURE_HEAD));
	if (error != 0) {
		free(html.bytes);
		return error;
	}

	/* The location, escaped so that it is text. */
	error = shell_text_escaped(&html, location);
	if (error != 0) {
		free(html.bytes);
		return error;
	}

	/* The paragraph of the reason. */
	error = shell_text_append(&html, SHELL_FAILURE_REASON, strlen(SHELL_FAILURE_REASON));
	if (error != 0) {
		free(html.bytes);
		return error;
	}

	/* The reason's words. */
	error = shell_text_escaped(&html, reason);
	if (error != 0) {
		free(html.bytes);
		return error;
	}

	/* The link that tries the location again, its target an attribute's value in quotes, escaped too. */
	error = shell_text_append(&html, SHELL_FAILURE_LINK, strlen(SHELL_FAILURE_LINK));
	if (error != 0) {
		free(html.bytes);
		return error;
	}

	/* The link's target. */
	error = shell_text_escaped(&html, location);
	if (error != 0) {
		free(html.bytes);
		return error;
	}

	/* The link's words and the end of the page. */
	error = shell_text_append(&html, SHELL_FAILURE_TAIL, strlen(SHELL_FAILURE_TAIL));
	if (error != 0) {
		free(html.bytes);
		return error;
	}

	/* The data: URL's prefix. */
	error = shell_text_append(page, SHELL_FAILURE_PREFIX, strlen(SHELL_FAILURE_PREFIX));
	if (error != 0) {
		free(html.bytes);
		return error;
	}

	/* The HTML, percent-encoded after the prefix. */
	error = shell_text_percent(page, html.bytes, html.length);
	if (error != 0) {
		free(html.bytes);
		return error;
	}

	/* The HTML is in the URL now. */
	free(html.bytes);

	/* Succeeded: the caller owns the URL's bytes. */
	return 0;
}

/* Appends bytes to a text, growing it as needed; ENOMEM when it cannot grow. */
static int
shell_text_append(
	struct shell_text *text,
	const char *bytes,
	size_t length)
{
	char *grown;
	size_t size;

	/* Room for the bytes and the NUL, doubling so that a run of appends stays linear. */
	if (text->length + length + 1U > text->size) {
		size = text->size * 2U;
		if (size < text->length + length + 1U)
			size = text->length + length + 1U;
		grown = realloc(text->bytes, size);
		if (grown == NULL)
			return ENOMEM;
		text->bytes = grown;
		text->size = size;
	}

	/* The bytes, and the NUL after them. */
	memcpy(text->bytes + text->length, bytes, length);
	text->length += length;
	text->bytes[text->length] = '\0';

	/* Succeeded: the text ends with the bytes. */
	return 0;
}

/* Appends a string to HTML as text, escaping the characters markup or an attribute's quotes would take; nonzero on failure. */
static int
shell_text_escaped(
	struct shell_text *text,
	const char *string)
{
	const char *entity;
	size_t index;
	int error;

	/* Each byte, or the entity of a special one. */
	for (index = 0; string[index] != '\0'; index++) {
		entity = NULL;
		if (string[index] == '&') {
			entity = "&amp;";
		} else if (string[index] == '<') {
			entity = "&lt;";
		} else if (string[index] == '>') {
			entity = "&gt;";
		} else if (string[index] == '"') {
			entity = "&quot;";
		}

		/* The entity in the special byte's place, or the byte itself. */
		if (entity != NULL) {
			error = shell_text_append(text, entity, strlen(entity));
		} else {
			error = shell_text_append(text, string + index, 1U);
		}

		/* A text that cannot grow ends the escaping. */
		if (error != 0)
			return error;
	}

	/* Succeeded: the string is text in the HTML. */
	return 0;
}

/* Appends bytes percent-encoded, as a data: URL's body, keeping only the unreserved characters as they are; nonzero on failure. */
static int
shell_text_percent(
	struct shell_text *text,
	const char *bytes,
	size_t length)
{
	static const char digits[] = "0123456789ABCDEF";
	unsigned char byte;
	char encoded[3];
	size_t index;
	int kept;
	int error;

	/* Each byte. */
	for (index = 0; index < length; index++) {
		byte = (unsigned char)bytes[index];

		/* A letter, a digit and - . _ ~ stand for themselves; anything else is %XX. */
		kept = 0;
		if (byte >= 'A' && byte <= 'Z') {
			kept = 1;
		} else if (byte >= 'a' && byte <= 'z') {
			kept = 1;
		} else if (byte >= '0' && byte <= '9') {
			kept = 1;
		} else if (byte == '-' || byte == '.' || byte == '_' || byte == '~') {
			kept = 1;
		}

		/* The byte, or its escape. */
		if (kept) {
			error = shell_text_append(text, bytes + index, 1U);
		} else {
			encoded[0] = '%';
			encoded[1] = digits[byte >> 4];
			encoded[2] = digits[byte & 15U];
			error = shell_text_append(text, encoded, 3U);
		}

		/* A text that cannot grow ends the encoding. */
		if (error != 0)
			return error;
	}

	/* Succeeded: the bytes are in the URL. */
	return 0;
}
