/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* A public-API test client captures the first acknowledged frame within an absolute deadline. */
#include <browser/browser.h>

#include "userland/tests/browser-threads/threads-posix.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Arguments and font strings remain borrowed from main's argv until the view is destroyed. */
struct ready_options {
	const char *url;
	const char *output;
	const char *message;
	struct browser_fonts fonts;
	unsigned width;
	unsigned height;
	unsigned deadline;
};

/* Console acknowledgement never reenters the view from its synchronous callback. */
struct ready_state {
	const char *message;
	size_t message_length;
	unsigned acknowledged;
	unsigned budget;
	int load_failed;
	int io_failed;
};

static int ready_number(const char *text, unsigned maximum, unsigned *result);
static int ready_options(int argc, char **argv, struct ready_options *options);
static int ready_run(struct browser_view *view, const struct ready_options *options, struct ready_state *state);
static int ready_capture(struct browser_view *view, const struct ready_options *options);
static int ready_write(const char *path, const uint32_t *pixels, unsigned width, unsigned height);
static void ready_console(void *context, struct browser_view *view, int level, const char *text, size_t length);
static void ready_load(void *context, struct browser_view *view, enum browser_load_state state, const char *url, int error, const char *reason);

/* Own one view until its acknowledged pixels and diagnostic console have been saved. */
int
main(
	int argc,
	char **argv)
{
	struct ready_options options;
	struct ready_state state;
	struct browser_callbacks callbacks;
	struct browser_view_options view_options;
	struct browser_view *view;
	int error;

	/* Refuse malformed invocation before allocating a browsing context. */
	error = ready_options(argc, argv, &options);
	if (error != 0) {
		fprintf(stderr, "usage: wpt-ready-render URL OUT.ppm READY_MESSAGE FONT MONO FALLBACK WIDTH HEIGHT DEADLINE_MS\n");
		return 2;
	}

	/* The engine makes its threads with the POSIX threads, before the view. */
	error = browser_threads_set(browser_threads_posix());
	if (error != 0) {
		fprintf(stderr, "wpt-ready-render: the engine refused the threads (%d)\n", error);
		return 2;
	}

	/* The opaque callback state borrows stable argv storage throughout all engine calls. */
	memset(&state, 0, sizeof(state));
	state.message = options.message;
	state.message_length = strlen(options.message);
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.context = &state;
	callbacks.console = ready_console;
	callbacks.load = ready_load;

	/* Use the component's public background-fetch and CPU-pixel interfaces without a window. */
	memset(&view_options, 0, sizeof(view_options));
	view_options.version = BROWSER_API_VERSION;
	view_options.fonts = &options.fonts;
	view_options.callbacks = &callbacks;
	view_options.stack_base = __builtin_frame_address(0);
	view_options.width = options.width;
	view_options.height = options.height;
	view_options.fetch = BROWSER_FETCH_BACKGROUND;
	error = browser_view_create(&view_options, &view);
	if (error != 0) {
		fprintf(stderr, "ready-render create error %d\n", error);
		return 1;
	}

	/* Teardown follows the outer operation, including every unsuccessful load or capture. */
	error = ready_run(view, &options, &state);
	browser_view_destroy(view);
	if (error != 0) {
		fprintf(stderr, "ready-render run error %d\n", error);
		return 1;
	}

	/* Succeeded: the caller still verifies the full nonce/URL/load protocol in the retained console. */
	return 0;
}

/* Parse one bounded decimal argument without silently accepting a suffix or overflow. */
static int
ready_number(
	const char *text,
	unsigned maximum,
	unsigned *result)
{
	char *end;
	unsigned long number;

	/* strtoul reports overflow separately from the caller's smaller operational bound. */
	errno = 0;
	number = strtoul(text, &end, 10);
	if (errno != 0)
		return errno;

	/* Empty values, trailing input and values beyond this field's bound are invalid. */
	if (end == text || *end != '\0' || number > maximum)
		return EINVAL;

	/* Succeeded: the entire argument denotes an in-range unsigned value. */
	*result = (unsigned)number;
	return 0;
}

/* Bind stable font strings and validate the finite viewport and virtual deadline. */
static int
ready_options(
	int argc,
	char **argv,
	struct ready_options *options)
{
	int error;

	/* Every required positional value has a single meaning and no implicit fallback. */
	if (argc != 10)
		return EINVAL;

	/* The view borrows only argv-backed strings whose owner is main. */
	memset(options, 0, sizeof(*options));
	options->url = argv[1];
	options->output = argv[2];
	options->message = argv[3];
	options->fonts.sans = argv[4];
	options->fonts.mono = argv[5];
	options->fonts.fallback = argv[6];

	/* The bounded width also makes every RGB row-size multiplication safe. */
	error = ready_number(argv[7], 4096U, &options->width);
	if (error != 0)
		return error;

	/* The bounded height keeps the entire pixel allocation at most 64 MiB. */
	error = ready_number(argv[8], 4096U, &options->height);
	if (error != 0)
		return error;

	/* The finite absolute deadline also bounds the number of settle invocations. */
	error = ready_number(argv[9], 60000U, &options->deadline);
	if (error != 0)
		return error;

	/* A zero-sized picture cannot prove a final paint. */
	if (options->width == 0U || options->height == 0U)
		return EINVAL;

	/* Succeeded: the caller has explicit bounded dimensions and stable borrowed strings. */
	return 0;
}

/* Advance only as far as the first acknowledged successful settle call. */
static int
ready_run(
	struct browser_view *view,
	const struct ready_options *options,
	struct ready_state *state)
{
	unsigned budget;
	int error;

