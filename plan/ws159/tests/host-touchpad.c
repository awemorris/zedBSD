/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the compositor's touch pad layer (ws159-p004,
 * userland/desktop/wayland/touchpad.c, compiled unchanged).
 *
 * Scripted multitouch reports of a pad with the Latitude 5330's resolution
 * (12 units a millimetre) check the rules of BUG-166 and the design's D8:
 * one finger moves the pointer (and faster when it moves faster); a short
 * touch is a tap whose left click, press and release, comes at the lift
 * (ws183-p002); a touch within TAP_DRAG_MS that moves drags until it
 * lifts, pressed before it moves the pointer; a quick second tap makes a
 * double click, a long still one adds no click; a tap of two fingers
 * clicks the right button; the pad pressed and moved drags, without the
 * press's first millimetre moving the pointer; two fingers scroll by
 * notches the natural way; a long touch is no tap.
 *
 *   plan/ws159/tests/run-host-touchpad.sh
 */

#include "touchpad.h"

#include <stdio.h>
#include <string.h>

/* The evdev codes the reports use. */
#define EV_KEY_TYPE		0x01U
#define EV_ABS_TYPE		0x03U
#define CODE_SLOT		0x2fU
#define CODE_X			0x35U
#define CODE_Y			0x36U
#define CODE_TRACKING		0x39U
#define CODE_BUTTON		0x110U

/* The 5330 pad's units per millimetre. */
#define RESOLUTION		12

/* The number of checks that failed, and of those that ran. */
static int failures;
static int checks;

/* The pad, the fake clock and the next tracking identifier. */
static struct kwl_touchpad pad;
static uint64_t now_ms;
static int32_t next_tracking;

/* The actions of the whole of one case, gathered from every call. */
static struct kwl_touchpad_action seen[256];
static unsigned seen_count;

static void check(int condition, const char *what);
static void gather(const struct kwl_touchpad_actions *actions);
static void frame(void);
static void tick(uint64_t milliseconds);
static void finger_down(int32_t slot, int32_t x, int32_t y);
static void finger_move(int32_t slot, int32_t x, int32_t y);
static void finger_up(int32_t slot);
static void button(int32_t pressed);
static void start_case(void);
static unsigned button_count(uint32_t code, uint32_t pressed);
static int button_at(unsigned index, uint32_t code, uint32_t pressed);
static int motion_before_button(void);
static int64_t motion_x(void);
static int32_t scroll_vertical(void);

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

/* Keeps the actions one call gave. */
static void
gather(
	const struct kwl_touchpad_actions *actions)
{
	unsigned index;

	/* Each action after the ones already seen. */
	for (index = 0; index < actions->count; index++) {
		if (seen_count < sizeof(seen) / sizeof(seen[0])) {
			seen[seen_count] = actions->actions[index];
			seen_count++;
		}
	}
}

/* Ends a report, 8 ms after the last. */
static void
frame(void)
{
	struct kwl_touchpad_actions actions;

	/* Time passes, then the report ends. */
	now_ms += 8U;
	kwl_touchpad_frame(&pad, now_ms, &actions);
	gather(&actions);
}

/* Lets time pass, ticking every millisecond. */
static void
tick(
	uint64_t milliseconds)
{
	struct kwl_touchpad_actions actions;
	uint64_t end;

	/* One tick a millisecond until the time is over. */
	end = now_ms + milliseconds;
	while (now_ms < end) {
		now_ms++;
		kwl_touchpad_tick(&pad, now_ms, &actions);
		gather(&actions);
	}
}

/* A finger touches at a place (in the pad's units). */
static void
finger_down(
	int32_t slot,
	int32_t x,
	int32_t y)
{
	/* Its slot, its new identifier and its place. */
	kwl_touchpad_event(&pad, EV_ABS_TYPE, CODE_SLOT, slot);
	kwl_touchpad_event(&pad, EV_ABS_TYPE, CODE_TRACKING, next_tracking);
	next_tracking++;
	kwl_touchpad_event(&pad, EV_ABS_TYPE, CODE_X, x);
	kwl_touchpad_event(&pad, EV_ABS_TYPE, CODE_Y, y);
}

