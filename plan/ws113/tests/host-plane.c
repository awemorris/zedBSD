/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the plane of the displays (ws113-p007,
 * userland/desktop/wayland/plane.c): which output holds a point, the
 * pointer's relative moves across shared edges and its stop at the others,
 * the display beside one, a window carried to another output, and where a
 * bar's widget is drawn on each output (ws113-p015).
 */

#include "plane.h"

#include <stdio.h>

static unsigned failures;

static void check(int condition, const char *what);
static void outputs_right(struct kwl_plane_rect *outputs, int32_t head_y);
static void test_at(void);
static void test_move(void);
static void test_neighbour(void);
static void test_carry(void);
static void test_places(void);

int
main(void)
{
	/* Each part. */
	test_at();
	test_move();
	test_neighbour();
	test_carry();
	test_places();
	if (failures != 0U) {
		fprintf(stderr, "%u failures\n", failures);
		return 1;
	}

	/* Succeeded. */
	printf("plane host test PASS\n");
	return 0;
}

/* Counts a failed condition. */
static void
check(
	int condition,
	const char *what)
{
	/* Told and counted. */
	if (!condition) {
		fprintf(stderr, "FAIL %s\n", what);
		failures++;
	}
}

/* The anchor 1280x800 at the origin and head 0 (slot 1) 1024x768 right of it at head_y; the other slots not shown. */
static void
outputs_right(
	struct kwl_plane_rect *outputs,
	int32_t head_y)
{
	unsigned slot;

	/* None shown. */
	for (slot = 0U; slot < KWL_PLANE_SLOTS; slot++) {
		outputs[slot].x = 0;
		outputs[slot].y = 0;
		outputs[slot].width = 0U;
		outputs[slot].height = 0U;
	}

	/* The two. */
	outputs[0].width = 1280U;
	outputs[0].height = 800U;
	outputs[1].x = 1280;
	outputs[1].y = head_y;
	outputs[1].width = 1024U;
	outputs[1].height = 768U;
}

/* The output that holds a point, half-open edges, none past them. */
static void
test_at(void)
{
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];

	/* Each side of the shared edge, and below the head. */
	outputs_right(outputs, 0);
	check(kwl_plane_at(outputs, KWL_PLANE_SLOTS, 1279, 0) == 0, "at anchor edge");
	check(kwl_plane_at(outputs, KWL_PLANE_SLOTS, 1280, 0) == 1, "at head edge");
	check(kwl_plane_at(outputs, KWL_PLANE_SLOTS, 1280, 768) == -1, "at below head");
	check(kwl_plane_at(outputs, KWL_PLANE_SLOTS, -1, 10) == -1, "at left of anchor");
	check(kwl_plane_at(outputs, KWL_PLANE_SLOTS, 0, 0) == 0, "at origin");
}

/* The pointer's moves: across, back, stopped at an edge no output shares, a long jump, a corner. */
static void
test_move(void)
{
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	unsigned slot;
	int32_t x;
	int32_t y;

	/* Across the shared edge, and back. */
	outputs_right(outputs, 0);
	slot = kwl_plane_move(outputs, KWL_PLANE_SLOTS, 0U, 1270, 100, 20, 0, &x, &y);
	check(slot == 1U && x == 1290 && y == 100, "move across");
	slot = kwl_plane_move(outputs, KWL_PLANE_SLOTS, 1U, 1285, 100, -10, 0, &x, &y);
	check(slot == 0U && x == 1275 && y == 100, "move back");

	/* Past the anchor's left edge, which no output shares: stopped there. */
	slot = kwl_plane_move(outputs, KWL_PLANE_SLOTS, 0U, 10, 10, -50, 0, &x, &y);
	check(slot == 0U && x == 0 && y == 10, "move stops at the left");

	/* Below the head's bottom, beside the anchor's lower part: kept on the anchor at its right edge. */
	slot = kwl_plane_move(outputs, KWL_PLANE_SLOTS, 0U, 1270, 790, 20, 0, &x, &y);
	check(slot == 0U && x == 1279 && y == 790, "move under the head stops");

	/* A long jump past the head: on the head's far edge. */
	slot = kwl_plane_move(outputs, KWL_PLANE_SLOTS, 0U, 1270, 100, 5000, 0, &x, &y);
	check(slot == 1U && x == 2303 && y == 100, "move far across");

	/* A head lower than the anchor: beside the anchor's top the edge is not shared. */
	outputs_right(outputs, 200);
	slot = kwl_plane_move(outputs, KWL_PLANE_SLOTS, 0U, 1270, 100, 20, 0, &x, &y);
	check(slot == 0U && x == 1279 && y == 100, "move beside a lower head stops");
	slot = kwl_plane_move(outputs, KWL_PLANE_SLOTS, 0U, 1270, 300, 20, 0, &x, &y);
	check(slot == 1U && x == 1290 && y == 300, "move into a lower head");

	/* A current output not shown counts as the anchor. */
	slot = kwl_plane_move(outputs, KWL_PLANE_SLOTS, 5U, 10, 10, -50, 0, &x, &y);
	check(slot == 0U && x == 0, "move from a slot not shown");
}

