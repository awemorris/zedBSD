/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests acquire fences of zed_gpu_buffer_v1 revision two in the compositor.
 *
 * A window (wltest's window and renderer, through the Vulkan WSI) draws a
 * few frames in one color.  Then the test gives the next commit one more
 * acquire fence, a kernel fence it signals itself, and presents a frame in
 * another color.  The compositor must not show that frame until the fence
 * is signaled, HOLD milliseconds later, or never with --hold-ms=0.
 */

#include "../../wltest/wltest.h"

#include "userland/base/libwayland/zed-gpu-buffer-v1-client-protocol.h"
#include <uapi/gpu-fence.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The finite run's parameters. */
struct test_options {
	const char *token;
	uint32_t width;
	uint32_t height;
	uint32_t color;
	uint32_t held_color;
	uint32_t frames;
	uint32_t hold_ms;
	uint32_t linger_ms;
};

/* Everything one run owns, and the step that failed. */
struct test_run {
	struct test_options options;
	struct wltest_window window;
	struct wltest_renderer renderer;
	struct zed_gpu_buffer_v1 *factory;
	uint64_t generation;
	int gpu;
	int fence;
	const char *step;
};

static int run_test(struct test_run *run);
static void run_close(struct test_run *run);
static int options_parse(int argc, char **argv, struct test_options *options);
static int option_number(const char *text, uint32_t maximum, uint32_t *number);
static int option_color(const char *text, uint32_t *color);
static int option_size(const char *text, uint32_t *width, uint32_t *height);
static void set_color(struct wltest_renderer *renderer, uint32_t color);
static int fence_open(struct test_run *run);
static int fence_signal(struct test_run *run);
static struct zed_gpu_buffer_v1 *factory_bind(struct wl_display *display);
static void registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
static void registry_remove(void *data, struct wl_registry *registry, uint32_t name);
static void pause_ms(uint32_t milliseconds);

static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_remove,
};

/*
 * Runs the window, the held frame and its fence, and reports the result.
 */
int
main(
	int argc,
	char **argv)
{
	struct test_run run;
	int status;

	/* Nothing is owned before the options are good. */
	memset(&run, 0, sizeof(run));
	run.gpu = -1;
	run.fence = -1;
	status = options_parse(argc, argv, &run.options);
	if (status != 0) {
		fprintf(stderr, "usage: acquire-fence-test [--token=NAME] [--size=WxH] [--color=RRGGBB] [--held-color=RRGGBB] [--frames=1..600] [--hold-ms=0..60000] [--linger-ms=0..60000]\n");
		return 2;
	}

	/* The run, then the release of what it owns. */
	status = run_test(&run);
	run_close(&run);
	if (status != 0) {
		fprintf(stderr, "FENCETEST FAILED run=%s step=%s\n", run.options.token, run.step);
		return 1;
	}

	/* Succeeded. */
	printf("FENCETEST DONE run=%s\n", run.options.token);
	return 0;
}

/*
 * Shows the first frames, commits the held frame with the test's fence,
 * and signals the fence after the hold (or never).
 */
static int
run_test(
	struct test_run *run)
{
	VkResult result;
	uint32_t frame;
	int status;

	/* The window, as wltest makes it. */
	run->step = "window";
	status = wltest_window_open(&run->window, NULL, run->options.width, run->options.height, 0);
	if (status != 0)
		return -1;

	/* Its renderer, drawing one solid color. */
	run->step = "renderer";
	result = wltest_renderer_open(&run->renderer, &run->window, VK_PRESENT_MODE_FIFO_KHR);
	if (result != VK_SUCCESS)
		return -1;
	run->renderer.solid_set = 1;
	set_color(&run->renderer, run->options.color);

	/* The first frames, in the first color. */
	run->step = "draw";
	for (frame = 1U; frame <= run->options.frames; frame++) {
		(void)wltest_window_dispatch(&run->window);
		result = wltest_renderer_draw(&run->renderer, frame);
		if (result != VK_SUCCESS)
			return -1;
	}

	/*
	 * The WSI commits from its own thread; waiting for the queue to be idle
	 * retires the last presentation, so the fence below goes with the held
	 * frame's commit and not with an earlier one.
	 */
	run->step = "idle";
	result = vkQueueWaitIdle(run->renderer.queue);
	if (result != VK_SUCCESS)
		return -1;

	/* The first color is on the screen. */
	printf("FENCETEST SHOWN run=%s frames=%u\n", run->options.token, run->options.frames);
	fflush(stdout);

	/* A fence of the test's own, pending. */
	run->step = "fence";
	status = fence_open(run);
	if (status != 0)
		return -1;

	/* The compositor's factory at revision two. */
	run->step = "factory";
	run->factory = factory_bind(run->window.display);
	if (run->factory == NULL)
		return -1;

	/* The fence goes with the next commit, which the WSI makes with its own fence as well. */
	zed_gpu_buffer_v1_set_acquire_fence(run->factory, run->window.surface, run->fence, run->generation);

