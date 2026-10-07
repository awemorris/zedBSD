/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the lock screen's clock layout (ws187-p001,
 * userland/desktop/wayland/lock-clock.c, compiled unchanged).
 *
 * The card's top is worked out as greeter.c's greeter_layout does it (one
 * user, or several with their rows).  On landscape, portrait and small
 * outputs the clock is above the middle, clear of the card by its gap and
 * of the output's top by its margin, sized by the shorter side within its
 * bounds, the same on a portrait output as on the landscape one of the same
 * panel, and narrow enough for the output.
 *
 *   plan/ws187/tests/run-host-lock-clock.sh
 */

#include "lock-clock.h"

#include <stdio.h>

/* The card's parts as greeter.c has them: the fixed height and a user's row (pixels). */
#define CARD_FIXED		276
#define CARD_ROW		44

/* The clock's bounds as lock-clock.c has them (pixels). */
#define CLOCK_LEAST		96
#define CLOCK_MOST		192
#define CLOCK_GAP		32
#define CLOCK_MARGIN		24

/* The number of checks that failed, and of those that ran. */
static int failures;
static int checks;

static void check(int condition, const char *what, int32_t width, int32_t height);
static int32_t card_top(int32_t height, unsigned users);
static void check_output(int32_t width, int32_t height, unsigned users, struct kwl_lock_clock *clock);

/* Counts one check, and reports it with the output's size when it failed. */
static void
check(
	int condition,
	const char *what,
	int32_t width,
	int32_t height)
{
	/* One more check ran. */
	checks++;

	/* A failed check is printed and counted. */
	if (!condition) {
		printf("FAIL: %dx%d: %s\n", width, height, what);
		failures++;
	}
}

/* Gives the card's top for an output's height and a number of users, as greeter_layout does. */
static int32_t
card_top(
	int32_t height,
	unsigned users)
{
	int32_t card_height;

	/* One user has no rows; several have one each. */
	card_height = CARD_FIXED;
	if (users > 1U)
		card_height += (int32_t)users * CARD_ROW;

	/* Succeeded: a little below the middle, as the greeter places it. */
	return height / 2 - card_height / 2 + height / 16;
}

/* Lays the clock out on an output and checks what holds on every output large enough. */
static void
check_output(
	int32_t width,
	int32_t height,
	unsigned users,
	struct kwl_lock_clock *clock)
{
	int32_t card;

	/* The layout above the card. */
	card = card_top(height, users);
	kwl_lock_clock_layout(width, height, card, clock);
	printf("%dx%d users=%u: pixels=%d top=%d time=%d date=%d bottom=%d card=%d\n",
	       width,
	       height,
	       users,
	       clock->pixels,
	       clock->top,
	       clock->time_baseline,
	       clock->date_baseline,
	       clock->bottom,
	       card);

	/* Clear of the card and of the output's top, above the middle, in order. */
	check(clock->bottom + CLOCK_GAP <= card, "clear of the card by the gap", width, height);
	check(clock->top >= CLOCK_MARGIN, "clear of the output's top", width, height);
	check(clock->top + clock->bottom < height, "above the middle", width, height);
	check(clock->top < clock->time_baseline, "the time's digits under the top", width, height);
	check(clock->time_baseline < clock->date_baseline, "the date under the time", width, height);
	check(clock->date_baseline < clock->bottom, "the date's descent under its baseline", width, height);

	/* Five characters of about six tenths of the size fit across. */
	check(clock->pixels * 3 < width, "the time fits across", width, height);
}

/* Runs every case. */
int
main(void)
{
	struct kwl_lock_clock landscape;
	struct kwl_lock_clock portrait;
	struct kwl_lock_clock small;
	struct kwl_lock_clock large;
	struct kwl_lock_clock crowded;
	struct kwl_lock_clock tiny;

	/* 1. The 5330's landscape output and the same panel turned to portrait: the same clock size. */
	check_output(1920, 1080, 1U, &landscape);
	check_output(1080, 1920, 1U, &portrait);
	check(landscape.pixels == portrait.pixels, "portrait and landscape of one panel: the same size", 1080, 1920);
	check(landscape.pixels >= CLOCK_LEAST && landscape.pixels <= CLOCK_MOST, "within the bounds", 1920, 1080);
	check(landscape.pixels >= 150, "large on a 1080 panel", 1920, 1080);

	/* 2. On the portrait output the clock is about a third of the way down (it is far from the card). */
	check(portrait.top + portrait.bottom >= 1920 * 2 * 28 / 100, "portrait: not crowded to the top", 1080, 1920);
	check(portrait.top + portrait.bottom <= 1920 * 2 * 36 / 100, "portrait: a third of the way down", 1080, 1920);

	/* 3. A small landscape output and a large one: smaller and larger, both within the bounds. */
	check_output(1280, 800, 1U, &small);
	check_output(2560, 1600, 1U, &large);
	check(small.pixels < landscape.pixels, "a smaller panel has a smaller clock", 1280, 800);
	check(small.pixels >= CLOCK_LEAST, "but no smaller than the least", 1280, 800);
	check(large.pixels == CLOCK_MOST, "a large panel stops at the largest", 2560, 1600);

	/* 4. Other outputs met in practice. */
	check_output(1366, 768, 1U, &small);
	check_output(800, 1280, 1U, &small);
	check_output(1024, 768, 1U, &small);
	check_output(1920, 1080, 3U, &small);

	/* 5. A short output whose card has many users: the clock becomes smaller to stay clear. */
	check_output(1280, 800, 8U, &crowded);
	check(crowded.pixels < CLOCK_LEAST, "crowded: smaller than the least to stay clear", 1280, 800);

	/* 6. An output too short for any clock: the floor's size at the top, whatever the card. */
	kwl_lock_clock_layout(640, 400, card_top(400, 8U), &tiny);
	check(tiny.pixels == 48, "too short: the floor's size", 640, 400);
	check(tiny.top == CLOCK_MARGIN, "too short: at the margin", 640, 400);

	/* The verdict. */
	if (failures != 0) {
		printf("host-lock-clock: FAIL (%d of %d checks)\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("host-lock-clock: ok (%d checks)\n", checks);
	return 0;
}
