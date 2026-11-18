/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * browser-probe: the second program over libbrowser (ws074-p057), which
 * shows that the engine works as a component without /bin/browser's
 * window: it uses <browser/browser.h> only, and links libbrowser.so and the C
 * library, not Wayland.
 *
 *   browser-probe [--width=N] [--height=N] [--gpu] [--tab=N] [--font=PATH]
 *                 [--mono-font=PATH] [--fallback-font=PATH] PAGE OUT.ppm
 *
 * It makes a view (with the system's fonts unless the command line names
 * others), loads PAGE (a path or a URL), lets it settle, presses
 * Tab N times (the focus ring shows on the element focused last), draws
 * the view with the CPU or, with --gpu, on the engine's own offscreen
 * image, and writes the picture as a binary PPM.  It writes what the view
 * told it on standard output, one line each: BROWSERPROBE TITLE,
 * CONSOLE, LINK (denied: the probe never navigates), LOAD (a failure) and
 * WROTE.  The exit status is 0, 1 when the page or the picture failed, 2
 * for a misuse.
 */

#include <browser/browser.h>

#include "userland/tests/browser-threads/threads-posix.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The picture's size unless told otherwise, in pixels. */
#define PROBE_DEFAULT_WIDTH	800U
#define PROBE_DEFAULT_HEIGHT	600U

/* The largest picture accepted, in pixels a side. */
#define PROBE_MAX_SIZE		8192UL

/* Marks a parameter a function has to take but does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* How long the page's timers run before it is drawn, in virtual milliseconds (as /bin/browser's headless modes). */
#define PROBE_SETTLE_BUDGET	5000.0

/*
 * What the command line asked for: the picture's size, the renderer, how
 * many times Tab is pressed, the fonts (NULL for the system's), the page
 * and the picture's file.
 */
struct probe_options {
	unsigned width;
	unsigned height;
	int gpu;
	unsigned tabs;
	struct browser_fonts fonts;
	const char *page;
	const char *output;
};

static int probe_parse(int argc, char **argv, struct probe_options *options);
static const char *probe_value(const char *argument, const char *name);
static int probe_number(const char *text, unsigned long limit, unsigned *number);
static int probe_draw(struct browser_view *view, const struct probe_options *options, uint32_t *pixels);
static int probe_draw_gpu(struct browser_view *view, const struct probe_options *options, uint32_t *pixels);
static int probe_write(const char *path, const uint32_t *pixels, unsigned width, unsigned height);
static void probe_title(void *context, struct browser_view *view, const char *title);
static void probe_load(void *context, struct browser_view *view, enum browser_load_state state, const char *url, int error, const char *reason);
static enum browser_policy probe_link(void *context, struct browser_view *view, const char *href);
static void probe_console(void *context, struct browser_view *view, int level, const char *text, size_t length);

/*
 * Draws a page with libbrowser into a PPM file.
 *
 * Returns the program's exit status.
 */
int
main(
	int argc,
	char **argv)
{
	struct probe_options options;
	struct browser_callbacks callbacks;
	struct browser_view_options view_options;
	struct browser_view *view;
	uint32_t *pixels;
	unsigned index;
	int error;

	/* The command line. */
	error = probe_parse(argc, argv, &options);
	if (error != 0) {
		fprintf(stderr, "usage: browser-probe [--width=N] [--height=N] [--gpu] [--tab=N] [--font=PATH] [--mono-font=PATH]\n"
		    "                     [--fallback-font=PATH] PAGE OUT.ppm\n");
		return 2;
	}

	/* The engine makes its threads with the POSIX threads. */
	error = browser_threads_set(browser_threads_posix());
	if (error != 0) {
		fprintf(stderr, "browser-probe: the engine refused the threads (%d)\n", error);
		return 1;
	}

	/* What the view tells the probe. */
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.title = probe_title;
	callbacks.load = probe_load;
	callbacks.link = probe_link;
	callbacks.console = probe_console;

	/* The view: the fonts, the picture's size, every page read at once. */
	memset(&view_options, 0, sizeof(view_options));
	view_options.version = BROWSER_API_VERSION;
	view_options.fonts = &options.fonts;
	view_options.callbacks = &callbacks;
	view_options.stack_base = __builtin_frame_address(0);
	view_options.width = options.width;
	view_options.height = options.height;
	view_options.fetch = BROWSER_FETCH_AT_ONCE;
	error = browser_view_create(&view_options, &view);
	if (error != 0) {
		fprintf(stderr, "browser-probe: cannot make a view: %s\n", strerror(error));
		return 1;
	}

	/* The page (a failure was told by the load callback). */
	error = browser_view_load(view, options.page);
	if (error != 0) {
		browser_view_destroy(view);
		return 1;
	}

	/* Its scripts and timers settle, and it is laid out with its images. */
	error = browser_view_settle(view, PROBE_SETTLE_BUDGET, BROWSER_SETTLE_LAYOUT);
	if (error != 0) {
		fprintf(stderr, "browser-probe: cannot settle %s: %s\n", options.page, strerror(error));
		browser_view_destroy(view);
		return 1;
	}

	/* The title the page has once it settled. */
	printf("BROWSERPROBE TITLE %s\n", browser_view_title(view));

	/* Tab, as a keyboard would press and release it. */
	for (index = 0; index < options.tabs; index++) {
		error = browser_view_key(view, "Tab", "Tab", "", 1, 0, 0);
		if (error != 0)
			break;
		error = browser_view_key(view, "Tab", "Tab", "", 0, 0, 0);
		if (error != 0)
			break;
	}

	/* A key the page could not take ends the probe. */
	if (error != 0) {
		fprintf(stderr, "browser-probe: Tab failed: %s\n", strerror(error));
		browser_view_destroy(view);
		return 1;
	}

	/* The picture's pixels. */
	pixels = calloc((size_t)options.width * (size_t)options.height, sizeof(uint32_t));
	if (pixels == NULL) {
		fprintf(stderr, "browser-probe: cannot draw: %s\n", strerror(ENOMEM));
		browser_view_destroy(view);
		return 1;
	}

	/* The drawing, after which the view is no longer needed. */
	error = probe_draw(view, &options, pixels);
	browser_view_destroy(view);
	if (error != 0) {
		free(pixels);
		return 1;
	}

	/* The file. */
	error = probe_write(options.output, pixels, options.width, options.height);
	free(pixels);
	if (error != 0) {
		fprintf(stderr, "browser-probe: cannot write %s: %s\n", options.output, strerror(error));
		return 1;
	}

	/* Succeeded: the picture is written. */
	printf("BROWSERPROBE WROTE %s %ux%u\n", options.output, options.width, options.height);
	return 0;
}

