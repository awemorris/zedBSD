/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the screen edges' gestures (WS181 p003, the 2026-10-07
 * UAT; userland/desktop/wayland/edge.c compiled unchanged).
 *
 * Checks: where a press is (the bottom strip for any pointer, the top band
 * for a touch only, not over the launcher or the top-right corner); what a
 * press held in the band is after a motion; what a drag on Home is; the
 * distance a docked title is pulled; (ws181-p008) how deep the desktop
 * layer and Home's content are on Home's way in and out; and (ws177-p033)
 * when a press resting in the band is a long press.
 *
 *   plan/ws181/tests/run-host-edge.sh
 */

#include "edge.h"

#include <math.h>
#include <stdio.h>

/* The output the cases are on. */
#define WIDTH		1280
#define HEIGHT		800

/* One case of where a press is: its point, whether it is a touch's, and where it must be. */
struct place_case {
	int32_t x;
	int32_t y;
	int touch;
	unsigned edge;
	const char *what;
};

/* One case of a motion (of a press held in the band, or of a drag on Home) and what it must be. */
struct motion_case {
	int32_t dx;
	int32_t dy;
	unsigned kind;
	const char *what;
};

/*
 * One case of Home's way in or out: how far Home is open, whether it is the
 * desktop layer's depth (else the content's), and where the layer must be.
 */
struct depth_case {
	float progress;
	int desktop;
	float x;
	float y;
	float scale;
	float opacity;
	const char *what;
};

/* Where presses are. */
static const struct place_case place_cases[] = {
	{ 640, 795, 0, KWL_EDGE_BOTTOM_STRIP, "the bottom strip, a mouse" },
	{ 640, 780, 1, KWL_EDGE_BOTTOM_STRIP, "the bottom strip's top line, a touch" },
	{ 640, 779, 1, KWL_EDGE_NONE, "just above the bottom strip" },
	{ 640, 5, 1, KWL_EDGE_TOP_BAND, "the top band, a touch" },
	{ 640, 5, 0, KWL_EDGE_NONE, "the top band is not a mouse's" },
	{ 640, 10, 1, KWL_EDGE_NONE, "just below the top band" },
	{ 39, 5, 1, KWL_EDGE_NONE, "over the launcher" },
	{ 40, 5, 1, KWL_EDGE_TOP_BAND, "just after the launcher" },
	{ 1251, 5, 1, KWL_EDGE_TOP_BAND, "just before the top-right corner" },
	{ 1252, 5, 1, KWL_EDGE_NONE, "in the top-right corner" },
};

/* What a press held in the band is after a motion. */
static const struct motion_case band_cases[] = {
	{ 0, 0, KWL_EDGE_BAND_WAIT, "not moved" },
	{ 0, 11, KWL_EDGE_BAND_WAIT, "down, not far enough" },
	{ 0, 12, KWL_EDGE_BAND_WISEVIEW, "down far enough" },
	{ 8, 12, KWL_EDGE_BAND_WISEVIEW, "down, a little across" },
	{ 8, 0, KWL_EDGE_BAND_WAIT, "across, within the slip" },
	{ 9, 0, KWL_EDGE_BAND_REPLAY, "across, past the slip" },
	{ -9, 4, KWL_EDGE_BAND_REPLAY, "across the other way" },
	{ 13, 13, KWL_EDGE_BAND_REPLAY, "as much across as down" },
	{ 0, -9, KWL_EDGE_BAND_REPLAY, "up, past the slip" },
};

/* What a drag on Home is. */
static const struct motion_case drag_cases[] = {
	{ 20, 0, KWL_EDGE_DRAG_PAGES, "sideways" },
	{ -20, 10, KWL_EDGE_DRAG_PAGES, "sideways, a little down" },
	{ 10, 10, KWL_EDGE_DRAG_PAGES, "as much sideways as down" },
	{ 5, 20, KWL_EDGE_DRAG_CLOSE, "down" },
	{ -5, -20, KWL_EDGE_DRAG_NONE, "up" },
};

/* The number of checks that failed, and of those that ran. */
/*
 * The desktop shrinks about the middle to 0.75 and fades out from 0.2 to
 * 0.9 of the way; the content grows about the middle from 0.85.
 */
