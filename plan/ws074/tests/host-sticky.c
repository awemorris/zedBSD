/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws074-p235: position: sticky against the page's scroll
 * (pages/sticky-page.html).  Draws the page at scroll 0, 300 and 1000 and
 * reads the colour of the header (red, top 30), the side box (green,
 * natural top 240, top 0) and the footer (blue, bottom 10).
 *
 *   host-sticky PAGES SANS MONO FALLBACK
 */

#include <browser/browser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH	800U
#define HEIGHT	600U

static int failures;

static uint32_t pixels[WIDTH * HEIGHT];

static void
check(
	int condition,
	const char *what)
{
	if (!condition) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

/* Tells whether the pixel at (x, y) is dominantly red, green or blue (r, g or b). */
static int
colour_at(
	unsigned x,
	unsigned y,
	char which)
{
	uint32_t value;
	unsigned red;
	unsigned green;
	unsigned blue;

	value = pixels[y * WIDTH + x];
	red = (value >> 16) & 0xffU;
	green = (value >> 8) & 0xffU;
	blue = value & 0xffU;
	if (which == 'r')
		return red > 150 && green < 90 && blue < 90;
	if (which == 'g')
		return green > 120 && red < 90 && blue < 90;
	return blue > 150 && red < 90 && green < 140;
}

static int
draw(
	struct browser_view *view,
	double scroll)
{
	(void)browser_view_scroll_to(view, 0.0, scroll);
	return browser_view_draw_pixels(view, pixels, WIDTH, HEIGHT, WIDTH * sizeof(uint32_t));
}

int
main(
	int argc,
	char **argv)
{
	struct browser_fonts paths;
	struct browser_view_options options;
	struct browser_view *view;
	char path[1024];
	int error;

	setenv("XDG_DATA_HOME", "build/ws074-host-data", 1);
	if (argc < 5) {
		fprintf(stderr, "usage: host-sticky PAGES SANS MONO FALLBACK\n");
		return 2;
	}
	memset(&paths, 0, sizeof(paths));
	paths.sans = argv[2];
	paths.mono = argv[3];
	paths.fallback = argv[4];
	memset(&options, 0, sizeof(options));
	options.version = BROWSER_API_VERSION;
	options.fonts = &paths;
	options.stack_base = __builtin_frame_address(0);
	options.width = WIDTH;
	options.height = HEIGHT;
	options.fetch = BROWSER_FETCH_AT_ONCE;
	error = browser_view_create(&options, &view);
	if (error != 0)
		return 1;
	snprintf(path, sizeof(path), "%s/sticky-page.html", argv[1]);
	if (browser_view_load(view, path) != 0)
		return 1;
	(void)browser_view_settle(view, 1000.0, BROWSER_SETTLE_LAYOUT);

	/* Scroll 0: the header is pushed down to 30, the footer pulled up to the bottom. */
	check(draw(view, 0.0) == 0, "draw 0");
	check(colour_at(300, 10, 'r') == 0, "0: nothing red at the top");
	check(colour_at(300, 50, 'r'), "0: header at 30..70");
	check(colour_at(50, 270, 'g'), "0: side stays at its natural 240..300");
	check(colour_at(300, 560, 'b'), "0: footer at 540..590");

	/* Scroll 300: the header keeps 30 from the top; the side has stuck at 0..60. */
	check(draw(view, 300.0) == 0, "draw 300");
	check(colour_at(300, 50, 'r'), "300: header at 30..70 of the view");
check(colour_at(95, 10, 'g'), "300: side stuck at the top");
	check(colour_at(300, 560, 'b'), "300: footer at the bottom of the view");

	/* Scroll 900: the footer's natural place (1200..1250) is in view, so it does not move. */
	check(draw(view, 900.0) == 0, "draw 900");
	check(colour_at(300, 50, 'r'), "900: header still at 30");
	check(colour_at(300, 560, 'b') == 0 && colour_at(300, 320, 'b'), "900: footer at its natural place");

	/* Back to the top: everything returns. */
	check(draw(view, 0.0) == 0, "draw 0 again");
	check(colour_at(300, 10, 'r') == 0 && colour_at(50, 270, 'g'), "0 again: as at first");
	printf("host-sticky: %s (%d failures)\n", failures == 0 ? "ok" : "FAILED", failures);
	return failures == 0 ? 0 : 1;
}
