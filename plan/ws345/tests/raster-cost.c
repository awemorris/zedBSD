/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A measurement probe: what one picture of a view of its own threads costs
 * its script thread with the CPU renderer (the interim before the committed
 * display list), on heavy pages: the CPU drawing of the page shown at
 * 800 x 600 once it is laid out (the median of 11), and once after each
 * resize (layout and drawing, the median of 11).
 *
 *   raster-cost PAGE...     (paths; run from the repository's root)
 *
 * Prints one line per page.  A probe of plan/ws345; not a test.
 */

#include <browser/browser.h>

#include "userland/tests/browser-threads/threads-posix.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double
probe_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec * 1e3 + (double)now.tv_nsec / 1e6;
}

static int
probe_compare(const void *left, const void *right)
{
	double a = *(const double *)left;
	double b = *(const double *)right;

	return (a > b) - (a < b);
}

int
main(
	int argc,
	char **argv)
{
	static const struct browser_fonts fonts = {
		"userland/desktop/fonts/Mahora-Regular.ttf", "userland/desktop/fonts/JetBrainsMono-Regular.ttf",
		"userland/desktop/fonts/DroidSansFallbackFull.ttf", "userland/desktop/fonts/Mahora-Bold.ttf",
		"userland/desktop/fonts/Mahora-Mono.ttf"
	};
	struct browser_view_options options;
	struct browser_view *view;
	double draw[11];
	double update[11];
	double start;
	uint32_t *pixels;
	int page;
	int round;

	setenv("XDG_DATA_HOME", "build/tmp/raster-cost-data", 1);
	if (browser_threads_set(browser_threads_posix()) != 0)
		return 2;
	pixels = malloc((size_t)900U * 600U * sizeof(uint32_t));
	if (pixels == NULL)
		return 2;
	for (page = 1; page < argc; page++) {
		memset(&options, 0, sizeof(options));
		options.version = BROWSER_API_VERSION;
		options.fonts = &fonts;
		options.stack_base = __builtin_frame_address(0);
		options.width = 800U;
		options.height = 600U;
		if (browser_view_create(&options, &view) != 0 || browser_view_load(view, argv[page]) != 0)
			return 2;
		(void)browser_view_settle(view, 20000.0, BROWSER_SETTLE_LAYOUT);
		(void)browser_view_draw_pixels(view, pixels, 800U, 600U, 800U * 4U);
		for (round = 0; round < 11; round++) {
			start = probe_ms();
			(void)browser_view_draw_pixels(view, pixels, 800U, 600U, 800U * 4U);
			draw[round] = probe_ms() - start;
		}
		for (round = 0; round < 11; round++) {
			(void)browser_view_resize(view, (round & 1) != 0 ? 800U : 900U, 600U);
			start = probe_ms();
			(void)browser_view_draw_pixels(view, pixels, (round & 1) != 0 ? 800U : 900U, 600U, ((round & 1) != 0 ? 800U : 900U) * 4U);
			update[round] = probe_ms() - start;
		}
		qsort(draw, 11, sizeof(draw[0]), probe_compare);
		qsort(update, 11, sizeof(update[0]), probe_compare);
		printf("raster-cost %s: drawing %.2f ms, layout and drawing after a resize %.2f ms (medians of 11)\n", argv[page], draw[5],
		    update[5]);
		browser_view_destroy(view);
	}
	free(pixels);
	return 0;
}