	/* The held frame, in the second color. */
	run->step = "held";
	set_color(&run->renderer, run->options.held_color);
	result = wltest_renderer_draw(&run->renderer, frame);
	if (result != VK_SUCCESS)
		return -1;

	/* The held frame is presented. */
	printf("FENCETEST HELD run=%s frame=%u generation=%llu\n", run->options.token, frame, (unsigned long long)run->generation);
	fflush(stdout);

	/* The fence is signaled after the hold, or never. */
	if (run->options.hold_ms != 0U) {
		pause_ms(run->options.hold_ms);
		run->step = "signal";
		status = fence_signal(run);
		if (status != 0)
			return -1;

		/* The held frame may be shown now. */
		printf("FENCETEST SIGNALED run=%s\n", run->options.token);
		fflush(stdout);
	}

	/* The window stays for the screen to be read. */
	pause_ms(run->options.linger_ms);

	/* Succeeded. */
	return 0;
}

/*
 * Releases what a run owns, in reverse order.
 */
static void
run_close(
	struct test_run *run)
{
	/* The factory binding. */
	if (run->factory != NULL)
		zed_gpu_buffer_v1_destroy(run->factory);

	/* The fence and the GPU open. */
	if (run->fence >= 0)
		close(run->fence);
	if (run->gpu >= 0)
		close(run->gpu);

	/* The renderer before the window it draws in. */
	(void)wltest_renderer_close(&run->renderer);
	wltest_window_close(&run->window);
}

/*
 * Parses the finite run's options.
 */
static int
options_parse(
	int argc,
	char **argv,
	struct test_options *options)
{
	const char *text;
	int index;
	int match;
	int status;

	/* A 300x200 red window that turns blue after two seconds. */
	memset(options, 0, sizeof(*options));
	options->token = "manual";
	options->width = 300U;
	options->height = 200U;
	options->color = 0xff0000U;
	options->held_color = 0x0000ffU;
	options->frames = 10U;
	options->hold_ms = 2000U;
	options->linger_ms = 3000U;