static const struct depth_case depth_cases[] = {
	{ 0.0f, 1, 0.0f, 0.0f, 1.0f, 1.0f, "the desktop, Home closed" },
	{ 0.2f, 1, 32.0f, 20.0f, 0.95f, 1.0f, "the desktop at the fade's start" },
	{ 0.55f, 1, 88.0f, 55.0f, 0.8625f, 0.5f, "the desktop half faded" },
	{ 0.9f, 1, 144.0f, 90.0f, 0.775f, 0.0f, "the desktop at the fade's end" },
	{ 1.0f, 1, 160.0f, 100.0f, 0.75f, 0.0f, "the desktop, Home open" },
	{ -0.5f, 1, 0.0f, 0.0f, 1.0f, 1.0f, "the desktop, before the way" },
	{ 1.5f, 1, 160.0f, 100.0f, 0.75f, 0.0f, "the desktop, past the way" },
	{ 0.0f, 0, 96.0f, 60.0f, 0.85f, 1.0f, "the content, Home closed" },
	{ 0.5f, 0, 48.0f, 30.0f, 0.925f, 1.0f, "the content half way" },
	{ 1.0f, 0, 0.0f, 0.0f, 1.0f, 1.0f, "the content, Home open" },
};

static int failures;
static int checks;

static void check(int condition, const char *what, const char *rule);
static int near(float value, float expected);

/* Runs every table. */
int
main(void)
{
	struct kwl_edge_depth depth;
	const struct depth_case *wanted;
	unsigned index;
	unsigned answer;
	int32_t distance;
	int held;

	/* 1. Where a press is. */
	for (index = 0U; index < sizeof(place_cases) / sizeof(place_cases[0]); index++) {
		answer = kwl_edge_classify(place_cases[index].x, place_cases[index].y, WIDTH, HEIGHT, place_cases[index].touch);
		check(answer == place_cases[index].edge, place_cases[index].what, "place");
	}

	/* 2. A press held in the band. */
	for (index = 0U; index < sizeof(band_cases) / sizeof(band_cases[0]); index++) {
		answer = kwl_edge_band_motion(band_cases[index].dx, band_cases[index].dy);
		check(answer == band_cases[index].kind, band_cases[index].what, "band");
	}

	/* 2b. A press held in the band that rests (ws177-p033): a long press from KWL_EDGE_BAND_HOLD_MS. */
	held = kwl_edge_band_held(0U);
	check(held == 0, "just pressed", "hold");
	held = kwl_edge_band_held(499U);
	check(held == 0, "a moment short of the hold", "hold");
	held = kwl_edge_band_held(500U);
	check(held == 1, "held long enough", "hold");
	held = kwl_edge_band_held(5000U);
	check(held == 1, "held long", "hold");

	/* 3. A drag on Home. */
	for (index = 0U; index < sizeof(drag_cases) / sizeof(drag_cases[0]); index++) {
		answer = kwl_edge_drag_axis(drag_cases[index].dx, drag_cases[index].dy);
		check(answer == drag_cases[index].kind, drag_cases[index].what, "drag");
	}

	/* 4. The pull's distance: in any direction, whole pixels rounded down. */
	distance = kwl_edge_distance(0, 48);
	check(distance == 48, "straight down", "distance");
	distance = kwl_edge_distance(-48, 0);
	check(distance == 48, "straight across", "distance");
	distance = kwl_edge_distance(30, 40);
	check(distance == 50, "a 3-4-5 slant", "distance");
	distance = kwl_edge_distance(33, 33);
	check(distance == 46, "a diagonal, rounded down", "distance");
	distance = kwl_edge_distance(0, 0);
	check(distance == 0, "not moved", "distance");

	/* 5. Home's way in and out: the desktop's and the content's depths. */
	for (index = 0U; index < sizeof(depth_cases) / sizeof(depth_cases[0]); index++) {
		wanted = &depth_cases[index];
		if (wanted->desktop) {
			kwl_edge_home_desktop(wanted->progress, WIDTH, HEIGHT, &depth);
		} else {
			kwl_edge_home_content(wanted->progress, WIDTH, HEIGHT, &depth);
		}
		check(near(depth.x, wanted->x), wanted->what, "depth x");
		check(near(depth.y, wanted->y), wanted->what, "depth y");
		check(near(depth.scale, wanted->scale), wanted->what, "depth scale");
		check(near(depth.opacity, wanted->opacity), wanted->what, "depth opacity");
	}

	/* The result. */
	printf("WS181 host-edge checks=%d failures=%d\n", checks, failures);
	if (failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/* Counts one check, and reports it when it failed. */
static void
check(
	int condition,
	const char *what,
	const char *rule)
{
	/* One more check ran. */
	checks++;

	/* A failed check is named. */
	if (!condition) {
		failures++;
		printf("FAIL %s: %s\n", rule, what);
	}
}

/* Tells whether a value is the expected one, to a thousandth of a pixel or of the whole. */
static int
near(
	float value,
	float expected)
{
	/* Close enough. */
	if (fabsf(value - expected) < 0.001f)
		return 1;

	/* Succeeded: the value is not the expected one. */
	return 0;
}
