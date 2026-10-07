/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * browser: the Web browser of the zedBSD desktop.
 *
 *   browser [--display=NAME] [--width=N] [--height=N] [--ca-file=PEM] [URL]
 *   browser --dump=dom|style|layout|paint [--width=N] [--height=N] [--font=PATH] FILE
 *   browser --run [--width=N] [--height=N] FILE
 *   browser --dump=ast [--module] [--strict] FILE.js
 *   browser --js [--strict] FILE.js
 *   browser --dump=code [--strict] FILE.js
 *   browser --render|--render-gpu --output=OUT.ppm [--width=N] [--height=N] [--font=PATH] FILE
 *   browser --version | --help
 *
 * Without a headless mode it opens a compositor window on URL, or on the
 * start page the package installs (MAIN_START_PAGE).  The headless
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
 * Since ws074-p057 the engine is libbrowser.so and the program uses it
 * through <browser/browser.h> only: the headless modes through a view, the
 * script modes (--js, --dump=ast, --dump=code) through the library's
 * script tools, and the window through shell/.
 * --ca-file (in any mode that loads a page) trusts the CA certificates of
 * a PEM file besides the system's roots for https (the tests' own CA).
 */

#include "shell/shell.h"

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

/* The page the window opens when the command line names none. */
#define MAIN_START_PAGE		KEILAND_DATADIR "/browser/start.html"

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
};

/*
 * The page of a headless mode: the command line, the view that holds the
 * page, and whether the load failed (the load callback said why).  It
 * lives in the mode's function for as long as the view.
 */
struct main_page {
	const struct main_options *options;
	struct browser_view *view;
	int failed;
	int output_failed;
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
static void main_loaded(void *context, struct browser_view *view, enum browser_load_state state, const char *url, int error, const char *reason);
static void main_console_out(void *context, struct browser_view *view, int level, const char *text, size_t length);
static void main_console_err(void *context, struct browser_view *view, int level, const char *text, size_t length);
static int main_draw_gpu(struct browser_view *view, uint32_t *pixels, unsigned width, unsigned height);
static int main_write_ppm(const char *path, const uint32_t *pixels, unsigned width, unsigned height);
static int main_parse_size(const char *text, unsigned *size);
static int main_parse_budget(const char *text, double *budget);
static const char *main_value(const char *argument, const char *name);
static void main_usage(FILE *stream);

/*
 * Runs the mode the command line names.
 */
int
main(
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
		options.shell.start = MAIN_START_PAGE;
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
	free(text);
	browser_view_destroy(page.view);
	if (error != 0) {
		fprintf(stderr, "browser: cannot dump %s: %s\n", options->shell.start, strerror(error));
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
		fprintf(stderr, "browser: cannot draw %s: %s\n", options->shell.start, strerror(ENOMEM));
		browser_view_destroy(page.view);
		return 1;
	}

	/* Draws the page with the GPU renderer (which says why it failed), or with the CPU renderer. */
	if (options->mode == MAIN_MODE_RENDER_GPU) {
		error = main_draw_gpu(page.view, pixels, width, height);
	} else {
		error = browser_view_draw_pixels(page.view, pixels, width, height, (size_t)width * sizeof(uint32_t));
		if (error != 0)
			fprintf(stderr, "browser: cannot draw %s: %s\n", options->shell.start, strerror(error));
	}

	/* The view is no longer needed; a picture that could not be drawn is not written. */
	browser_view_destroy(page.view);
	if (error != 0) {
		free(pixels);
		return 1;
	}

	/* Writes the picture. */
	error = main_write_ppm(options->output, pixels, width, height);
	free(pixels);
	if (error != 0) {
		fprintf(stderr, "browser: cannot write %s: %s\n", options->output, strerror(error));
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
	browser_view_destroy(page.view);
	if (page.output_failed) {
		fprintf(stderr, "browser: cannot write page console: %s\n", strerror(EIO));
		return 1;
	}

	/* Succeeded: the page ran. */
	return 0;
}

/*
 * Makes the view of a headless mode and loads the page the command line
 * names into it, then lets it settle: its timers run on the virtual clock
 * and, with BROWSER_SETTLE_LAYOUT in flags, it is laid out with its images.
 * Reports the exit status of a failure after saying why, or 0 with the
 * view in page->view.
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
	error = browser_view_create(&view_options, &page->view);
	if (error != 0) {
		fprintf(stderr, "browser: cannot make a view: %s\n", strerror(error));
		return 1;
	}

	/* The page, running its scripts (a load that failed was said by the callback). */
	error = browser_view_load(page->view, options->shell.start);
	if (error != 0) {
		if (!page->failed)
			fprintf(stderr, "browser: cannot load %s: %s\n", options->shell.start, strerror(error));
		browser_view_destroy(page->view);
		return 1;
	}

	/* The page settled: fetched, its timers run, and laid out with its images when the mode needs that. */
	error = browser_view_settle(page->view, options->settle_budget, flags);
	if (error != 0) {
		if (!page->failed)
			fprintf(stderr, "browser: cannot run %s: %s\n", options->shell.start, strerror(error));
		browser_view_destroy(page->view);
		return 1;
	}

	/* Succeeded: the page is ready for the mode. */
	return 0;
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

	/* Only a failure is said. */
	if (state != BROWSER_LOAD_FAILED)
		return;

	/* The line, by the location the command line gave. */
	page = context;
	fprintf(stderr, "browser: cannot load %s: %s", page->options->shell.start, strerror(error));
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
 * packed rows); says why on standard error when it cannot.
 */
static int
main_draw_gpu(
	struct browser_view *view,
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
		fprintf(stderr, "browser: cannot draw on the GPU: %s\n", strerror(error));
		return error;
	}

	/* The view draws into it with its device, and waits for the drawing. */
	browser_offscreen_target(offscreen, &gpu, &target);
	browser_view_set_gpu(view, &gpu);
	error = browser_view_draw(view, &target, VK_NULL_HANDLE, VK_NULL_HANDLE);
	if (error == EIO) {
		browser_view_gpu_failure(view, &failure);
		fprintf(stderr, "browser: cannot draw on the GPU: %s failed (%d)\n", failure.operation, (int)failure.result);
	} else if (error != 0) {
		fprintf(stderr, "browser: cannot draw on the GPU: %s\n", strerror(error));
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

		/* The headless modes fetch the page and its images with the asynchronous loader. */
		differs = strcmp(argv[index], "--async");
		if (differs == 0) {
			options->async = 1;
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
		"usage: browser [--display=NAME] [--width=N] [--height=N] [--ca-file=PEM] [URL]\n"
		"       browser --dump=dom|style|layout|paint [--width=N] [--height=N] [--font=PATH]\n"
		"                        [--mono-font=PATH] [--fallback-font=PATH] FILE\n"
		"       browser --run [--width=N] [--height=N] FILE\n"
		"       browser --render|--render-gpu --output=OUT.ppm [--width=N] [--height=N] [--font=PATH] [--async]\n"
		"                        [--mono-font=PATH] [--fallback-font=PATH] FILE\n"
		"       browser --dump=ast [--module] [--strict] FILE.js\n"
		"       browser --js [--strict] FILE.js\n"
		"       browser --dump=code [--strict] FILE.js\n"
		"       browser --version | --help\n"
		"       headless page modes: [--settle-ms=0..60000] (default 5000)\n");
}
