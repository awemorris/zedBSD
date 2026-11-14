/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * browser: the Web browser of the zedBSD desktop.
 *
 *   browser [--display=NAME] [--width=N] [--height=N] [--ca-file=PEM] [--no-gpu] [URL]
 *   browser --dump=dom|style|layout|paint [--width=N] [--height=N] [--font=PATH] FILE
 *   browser --run [--width=N] [--height=N] FILE
 *   browser --dump=ast [--module] [--strict] FILE.js
 *   browser --js [--strict] FILE.js
 *   browser --dump=code [--strict] FILE.js
 *   browser --render|--render-gpu --output=OUT.ppm [--width=N] [--height=N] [--font=PATH] FILE
 *   browser --version | --help
 *
 * Without a headless mode it opens a compositor window on URL, or on the
 * home page (MAIN_HOME_PAGE, https://www.google.com/); --no-gpu draws the
 * window with the CPU (as it does when Vulkan cannot show the window).  The headless
 * modes (added with the engine, one per phase) draw or dump a page, or run
 * a script, without a window; the tests use them on the host and in the
 * guest.  The modes that load a page do it through a view (<browser/browser.h>),
 * as the window does: the page's scripts run, its timers run on a virtual
 * clock up to --settle-ms (5000 by default, browser_view_settle), and the page is
 * drawn with the CPU (browser_view_draw_pixels), with the GPU into an
 * offscreen image read back (browser_offscreen, --render-gpu), or dumped
 * (browser_view_dump).  --run writes the page's console to standard
 * output (the other modes write it to standard error).  Every mode reports
 * failure with a non-zero exit status and one line on standard error.
 *
 * --async fetches the page and its images without blocking, through the
 * view's loader, and waits for them; without it they are read at once.
 *
 * --follow=N carries out up to N navigations the page's scripts ask for
 * (a location set, a reload, a form's submit) as the window does, each
 * page settled in turn; without it the first page is the one used.
 *
 * --threads runs the page on the view's own threads (BROWSER_THREADING_OWN),
 * as the window does, instead of in the program's calls: the settle runs
 * the page on its own clock until it is at rest, a thread of the
 * program's stands in for the window's drawing (it draws each committed
 * frame and tells of its present, when the page's animation frames run),
 * and the picture is drawn from the committed frame.
 *
 * Since ws074-p057 the engine is libbrowser.so and the program uses it
 * through <browser/browser.h> only: the headless modes through a view, the
 * script modes (--js, --dump=ast, --dump=code) through the library's
 * script tools, and the window through shell/.
 * --ca-file (in any mode that loads a page) trusts the CA certificates of
 * a PEM file besides the system's roots for https (the tests' own CA).
 */

#include "browser-main.h"
#include "shell/shell.h"
#include "../libbrowser/base/os-error.h"

#include <browser/browser.h>

#include "userland/desktop/paths.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The version the program reports. */
#define MAIN_VERSION		"0.1 (ws074)"

/* The window size used unless told otherwise, in pixels. */
#define MAIN_DEFAULT_WIDTH	1024U
#define MAIN_DEFAULT_HEIGHT	768U

/* The largest window size accepted on the command line, in pixels. */
#define MAIN_MAX_SIZE		16384UL

/* The page the window opens on when none is named (ws074-p228, 2026-10-14 user). */
#define MAIN_HOME_PAGE		"https://www.google.com/"

/*
 * How long the headless modes let a page's timers run, in virtual
 * milliseconds (the budget the Chromium references are made with).
 */
#define MAIN_SETTLE_BUDGET	5000.0

/* The largest explicit headless virtual-time budget, in milliseconds. */
#define MAIN_MAX_SETTLE_BUDGET	60000UL

/*
 * What the program was asked to do.
 */
enum main_mode {
	MAIN_MODE_WINDOW,
	MAIN_MODE_VERSION,
	MAIN_MODE_HELP,
	MAIN_MODE_DUMP_DOM,
	MAIN_MODE_DUMP_STYLE,
	MAIN_MODE_DUMP_LAYOUT,
	MAIN_MODE_DUMP_PAINT,
	MAIN_MODE_DUMP_AST,
	MAIN_MODE_RUN_JS,
	MAIN_MODE_DUMP_CODE,
	MAIN_MODE_RENDER,
	MAIN_MODE_RENDER_GPU,
	MAIN_MODE_RUN_PAGE
};

/*
 * What the command line asked for.
 */
struct main_options {
	enum main_mode mode;
	struct shell_options shell;
	struct browser_fonts fonts;
	const char *output;
	unsigned parse;
	double settle_budget;
	int async;
	int threads;
	unsigned follow;
};

/*
 * The page of a headless mode: the command line, the view that holds the
 * page, and whether the load failed (the load callback said why).  It
 * lives in the mode's function for as long as the view.
 *
 * With --threads: the program's threads (table), the stand-in for the
 * window's drawing thread (drawer, NULL when it does not run) with its
 * lock and condition, and under the lock whether a new frame was committed
 * (woken) and whether the drawer is to end (ending); allowed counts the
 * navigations of the page's scripts the link callback let through (on the
 * view's script thread, atomically), settled those a settle followed.
 */
struct main_page {
	const struct main_options *options;
	struct browser_view *view;
	int failed;
	int output_failed;
	unsigned started;
	const struct browser_threads *table;
	void *drawer;
	void *lock;
	void *cond;
	int woken;
	int ending;
	unsigned allowed;
	unsigned settled;
};

/*
 * One headless dump: the name --dump= takes and the mode it selects.
 */
struct main_dump_name {
	const char *name;
	enum main_mode mode;
};

/*
 * The dumps --dump= knows, ending with a NULL name.  The table is constant
 * for the life of the program.
 */
static const struct main_dump_name main_dumps[] = {
	{ "dom", MAIN_MODE_DUMP_DOM },
	{ "style", MAIN_MODE_DUMP_STYLE },
	{ "layout", MAIN_MODE_DUMP_LAYOUT },
	{ "paint", MAIN_MODE_DUMP_PAINT },
	{ "ast", MAIN_MODE_DUMP_AST },
	{ "code", MAIN_MODE_DUMP_CODE },
	{ NULL, MAIN_MODE_WINDOW }
};

static int main_parse(int argc, char **argv, struct main_options *options);
static int main_dump(const struct main_options *options);
static int main_render(const struct main_options *options);
static int main_run_page(const struct main_options *options);
static int main_open(const struct main_options *options, const void *stack_base, unsigned flags, struct main_page *page);
static int main_settle(struct main_page *page, unsigned flags);
static void main_close(struct main_page *page);
static int main_drawer_start(struct main_page *page);
static void main_drawer_stop(struct main_page *page);
static void main_drawer_run(void *argument);
static void main_frame_wake(void *context);
static enum browser_policy main_link(void *context, struct browser_view *view, const char *href);
static int main_draw_gpu_frame(struct browser_view *view, const struct browser_gpu *gpu, const struct browser_target *target);
static void main_loaded(void *context, struct browser_view *view, enum browser_load_state state, const char *url, int error, const char *reason);
static void main_console_out(void *context, struct browser_view *view, int level, const char *text, size_t length);
static void main_console_err(void *context, struct browser_view *view, int level, const char *text, size_t length);
static int main_draw_gpu(struct browser_view *view, int threads, uint32_t *pixels, unsigned width, unsigned height);
static int main_write_ppm(const char *path, const uint32_t *pixels, unsigned width, unsigned height);
static int main_parse_size(const char *text, unsigned *size);
static int main_parse_budget(const char *text, double *budget);
static const char *main_value(const char *argument, const char *name);
static void main_usage(FILE *stream);

/*
 * Runs the mode the command line names.
 */
int
browser_main(
	int argc,
	char **argv)
{
	struct main_options options;
	int error;
	int status;

	/* Reads the command line. */
	error = main_parse(argc, argv, &options);
	if (error != 0) {
		main_usage(stderr);
		return 2;
	}

	/* The engine makes its threads through the program's, whatever the mode. */
	error = browser_threads_set(shell_threads());
	if (error != 0) {
		fprintf(stderr, "browser: cannot give the engine its threads: %s\n", wb_os_error_text(error));
		return 1;
	}

	/* Runs the mode. */
	switch (options.mode) {
	case MAIN_MODE_VERSION:
		printf("browser %s\n", MAIN_VERSION);
		return 0;
	case MAIN_MODE_HELP:
		main_usage(stdout);
		return 0;
	case MAIN_MODE_DUMP_DOM:
	case MAIN_MODE_DUMP_STYLE:
	case MAIN_MODE_DUMP_LAYOUT:
	case MAIN_MODE_DUMP_PAINT:
		status = main_dump(&options);
		return status;
	case MAIN_MODE_RENDER:
	case MAIN_MODE_RENDER_GPU:
		status = main_render(&options);
		return status;
	case MAIN_MODE_DUMP_AST:
		status = browser_script_tool(BROWSER_SCRIPT_DUMP_AST, options.shell.start, options.parse, __builtin_frame_address(0));
		return status;
	case MAIN_MODE_RUN_JS:
		status = browser_script_tool(BROWSER_SCRIPT_RUN, options.shell.start, options.parse, __builtin_frame_address(0));
		return status;
	case MAIN_MODE_DUMP_CODE:
		status = browser_script_tool(BROWSER_SCRIPT_DUMP_CODE, options.shell.start, options.parse, __builtin_frame_address(0));
		return status;
	case MAIN_MODE_RUN_PAGE:
		status = main_run_page(&options);
		return status;
	case MAIN_MODE_WINDOW:
		break;
	}

	/* Opens the window (on the start page when no page is named) and stays in it until it closes. */
	if (options.shell.start == NULL)
		options.shell.start = MAIN_HOME_PAGE;
	options.shell.fonts = &options.fonts;
	status = shell_run(&options.shell);
	if (status != 0)
		return status;

	/* Succeeded: the window was closed. */
	return 0;
}

/* Loads the page the command line names and writes one of its dumps to standard output. */
static int
main_dump(
	const struct main_options *options)
{
	struct main_page page;
	enum browser_dump kind;
	unsigned flags;
	size_t length;
	size_t written;
	char *text;
	int status;
	int error;
	int flushed;

	/* The dump the mode names; the layout and the display list need the page laid out with its images. */
	flags = BROWSER_SETTLE_LAYOUT;
	if (options->mode == MAIN_MODE_DUMP_DOM) {
		kind = BROWSER_DUMP_DOM;
		flags = 0;
	} else if (options->mode == MAIN_MODE_DUMP_STYLE) {
		kind = BROWSER_DUMP_STYLE;
		flags = 0;
	} else if (options->mode == MAIN_MODE_DUMP_LAYOUT) {
		kind = BROWSER_DUMP_LAYOUT;
	} else {
		kind = BROWSER_DUMP_PAINT;
	}

	/* Loads the page and lets it settle. */
	status = main_open(options, __builtin_frame_address(0), flags, &page);
	if (status != 0)
		return status;

	/* The dump, written out. */
	error = browser_view_dump(page.view, kind, &text, &length);
	if (error == 0) {
		written = fwrite(text, 1, length, stdout);
		if (written != length)
			error = EIO;
		flushed = fflush(stdout);
		if (flushed != 0)
			error = EIO;
	}

	/* Releases the owned dump and view on either write outcome. */
	browser_free(text);
	main_close(&page);
	if (error != 0) {
		fprintf(stderr, "browser: cannot dump %s: %s\n", options->shell.start, wb_os_error_text(error));
		return 1;
	}

	/* Succeeded: the dump is written. */
	return 0;
}

/* Draws the page the command line names with the CPU or the GPU renderer and writes it as a PPM file. */
static int
main_render(
	const struct main_options *options)
{
	struct main_page page;
	uint32_t *pixels;
	unsigned width;
	unsigned height;
	int status;
	int error;

	/* A picture needs a file to go to. */
	if (options->output == NULL) {
		fprintf(stderr, "browser: --render needs --output=FILE\n");
		return 2;
	}

	/* Loads the page and lets it settle, laid out with its images. */
	status = main_open(options, __builtin_frame_address(0), BROWSER_SETTLE_LAYOUT, &page);
	if (status != 0)
		return status;

	/* The picture: the viewport's worth of the page from its top. */
	width = options->shell.width;
	height = options->shell.height;
	pixels = calloc((size_t)width * (size_t)height, sizeof(uint32_t));
	if (pixels == NULL) {
		fprintf(stderr, "browser: cannot draw %s: %s\n", options->shell.start, wb_os_error_text(ENOMEM));
		main_close(&page);
		return 1;
	}

	/* Draws the page with the GPU renderer (which says why it failed), or with the CPU renderer. */
	if (options->mode == MAIN_MODE_RENDER_GPU) {
		error = main_draw_gpu(page.view, options->threads, pixels, width, height);
	} else {
		error = browser_view_draw_pixels(page.view, pixels, width, height, (size_t)width * sizeof(uint32_t));
		if (error != 0)
			fprintf(stderr, "browser: cannot draw %s: %s\n", options->shell.start, wb_os_error_text(error));
	}

	/* The view is no longer needed; a picture that could not be drawn is not written. */
	main_close(&page);
	if (error != 0) {
		free(pixels);
		return 1;
	}

	/* Writes the picture. */
	error = main_write_ppm(options->output, pixels, width, height);
	free(pixels);
	if (error != 0) {
		fprintf(stderr, "browser: cannot write %s: %s\n", options->output, wb_os_error_text(error));
		return 1;
	}

	/* Succeeded: the picture is written. */
	return 0;
}

/* Loads a page, runs its scripts and timers, and writes its console to standard output. */
static int
main_run_page(
	const struct main_options *options)
{
	struct main_page page;
	int status;

	/* The page, loaded and settled with its console on standard output. */
	status = main_open(options, __builtin_frame_address(0), 0, &page);
	if (status != 0)
		return status;

	/* The page is no longer needed. */
	main_close(&page);
	if (page.output_failed) {
		fprintf(stderr, "browser: cannot write page console: %s\n", wb_os_error_text(EIO));
		return 1;
	}

	/* Succeeded: the page ran. */
	return 0;
}

/*
 * Makes the view of a headless mode and loads the page the command line
 * names into it, then lets it settle: its timers run on the virtual clock
 * (with --threads, on its own clock in the view's threads) and, with
 * BROWSER_SETTLE_LAYOUT in flags, it is laid out with its images.
 * Reports the exit status of a failure after saying why, or 0 with the
 * view in page->view (main_close ends it).
 *
 * stack_base is the caller's frame: the pages' heaps scan the stack up to
 * it, and the caller goes on using the view after this returns.
 */
static int
main_open(
	const struct main_options *options,
	const void *stack_base,
	unsigned flags,
	struct main_page *page)
{
	struct browser_callbacks callbacks;
	struct browser_view_options view_options;
	int error;

	/* Every headless mode needs a page. */
	memset(page, 0, sizeof(*page));
	page->options = options;
	page->table = shell_threads();
	if (options->shell.start == NULL) {
		fprintf(stderr, "browser: a file to load is needed\n");
		return 2;
	}

	/* The callbacks: a failed load is said, and the console goes to standard error (--run's to standard output). */
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.load = main_loaded;
	callbacks.console = main_console_err;
	if (options->mode == MAIN_MODE_RUN_PAGE)
		callbacks.console = main_console_out;
	callbacks.context = page;

	/* The view at the viewport's size, reading at once or, with --async, fetching the page and its images in the background. */
	memset(&view_options, 0, sizeof(view_options));
	view_options.version = BROWSER_API_VERSION;
	view_options.fonts = &options->fonts;
	view_options.callbacks = &callbacks;
	view_options.stack_base = stack_base;
	view_options.width = options->shell.width;
	view_options.height = options->shell.height;
	view_options.fetch = BROWSER_FETCH_AT_ONCE;
	if (options->async)
		view_options.fetch = BROWSER_FETCH_BACKGROUND;

	/*
	 * With --threads, a view of its own threads, as the window's: it fetches
	 * as the window does unless --async (its iframes' threads fetch through
	 * the loader, which a view that reads every page at once has not), its
	 * scripts' navigations are let through by the link callback and its
	 * frames are drawn by the stand-in for the window's drawing.
	 */
	if (options->threads) {
		if (!options->async)
			view_options.fetch = BROWSER_FETCH_DEFAULT;
		callbacks.link = main_link;
		view_options.threading = BROWSER_THREADING_OWN;
		view_options.frame_wake = main_frame_wake;
		view_options.frame_wake_context = page;
		error = page->table->mutex_create(page->table->context, &page->lock);
		if (error == 0)
			error = page->table->cond_create(page->table->context, &page->cond);
		if (error != 0) {
			fprintf(stderr, "browser: cannot make a view: %s\n", wb_os_error_text(error));
			main_close(page);
			return 1;
		}
	}

	/* The view. */
	error = browser_view_create(&view_options, &page->view);
	if (error != 0) {
		fprintf(stderr, "browser: cannot make a view: %s\n", wb_os_error_text(error));
		main_close(page);
		return 1;
	}

	/* The stand-in for the window's drawing, while the page settles. */
	if (options->threads) {
		error = main_drawer_start(page);
		if (error != 0) {
			fprintf(stderr, "browser: cannot start the drawing: %s\n", wb_os_error_text(error));
			main_close(page);
			return 1;
		}
	}

	/* The page, running its scripts (a load that failed was said by the callback). */
	error = browser_view_load(page->view, options->shell.start);
	if (error != 0) {
		if (!page->failed)
			fprintf(stderr, "browser: cannot load %s: %s\n", options->shell.start, wb_os_error_text(error));
		main_close(page);
		return 1;
	}

	/* The page settled: fetched, its timers run, and laid out with its images when the mode needs that. */
	error = main_settle(page, flags);

	/* The drawing stands still from here (the mode draws or dumps the page at rest). */
	main_drawer_stop(page);
	if (error != 0) {
		if (!page->failed)
			fprintf(stderr, "browser: cannot run %s: %s\n", options->shell.start, wb_os_error_text(error));
		main_close(page);
		return 1;
	}

	/* Succeeded: the page is ready for the mode. */
	return 0;
}

/*
 * Settles the page, and with --follow the navigations its scripts asked
 * for, each page settled in turn: the program's round carries out one in
 * a view of the caller's thread; in a view of its own threads they happen
 * as the page runs, as many as the link callback lets through.  Reports 0
 * or the settle's error.
 */
static int
main_settle(
	struct main_page *page,
	unsigned flags)
{
	const struct main_options *options;
	unsigned followed;
	unsigned started;
	unsigned allowed;
	int error;

	/* The first page. */
	options = page->options;
	error = browser_view_settle(page->view, options->settle_budget, flags);

	/* Each navigation the scripts asked for, while --follow allows. */
	for (followed = 0; error == 0 && followed < options->follow; followed++) {
		/* A view of its own threads followed it already: a page that came since the last settle is settled again. */
		if (options->threads) {
			allowed = __atomic_load_n(&page->allowed, __ATOMIC_ACQUIRE);
			if (allowed == page->settled)
				break;
			page->settled = allowed;
			error = browser_view_settle(page->view, options->settle_budget, flags);
			continue;
		}

		/* A view of the caller's thread follows it in the program's round. */
		started = page->started;
		browser_view_process(page->view);
		if (page->started == started)
			break;
		error = browser_view_settle(page->view, options->settle_budget, flags);
	}

	/* A settle that failed. */
	if (error != 0)
		return error;

	/* Succeeded: the page is at rest. */
	return 0;
}

/* Ends the page of a headless mode: the stand-in drawing, the view, and the drawing's lock and condition. */
static void
main_close(
	struct main_page *page)
{
	/* The drawing first, which draws from the view. */
	main_drawer_stop(page);

	/* The view, after which nothing wakes the drawing. */
	if (page->view != NULL)
		browser_view_destroy(page->view);
	page->view = NULL;

	/* The drawing's lock and condition. */
	if (page->cond != NULL)
		page->table->cond_destroy(page->table->context, page->cond);
	if (page->lock != NULL)
		page->table->mutex_destroy(page->table->context, page->lock);
	page->cond = NULL;
	page->lock = NULL;
}

/* Starts the stand-in for the window's drawing thread of a view of its own threads; reports 0 or why not. */
static int
main_drawer_start(
	struct main_page *page)
{
	int error;

	/* The thread, by the program's table as the window's is. */
	page->ending = 0;
	error = page->table->start(page->table->context, "browser-draw", 0, main_drawer_run, page, &page->drawer);
	if (error != 0) {
		page->drawer = NULL;
		return error;
	}

	/* Succeeded: it draws each committed frame. */
	return 0;
}

/* Stops the stand-in drawing (when it runs) and joins it. */
static void
main_drawer_stop(
	struct main_page *page)
{
	/* Nothing runs. */
	if (page->drawer == NULL)
		return;

	/* The end, told. */
	page->table->mutex_lock(page->table->context, page->lock);

	page->ending = 1;
	page->table->cond_signal(page->table->context, page->cond);

	page->table->mutex_unlock(page->table->context, page->lock);

	/* Joined. */
	page->table->join(page->table->context, page->drawer);
	page->drawer = NULL;
}

/*
 * The stand-in for the window's drawing thread: waits for a committed
 * frame, draws it with the CPU as the window does without the GPU, and
 * tells the view of its present (the chance for the page's animation
 * frames), until it is told to end.
 */
static void
main_drawer_run(
	void *argument)
{
	struct main_page *page;
	uint32_t *pixels;
	unsigned width;
	unsigned height;
	int ending;
	int error;

	/* The picture, at the viewport's size. */
	page = argument;
	width = page->options->shell.width;
	height = page->options->shell.height;
	pixels = calloc((size_t)width * (size_t)height, sizeof(uint32_t));

	/* Each frame committed, until the end. */
	for (;;) {
		/* The wait. */
		page->table->mutex_lock(page->table->context, page->lock);

		while (!page->woken && !page->ending)
			page->table->cond_wait(page->table->context, page->cond, page->lock);
		page->woken = 0;
		ending = page->ending;

		page->table->mutex_unlock(page->table->context, page->lock);

		/* The end. */
		if (ending)
			break;

		/* The frame drawn and presented (without a picture, only presented). */
		error = 0;
		if (pixels != NULL)
			error = browser_view_render_pixels(page->view, pixels, width, height, (size_t)width * sizeof(uint32_t));
		if (error == 0)
			browser_view_presented(page->view, 0);
	}

	/* The picture goes. */
	free(pixels);
}

/* The view's frame wake (on its thread): the stand-in drawing has a committed frame to draw. */
static void
main_frame_wake(
	void *context)
{
	struct main_page *page;

	/* The news, under the drawing's lock. */
	page = context;
	page->table->mutex_lock(page->table->context, page->lock);

	page->woken = 1;
	page->table->cond_signal(page->table->context, page->cond);

	page->table->mutex_unlock(page->table->context, page->lock);
}

/*
 * The link callback of a view of its own threads (on its script thread):
 * a navigation of the page's scripts is let through while --follow
 * allows more, as the program's round of a view of the caller's thread
 * carries them out.
 */
static enum browser_policy
main_link(
	void *context,
	struct browser_view *view,
	const char *href)
{
	struct main_page *page;
	unsigned allowed;

	UNUSED_PARAMETER(view);
	UNUSED_PARAMETER(href);

	/* One more, while --follow allows. */
	page = context;
	allowed = __atomic_load_n(&page->allowed, __ATOMIC_ACQUIRE);
	if (allowed >= page->options->follow)
		return BROWSER_POLICY_DENY;
	__atomic_store_n(&page->allowed, allowed + 1U, __ATOMIC_RELEASE);

	/* Followed. */
	return BROWSER_POLICY_ALLOW;
}

/* The view's load callback: a load that failed, and why (the TLS reason for an https page). */
static void
main_loaded(
	void *context,
	struct browser_view *view,
	enum browser_load_state state,
	const char *url,
	int error,
	const char *reason)
{
	struct main_page *page;

	UNUSED_PARAMETER(view);
	UNUSED_PARAMETER(url);

	/* A load that starts is counted (--follow sees whether a navigation was carried out). */
	page = context;
	if (state == BROWSER_LOAD_STARTED)
		page->started++;

	/* Only a failure is said. */
	if (state != BROWSER_LOAD_FAILED)
		return;

	/* The line, by the location the command line gave. */
	fprintf(stderr, "browser: cannot load %s: %s", page->options->shell.start, wb_os_error_text(error));
	if (reason[0] != '\0')
		fprintf(stderr, " (TLS: %s)", reason);
	fputc('\n', stderr);

	/* The mode ends without saying it again. */
	page->failed = 1;
}

/* The view's console callback of --run: the line as it is, on standard output. */
static void
main_console_out(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	struct main_page *page;
	size_t written;
	int newline;
	int flushed;

	UNUSED_PARAMETER(view);
	UNUSED_PARAMETER(level);

	/* A failed write is retained until the enclosing --run call returns. */
	page = context;
	written = fwrite(text, 1, length, stdout);
	newline = fputc('\n', stdout);
	flushed = fflush(stdout);
	if (written != length || newline == EOF || flushed != 0)
		page->output_failed = 1;
}

/* The view's console callback of the other modes: the line on standard error, marked as the console's. */
static void
main_console_err(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(view);
	UNUSED_PARAMETER(level);

	/* The line. */
	fprintf(stderr, "console: %.*s\n", (int)length, text);
}

/*
 * Draws the view with the GPU renderer into an offscreen image (a device
 * of the engine's own) and reads it back into pixels (width by height,
 * packed rows); says why on standard error when it cannot.  A view of its
 * own threads (threads) draws its latest committed frame, as the window's
 * drawing thread does.
 */
static int
main_draw_gpu(
	struct browser_view *view,
	int threads,
	uint32_t *pixels,
	unsigned width,
	unsigned height)
{
	struct browser_offscreen *offscreen;
	struct browser_gpu_failure failure;
	struct browser_target target;
	struct browser_gpu gpu;
	int error;

	/* The offscreen image and its device; a Vulkan call that failed is named. */
	error = browser_offscreen_create(width, height, &offscreen, &failure);
	if (error == EIO) {
		fprintf(stderr, "browser: cannot draw on the GPU: %s failed (%d)\n", failure.operation, (int)failure.result);
		return error;
	} else if (error != 0) {
		fprintf(stderr, "browser: cannot draw on the GPU: %s\n", wb_os_error_text(error));
		return error;
	}

	/* The view draws into it with its device, and waits for the drawing. */
	browser_offscreen_target(offscreen, &gpu, &target);
	browser_view_set_gpu(view, &gpu);
	if (threads) {
		error = main_draw_gpu_frame(view, &gpu, &target);
	} else {
		error = browser_view_draw(view, &target, VK_NULL_HANDLE, VK_NULL_HANDLE);
	}
	if (error == EIO) {
		browser_view_gpu_failure(view, &failure);
		fprintf(stderr, "browser: cannot draw on the GPU: %s failed (%d)\n", failure.operation, (int)failure.result);
	} else if (error != 0) {
		fprintf(stderr, "browser: cannot draw on the GPU: %s\n", wb_os_error_text(error));
	}

	/* The picture read back. */
	if (error == 0) {
		error = browser_offscreen_read(offscreen, pixels, (size_t)width * sizeof(uint32_t), &failure);
		if (error != 0)
			fprintf(stderr, "browser: cannot read the GPU's picture: %s failed (%d)\n", failure.operation, (int)failure.result);
	}

	/* The view lets the device go before the offscreen image and its device end. */
	browser_view_set_gpu(view, NULL);
	browser_offscreen_destroy(offscreen);
	if (error != 0)
		return error;

	/* Succeeded: the pixels hold the picture. */
	return 0;
}

/*
 * Draws the latest committed frame of a view of its own threads into an
 * image on the GPU: records it into a command buffer of the program's,
 * submits it on the device's queue and waits for it, as the window's
 * drawing thread does.  Reports 0, the view's error, or EIO when a Vulkan
 * call failed.
 */
static int
main_draw_gpu_frame(
	struct browser_view *view,
	const struct browser_gpu *gpu,
	const struct browser_target *target)
{
	VkCommandPoolCreateInfo pool_info;
	VkCommandBufferAllocateInfo allocate;
	VkCommandBufferBeginInfo begin;
	VkFenceCreateInfo fence_info;
	VkSubmitInfo submit;
	VkCommandPool pool;
	VkCommandBuffer commands;
	VkFence fence;
	VkQueue queue;
	VkResult result;
	uint64_t serial;
	int error;

	/* The commands, from the queue family the view draws with. */
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.queueFamilyIndex = gpu->queue_family;
	result = vkCreateCommandPool(gpu->device, &pool_info, NULL, &pool);
	if (result != VK_SUCCESS)
		return EIO;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate.commandPool = pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	result = vkAllocateCommandBuffers(gpu->device, &allocate, &commands);
	if (result != VK_SUCCESS) {
		vkDestroyCommandPool(gpu->device, pool, NULL);
		return EIO;
	}

	/* The frame recorded between the program's begin and end. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(commands, &begin);
	if (result != VK_SUCCESS) {
		vkDestroyCommandPool(gpu->device, pool, NULL);
		return EIO;
	}
	error = browser_view_render(view, target, commands, &serial);
	result = vkEndCommandBuffer(commands);
	if (error == 0 && result != VK_SUCCESS)
		error = EIO;
	if (error != 0) {
		vkDestroyCommandPool(gpu->device, pool, NULL);
		return error;
	}

	/* Submitted on the device's queue and waited for. */
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	result = vkCreateFence(gpu->device, &fence_info, NULL, &fence);
	if (result != VK_SUCCESS) {
		vkDestroyCommandPool(gpu->device, pool, NULL);
		return EIO;
	}
	vkGetDeviceQueue(gpu->device, gpu->queue_family, 0, &queue);
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &commands;
	result = vkQueueSubmit(queue, 1, &submit, fence);
	if (result == VK_SUCCESS)
		result = vkWaitForFences(gpu->device, 1, &fence, VK_TRUE, UINT64_MAX);

	/* The finished commands and their fence go. */
	vkDestroyFence(gpu->device, fence, NULL);
	vkDestroyCommandPool(gpu->device, pool, NULL);
	if (result != VK_SUCCESS)
		return EIO;

	/* Succeeded: the image holds the frame. */
	return 0;
}

/* Writes pixels (0xAARRGGBB, packed rows) as a binary PPM file. */
static int
main_write_ppm(
	const char *path,
	const uint32_t *pixels,
	unsigned width,
	unsigned height)
{
	unsigned char sample[3];
	size_t count;
	size_t index;
	size_t written;
	FILE *file;
	int header;
	int failed;
	int closed;
	int error;

	/* The file. */
	file = fopen(path, "wb");
	if (file == NULL)
		return errno;

	/* The header: the magic, the size and the largest sample. */
	header = fprintf(file, "P6\n%u %u\n255\n", width, height);
	error = 0;
	if (header < 0)
		error = EIO;

	/* Each pixel's red, green and blue, while the writes go through. */
	count = (size_t)width * (size_t)height;
	for (index = 0; index < count && error == 0; index++) {
		sample[0] = (unsigned char)(pixels[index] >> 16);
		sample[1] = (unsigned char)(pixels[index] >> 8);
		sample[2] = (unsigned char)pixels[index];
		written = fwrite(sample, 1, sizeof(sample), file);
		if (written != sizeof(sample))
			error = EIO;
	}

	/* A write that failed, or a close that did. */
	failed = ferror(file);
	if (failed && error == 0)
		error = EIO;
	closed = fclose(file);
	if (closed != 0 && error == 0)
		error = errno;
	if (error != 0)
		return error;

	/* Succeeded: the file holds the picture. */
	return 0;
}

/* Reads the command line into options; returns EINVAL for a word it does not know. */
static int
main_parse(
	int argc,
	char **argv,
	struct main_options *options)
{
	const char *value;
	int differs;
	int index;
	int dump;
	int error;

	/* Starts from the window mode with the default size, and the system's fonts. */
	memset(options, 0, sizeof(*options));
	options->mode = MAIN_MODE_WINDOW;
	options->shell.width = MAIN_DEFAULT_WIDTH;
	options->shell.height = MAIN_DEFAULT_HEIGHT;
	options->settle_budget = MAIN_SETTLE_BUDGET;

	/* Takes each word in turn. */
	for (index = 1; index < argc; index++) {
		/* The informational modes. */
		differs = strcmp(argv[index], "--version");
		if (differs == 0) {
			options->mode = MAIN_MODE_VERSION;
			continue;
		}

		/* The help. */
		differs = strcmp(argv[index], "--help");
		if (differs == 0) {
			options->mode = MAIN_MODE_HELP;
			continue;
		}

		/* Running a page for its console. */
		differs = strcmp(argv[index], "--run");
		if (differs == 0) {
			options->mode = MAIN_MODE_RUN_PAGE;
			continue;
		}

		/* The headless dumps of a page. */
		value = main_value(argv[index], "--dump=");
		if (value != NULL) {
			/* Finds the dump's name among the known ones. */
			for (dump = 0; main_dumps[dump].name != NULL; dump++) {
				differs = strcmp(value, main_dumps[dump].name);
				if (differs == 0)
					break;
			}

			/* A name that is not known is refused. */
			if (main_dumps[dump].name == NULL) {
				fprintf(stderr, "browser: unknown dump %s\n", value);
				return EINVAL;
			}

			/* The dump's mode. */
			options->mode = main_dumps[dump].mode;
			continue;
		}

		/* The headless drawing of a page. */
		differs = strcmp(argv[index], "--render");
		if (differs == 0) {
			options->mode = MAIN_MODE_RENDER;
			continue;
		}

		/* The same with the GPU renderer. */
		differs = strcmp(argv[index], "--render-gpu");
		if (differs == 0) {
			options->mode = MAIN_MODE_RENDER_GPU;
			continue;
		}

		/* The headless run of a script. */
		differs = strcmp(argv[index], "--js");
		if (differs == 0) {
			options->mode = MAIN_MODE_RUN_JS;
			continue;
		}

		/* How a script is parsed: as a module, or as strict code. */
		differs = strcmp(argv[index], "--module");
		if (differs == 0) {
			options->parse |= BROWSER_SCRIPT_MODULE;
			continue;
		}

		/* As strict code. */
		differs = strcmp(argv[index], "--strict");
		if (differs == 0) {
			options->parse |= BROWSER_SCRIPT_STRICT;
			continue;
		}

		/* The headless modes run the page on the view's own threads, as the window does. */
		differs = strcmp(argv[index], "--threads");
		if (differs == 0) {
			options->threads = 1;
			continue;
		}

		/* The headless modes fetch the page and its images with the asynchronous loader. */
		differs = strcmp(argv[index], "--async");
		if (differs == 0) {
			options->async = 1;
			continue;
		}

		/* How many navigations of the page's scripts a headless mode carries out. */
		value = main_value(argv[index], "--follow=");
		if (value != NULL) {
			if (value[0] < '0' || value[0] > '9' || value[1] != '\0') {
				fprintf(stderr, "browser: --follow takes 0 to 9\n");
				return EINVAL;
			}
			options->follow = (unsigned)(value[0] - '0');
			continue;
		}

		/* The virtual clock budget for headless page tasks. */
		value = main_value(argv[index], "--settle-ms=");
		if (value != NULL) {
			error = main_parse_budget(value, &options->settle_budget);
			if (error != 0)
				return error;

			continue;
		}

		/* The file a drawing goes to. */
		value = main_value(argv[index], "--output=");
		if (value != NULL) {
			options->output = value;
			continue;
		}

		/* The fonts. */
		value = main_value(argv[index], "--font=");
		if (value != NULL) {
			options->fonts.sans = value;
			continue;
		}

		/* The monospace font. */
		value = main_value(argv[index], "--mono-font=");
		if (value != NULL) {
			options->fonts.mono = value;
			continue;
		}

		/* The fallback font. */
		value = main_value(argv[index], "--fallback-font=");
		if (value != NULL) {
			options->fonts.fallback = value;
			continue;
		}

		/* A CA file for https besides the system's roots. */
		value = main_value(argv[index], "--ca-file=");
		if (value != NULL) {
			error = browser_add_ca_file(value);
			if (error != 0)
				return error;

			continue;
		}

		/* The window drawn with the CPU, without trying the GPU. */
		differs = strcmp(argv[index], "--no-gpu");
		if (differs == 0) {
			options->shell.no_gpu = 1;
			continue;
		}

		/* The Wayland display to connect to. */
		value = main_value(argv[index], "--display=");
		if (value != NULL) {
			options->shell.display = value;
			continue;
		}

		/* The window's width. */
		value = main_value(argv[index], "--width=");
		if (value != NULL) {
			error = main_parse_size(value, &options->shell.width);
			if (error != 0)
				return error;

			continue;
		}

		/* The window's height. */
		value = main_value(argv[index], "--height=");
		if (value != NULL) {
			error = main_parse_size(value, &options->shell.height);
			if (error != 0)
				return error;

			continue;
		}

		/* An unknown option is refused. */
		if (argv[index][0] == '-') {
			fprintf(stderr, "browser: unknown option %s\n", argv[index]);
			return EINVAL;
		}

		/* The one word that is not an option is the page to open. */
		if (options->shell.start != NULL) {
			fprintf(stderr, "browser: more than one page given\n");
			return EINVAL;
		}

		/* Remembers the page. */
		options->shell.start = argv[index];
	}

	/* Succeeded: options holds the request. */
	return 0;
}

/* Reads a bounded virtual-time budget in milliseconds. */
static int
main_parse_budget(
	const char *text,
	double *budget)
{
	unsigned long milliseconds;
	const char *digit;
	char *end;

	/* Accepts only decimal digits, including zero for tasks already due. */
	if (text[0] == '\0')
		return EINVAL;

	/* Refuses signs, whitespace and fractional or non-finite input. */
	for (digit = text; *digit != '\0'; digit++) {
		/* Each character must belong to the decimal representation. */
		if (*digit < '0' || *digit > '9')
			return EINVAL;
	}

	/* Reads the complete budget without allowing overflow. */
	errno = 0;
	milliseconds = strtoul(text, &end, 10);
	if (errno != 0 || *end != '\0')
		return EINVAL;

	/* Keeps caller-selected execution finite. */
	if (milliseconds > MAIN_MAX_SETTLE_BUDGET)
		return EINVAL;

	/* Publishes the validated clock limit. */
	*budget = (double)milliseconds;

	/* Succeeded: the caller has a finite headless clock budget. */
	return 0;
}

/* Reads a window size in pixels. */
static int
main_parse_size(
	const char *text,
	unsigned *size)
{
	unsigned long value;
	char *end;

	/* Reads the decimal number, which must be the whole word. */
	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno != 0 ||
	    end == text ||
	    *end != '\0') {
		fprintf(stderr, "browser: %s is not a size\n", text);
		return EINVAL;
	}

	/* Refuses a size of nothing or past what a window can be. */
	if (value == 0 || value > MAIN_MAX_SIZE) {
		fprintf(stderr, "browser: size %s is out of range\n", text);
		return EINVAL;
	}

	/* Succeeded: the size fits. */
	*size = (unsigned)value;
	return 0;
}

/* Finds the value after name= in an argument, or NULL when the argument is another option. */
static const char *
main_value(
	const char *argument,
	const char *name)
{
	size_t length;
	int differs;

	/* Compares the option's name with the start of the argument. */
	length = strlen(name);
	differs = strncmp(argument, name, length);
	if (differs != 0)
		return NULL;

	/* Reports what follows the name. */
	return argument + length;
}

/* Prints how to run the program. */
static void
main_usage(
	FILE *stream)
{
	/* Lists the forms of the command line. */
	fprintf(stream,
		"usage: browser [--display=NAME] [--width=N] [--height=N] [--ca-file=PEM] [--no-gpu] [URL]\n"
		"       browser --dump=dom|style|layout|paint [--width=N] [--height=N] [--font=PATH]\n"
		"                        [--mono-font=PATH] [--fallback-font=PATH] FILE\n"
		"       browser --run [--width=N] [--height=N] FILE\n"
		"       browser --render|--render-gpu --output=OUT.ppm [--width=N] [--height=N] [--font=PATH] [--async]\n"
		"                        [--mono-font=PATH] [--fallback-font=PATH] FILE\n"
		"       browser --dump=ast [--module] [--strict] FILE.js\n"
		"       browser --js [--strict] FILE.js\n"
		"       browser --dump=code [--strict] FILE.js\n"
		"       browser --version | --help\n"
		"       headless page modes: [--settle-ms=0..60000] (default 5000) [--follow=0..9] (default 0) [--threads]\n");
}
