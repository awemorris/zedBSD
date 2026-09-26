/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Runs a finite standard Wayland/Vulkan presentation and capture session.
 */

#include "wltest.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* One invocation selects its frame count, pacing mode and optional capture handshake. */
struct wltest_options {
	const char *token;
	const char *display;
	const char *mode_name;
	VkPresentModeKHR mode;
	uint32_t frames;
	uint32_t recreate;
	uint32_t delay;
	int verify;
	int windowed;
	uint32_t width;
	uint32_t height;
	int solid;
	uint32_t color;
	uint32_t fullscreen_at;
	uint32_t unfullscreen_at;
};

static int options_parse(int argc, char **argv, struct wltest_options *options);
static int option_number(const char *text, uint32_t maximum, uint32_t *number);
static int option_size(const char *text, uint32_t *width, uint32_t *height);
static int option_color(const char *text, uint32_t *color);
static VkResult follow_size(struct wltest_renderer *renderer, struct wltest_window *window, const struct wltest_options *options);
static int capture_wait(struct wltest_window *window);

/*
 * Keeps setup, drawing and shutdown results independently observable to the harness.
 */
int
main(
	int argc,
	char **argv)
{
	struct wltest_window window;
	struct wltest_renderer renderer;
	struct wltest_options options;
	struct timespec pause;
	const char *operation;
	uint64_t present_ns;
	uint64_t present_max_ns;
	uint32_t frame;
	uint32_t completed;
	VkResult result;
	VkResult cleanup;
	int status;

	/* An unopened native window still supports the ordinary cleanup path. */
	memset(&window, 0, sizeof(window));

	/* No Vulkan object is owned until renderer initialization publishes it. */
	memset(&renderer, 0, sizeof(renderer));

	/* Argument failure starts no connection or GPU namespace. */
	status = options_parse(argc, argv, &options);
	if (status != 0) {
		fprintf(stderr, "usage: wltest [--display=NAME] [--frames=1..3600] [--mode=fifo|mailbox] [--verify-session] [--recreate-at=N] [--delay-ms=0..1000] [--token=NAME] [--windowed] [--size=WxH] [--color=RRGGBB] [--fullscreen-at=N] [--unfullscreen-at=N]\n");
		return 2;
	}

	/* Publish the selected finite run before setup can produce a diagnostic. */
	printf("WLTEST START run=%s mode=%s frames=%u\n", options.token, options.mode_name, options.frames);
	fflush(stdout);

	/* A setup failure reports no completed frames and retains its native operation name. */
	completed = 0U;
	result = VK_SUCCESS;
	operation = "wltest_window_open";
	status = wltest_window_open(&window, options.display, options.width, options.height, !options.windowed);
	if (status != 0)
		goto cleanup;

	/* Create renderer ownership only after the native surface is configured. */
	result = wltest_renderer_open(&renderer, &window, options.mode);
	operation = renderer.operation;
	if (result != VK_SUCCESS) {
		status = -1;
		goto cleanup;
	}

	/* A window test draws one solid color. */
	renderer.solid_set = options.solid;
	renderer.solid[0] = (float)((options.color >> 16) & 0xffU) / 255.0f;
	renderer.solid[1] = (float)((options.color >> 8) & 0xffU) / 255.0f;
	renderer.solid[2] = (float)(options.color & 0xffU) / 255.0f;

	/* The ordinary path animates without CPU readback; capture pauses are explicit test options. */
	pause.tv_sec = options.delay / 1000U;
	pause.tv_nsec = (options.delay % 1000U) * 1000000L;
	for (frame = 1U; frame <= options.frames; frame++) {
		/* Native closure ends the finite run before another image is submitted. */
		operation = "wltest_window_dispatch";
		status = wltest_window_dispatch(&window);
		if (status != 0 || window.closed != 0)
			break;

		/* Asks for fullscreen, or to leave it, at the selected frames. */
		if (frame == options.fullscreen_at) {
			xdg_toplevel_set_fullscreen(window.toplevel, NULL);
			printf("WLTEST FULLSCREEN run=%s before=%u\n", options.token, frame);
		}

		/* And leaves it. */
		if (frame == options.unfullscreen_at) {
			xdg_toplevel_unset_fullscreen(window.toplevel);
			printf("WLTEST UNFULLSCREEN run=%s before=%u\n", options.token, frame);
		}

		/* A size the compositor configured replaces the swapchain. */
		result = follow_size(&renderer, &window, &options);
		operation = renderer.operation;
		if (result != VK_SUCCESS) {
			status = -1;
			break;
		}

		/* Recreate at the selected frame while retaining the configured native surface. */
		if (frame == options.recreate) {
			result = wltest_renderer_recreate(&renderer);
			operation = renderer.operation;
			if (result != VK_SUCCESS) {
				status = -1;
				break;
			}

			/* The capture harness can associate subsequent frames with the replacement chain. */
			printf("WLTEST RECREATE run=%s before=%u\n", options.token, frame);
		}

		/* Submit this frame using the standard Vulkan renderer. */
		result = wltest_renderer_draw(&renderer, frame);
		operation = renderer.operation;
		if (result != VK_SUCCESS) {
			status = -1;
			break;
		}

		/* Only completed draws advance the externally observed frame count. */
		completed++;
		printf(
			"WLTEST FRAME run=%s frame=%u width=%u height=%u mode=%s\n",
			options.token,
			frame,
			renderer.extent.width,
			renderer.extent.height,
			options.mode_name);
		fflush(stdout);

		/* Verification holds the image stable; ordinary animation uses the configured delay. */
		if (options.verify != 0) {
			operation = "capture_wait";
			status = capture_wait(&window);
			if (status != 0)
				break;
		} else if (options.delay != 0U) {
			status = nanosleep(&pause, NULL);
			if (status != 0 && errno != EINTR) {
				operation = "nanosleep";
				break;
			}

			/* An interrupted animation delay does not invalidate the completed frame. */
			status = 0;
		}
	}

cleanup:
	/* Cleanup never overwrites the operation that explains a failed render or native request. */
	present_ns = renderer.present_ns;
	present_max_ns = renderer.present_max_ns;
	cleanup = wltest_renderer_close(&renderer);
	wltest_window_close(&window);
	if (status != 0 ||
	    result != VK_SUCCESS ||
	    cleanup != VK_SUCCESS) {
		fprintf(
			stderr,
			"WLTEST FAILED run=%s api=%s result=%d cleanup=%d errno=%d frames=%u\n",
			options.token,
			operation,
			(int)result,
			(int)cleanup,
			errno,
			completed);
		return 1;
	}

	/* Report the finite run only after both renderer and native ownership have retired. */
	printf("WLTEST PRESENT run=%s frames=%u total_us=%llu max_us=%llu\n", options.token, completed, (unsigned long long)(present_ns / 1000U), (unsigned long long)(present_max_ns / 1000U));
	printf("WLTEST DONE run=%s frames=%u\n", options.token, completed);
	fflush(stdout);

	/* Succeeded: the completed frame count and clean shutdown are visible to the harness. */
	return 0;
}