/* A finger moves to a place. */
static void
finger_move(
	int32_t slot,
	int32_t x,
	int32_t y)
{
	/* Its slot and its place. */
	kwl_touchpad_event(&pad, EV_ABS_TYPE, CODE_SLOT, slot);
	kwl_touchpad_event(&pad, EV_ABS_TYPE, CODE_X, x);
	kwl_touchpad_event(&pad, EV_ABS_TYPE, CODE_Y, y);
}

/* A finger lifts. */
static void
finger_up(
	int32_t slot)
{
	/* Its slot ends its identifier. */
	kwl_touchpad_event(&pad, EV_ABS_TYPE, CODE_SLOT, slot);
	kwl_touchpad_event(&pad, EV_ABS_TYPE, CODE_TRACKING, -1);
}

/* The pad pressed or let go. */
static void
button(
	int32_t pressed)
{
	/* BTN_LEFT. */
	kwl_touchpad_event(&pad, EV_KEY_TYPE, CODE_BUTTON, pressed);
}

/* Starts a case on a fresh pad, long after any earlier one. */
static void
start_case(void)
{
	/* A pad with nothing on it, and no action seen. */
	kwl_touchpad_init(&pad, RESOLUTION, RESOLUTION);
	now_ms += 10000U;
	seen_count = 0;
}

/* Counts the presses or releases of a button. */
static unsigned
button_count(
	uint32_t code,
	uint32_t pressed)
{
	unsigned count;
	unsigned index;

	/* Every button action of that button and state. */
	count = 0;
	for (index = 0; index < seen_count; index++) {
		if (seen[index].kind == KWL_TOUCHPAD_BUTTON && seen[index].button == code && seen[index].pressed == pressed)
			count++;
	}

	/* The count. */
	return count;
}

/* Tells whether the action at an index is a given button's press or release. */
static int
button_at(
	unsigned index,
	uint32_t code,
	uint32_t pressed)
{
	/* An action that is not there. */
	if (index >= seen_count)
		return 0;

	/* The button and its state. */
	if (seen[index].kind != KWL_TOUCHPAD_BUTTON)
		return 0;
	if (seen[index].button != code || seen[index].pressed != pressed)
		return 0;
	return 1;
}

/* Tells whether a motion comes before the first button action of the case. */
static int
motion_before_button(void)
{
	unsigned index;

	/* The actions in order until the first button. */
	for (index = 0; index < seen_count; index++) {
		if (seen[index].kind == KWL_TOUCHPAD_BUTTON)
			return 0;
		if (seen[index].kind == KWL_TOUCHPAD_MOTION)
			return 1;
	}

	/* No motion before a button. */
	return 0;
}

/* Adds up the pointer's motion across. */
static int64_t
motion_x(void)
{
	int64_t total;
	unsigned index;

	/* Every motion action. */
	total = 0;
	for (index = 0; index < seen_count; index++) {
		if (seen[index].kind == KWL_TOUCHPAD_MOTION)
			total += seen[index].dx;
	}

	/* The motion. */
	return total;
}

/* Adds up the vertical scrolling. */
static int32_t
scroll_vertical(void)
{
	int32_t total;
	unsigned index;

	/* Every scroll action. */
	total = 0;
	for (index = 0; index < seen_count; index++) {
		if (seen[index].kind == KWL_TOUCHPAD_SCROLL)
			total += seen[index].vertical;
	}

	/* The scrolling. */
	return total;
}