	/* Each option in command-line order. */
	for (index = 1; index < argc; index++) {
		text = argv[index];

		/* The run's name in the output. */
		match = strncmp(text, "--token=", 8U);
		if (match == 0) {
			options->token = text + 8U;
			continue;
		}

		/* The window's size. */
		match = strncmp(text, "--size=", 7U);
		if (match == 0) {
			status = option_size(text + 7U, &options->width, &options->height);
			if (status != 0)
				return -1;
			continue;
		}

		/* The first color. */
		match = strncmp(text, "--color=", 8U);
		if (match == 0) {
			status = option_color(text + 8U, &options->color);
			if (status != 0)
				return -1;
			continue;
		}

		/* The held frame's color. */
		match = strncmp(text, "--held-color=", 13U);
		if (match == 0) {
			status = option_color(text + 13U, &options->held_color);
			if (status != 0)
				return -1;
			continue;
		}

		/* The number of frames before the held one. */
		match = strncmp(text, "--frames=", 9U);
		if (match == 0) {
			status = option_number(text + 9U, 600U, &options->frames);
			if (status != 0 || options->frames == 0U)
				return -1;
			continue;
		}

		/* The time until the fence is signaled; zero is never. */
		match = strncmp(text, "--hold-ms=", 10U);
		if (match == 0) {
			status = option_number(text + 10U, 60000U, &options->hold_ms);
			if (status != 0)
				return -1;
			continue;
		}

		/* The time the window stays afterwards. */
		match = strncmp(text, "--linger-ms=", 12U);
		if (match == 0) {
			status = option_number(text + 12U, 60000U, &options->linger_ms);
			if (status != 0)
				return -1;
			continue;
		}

		/* No other option exists. */
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Parses a decimal number no larger than maximum.
 */
static int
option_number(
	const char *text,
	uint32_t maximum,
	uint32_t *number)
{
	unsigned long parsed;
	char *end;

	/* Digits only. */
	if (*text < '0' || *text > '9')
		return -1;
	parsed = strtoul(text, &end, 10);

	/* Nothing after them, and within the bound. */
	if (*end != '\0' || parsed > maximum)
		return -1;

	/* Succeeded. */
	*number = (uint32_t)parsed;
	return 0;
}

/*
 * Parses a color written as six hexadecimal digits, RRGGBB.
 */
static int
option_color(
	const char *text,
	uint32_t *color)
{
	unsigned long parsed;
	size_t length;
	char *end;

	/* Exactly six digits. */
	length = strlen(text);
	if (length != 6U)
		return -1;
	parsed = strtoul(text, &end, 16);
	if (*end != '\0')
		return -1;

	/* Succeeded. */
	*color = (uint32_t)parsed;
	return 0;
}

/*
 * Parses a size written as WIDTHxHEIGHT.
 */
static int
option_size(
	const char *text,
	uint32_t *width,
	uint32_t *height)
{
	unsigned long parsed;
	char *end;

	/* The width, at least what the renderer draws in. */
	parsed = strtoul(text, &end, 10);
	if (*end != 'x' || parsed < 64U || parsed > 4096U)
		return -1;
	*width = (uint32_t)parsed;

	/* The height after the x. */
	parsed = strtoul(end + 1, &end, 10);
	if (*end != '\0' || parsed < 16U || parsed > 4096U)
		return -1;
	*height = (uint32_t)parsed;

	/* Succeeded. */
	return 0;
}

/*
 * Sets the renderer's solid color from 0xRRGGBB.
 */
static void
set_color(
	struct wltest_renderer *renderer,
	uint32_t color)
{
	/* Each channel from zero to one. */
	renderer->solid[0] = (float)((color >> 16) & 0xffU) / 255.0f;
	renderer->solid[1] = (float)((color >> 8) & 0xffU) / 255.0f;
	renderer->solid[2] = (float)(color & 0xffU) / 255.0f;
}

/*
 * Creates a pending kernel fence and takes the right to signal its first
 * generation, as a producer of work would.
 */
static int
fence_open(
	struct test_run *run)
{
	struct gpu_fence_create create;
	struct gpu_fence_bind bind;
	int error;

	/* The fence belongs to the display's GPU. */
	run->gpu = open("/dev/gpu0", O_RDWR | O_CLOEXEC);
	if (run->gpu < 0)
		return -1;

	/* One pending fence at its first generation. */
	memset(&create, 0, sizeof(create));
	create.version = GPU_ABI_VERSION;
	create.size = sizeof(create);
	create.flags = GPU_HANDLE_CLOEXEC;
	create.fd = -1;
	error = ioctl(run->gpu, GPU_FENCE_CREATE, &create);
	if (error != 0)
		return -1;
	run->fence = create.fd;
	run->generation = create.generation;

	/* This open becomes the generation's producer. */
	memset(&bind, 0, sizeof(bind));
	bind.version = GPU_ABI_VERSION;
	bind.size = sizeof(bind);
	bind.fd = run->fence;
	bind.generation = run->generation;
	error = ioctl(run->gpu, GPU_FENCE_BIND, &bind);
	if (error != 0)
		return -1;

	/* Succeeded. */
	return 0;
}

/*
 * Signals the fence's generation as done without error.
 */
static int
fence_signal(
	struct test_run *run)
{
	struct gpu_fence_state state;
	int error;

	/* The producer reports success. */
	memset(&state, 0, sizeof(state));
	state.version = GPU_ABI_VERSION;
	state.size = sizeof(state);
	state.fd = run->fence;
	state.generation = run->generation;
	error = ioctl(run->gpu, GPU_FENCE_SIGNAL, &state);
	if (error != 0)
		return -1;

	/* Succeeded. */
	return 0;
}

/*
 * Binds the compositor's zed_gpu_buffer_v1 at revision two, or returns NULL.
 */
static struct zed_gpu_buffer_v1 *
factory_bind(
	struct wl_display *display)
{
	struct zed_gpu_buffer_v1 *factory;
	struct wl_registry *registry;
	int status;

	/* A registry of the test's own. */
	factory = NULL;
	registry = wl_display_get_registry(display);
	if (registry == NULL)
		return NULL;

	/* Read once: the listener binds the factory. */
	status = wl_registry_add_listener(registry, &registry_listener, &factory);
	if (status == 0)
		status = wl_display_roundtrip(display);
	wl_registry_destroy(registry);

	/* A failed round trip leaves no factory. */
	if (status < 0 && factory != NULL) {
		zed_gpu_buffer_v1_destroy(factory);
		factory = NULL;
	}

	/* The factory, if the compositor has revision two. */
	return factory;
}

/*
 * Binds zed_gpu_buffer_v1 when it is offered at revision two or later.
 */
static void
registry_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version)
{
	struct zed_gpu_buffer_v1 **factory;
	int match;

	/* Only the factory, at revision two, and only once. */
	factory = data;
	match = strcmp(interface, "zed_gpu_buffer_v1");
	if (match != 0 || version < 2U || *factory != NULL)
		return;

	/* Succeeded. */
	*factory = wl_registry_bind(registry, name, &zed_gpu_buffer_v1_interface, 2U);
}

/*
 * Ignores global removal, which does not matter in this finite run.
 */
static void
registry_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name)
{
	/* Nothing is done. */
	(void)data;
	(void)registry;
	(void)name;
}

/*
 * Sleeps for a number of milliseconds.
 */
static void
pause_ms(
	uint32_t milliseconds)
{
	struct timespec pause;

	/* Seconds and the rest in nanoseconds. */
	pause.tv_sec = milliseconds / 1000U;
	pause.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	(void)nanosleep(&pause, NULL);
}