/* Parses only finite, deterministic runtime controls. */
static int
options_parse(
	int argc,
	char **argv,
	struct wltest_options *options)
{
	uint32_t number;
	int index;
	int match;
	int status;
	const char *position;

	/* The normal invocation terminates after sixty paced frames. */
	memset(options, 0, sizeof(*options));
	options->frames = 60U;
	options->delay = 16U;
	options->token = "manual";
	options->mode_name = "fifo";
	options->mode = VK_PRESENT_MODE_FIFO_KHR;
	options->width = 320U;
	options->height = 240U;

	/* Apply each explicit option in command-line order. */
	for (index = 1; index < argc; index++) {
		/* A window: no fullscreen request at the start. */
		match = strcmp(argv[index], "--windowed");
		if (match == 0) {
			options->windowed = 1;
			continue;
		}

		/* The size the window has when the compositor leaves it to the client. */
		match = strncmp(argv[index], "--size=", 7U);
		if (match == 0) {
			status = option_size(argv[index] + 7U, &options->width, &options->height);
			if (status != 0)
				return -1;
			continue;
		}

		/* One solid color instead of the test pattern. */
		match = strncmp(argv[index], "--color=", 8U);
		if (match == 0) {
			status = option_color(argv[index] + 8U, &options->color);
			if (status != 0)
				return -1;
			options->solid = 1;
			continue;
		}

		/* The frame before which fullscreen is asked for. */
		match = strncmp(argv[index], "--fullscreen-at=", 16U);
		if (match == 0) {
			status = option_number(argv[index] + 16U, 3600U, &options->fullscreen_at);
			if (status != 0)
				return -1;
			continue;
		}

		/* The frame before which fullscreen is left. */
		match = strncmp(argv[index], "--unfullscreen-at=", 18U);
		if (match == 0) {
			status = option_number(argv[index] + 18U, 3600U, &options->unfullscreen_at);
			if (status != 0)
				return -1;
			continue;
		}

		/* Verification selects the six frames understood by the capture oracle. */
		match = strcmp(argv[index], "--verify-session");
		if (match == 0) {
			options->verify = 1;
			options->frames = 6U;
			continue;
		}

		/* MAILBOX permits replacement of images the compositor has not presented. */
		match = strcmp(argv[index], "--mode=mailbox");
		if (match == 0) {
			options->mode_name = "mailbox";
			options->mode = VK_PRESENT_MODE_MAILBOX_KHR;
			continue;
		}

		/* FIFO requests ordered presentation through the same standard swapchain API. */
		match = strcmp(argv[index], "--mode=fifo");
		if (match == 0) {
			options->mode_name = "fifo";
			options->mode = VK_PRESENT_MODE_FIFO_KHR;
			continue;
		}

		/* The drawing loop must have a positive, finite frame bound. */
		match = strncmp(argv[index], "--frames=", 9U);
		if (match == 0) {
			status = option_number(argv[index] + 9U, 3600U, &number);
			if (status != 0 || number == 0U)
				return -1;

			/* A validated explicit count supersedes any earlier default or verification count. */
			options->frames = number;
			continue;
		}

		/* Zero leaves recreation disabled; a positive value selects its frame boundary. */
		match = strncmp(argv[index], "--recreate-at=", 14U);
		if (match == 0) {
			status = option_number(argv[index] + 14U, 3600U, &options->recreate);
			if (status != 0)
				return -1;

			/* The validated recreation option has no other side effects. */
			continue;
		}

		/* Animation delay remains bounded and permits an exact one-second setting. */
		match = strncmp(argv[index], "--delay-ms=", 11U);
		if (match == 0) {
			status = option_number(argv[index] + 11U, 1000U, &options->delay);
			if (status != 0)
				return -1;

			/* Main normalizes this millisecond value into seconds and nanoseconds. */
			continue;
		}

		/* An explicit display name cannot be empty. */
		match = strncmp(argv[index], "--display=", 10U);
		if (match == 0) {
			options->display = argv[index] + 10U;
			if (*options->display == '\0')
				return -1;

			/* Immutable argv storage keeps the selected endpoint valid through setup. */
			continue;
		}

		/* Restrict run tokens to an unambiguous, bounded machine-log field. */
		match = strncmp(argv[index], "--token=", 8U);
		if (match == 0) {
			options->token = argv[index] + 8U;
			position = options->token;
			if (*position == '\0')
				return -1;

			/* Reject whitespace and control characters before a token can reach machine logs. */
			while (*position != '\0') {
				if (!((*position >= 'a' && *position <= 'z') ||
				      (*position >= 'A' && *position <= 'Z') ||
				      (*position >= '0' && *position <= '9') ||
				      *position == '-' ||
				      *position == '_'))
					return -1;

				/* Every accepted character advances the same immutable token span. */
				position++;
			}

			/* The harness does not accept arbitrarily long identity fields. */
			if (position - options->token > 64)
				return -1;

			/* All characters and the complete token length are valid. */
			continue;
		}

		/* Unknown options cannot silently change the intended verification session. */
		return -1;
	}

