/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the lock screen's unlocking moves and grace
 * (ws187-p002, userland/desktop/wayland/lock-swipe.c, compiled unchanged).
 *
 * A press in the lower third that goes up far enough, more up than across,
 * swipes; one higher up, too short or sideways does not.  The wheel opens on
 * two notches up in a row, not on one, a pause or a turn down.  Two fingers
 * on a touch pad swipe on 10 mm up their own way, once a touch; a gesture up
 * of 10 mm swipes.  A lock the user chose has no grace; one the session made
 * has five minutes, none when the clock went back.
 *
 *   plan/ws187/tests/run-host-lock-swipe.sh
 */

#include "lock-swipe.h"

#include <stdio.h>

/* The output of the cases, and the grace the lock screen gives (seconds). */
#define WIDTH			1920
#define HEIGHT			1080
#define GRACE			300

/* The number of checks that failed, and of those that ran. */
static int failures;
static int checks;

static void check(int condition, const char *what);
static int swipe_from(struct kwl_lock_swipe *swipe, int32_t x, int32_t y, int32_t to_x, int32_t to_y);

/* Counts one check, and reports it when it failed. */
static void
check(
	int condition,
	const char *what)
{
	/* One more check ran. */
	checks++;

	/* A failed check is printed and counted. */
	if (!condition) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

/* Presses at a place, moves to another in four steps and lets go: reports whether it swiped. */
static int
swipe_from(
	struct kwl_lock_swipe *swipe,
	int32_t x,
	int32_t y,
	int32_t to_x,
	int32_t to_y)
{
	int step;
	int swiped;

	/* The press, the steps and the release. */
	kwl_lock_swipe_reset(swipe);
	(void)kwl_lock_swipe_press(swipe, x, y, HEIGHT);
	for (step = 1; step <= 4; step++)
		kwl_lock_swipe_motion(swipe, x + (to_x - x) * step / 4, y + (to_y - y) * step / 4);
	swiped = kwl_lock_swipe_release(swipe, HEIGHT);

	/* Succeeded: whether it swiped. */
	return swiped;
}

/* Runs every case. */
int
main(void)
{
	struct kwl_lock_swipe swipe;
	int32_t distance;
	int followed;

	/* 1. The distance: 12% of the height, at least 80. */
	distance = kwl_lock_swipe_distance(HEIGHT);
	check(distance == 129, "1080 high: 129 up");
	check(kwl_lock_swipe_distance(400) == 80, "a short output: at least 80");

	/* 2. A press in the lower third is followed; one in the middle is not. */
	kwl_lock_swipe_reset(&swipe);
	followed = kwl_lock_swipe_press(&swipe, 960, 900, HEIGHT);
	check(followed, "a press near the foot is followed");
	followed = kwl_lock_swipe_press(&swipe, 960, 724, HEIGHT);
	check(followed, "a press in the lower third, not at the edge, is followed");
	followed = kwl_lock_swipe_press(&swipe, 960, 540, HEIGHT);
	check(!followed, "a press in the middle is not");

	/* 3. Up the distance and more: a swipe; short of it, sideways, down, or from the middle: none. */
	check(swipe_from(&swipe, 960, 900, 960, 900 - distance), "straight up the distance swipes");
	check(swipe_from(&swipe, 960, 800, 1000, 500), "a long swipe from the lower third, a little aslant, swipes");
	check(!swipe_from(&swipe, 960, 900, 960, 900 - distance + 1), "short of the distance does not");
	check(!swipe_from(&swipe, 960, 900, 1260, 700), "more across than up does not");
	check(!swipe_from(&swipe, 960, 800, 960, 1000), "down does not");
	check(!swipe_from(&swipe, 960, 600, 960, 200), "from above the lower third does not");

	/* 4. A release without a press followed says nothing. */
	kwl_lock_swipe_reset(&swipe);
	check(!kwl_lock_swipe_release(&swipe, HEIGHT), "a release alone does not swipe");

	/* 5. The wheel: two notches up in a row open; one does not, nor a pause, nor a turn down between. */
	kwl_lock_swipe_reset(&swipe);
	check(!kwl_lock_swipe_wheel(&swipe, -1, 1000), "one notch up: not yet");
	check(kwl_lock_swipe_wheel(&swipe, -1, 1200), "a second notch soon after: open");
	check(!kwl_lock_swipe_wheel(&swipe, -1, 1300), "the count starts afresh after it");
	check(!kwl_lock_swipe_wheel(&swipe, -1, 2400), "a notch after a long pause starts again");
	check(!kwl_lock_swipe_wheel(&swipe, 1, 2500), "a notch down");
	check(!kwl_lock_swipe_wheel(&swipe, -1, 2600), "then one up: not yet");
	check(kwl_lock_swipe_wheel(&swipe, -2, 2700), "two notches up at once after it: open");

	/* 6. Two fingers on a touch pad: 10 mm up their own way, once a touch. */
	kwl_lock_swipe_reset(&swipe);
	check(!kwl_lock_swipe_pad(&swipe, -7500), "7.5 mm up: not yet");
	check(!kwl_lock_swipe_pad(&swipe, 2500), "back down 2.5 mm");
	check(!kwl_lock_swipe_pad(&swipe, -2500), "up again to 7.5 mm");
	check(kwl_lock_swipe_pad(&swipe, -2500), "10 mm up: open");
	check(!kwl_lock_swipe_pad(&swipe, -10000), "the same touch does not again");
	kwl_lock_swipe_pad_end(&swipe);
	check(!kwl_lock_swipe_pad(&swipe, 20000), "a new touch moving down does not");
	check(kwl_lock_swipe_pad(&swipe, -12500), "then 10 mm up past where it started: open");
	kwl_lock_swipe_pad_end(&swipe);

	/* 7. A gesture up: 10 mm open, less does not. */
	check(!kwl_lock_swipe_pad_gesture(&swipe, 9000), "a gesture of 9 mm does not");
	kwl_lock_swipe_pad_end(&swipe);
	check(kwl_lock_swipe_pad_gesture(&swipe, 15000), "a gesture of 15 mm opens");

	/* 8. The reasons: the user's locks and unknown ones are manual, the session's own are not. */
	check(kwl_lock_reason_manual("key"), "Super+L is manual");
	check(kwl_lock_reason_manual("home"), "App Home's Lock Screen is manual");
	check(kwl_lock_reason_manual("unknown"), "an unknown reason is manual");
	check(kwl_lock_reason_manual(NULL), "no reason is manual");
	check(!kwl_lock_reason_manual("lid"), "the lid is not");
	check(!kwl_lock_reason_manual("sleep"), "sleep is not");
	check(!kwl_lock_reason_manual("idle"), "idleness is not");

	/* 9. The grace: none for a manual lock, five minutes for the session's, none when the clock went back. */
	check(!kwl_lock_grace(1U, 1000, 1001, GRACE), "a manual lock has no grace");
	check(kwl_lock_grace(0U, 1000, 1000, GRACE), "at once: in the grace");
	check(kwl_lock_grace(0U, 1000, 1300, GRACE), "five minutes later: still in it");
	check(!kwl_lock_grace(0U, 1000, 1301, GRACE), "a second more: past it");
	check(!kwl_lock_grace(0U, 1000, 999, GRACE), "a clock that went back: none");

	/* The verdict. */
	if (failures != 0) {
		printf("host-lock-swipe: FAIL (%d of %d checks)\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("host-lock-swipe: ok (%d checks)\n", checks);
	return 0;
}