	/* Background loading remains the native component's responsibility. */
	error = browser_view_load(view, options->url);
	if (error != 0)
		return error;

	/* A one-millisecond absolute step cannot drain distant timers after readiness. */
	for (budget = 0U; budget <= options->deadline; budget++) {
		state->budget = budget;
		error = browser_view_settle(view, (double)budget, BROWSER_SETTLE_LAYOUT);
		if (error != 0 && error != ETIMEDOUT)
			return error;

		/* A failed main load never becomes a successful blank screenshot. */
		if (state->load_failed)
			return EIO;

		/* Losing protocol text invalidates any apparent acknowledgement. */
		if (state->io_failed)
			return EIO;

		/* Saturated duplicate acknowledgement is a protocol failure, not a chosen reply. */
		if (state->acknowledged > 1U)
			return EPROTO;

		/* An incomplete settle call cannot supply the acknowledged capture point. */
		if (error == 0 && state->acknowledged == 1U)
			break;
	}

	/* An exhausted native work bound remains a failure even if a reply was printed. */
	if (error != 0)
		return error;

	/* Missing readiness still retains a diagnostic image for the caller's non-PASS result. */
	error = ready_capture(view, options);
	if (error != 0)
		return error;

	/* Record the actual capture boundary without advancing the view again. */
	fprintf(stderr, "READY_CAPTURE budget=%u acknowledged=%u\n", state->budget, state->acknowledged);

	/* Succeeded: pixels were captured at the first acknowledged boundary or the diagnostic deadline. */
	return 0;
}

/* Draw into caller-owned pixels and publish them only through the bounded PPM writer. */
static int
ready_capture(
	struct browser_view *view,
	const struct ready_options *options)
{
	uint32_t *pixels;
	size_t count;
	int error;

	/* Both dimensions are at most 4096, so this allocation fits size_t on supported hosts. */
	count = (size_t)options->width * (size_t)options->height;
	pixels = calloc(count, sizeof(*pixels));
	if (pixels == NULL)
		return ENOMEM;

	/* The public CPU renderer uses packed ARGB rows with explicit caller ownership. */
	error = browser_view_draw_pixels(view, pixels, options->width, options->height, (size_t)options->width * sizeof(*pixels));
	if (error == 0)
		error = ready_write(options->output, pixels, options->width, options->height);

	/* Pixel ownership ends on both the draw and write failure paths. */
	free(pixels);
	if (error != 0)
		return error;

	/* Succeeded: the independent image file contains the captured CPU frame. */
	return 0;
}

/* Preserve all RGB samples in a portable lossless image with checked writes. */
static int
ready_write(
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

	/* A unique caller-provided artifact path owns this capture alone. */
	file = fopen(path, "wb");
	if (file == NULL)
		return errno;

	/* The PPM header describes every sample that follows. */
	header = fprintf(file, "P6\n%u %u\n255\n", width, height);
	error = 0;
	if (header < 0)
		error = EIO;

	/* Convert public packed ARGB to RGB without a lossy image codec. */
	count = (size_t)width * (size_t)height;
	for (index = 0; index < count && error == 0; index++) {
		sample[0] = (unsigned char)(pixels[index] >> 16);
		sample[1] = (unsigned char)(pixels[index] >> 8);
		sample[2] = (unsigned char)pixels[index];
		written = fwrite(sample, 1, sizeof(sample), file);
		if (written != sizeof(sample))
			error = EIO;
	}

	/* Stream errors and failed close must not leave an apparently valid success result. */
	failed = ferror(file);
	if (failed && error == 0)
		error = EIO;
	closed = fclose(file);
	if (closed != 0 && error == 0)
		error = EIO;
	if (error != 0)
		return error;

	/* Succeeded: every RGB sample reached the retained artifact. */
	return 0;
}

/* Save console text and a saturated acknowledgement count without engine reentry. */
static void
ready_console(
	void *context,
	struct browser_view *view,
	int level,
	const char *text,
	size_t length)
{
	struct ready_state *state;
	size_t written;
	int prefix;
	int ending;
	int same;

	/* The callback only uses caller-owned state and the borrowed console span. */
	(void)view;
	(void)level;
	state = context;
	prefix = fputs("console: ", stderr);
	written = fwrite(text, 1, length, stderr);
	ending = fputc('\n', stderr);
	if (prefix == EOF || written != length || ending == EOF)
		state->io_failed = 1;

	/* A complete byte match preserves nonce, URL, load state and adapter-removal identity. */
	if (length == state->message_length) {
		same = memcmp(text, state->message, length);
		if (same == 0 && state->acknowledged < 2U)
			state->acknowledged++;
	}

	/* Succeeded: any duplicate is retained as two, and no view operation ran in this callback. */
	return;
}

/* Retain native main-load failure separately from ordinary JavaScript console exceptions. */
static void
ready_load(
	void *context,
	struct browser_view *view,
	enum browser_load_state state,
	const char *url,
	int error,
	const char *reason)
{
	struct ready_state *capture;

	/* Non-failure notifications carry no failed-load state into the capture. */
	(void)view;
	capture = context;
	if (state != BROWSER_LOAD_FAILED)
		return;

	/* The outer settle invocation observes this failure before drawing a diagnostic frame. */
	capture->load_failed = 1;
	if (url == NULL)
		url = "";

	/* Optional native diagnostic strings remain borrowed for this one print. */
	if (reason == NULL)
		reason = "";
	fprintf(stderr, "ready-render load error %d %s %s\n", error, url, reason);

	/* Succeeded: the main-load failure remains visible to the outer operation and the raw log. */
	return;
}