	/* Succeeded: every requested runtime control has a finite, understood interpretation. */
	return 0;
}

/* Rejects signs, overflow and trailing input before an option changes runtime bounds. */
static int
option_number(
	const char *text,
	uint32_t maximum,
	uint32_t *number)
{
	uint32_t parsed;
	uint32_t digit;

	/* Every numeric option has a bounded decimal grammar. */
	parsed = 0U;
	if (*text == '\0')
		return -1;

	/* Extend the parsed value only after the next decimal digit passes the bound. */
	while (*text != '\0') {
		if (*text < '0' || *text > '9')
			return -1;

		/* Refuse overflow before multiplication or addition can lose information. */
		digit = (uint32_t)(*text - '0');
		if (parsed > maximum / 10U ||
		    (parsed == maximum / 10U && digit > maximum % 10U))
			return -1;

		/* The checked digit advances both the value and its unconsumed input suffix. */
		parsed = parsed * 10U + digit;
		text++;
	}

	/* Publish the option only after its entire string is accepted. */
	*number = parsed;

	/* Succeeded: the caller receives one decimal value within its requested bound. */
	return 0;
}

/* Keeps the real displayed frame stable while the external harness captures it. */
static int
capture_wait(
	struct wltest_window *window)
{
	struct pollfd descriptor;
	char line[16];
	size_t used;
	ssize_t bytes;
	uint32_t waits;
	int status;