/* Runs every case. */
int
main(void)
{
	struct kwl_touchpad_actions released;
	int64_t slow;
	int64_t fast;
	int step;

	/* 1. One finger moving 24 mm slowly, then 24 mm fast: the fast stroke goes further. */
	start_case();
	finger_down(0, 100, 100);
	frame();
	for (step = 1; step <= 24; step++) {
		finger_move(0, 100 + step * 12, 100);
		frame();
	}

	/* The slow stroke's motion. */
	slow = motion_x();
	check(slow > 0, "a finger moving right moves the pointer right");
	check(button_count(KWL_TOUCHPAD_BUTTON_LEFT, 1U) == 0U, "a moving finger presses nothing");
	finger_up(0);
	frame();
	start_case();
	finger_down(0, 100, 100);
	frame();
	for (step = 1; step <= 4; step++) {
		finger_move(0, 100 + step * 72, 100);
		frame();
	}

	/* The fast stroke's motion. */
	fast = motion_x();
	check(fast > slow, "a fast stroke of the same length moves the pointer further");
	finger_up(0);
	frame();

	/* 2. A tap: the left press and release at the lift (ws183-p002), nothing later. */
	start_case();
	finger_down(0, 500, 300);
	frame();
	finger_up(0);
	frame();
	check(seen_count == 2U, "a tap gives two button actions at the lift");
	check(button_at(0, KWL_TOUCHPAD_BUTTON_LEFT, 1U) && button_at(1, KWL_TOUCHPAD_BUTTON_LEFT, 0U), "the tap's click completes at the lift");
	tick(400U);
	check(seen_count == 2U, "nothing more after the drag time");

	/* 3. A tap and a touch within 300 ms that moves: a drag with the button held until the lift. */
	start_case();
	finger_down(0, 500, 300);
	frame();
	finger_up(0);
	frame();
	tick(100U);
	finger_down(0, 520, 300);
	frame();
	for (step = 1; step <= 10; step++) {
		finger_move(0, 520 + step * 12, 300);
		frame();
	}

	/* The finger stays down a while. */
	tick(500U);
	check(button_at(0, KWL_TOUCHPAD_BUTTON_LEFT, 1U) && button_at(1, KWL_TOUCHPAD_BUTTON_LEFT, 0U), "the tap's click first");
	check(button_at(2, KWL_TOUCHPAD_BUTTON_LEFT, 1U), "the drag presses before any motion");
	check(button_count(KWL_TOUCHPAD_BUTTON_LEFT, 0U) == 1U, "the drag holds the button while the finger is down");
	check(motion_x() > 0, "the drag moves the pointer");
	finger_up(0);
	frame();
	check(button_count(KWL_TOUCHPAD_BUTTON_LEFT, 1U) == 2U && button_count(KWL_TOUCHPAD_BUTTON_LEFT, 0U) == 2U, "the lift ends the drag: the tap's click and the drag's press and release");
	check(!motion_before_button(), "no motion before the tap's click");

	/* 4. Two quick taps: a double click (press, release, press, release). */
	start_case();
	finger_down(0, 500, 300);
	frame();
	finger_up(0);
	frame();
	tick(80U);
	finger_down(0, 501, 300);
	frame();
	finger_up(0);
	frame();
	check(seen_count == 4U, "two taps give four button actions");
	check(button_at(0, KWL_TOUCHPAD_BUTTON_LEFT, 1U) && button_at(1, KWL_TOUCHPAD_BUTTON_LEFT, 0U), "the first click");
	check(button_at(2, KWL_TOUCHPAD_BUTTON_LEFT, 1U) && button_at(3, KWL_TOUCHPAD_BUTTON_LEFT, 0U), "the second click");

	/* 5. A tap of two fingers clicks the right button. */
	start_case();
	finger_down(0, 500, 300);
	finger_down(1, 700, 300);
	frame();
	finger_up(0);
	finger_up(1);
	frame();
	check(button_count(KWL_TOUCHPAD_BUTTON_RIGHT, 1U) == 1U && button_count(KWL_TOUCHPAD_BUTTON_RIGHT, 0U) == 1U, "a two-finger tap is a right click");
	check(button_count(KWL_TOUCHPAD_BUTTON_LEFT, 1U) == 0U, "and no left press");

	/* 6. The pad pressed and moved: a left drag, the first millimetre still. */
	start_case();
	finger_down(0, 500, 600);
	frame();
	button(1);
	finger_move(0, 506, 600);
	frame();
	check(button_count(KWL_TOUCHPAD_BUTTON_LEFT, 1U) == 1U, "the press is the left button");
	check(motion_x() == 0, "half a millimetre after the press does not move the pointer");
	for (step = 1; step <= 10; step++) {
		finger_move(0, 506 + step * 12, 600);
		frame();
	}

	/* The drag moved the pointer. */
	check(motion_x() > 0, "moving while pressed drags");
	button(0);
	frame();
	finger_up(0);
	frame();
	tick(400U);
	check(button_count(KWL_TOUCHPAD_BUTTON_LEFT, 0U) == 1U, "letting go releases it once");
	check(button_count(KWL_TOUCHPAD_BUTTON_LEFT, 1U) == 1U, "a pressed touch is no tap");

	/* 7. Two fingers moving down 10 mm: notches the natural way (the content follows: scrolling up, negative). */
	start_case();
	finger_down(0, 500, 200);
	finger_down(1, 700, 200);
	frame();
	for (step = 1; step <= 10; step++) {
		finger_move(0, 500, 200 + step * 12);
		finger_move(1, 700, 200 + step * 12);
		frame();
	}

	/* The notches. */
	check(scroll_vertical() == -4, "10 mm down is four notches up");
	check(motion_x() == 0, "scrolling does not move the pointer");
	finger_up(0);
	finger_up(1);
	frame();
	tick(400U);
	check(button_count(KWL_TOUCHPAD_BUTTON_RIGHT, 1U) == 0U, "a scroll is no tap");

	/* 8. A finger held 300 ms without moving is no tap. */
	start_case();
	finger_down(0, 500, 300);
	frame();
	tick(300U);
	finger_up(0);
	frame();
	tick(400U);
	check(button_count(KWL_TOUCHPAD_BUTTON_LEFT, 1U) == 0U, "a long touch presses nothing");

	/* 9. The pad pressed with two fingers on it is the right button. */
	start_case();
	finger_down(0, 500, 600);
	finger_down(1, 800, 600);
	frame();
	button(1);
	frame();
	button(0);
	frame();
	check(button_count(KWL_TOUCHPAD_BUTTON_RIGHT, 1U) == 1U && button_count(KWL_TOUCHPAD_BUTTON_RIGHT, 0U) == 1U, "a two-finger press is the right button");

	/* 10. A tap, then a touch within the drag time held still and long: no second click. */
	start_case();
	finger_down(0, 500, 300);
	frame();
	finger_up(0);
	frame();
	tick(100U);
	finger_down(0, 501, 300);
	frame();
	tick(400U);
	finger_up(0);
	frame();
	tick(400U);
	check(button_count(KWL_TOUCHPAD_BUTTON_LEFT, 1U) == 1U && button_count(KWL_TOUCHPAD_BUTTON_LEFT, 0U) == 1U, "a long still touch after a tap adds no click");
	check(motion_x() == 0, "nor moves the pointer");

	/* 11. A touch after a tap going with the device: no button moves (the tap's click was complete). */
	start_case();
	finger_down(0, 500, 300);
	frame();
	finger_up(0);
	frame();
	tick(50U);
	finger_down(0, 500, 300);
	frame();
	kwl_touchpad_release_all(&pad, &released);
	gather(&released);
	check(seen_count == 2U, "release_all after a tap releases nothing more");

	/* 12. A touch after the drag time is a touch of its own: it moves the pointer and presses nothing. */
	start_case();
	finger_down(0, 500, 300);
	frame();
	finger_up(0);
	frame();
	tick(320U);
	finger_down(0, 500, 300);
	frame();
	for (step = 1; step <= 10; step++) {
		finger_move(0, 500 + step * 12, 300);
		frame();
	}

	/* The finger lifts after its stroke. */
	finger_up(0);
	frame();
	check(button_count(KWL_TOUCHPAD_BUTTON_LEFT, 1U) == 1U, "a touch after the drag time is no drag");
	check(motion_x() > 0, "and moves the pointer");

	/* The verdict. */
	if (failures != 0) {
		printf("host-touchpad: FAIL (%d of %d checks)\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("host-touchpad: ok (%d checks)\n", checks);
	return 0;
}