/* The display beside one, either way. */
static void
test_neighbour(void)
{
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];

	/* Right of the anchor. */
	outputs_right(outputs, 0);
	check(kwl_plane_neighbour(outputs, KWL_PLANE_SLOTS, 0U, 1) == 1, "neighbour right");
	check(kwl_plane_neighbour(outputs, KWL_PLANE_SLOTS, 0U, -1) == -1, "no neighbour left");
	check(kwl_plane_neighbour(outputs, KWL_PLANE_SLOTS, 1U, -1) == 0, "neighbour back");

	/* Left of the anchor. */
	outputs[1].x = -1024;
	check(kwl_plane_neighbour(outputs, KWL_PLANE_SLOTS, 0U, -1) == 1, "neighbour left");
	check(kwl_plane_neighbour(outputs, KWL_PLANE_SLOTS, 0U, 1) == -1, "no neighbour right");

	/* Three in a row: the nearer one. */
	outputs[2].x = 1280;
	outputs[2].y = 0;
	outputs[2].width = 800U;
	outputs[2].height = 600U;
	outputs[3].x = 2080;
	outputs[3].y = 0;
	outputs[3].width = 800U;
	outputs[3].height = 600U;
	check(kwl_plane_neighbour(outputs, KWL_PLANE_SLOTS, 0U, 1) == 2, "neighbour nearer");
}

/* A window carried: at the same share of the way, kept inside, larger ones from the corner. */
static void
test_carry(void)
{
	struct kwl_plane_rect outputs[KWL_PLANE_SLOTS];
	int32_t x;
	int32_t y;

	/* The same share across and down. */
	outputs_right(outputs, 0);
	kwl_plane_carry(&outputs[0], &outputs[1], 340, 200, 600U, 400U, 52, &x, &y);
	check(x == 1280 + 272 && y == 192, "carry share");

	/* Kept inside at the far side. */
	kwl_plane_carry(&outputs[0], &outputs[1], 1000, 700, 600U, 400U, 52, &x, &y);
	check(x == 1280 + 1024 - 600 && y == 768 - 400, "carry kept inside");

	/* Below the top. */
	kwl_plane_carry(&outputs[0], &outputs[1], 0, 0, 600U, 400U, 52, &x, &y);
	check(x == 1280 && y == 52, "carry below the top");

	/* Larger than the output: from its left and its top. */
	kwl_plane_carry(&outputs[0], &outputs[1], 100, 100, 1100U, 900U, 52, &x, &y);
	check(x == 1280 && y == 52, "carry larger");

	/* Back to the anchor under its bar. */
	kwl_plane_carry(&outputs[1], &outputs[0], 1280 + 512, 0, 600U, 400U, 96, &x, &y);
	check(x == 640 && y == 96, "carry back");
}

/* A widget's places on the bars: none at first, each output's own, the last one drawn kept, no slot past the last. */
static void
test_places(void)
{
	static struct kwl_plane_places places;
	int32_t x;
	int32_t top;
	int placed;

	/* Nothing placed at first (a static owner's zero). */
	placed = kwl_plane_placed(&places, 0U, &x, &top);
	check(placed == 0, "places none at first");

	/* The anchor's and a head's, each its own. */
	kwl_plane_place(&places, 0U, 900, 0);
	kwl_plane_place(&places, 1U, 1280 + 700, 120);
	placed = kwl_plane_placed(&places, 0U, &x, &top);
	check(placed == 1 && x == 900 && top == 0, "places anchor");
	placed = kwl_plane_placed(&places, 1U, &x, &top);
	check(placed == 1 && x == 1280 + 700 && top == 120, "places head");

	/* Another output never drawn on has none. */
	placed = kwl_plane_placed(&places, 2U, &x, &top);
	check(placed == 0, "places other head none");

	/* Drawn again elsewhere: the last place. */
	kwl_plane_place(&places, 1U, 1280 + 650, 120);
	placed = kwl_plane_placed(&places, 1U, &x, &top);
	check(placed == 1 && x == 1280 + 650, "places moved");

	/* No slot past the last: nothing kept, nothing given. */
	kwl_plane_place(&places, KWL_PLANE_SLOTS, 1, 1);
	placed = kwl_plane_placed(&places, KWL_PLANE_SLOTS, &x, &top);
	check(placed == 0, "places out of range");
}
