/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the arrangement of windows (WS181 p004, the 2026-10-07
 * UAT; userland/desktop/wayland/arrange.c compiled unchanged).
 *
 * Checks, for each of the five layouts and every number of windows up to
 * its limit: the slots are inside the area's margin, do not overlap, keep
 * the gap between neighbours, and fill the area; the shapes (side by side,
 * stacked, one on the right or left, the grid's last row widened).  And the
 * assignment: windows side by side keep their order across, the four
 * corners' windows go to the grid's corners, and the result is the least
 * of every assignment tried by brute force.  And (ws177-p035) where a
 * window of limited size goes in its slot's body, and when it is too large.
 *
 *   plan/ws181/tests/run-host-arrange.sh
 */

#include "arrange.h"

#include <stdio.h>
#include <string.h>

/* The work area the cases use (the docked space of a 1280x800 output). */
#define AREA_X		4
#define AREA_Y		48
#define AREA_W		1272
#define AREA_H		748

/* The number of checks that failed, and of those that ran. */
static int failures;
static int checks;

static void check(int condition, const char *what, const char *rule);
static int overlap(const struct kwl_arrange_rect *a, const struct kwl_arrange_rect *b);
static long long area_of(const struct kwl_arrange_rect *rect);
static long long cost_of(const int32_t centres[][2], unsigned count, const struct kwl_arrange_rect *slots, const unsigned *order);
static long long brute(const int32_t centres[][2], unsigned count, const struct kwl_arrange_rect *slots, unsigned *order, unsigned depth, unsigned used);

/* Runs every check. */
int
main(void)
{
	struct kwl_arrange_rect area;
	struct kwl_arrange_rect slots[KWL_ARRANGE_MAX];
	int32_t centres[KWL_ARRANGE_MAX][2];
	unsigned order[KWL_ARRANGE_MAX];
	unsigned trial[KWL_ARRANGE_MAX];
	unsigned layout;
	unsigned count;
	unsigned made;
	unsigned index;
	unsigned other;
	long long covered;
	long long gaps;
	long long least;
	long long found;
	char what[96];
	struct kwl_arrange_rect body;
	struct kwl_arrange_rect placed;
	int32_t limits[4];
	int fits;

	/* The work area. */
	area.x = AREA_X;
	area.y = AREA_Y;
	area.width = AREA_W;
	area.height = AREA_H;

	/* 1. Every layout, every number of windows: inside, apart, filling. */
	for (layout = 0U; layout < KWL_ARRANGE_LAYOUTS; layout++) {
		for (count = 1U; count <= kwl_arrange_limit(layout) + 1U; count++) {
			made = kwl_arrange_slots(layout, count, &area, slots);
			snprintf(what, sizeof(what), "%s n=%u", kwl_arrange_name(layout), count);
			if (count <= kwl_arrange_limit(layout)) {
				check(made == count, what, "slots made");
			} else {
				check(made == kwl_arrange_limit(layout), what, "slots up to the limit");
			}

			/* Inside the margin, and no two overlapping. */
			covered = 0;
			for (index = 0U; index < made; index++) {
				check(slots[index].x >= AREA_X + KWL_ARRANGE_MARGIN, what, "left margin");
				check(slots[index].y >= AREA_Y + KWL_ARRANGE_MARGIN, what, "top margin");
				check(slots[index].x + slots[index].width <= AREA_X + AREA_W - KWL_ARRANGE_MARGIN, what, "right margin");
				check(slots[index].y + slots[index].height <= AREA_Y + AREA_H - KWL_ARRANGE_MARGIN, what, "bottom margin");
				check(slots[index].width > 0 && slots[index].height > 0, what, "not empty");
				covered += area_of(&slots[index]);
				for (other = index + 1U; other < made; other++)
					check(!overlap(&slots[index], &slots[other]), what, "no overlap");
			}

			/* What the slots do not cover is only the gaps: less than a gap's strip per slot each way. */
			gaps = (long long)(AREA_W - 2 * KWL_ARRANGE_MARGIN) * (AREA_H - 2 * KWL_ARRANGE_MARGIN) - covered;
			check(gaps >= 0, what, "covers no more than the area");
			check(gaps <= (long long)made * KWL_ARRANGE_GAP * (AREA_W + AREA_H), what, "fills the area but the gaps");
		}
	}

	/* 2. The shapes. */
	made = kwl_arrange_slots(KWL_ARRANGE_COLUMNS, 3U, &area, slots);
	check(slots[0].x < slots[1].x && slots[1].x < slots[2].x && slots[0].y == slots[2].y, "columns n=3", "side by side");
	check(slots[1].x == slots[0].x + slots[0].width + KWL_ARRANGE_GAP, "columns n=3", "gap");
	made = kwl_arrange_slots(KWL_ARRANGE_ROWS, 2U, &area, slots);
	check(slots[0].y < slots[1].y && slots[0].x == slots[1].x && slots[0].width == slots[1].width, "rows n=2", "stacked");
	made = kwl_arrange_slots(KWL_ARRANGE_RIGHT_MAIN, 3U, &area, slots);
	check(slots[0].x > slots[1].x && slots[1].x == slots[2].x && slots[1].y < slots[2].y, "right-main n=3", "one on the right");
	check(slots[0].height == AREA_H - 2 * KWL_ARRANGE_MARGIN, "right-main n=3", "the right one is whole height");
	made = kwl_arrange_slots(KWL_ARRANGE_LEFT_MAIN, 3U, &area, slots);
	check(slots[0].x < slots[1].x && slots[1].x == slots[2].x, "left-main n=3", "one on the left");
	made = kwl_arrange_slots(KWL_ARRANGE_RIGHT_MAIN, 2U, &area, slots);
	check(slots[0].y == slots[1].y && slots[0].height == slots[1].height && slots[0].x > slots[1].x, "right-main n=2", "two halves");
	made = kwl_arrange_slots(KWL_ARRANGE_TOP_MAIN, 3U, &area, slots);
	check(slots[0].y < slots[1].y && slots[1].y == slots[2].y && slots[1].x < slots[2].x, "top-main n=3", "one on the top");
	check(slots[0].width == AREA_W - 2 * KWL_ARRANGE_MARGIN, "top-main n=3", "the top one is whole width");
	check(slots[1].y == slots[0].y + slots[0].height + KWL_ARRANGE_GAP, "top-main n=3", "gap under the top one");
	made = kwl_arrange_slots(KWL_ARRANGE_BOTTOM_MAIN, 3U, &area, slots);
	check(slots[0].y > slots[1].y && slots[1].y == slots[2].y && slots[0].width == AREA_W - 2 * KWL_ARRANGE_MARGIN, "bottom-main n=3", "one on the bottom");
	made = kwl_arrange_slots(KWL_ARRANGE_BOTTOM_MAIN, 1U, &area, slots);
	check(made == 1U && slots[0].height == AREA_H - 2 * KWL_ARRANGE_MARGIN, "bottom-main n=1", "one window takes the whole area");
	check(strcmp(kwl_arrange_name(KWL_ARRANGE_TOP_MAIN), "top-main") == 0 && strcmp(kwl_arrange_name(KWL_ARRANGE_BOTTOM_MAIN), "bottom-main") == 0, "the new names", "name");
	check(kwl_arrange_limit(KWL_ARRANGE_TOP_MAIN) == 4U, "top-main limit", "four");
	made = kwl_arrange_slots(KWL_ARRANGE_GRID, 3U, &area, slots);
	check(slots[2].width > slots[0].width && slots[2].y > slots[0].y, "grid n=3", "the last row widened");
	made = kwl_arrange_slots(KWL_ARRANGE_GRID, 4U, &area, slots);
	check(slots[0].width == slots[3].width || slots[0].width + 1 >= slots[3].width - 1, "grid n=4", "two by two");
	check(slots[1].y == slots[0].y && slots[2].y > slots[0].y && slots[3].x == slots[1].x, "grid n=4", "two rows of two");
	made = kwl_arrange_slots(KWL_ARRANGE_GRID, 1U, &area, slots);
	check(slots[0].width == AREA_W - 2 * KWL_ARRANGE_MARGIN, "grid n=1", "one window takes the whole area");

	/* 3. The assignment: three windows across, given in another order, keep their order across. */
	made = kwl_arrange_slots(KWL_ARRANGE_COLUMNS, 3U, &area, slots);
	centres[0][0] = 900;
	centres[0][1] = 400;
	centres[1][0] = 100;
	centres[1][1] = 300;
	centres[2][0] = 500;
	centres[2][1] = 500;
	kwl_arrange_assign(centres, 3U, slots, order);
	check(order[0] == 2U && order[1] == 0U && order[2] == 1U, "three windows across", "order kept");

	/* The four corners' windows go to the grid's corners. */
	made = kwl_arrange_slots(KWL_ARRANGE_GRID, 4U, &area, slots);
	centres[0][0] = 1200;
	centres[0][1] = 700;
	centres[1][0] = 100;
	centres[1][1] = 100;
	centres[2][0] = 1200;
	centres[2][1] = 100;
	centres[3][0] = 100;
	centres[3][1] = 700;
	kwl_arrange_assign(centres, 4U, slots, order);
	check(order[0] == 3U && order[1] == 0U && order[2] == 1U && order[3] == 2U, "four corners", "corners kept");

	/* The least of every assignment, for every grid size, against brute force. */
	for (count = 1U; count <= KWL_ARRANGE_MAX; count++) {
		made = kwl_arrange_slots(KWL_ARRANGE_GRID, count, &area, slots);
		for (index = 0U; index < count; index++) {
			centres[index][0] = (int32_t)((index * 397U + 113U) % (unsigned)AREA_W);
			centres[index][1] = (int32_t)((index * 211U + 59U) % (unsigned)AREA_H) + AREA_Y;
		}
		kwl_arrange_assign(centres, count, slots, order);
		found = cost_of(centres, count, slots, order);
		least = brute(centres, count, slots, trial, 0U, 0U);
		snprintf(what, sizeof(what), "grid n=%u", count);
		check(found == least, what, "least sum");

		/* Each slot used once. */
		for (index = 0U; index < count; index++)
			for (other = index + 1U; other < count; other++)
				check(order[index] != order[other], what, "each slot once");
	}

	/* The names. */
	check(strcmp(kwl_arrange_name(KWL_ARRANGE_RIGHT_MAIN), "right-main") == 0, "the right-main name", "name");
	check(strcmp(kwl_arrange_name(KWL_ARRANGE_GRID), "grid") == 0, "the grid name", "name");

	/* 4. A window of limited size in its slot's body (ws177-p035): the body 600x400 at (100, 50). */
	body.x = 100;
	body.y = 50;
	body.width = 600;
	body.height = 400;

	/* No limits: the whole body. */
	memset(limits, 0, sizeof(limits));
	fits = kwl_arrange_fit(&body, limits, &placed);
	check(fits == 1 && placed.x == 100 && placed.y == 50 && placed.width == 600 && placed.height == 400, "no limits", "fit");

	/* A smallest size within the body: still the whole body. */
	limits[0] = 300;
	limits[1] = 200;
	fits = kwl_arrange_fit(&body, limits, &placed);
	check(fits == 1 && placed.width == 600 && placed.height == 400, "a small smallest size", "fit");

	/* A fixed size smaller than the body: its size, in the middle (letterboxed). */
	limits[0] = 320;
	limits[1] = 240;
	limits[2] = 320;
	limits[3] = 240;
	fits = kwl_arrange_fit(&body, limits, &placed);
	check(fits == 1 && placed.x == 240 && placed.y == 130 && placed.width == 320 && placed.height == 240, "a fixed size", "fit");

	/* A largest width only: that width in the middle across, the whole height. */
	memset(limits, 0, sizeof(limits));
	limits[2] = 500;
	fits = kwl_arrange_fit(&body, limits, &placed);
	check(fits == 1 && placed.x == 150 && placed.y == 50 && placed.width == 500 && placed.height == 400, "a largest width", "fit");

	/* A smallest width larger than the body: too large, at its smallest size about the middle. */
	memset(limits, 0, sizeof(limits));
	limits[0] = 700;
	fits = kwl_arrange_fit(&body, limits, &placed);
	check(fits == 0 && placed.width == 700 && placed.x == 50 && placed.height == 400, "a smallest width too large", "fit");

	/* A smallest height larger than the body: too large too. */
	memset(limits, 0, sizeof(limits));
	limits[1] = 401;
	fits = kwl_arrange_fit(&body, limits, &placed);
	check(fits == 0 && placed.height == 401, "a smallest height too large", "fit");

	/* A fixed size larger than the body: too large. */
	limits[0] = 800;
	limits[1] = 500;
	limits[2] = 800;
	limits[3] = 500;
	fits = kwl_arrange_fit(&body, limits, &placed);
	check(fits == 0 && placed.width == 800 && placed.height == 500, "a fixed size too large", "fit");

	/* The result. */
	printf("WS181 host-arrange checks=%d failures=%d\n", checks, failures);
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

/* Tells whether two rectangles share any pixel. */
static int
overlap(
	const struct kwl_arrange_rect *a,
	const struct kwl_arrange_rect *b)
{
	/* Apart across or down. */
	if (a->x + a->width <= b->x || b->x + b->width <= a->x)
		return 0;
	if (a->y + a->height <= b->y || b->y + b->height <= a->y)
		return 0;

	/* Succeeded: they overlap. */
	return 1;
}

/* Gives a rectangle's area. */
static long long
area_of(
	const struct kwl_arrange_rect *rect)
{
	/* Width by height. */
	return (long long)rect->width * rect->height;
}

/* Gives an assignment's sum of squared distances from the centres to the slots' centres. */
static long long
cost_of(
	const int32_t centres[][2],
	unsigned count,
	const struct kwl_arrange_rect *slots,
	const unsigned *order)
{
	long long sum;
	long long dx;
	long long dy;
	unsigned index;

	/* Each window to its slot. */
	sum = 0;
	for (index = 0U; index < count; index++) {
		dx = (long long)centres[index][0] - (slots[order[index]].x + slots[order[index]].width / 2);
		dy = (long long)centres[index][1] - (slots[order[index]].y + slots[order[index]].height / 2);
		sum += dx * dx + dy * dy;
	}

	/* Succeeded: the sum. */
	return sum;
}

/* Gives the least sum of every assignment of the windows from depth on to the slots not used. */
static long long
brute(
	const int32_t centres[][2],
	unsigned count,
	const struct kwl_arrange_rect *slots,
	unsigned *order,
	unsigned depth,
	unsigned used)
{
	long long least;
	long long sum;
	unsigned slot;

	/* Every window placed: this assignment's sum. */
	if (depth == count) {
		sum = cost_of(centres, count, slots, order);
		return sum;
	}

	/* Each free slot for this window. */
	least = -1;
	for (slot = 0U; slot < count; slot++) {
		if ((used & (1U << slot)) != 0U)
			continue;
		order[depth] = slot;
		sum = brute(centres, count, slots, order, depth + 1U, used | (1U << slot));
		if (least < 0 || sum < least)
			least = sum;
	}

	/* Succeeded: the least. */
	return least;
}