	/* A missing harness cannot leave a guest test waiting indefinitely. */
	used = 0U;
	descriptor.fd = STDIN_FILENO;
	descriptor.events = POLLIN;
	for (waits = 0U; waits < 600U; waits++) {
		/* Native closure or a failed event connection invalidates further captures. */
		status = wltest_window_dispatch(window);
		if (status != 0 || window->closed != 0)
			return -1;

		/* Poll in bounded intervals so native events and the capture deadline remain observable. */
		descriptor.revents = 0;
		status = poll(&descriptor, 1U, 100);
		if (status < 0) {
			if (errno == EINTR)
				continue;

			/* Other polling failures end this capture handshake. */
			return -1;
		}

		/* An idle interval consumes time without consuming an input byte. */
		if (status == 0)
			continue;

		/* The bounded command buffer advances one received byte at a time. */
		bytes = read(STDIN_FILENO, line + used, 1U);
		if (bytes != 1)
			return -1;

		/* Only an exact completed next command authorizes a new rendered frame. */
		if (line[used] == '\n') {
			line[used] = '\0';
			status = strcmp(line, "next");
			if (status != 0)
				return -1;

			/* End the wait without consuming input intended for a later frame. */
			break;
		}

		/* Leave room for the terminator while refusing overlong harness commands. */
		used++;
		if (used == sizeof(line) - 1U)
			return -1;
	}

	/* Exhausting the finite wait budget is distinct from receiving a valid command. */
	if (waits == 600U) {
		errno = ETIMEDOUT;
		return -1;
	}

	/* Succeeded: the harness has finished capturing this displayed frame. */
	return 0;
}

/* Parses WIDTHxHEIGHT, each 64..8192. */
static int
option_size(
	const char *text,
	uint32_t *width,
	uint32_t *height)
{
	char *end;
	unsigned long number;

	/* The width, then x. */
	number = strtoul(text, &end, 10);
	if (end == text || *end != 'x' || number < 64UL || number > 8192UL)
		return -1;
	*width = (uint32_t)number;

	/* The height, and nothing after it. */
	text = end + 1;
	number = strtoul(text, &end, 10);
	if (end == text || *end != '\0' || number < 64UL || number > 8192UL)
		return -1;
	*height = (uint32_t)number;

	/* Succeeded. */
	return 0;
}

/* Parses a color written as six hexadecimal digits RRGGBB. */
static int
option_color(
	const char *text,
	uint32_t *color)
{
	char *end;
	unsigned long number;
	size_t length;

	/* Exactly six hexadecimal digits. */
	length = strlen(text);
	if (length != 6U)
		return -1;
	number = strtoul(text, &end, 16);
	if (*end != '\0')
		return -1;

	/* Succeeded. */
	*color = (uint32_t)number;
	return 0;
}

/*
 * Replaces the swapchain when the compositor configured a size other than
 * the one the images have.
 */
static VkResult
follow_size(
	struct wltest_renderer *renderer,
	struct wltest_window *window,
	const struct wltest_options *options)
{
	VkResult result;

	/* The same size needs nothing. */
	if (window->width == renderer->extent.width &&
	    window->height == renderer->extent.height)
		return VK_SUCCESS;

	/* A new chain of the configured size. */
	renderer->extent.width = window->width;
	renderer->extent.height = window->height;
	result = wltest_renderer_recreate(renderer);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the harness sees the new size. */
	printf("WLTEST RESIZE run=%s width=%u height=%u\n", options->token, window->width, window->height);
	return VK_SUCCESS;
}