/* Reads the command line; nonzero for one it does not understand. */
static int
probe_parse(
	int argc,
	char **argv,
	struct probe_options *options)
{
	const char *argument;
	const char *value;
	int index;
	int same;
	int error;

	/* The defaults: the CPU, no Tab, the system's fonts, the default size. */
	memset(options, 0, sizeof(*options));
	options->width = PROBE_DEFAULT_WIDTH;
	options->height = PROBE_DEFAULT_HEIGHT;

	/* Each word: an option, the page, then the picture's file. */
	for (index = 1; index < argc; index++) {
		argument = argv[index];

		/* The picture's width. */
		value = probe_value(argument, "--width=");
		if (value != NULL) {
			error = probe_number(value, PROBE_MAX_SIZE, &options->width);
			if (error != 0)
				return error;
			continue;
		}

		/* The picture's height. */
		value = probe_value(argument, "--height=");
		if (value != NULL) {
			error = probe_number(value, PROBE_MAX_SIZE, &options->height);
			if (error != 0)
				return error;
			continue;
		}

		/* How many times Tab is pressed. */
		value = probe_value(argument, "--tab=");
		if (value != NULL) {
			error = probe_number(value, 1000UL, &options->tabs);
			if (error != 0)
				return error;
			continue;
		}

		/* The sans-serif font (each font left out is the system's). */
		value = probe_value(argument, "--font=");
		if (value != NULL) {
			options->fonts.sans = value;
			continue;
		}

		/* The monospace font. */
		value = probe_value(argument, "--mono-font=");
		if (value != NULL) {
			options->fonts.mono = value;
			continue;
		}

		/* The font of the characters the two others lack. */
		value = probe_value(argument, "--fallback-font=");
		if (value != NULL) {
			options->fonts.fallback = value;
			continue;
		}

		/* The GPU renderer. */
		same = strcmp(argument, "--gpu");
		if (same == 0) {
			options->gpu = 1;
			continue;
		}

		/* Any other option is not understood. */
		if (argument[0] == '-')
			return EINVAL;

		/* The page, then the picture's file, then nothing more. */
		if (options->page == NULL) {
			options->page = argument;
		} else if (options->output == NULL) {
			options->output = argument;
		} else {
			return EINVAL;
		}
	}

	/* The page and the picture's file are both needed. */
	if (options->output == NULL)
		return EINVAL;

	/* Succeeded: the command line is read. */
	return 0;
}

/* Finds the value of an option word of the form NAME=VALUE (name ends with '='); NULL for another word. */
static const char *
probe_value(
	const char *argument,
	const char *name)
{
	size_t length;
	int differs;

	/* The word must start with the name. */
	length = strlen(name);
	differs = strncmp(argument, name, length);
	if (differs != 0)
		return NULL;

	/* The value after it. */
	return argument + length;
}

/* Reads a decimal number from 1 to a limit; nonzero for anything else. */
static int
probe_number(
	const char *text,
	unsigned long limit,
	unsigned *number)
{
	unsigned long value;
	char *end;

	/* The digits, all of them. */
	value = strtoul(text, &end, 10);
	if (end == text || *end != '\0')
		return EINVAL;

	/* Within the range. */
	if (value == 0 || value > limit)
		return EINVAL;

	/* Succeeded: the number. */
	*number = (unsigned)value;
	return 0;
}

