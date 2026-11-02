/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of a background image taller than the GPU renderer's atlas
 * (a sprite sheet of icons, each shown by its background-position): on
 * plan/ws074/tests/pages/tall-sprite.html (a 40 by 3000 pixel image of
 * 40 pixel bands, red, green, blue and yellow in turn), the icons drawn by
 * the GPU (Vulkan, Mesa's lavapipe without a GPU) have the colours the CPU
 * draws: the bands at the top and near the bottom of the image, and a
 * band of the image scaled down by background-size.
 *
 *   host-tall-image
 *
 * Run from the repository's root (the page and the fonts are the
 * checked-in ones).  Prints one line per failed check and a summary; exits
 * 77 when the host has no Vulkan device.
 */

#include <browser/browser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The picture's size. */
#define TALL_WIDTH	240U
#define TALL_HEIGHT	50U

static unsigned checks;
static unsigned failures;

static void tall_check(int condition, const char *what);

int
main(
	void)
{
	static const struct browser_fonts fonts = {
		"userland/desktop/fonts/Mahora-Regular.ttf", "userland/desktop/fonts/JetBrainsMono-Regular.ttf",
		"userland/desktop/fonts/DroidSansFallbackFull.ttf", "userland/desktop/fonts/Mahora-Bold.ttf",
		"userland/desktop/fonts/Mahora-Mono.ttf"
	};
	/* The places to compare: the middle of each icon (the bands 0, 1, 73, 74, and band 1 of the image at half size). */
	static const unsigned places[][2] = { { 20, 20 }, { 60, 20 }, { 100, 20 }, { 140, 20 }, { 170, 10 } };
	static const uint32_t colours[] = { 0xdc2828U, 0x28a03cU, 0x28a03cU, 0x2850dcU, 0x28a03cU };
	struct browser_view_options options;
	struct browser_offscreen *offscreen;
	struct browser_gpu_failure failure;
	struct browser_target target;
	struct browser_gpu gpu;
	struct browser_view *view;
	uint32_t *cpu;
	uint32_t *gpu_pixels;
	char what[96];
	size_t index;
	uint32_t at;
	int error;

	/* Storage stays under build/, away from the home directory. */
	setenv("XDG_DATA_HOME", "build/ws074-host-data", 1);

	/* The page, laid out at once. */
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.fonts = &fonts;
	options.stack_base = __builtin_frame_address(0);
	options.width = TALL_WIDTH;
	options.height = TALL_HEIGHT;
	error = browser_view_create(&options, &view);
	tall_check(error == 0, "view: made");
	if (error != 0)
		return 1;
	error = browser_view_load(view, "plan/ws074/tests/pages/tall-sprite.html");
	tall_check(error == 0, "page: loads");
	(void)browser_view_settle(view, 1000.0, BROWSER_SETTLE_LAYOUT);

	/* The CPU's picture. */
	cpu = calloc((size_t)TALL_WIDTH * TALL_HEIGHT, sizeof(uint32_t));
	gpu_pixels = calloc((size_t)TALL_WIDTH * TALL_HEIGHT, sizeof(uint32_t));
	if (cpu == NULL || gpu_pixels == NULL)
		return 2;
	error = browser_view_draw_pixels(view, cpu, TALL_WIDTH, TALL_HEIGHT, (size_t)TALL_WIDTH * sizeof(uint32_t));
	tall_check(error == 0, "CPU: draws");

	/* The GPU's picture (a host without a device skips the test). */
	error = browser_offscreen_create(TALL_WIDTH, TALL_HEIGHT, &offscreen, &failure);
	if (error != 0) {
		printf("host-tall-image: no Vulkan device (%d), skipped\n", error);
		return 77;
	}
	browser_offscreen_target(offscreen, &gpu, &target);
	browser_view_set_gpu(view, &gpu);
	error = browser_view_draw(view, &target, VK_NULL_HANDLE, VK_NULL_HANDLE);
	tall_check(error == 0, "GPU: draws");
	if (error == 0)
		error = browser_offscreen_read(offscreen, gpu_pixels, (size_t)TALL_WIDTH * sizeof(uint32_t), &failure);
	tall_check(error == 0, "GPU: reads back");
	browser_view_set_gpu(view, NULL);
	browser_offscreen_destroy(offscreen);

	/* Each icon: the CPU's colour is the band's, and the GPU's is the CPU's. */
	for (index = 0; index < sizeof(places) / sizeof(places[0]); index++) {
		at = places[index][1] * TALL_WIDTH + places[index][0];
		snprintf(what, sizeof(what), "icon %zu: the CPU draws #%06x (has #%06x)", index, (unsigned)colours[index], (unsigned)(cpu[at] & 0xffffffU));
		tall_check((cpu[at] & 0xffffffU) == colours[index], what);
		snprintf(what, sizeof(what), "icon %zu: the GPU draws what the CPU does (#%06x, has #%06x)", index, (unsigned)(cpu[at] & 0xffffffU),
		    (unsigned)(gpu_pixels[at] & 0xffffffU));
		tall_check((gpu_pixels[at] & 0xffffffU) == (cpu[at] & 0xffffffU), what);
	}
	free(cpu);
	free(gpu_pixels);
	browser_view_destroy(view);

	/* The summary. */
	printf("host-tall-image: %u checks, %u failed\n", checks, failures);
	if (failures != 0)
		return 1;
	return 0;
}

/* Counts a check and reports it when it failed. */
static void
tall_check(
	int condition,
	const char *what)
{
	checks++;
	if (!condition) {
		failures++;
		printf("FAIL %s\n", what);
	}
}
