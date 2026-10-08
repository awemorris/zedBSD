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

	/* Presses at the place. */
	kwl_lock_swipe_reset(swipe);
	(void)kwl_lock_swipe_press(swipe, x, y, HEIGHT);

	/* Moves to the other place in four steps. */
	for (step = 1; step <= 4; step++)
		kwl_lock_swipe_motion(swipe, x + (to_x - x) * step / 4, y + (to_y - y) * step / 4);

	/* Lets go, and asks whether it swiped. */
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
	int32_t short_distance;
	int followed;
	int opened;

	/* 1. The distance: 12% of the height, at least 80. */
	distance = kwl_lock_swipe_distance(HEIGHT);
	check(distance == 129, "1080 high: 129 up");
	short_distance = kwl_lock_swipe_distance(400);
	check(short_distance == 80, "a short output: at least 80");

	/* 2. A press in the lower third is followed; one in the middle is not. */
	kwl_lock_swipe_reset(&swipe);
	followed = kwl_lock_swipe_press(&swipe, 960, 900, HEIGHT);
	check(followed, "a press near the foot is followed");
	followed = kwl_lock_swipe_press(&swipe, 960, 724, HEIGHT);
	check(followed, "a press in the lower third, not at the edge, is followed");
	followed = kwl_lock_swipe_press(&swipe, 960, 540, HEIGHT);
	check(!followed, "a press in the middle is not");

	/* 3. Up the distance and more: a swipe; short of it, sideways, down, or from the middle: none. */
	opened = swipe_from(&swipe, 960, 900, 960, 900 - distance);
	check(opened, "straight up the distance swipes");
	opened = swipe_from(&swipe, 960, 800, 1000, 500);
	check(opened, "a long swipe from the lower third, a little aslant, swipes");
	opened = swipe_from(&swipe, 960, 900, 960, 900 - distance + 1);
	check(!opened, "short of the distance does not");
	opened = swipe_from(&swipe, 960, 900, 1260, 700);
	check(!opened, "more across than up does not");
	opened = swipe_from(&swipe, 960, 800, 960, 1000);
	check(!opened, "down does not");
	opened = swipe_from(&swipe, 960, 600, 960, 200);
	check(!opened, "from above the lower third does not");

	/* 4. A release without a press followed says nothing. */
	kwl_lock_swipe_reset(&swipe);
	opened = kwl_lock_swipe_release(&swipe, HEIGHT);
	check(!opened, "a release alone does not swipe");

	/* 5. The wheel: two notches up in a row open; one does not, nor a pause, nor a turn down between. */
	kwl_lock_swipe_reset(&swipe);
	opened = kwl_lock_swipe_wheel(&swipe, -1, 1000);
	check(!opened, "one notch up: not yet");
	opened = kwl_lock_swipe_wheel(&swipe, -1, 1200);
	check(opened, "a second notch soon after: open");
	opened = kwl_lock_swipe_wheel(&swipe, -1, 1300);
	check(!opened, "the count starts afresh after it");
	opened = kwl_lock_swipe_wheel(&swipe, -1, 2400);
	check(!opened, "a notch after a long pause starts again");
	opened = kwl_lock_swipe_wheel(&swipe, 1, 2500);
	check(!opened, "a notch down");
	opened = kwl_lock_swipe_wheel(&swipe, -1, 2600);
	check(!opened, "then one up: not yet");
	opened = kwl_lock_swipe_wheel(&swipe, -2, 2700);
	check(opened, "two notches up at once after it: open");

	/* 6. Two fingers on a touch pad: 10 mm up their own way, once a touch. */
	kwl_lock_swipe_reset(&swipe);
	opened = kwl_lock_swipe_pad(&swipe, -7500);
	check(!opened, "7.5 mm up: not yet");
	opened = kwl_lock_swipe_pad(&swipe, 2500);
	check(!opened, "back down 2.5 mm");
	opened = kwl_lock_swipe_pad(&swipe, -2500);
	check(!opened, "up again to 7.5 mm");
	opened = kwl_lock_swipe_pad(&swipe, -2500);
	check(opened, "10 mm up: open");
	opened = kwl_lock_swipe_pad(&swipe, -10000);
	check(!opened, "the same touch does not again");
	kwl_lock_swipe_pad_end(&swipe);
	opened = kwl_lock_swipe_pad(&swipe, 20000);
	check(!opened, "a new touch moving down does not");
	opened = kwl_lock_swipe_pad(&swipe, -12500);
	check(opened, "then 10 mm up past where it started: open");
	kwl_lock_swipe_pad_end(&swipe);

	/* 7. A gesture up: 10 mm open, less does not. */
	opened = kwl_lock_swipe_pad_gesture(&swipe, 9000);
	check(!opened, "a gesture of 9 mm does not");
	kwl_lock_swipe_pad_end(&swipe);
	opened = kwl_lock_swipe_pad_gesture(&swipe, 15000);
	check(opened, "a gesture of 15 mm opens");

	/* 8. The reasons: the user's locks and unknown ones are manual, the session's own are not. */
	opened = kwl_lock_reason_manual("key");
	check(opened, "Super+L is manual");
	opened = kwl_lock_reason_manual("home");
	check(opened, "App Home's Lock Screen is manual");
	opened = kwl_lock_reason_manual("unknown");
	check(opened, "an unknown reason is manual");
	opened = kwl_lock_reason_manual(NULL);
	check(opened, "no reason is manual");
	opened = kwl_lock_reason_manual("sleep-sleep-button");
	check(opened, "the sleep button is manual");
	opened = kwl_lock_reason_manual("sleep-app");
	check(opened, "a Sleep chosen in App Home or an application is manual");
	opened = kwl_lock_reason_manual("sleep");
	check(opened, "a sleep of no known cause is manual");
	opened = kwl_lock_reason_manual("lid");
	check(!opened, "the lid is not");
	opened = kwl_lock_reason_manual("idle");
	check(!opened, "idleness is not");
	opened = kwl_lock_reason_manual("sleep-lid");
	check(!opened, "the lid's sleep is not");
	opened = kwl_lock_reason_manual("sleep-idle");
	check(!opened, "an idle sleep is not");
	opened = kwl_lock_reason_manual("sleep-rest");
	check(!opened, "the rest after a wake is not");

	/* 9. The grace: none for a manual lock, five minutes for the session's, none when the clock went back. */
	opened = kwl_lock_grace(1U, 1000, 1001, GRACE);
	check(!opened, "a manual lock has no grace");
	opened = kwl_lock_grace(0U, 1000, 1000, GRACE);
	check(opened, "at once: in the grace");
	opened = kwl_lock_grace(0U, 1000, 1300, GRACE);
	check(opened, "five minutes later: still in it");
	opened = kwl_lock_grace(0U, 1000, 1301, GRACE);
	check(!opened, "a second more: past it");
	opened = kwl_lock_grace(0U, 1000, 999, GRACE);
	check(!opened, "a clock that went back: none");

	/* The verdict. */
	if (failures != 0) {
		printf("host-lock-swipe: FAIL (%d of %d checks)\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("host-lock-swipe: ok (%d checks)\n", checks);
	return 0;
}