/* Draws the view into the picture's pixels with the renderer asked for; says why it failed. */
static int
probe_draw(
	struct browser_view *view,
	const struct probe_options *options,
	uint32_t *pixels)
{
	int error;

	/* The GPU, on the engine's offscreen image. */
	if (options->gpu) {
		error = probe_draw_gpu(view, options, pixels);
		if (error != 0)
			return error;
		return 0;
	}

	/* The CPU, into the pixels directly. */
	error = browser_view_draw_pixels(view, pixels, options->width, options->height, (size_t)options->width * sizeof(uint32_t));
	if (error != 0) {
		fprintf(stderr, "browser-probe: cannot draw: %s\n", strerror(error));
		return error;
	}

	/* Succeeded: the pixels hold the page. */
	return 0;
}

/*
 * Draws the view with the GPU into an offscreen image of the engine's own
 * and reads it back: the view is lent the offscreen's device, draws, and
 * lets the device go before the offscreen is destroyed.
 */
static int
probe_draw_gpu(
	struct browser_view *view,
	const struct probe_options *options,
	uint32_t *pixels)
{
	struct browser_offscreen *offscreen;
	struct browser_gpu_failure failure;
	struct browser_gpu gpu;
	struct browser_target target;
	int error;

	/* The offscreen image. */
	error = browser_offscreen_create(options->width, options->height, &offscreen, &failure);
	if (error != 0) {
		fprintf(stderr, "browser-probe: cannot make an offscreen image: %s failed (%d)\n", failure.operation, (int)failure.result);
		return error;
	}

	/* The view draws on its device. */
	browser_offscreen_target(offscreen, &gpu, &target);
	browser_view_set_gpu(view, &gpu);
	error = browser_view_draw(view, &target, VK_NULL_HANDLE, VK_NULL_HANDLE);
	if (error == EIO) {
		browser_view_gpu_failure(view, &failure);
		fprintf(stderr, "browser-probe: cannot draw: %s failed (%d)\n", failure.operation, (int)failure.result);
	} else if (error != 0) {
		fprintf(stderr, "browser-probe: cannot draw: %s\n", strerror(error));
	}

	/* The drawing read back, when there is one. */
	if (error == 0) {
		error = browser_offscreen_read(offscreen, pixels, (size_t)options->width * sizeof(uint32_t), &failure);
		if (error != 0)
			fprintf(stderr, "browser-probe: cannot read the image: %s failed (%d)\n", failure.operation, (int)failure.result);
	}

	/* The view lets the device go, then the offscreen goes. */
	browser_view_set_gpu(view, NULL);
	browser_offscreen_destroy(offscreen);
	if (error != 0)
		return error;

	/* Succeeded: the pixels hold the page. */
	return 0;
}

/* Writes 0xAARRGGBB pixels as a binary PPM file. */
static int
probe_write(
	const char *path,
	const uint32_t *pixels,
	unsigned width,
	unsigned height)
{
	unsigned char row_bytes[3 * PROBE_MAX_SIZE];
	const uint32_t *row;
	FILE *file;
	unsigned x;
	unsigned y;
	size_t written;
	int closed;

	/* The file, with the PPM header. */
	file = fopen(path, "wb");
	if (file == NULL)
		return errno;
	fprintf(file, "P6\n%u %u\n255\n", width, height);

	/* Each row as red, green and blue bytes. */
	for (y = 0; y < height; y++) {
		row = pixels + (size_t)y * width;
		for (x = 0; x < width; x++) {
			row_bytes[3 * x] = (unsigned char)(row[x] >> 16);
			row_bytes[3 * x + 1] = (unsigned char)(row[x] >> 8);
			row_bytes[3 * x + 2] = (unsigned char)row[x];
		}

		/* The row, written whole. */
		written = fwrite(row_bytes, 3, width, file);
		if (written != width) {
			fclose(file);
			return EIO;
		}
	}

	/* The file closed, which writes what is left. */
	closed = fclose(file);
	if (closed != 0)
		return EIO;

	/* Succeeded: the picture is in the file. */
	return 0;
}

/* The view's callback: the page's title changed (a script set it). */
static void
probe_title(
	void *context,
	struct browser_view *view,
	const char *title)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(view);

	/* The line. */
	printf("BROWSERPROBE TITLE %s\n", title);
}

/* The view's callback: a load failed (the probe reads every page at once, so only failures come). */
static void
probe_load(
	void *context,
	struct browser_view *view,
	enum browser_load_state state,
	const char *url,
	int error,
	const char *reason)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(view);

	/* Only a failure is said. */
	if (state != BROWSER_LOAD_FAILED)
		return;

	/* The line. */
	printf("BROWSERPROBE LOAD failed url=%s error=%s tls=%s\n", url, strerror(error), reason);
}

/* The view's callback: a link was opened; the probe says so and stays on its page. */
static enum browser_policy
probe_link(
	void *context,
	struct browser_view *view,
	const char *href)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(view);

	/* The line. */
	printf("BROWSERPROBE LINK %s\n", href);

	/* The probe draws the page it was given. */
	return BROWSER_POLICY_DENY;
}

/* The view's callback: a script wrote to the console. */
static void
probe_console(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(view);

	/* The line. */
	printf("BROWSERPROBE CONSOLE level=%d %.*s\n", level, (int)length, text);
}
