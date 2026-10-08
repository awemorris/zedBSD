/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The large clock's layout on the lock screen and the login screen
 * (ws187-p001, the 2026-10-08 user: "ロック画面は縦長のディスプレイでも
 * きれいに見えるよう、時計を中央より上に大きく表示してほしい").
 *
 * The time's size follows the output's shorter side, so a portrait output
 * and a landscape one of the same panel show the same clock, within a
 * least and a largest size.  The two lines are centred on a point a third
 * of the way down the output.  The card with the password field comes
 * first: the clock moves up to keep a gap above it, and when even the top
 * of the output is too near it becomes smaller.
 */

#include "lock-clock.h"

/* The time's size: this share of the output's shorter side (percent), within a least and a largest size (pixels). */
#define LOCK_CLOCK_SHARE	16
#define LOCK_CLOCK_LEAST	96
#define LOCK_CLOCK_MOST		192

/* The smallest the time becomes to clear the card on a short output (pixels). */
#define LOCK_CLOCK_FLOOR	48

/* Where the middle of the two lines is, as a share of the output's height (percent). */
#define LOCK_CLOCK_CENTRE	32

/* The height of the time's digits over its size (percent). */
#define LOCK_CLOCK_DIGIT	72

/* How far below the time's baseline the date's is: a share of the time's size (percent) and a line of the date (pixels). */
#define LOCK_CLOCK_DATE_SPACE	22
#define LOCK_CLOCK_DATE_LINE	28

/* How far the date's letters reach below its baseline (pixels). */
#define LOCK_CLOCK_DATE_DESCENT	8

/* The least gap between the clock and the card under it, and between the clock and the output's top (pixels). */
#define LOCK_CLOCK_GAP		32
#define LOCK_CLOCK_MARGIN	24

static int32_t lock_clock_digit(int32_t pixels);
static int32_t lock_clock_date_below(int32_t pixels);
static int32_t lock_clock_height(int32_t pixels);

/*
 * Lays the clock out on an output of a size, above a card whose top is at
 * card_top.
 */
void
kwl_lock_clock_layout(
	int32_t width,
	int32_t height,
	int32_t card_top,
	struct kwl_lock_clock *clock)
{
	int32_t shorter;
	int32_t pixels;
	int32_t block;
	int32_t top;
	int32_t limit;
	int32_t room;

	/* Takes the output's shorter side as the base of the time's size. */
	shorter = width;
	if (height < shorter)
		shorter = height;

	/* Sizes the time as a share of that side, within its least and largest. */
	pixels = shorter * LOCK_CLOCK_SHARE / 100;
	if (pixels < LOCK_CLOCK_LEAST)
		pixels = LOCK_CLOCK_LEAST;
	if (pixels > LOCK_CLOCK_MOST)
		pixels = LOCK_CLOCK_MOST;

	/* Centres the two lines a third of the way down. */
	block = lock_clock_height(pixels);
	top = height * LOCK_CLOCK_CENTRE / 100 - block / 2;

	/* Moves a clock that would come near the card up to keep the gap. */
	limit = card_top - LOCK_CLOCK_GAP;
	if (top + block > limit)
		top = limit - block;

	/*
	 * A clock that would then leave the output's top becomes as large as
	 * the room between the top and the gap allows (the date keeps its
	 * size), but no smaller than its floor.
	 */
	if (top < LOCK_CLOCK_MARGIN) {
		room = limit - LOCK_CLOCK_MARGIN;
		pixels = (room - LOCK_CLOCK_DATE_LINE - LOCK_CLOCK_DATE_DESCENT) * 100 / (LOCK_CLOCK_DIGIT + LOCK_CLOCK_DATE_SPACE);
		if (pixels < LOCK_CLOCK_FLOOR)
			pixels = LOCK_CLOCK_FLOOR;
		block = lock_clock_height(pixels);
		top = LOCK_CLOCK_MARGIN;
	}

	/* Succeeded: the time's size, the baselines, and the two lines' extent. */
	clock->pixels = pixels;
	clock->top = top;
	clock->time_baseline = top + lock_clock_digit(pixels);
	clock->date_baseline = clock->time_baseline + lock_clock_date_below(pixels);
	clock->bottom = top + block;
}

/* Gives the height of the time's digits at a size. */
static int32_t
lock_clock_digit(
	int32_t pixels)
{
	/* A share of the size. */
	return pixels * LOCK_CLOCK_DIGIT / 100;
}

/* Gives how far below the time's baseline the date's baseline is, at a time's size. */
static int32_t
lock_clock_date_below(
	int32_t pixels)
{
	/* A share of the time's size and one line of the date. */
	return pixels * LOCK_CLOCK_DATE_SPACE / 100 + LOCK_CLOCK_DATE_LINE;
}

/* Gives the height of the two lines from the digits' top to the date's descent, at a time's size. */
static int32_t
lock_clock_height(
	int32_t pixels)
{
	int32_t digit;
	int32_t below;

	/* The digits, the space down to the date's baseline, and the date's descent. */
	digit = lock_clock_digit(pixels);
	below = lock_clock_date_below(pixels);

	/* Succeeded: the height. */
	return digit + below + LOCK_CLOCK_DATE_DESCENT;
}
