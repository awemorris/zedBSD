/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the touch pad layer's next timer (kwl_touchpad_due,
 * userland/desktop/wayland/touchpad.c compiled unchanged), which the
 * compositor's input thread waits until instead of a fixed tick: nothing
 * waits on a pad at rest; a tap of one finger clicks at its lift and waits
 * for its drag until the lift plus the drag time; a tick before that keeps
 * the wait, one at it ends it with no action; a second touch within the
 * time is the tap's second touch (no timer then), and a lift long after the
 * time does not make it one.
 *
 *   sh plan/ws352/tests/pad-due/run.sh
 */

#include "touchpad.h"

#include <stdio.h>
#include <string.h>

#define EV_ABS_TYPE		0x03U
#define CODE_SLOT		0x2fU
#define CODE_X			0x35U
#define CODE_Y			0x36U
#define CODE_TRACKING		0x39U

static int failures;
static int checks;
static struct kwl_touchpad pad;
static uint64_t now_ms;
static int32_t next_tracking;
static unsigned clicks;

static void check(int condition, const char *what);
static void frame_after(uint64_t milliseconds);
static void finger_down(int32_t x, int32_t y);
static void finger_up(void);
static uint64_t tap(void);

/* Counts a check, and prints and counts a failed one. */
static void
check(
	int condition,
	const char *what)
{
	checks++;
	if (!condition) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

/* Ends a report some time after the last, counting the left button's presses. */
static void
frame_after(
	uint64_t milliseconds)
{
	struct kwl_touchpad_actions actions;
	unsigned index;

	now_ms += milliseconds;
	kwl_touchpad_frame(&pad, now_ms, &actions);
	for (index = 0; index < actions.count; index++) {
		if (actions.actions[index].kind == KWL_TOUCHPAD_BUTTON && actions.actions[index].button == KWL_TOUCHPAD_BUTTON_LEFT && actions.actions[index].pressed == 1U)
			clicks++;
	}
}

/* One finger touches at a place. */
static void
finger_down(
	int32_t x,
	int32_t y)
{
	kwl_touchpad_event(&pad, EV_ABS_TYPE, CODE_SLOT, 0);
	kwl_touchpad_event(&pad, EV_ABS_TYPE, CODE_TRACKING, next_tracking);
	next_tracking++;
	kwl_touchpad_event(&pad, EV_ABS_TYPE, CODE_X, x);
	kwl_touchpad_event(&pad, EV_ABS_TYPE, CODE_Y, y);
}

/* The finger lifts. */
static void
finger_up(void)
{
	kwl_touchpad_event(&pad, EV_ABS_TYPE, CODE_SLOT, 0);
	kwl_touchpad_event(&pad, EV_ABS_TYPE, CODE_TRACKING, -1);
}

/* A quick still touch of one finger; returns the time of its lift. */
static uint64_t
tap(void)
{
	finger_down(600, 400);
	frame_after(8U);
	finger_up();
	frame_after(40U);
	return now_ms;
}

int
main(void)
{
	struct kwl_touchpad_actions actions;
	uint64_t lifted;
	uint64_t due;

	/* A pad at rest waits for nothing. */
	memset(&pad, 0, sizeof(pad));
	kwl_touchpad_init(&pad, 12, 12);
	kwl_touchpad_set_size(&pad, 1336, 760);
	now_ms = 100000U;
	check(kwl_touchpad_due(&pad) == UINT64_MAX, "a pad at rest has no timer");

	/* A tap clicks at its lift and waits for its drag a while. */
	lifted = tap();
	due = kwl_touchpad_due(&pad);
	check(clicks == 1U, "the tap clicked once");
	check(due != UINT64_MAX && due > lifted && due - lifted <= 1000U, "the tap waits for its drag a while after its lift");

	/* A tick before the time keeps the wait and acts not. */
	kwl_touchpad_tick(&pad, due - 1U, &actions);
	check(actions.count == 0U, "a tick before the time gives nothing");
	check(kwl_touchpad_due(&pad) == due, "a tick before the time keeps the wait");

	/* A tick at the time ends it with no action. */
	kwl_touchpad_tick(&pad, due, &actions);
	check(actions.count == 0U, "the tick at the time gives nothing");
	check(kwl_touchpad_due(&pad) == UINT64_MAX, "the tick at the time ends the wait");

	/* A second touch within the time is the tap's: no timer while it lasts. */
	now_ms += 5000U;
	(void)tap();
	finger_down(600, 400);
	frame_after(50U);
	check(kwl_touchpad_due(&pad) == UINT64_MAX, "a second touch within the time ends the timer");
	finger_up();
	frame_after(40U);
	check(clicks == 3U, "the second quick touch is a double click");

	/* A touch after the time with no tick between (the thread did not run): the wait is over, the touch a new tap. */
	now_ms += 5000U;
	(void)tap();
	due = kwl_touchpad_due(&pad);
	now_ms = due + 100U;
	clicks = 0U;
	(void)tap();
	check(clicks == 1U, "a touch after the time is a new tap, one click");
	check(kwl_touchpad_due(&pad) != UINT64_MAX, "the new tap waits for its drag");

	/* The result. */
	if (failures != 0) {
		printf("host-pad-due: FAIL (%d of %d checks)\n", failures, checks);
		return 1;
	}
	printf("host-pad-due: ok (%d checks)\n", checks);
	return 0;
}
