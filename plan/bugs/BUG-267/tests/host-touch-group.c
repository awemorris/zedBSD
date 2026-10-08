/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of BUG-267's second half: the compositor's touch screen
 * (userland/desktop/wayland/touch.c, compiled unchanged) given the finger
 * reports of the 2026-10-08 UAT 2 (plan/bugs/BUG-267/uat2/session.log:
 * two fingers swiped in from the left, the right and the bottom edge of
 * the desktop, where Files' desktop surface hears wl_touch).  This file
 * stands in for the rest of the compositor: one client with wl_touch whose
 * desktop surface covers the output, and a shell that takes a press in the
 * side strips (under the system bar) and the bottom strip, as shell.c's
 * desktops' swipe and App Home's swipe do.  It checks where each finger
 * went: to the client (wl_touch down, motion, cancel) or to the shell
 * (the press, the pointer's way, the release).
 * usage: host-touch-group
 */

#include "userland/desktop/wayland/kwl.h"
#include "userland/desktop/wayland/touch.h"
#include "userland/desktop/wayland/data.h"
#include "userland/desktop/wayland/edge.h"
#include <keiland/keiland.h>

#include <stdio.h>
#include <string.h>

/* The output: the 5330's USB-C monitor's size, and the touch screen's range is one unit a pixel. */
#define OUTPUT_WIDTH	1920
#define OUTPUT_HEIGHT	1280

/* The shell's own strips, as shell.c and edge.h have them: the side strips and the bottom one. */
#define SHELL_SIDE	16
#define SHELL_BOTTOM	KWL_EDGE_BOTTOM_HEIGHT

/* How many fingers one report of a scenario names at most. */
#define FINGERS_MAX	4U

/* The ID of the desktop surface (Files' desktop, client 3 surface 19 in the UAT's log) and of its client's wl_touch. */
#define DESKTOP_ID	19U
#define TOUCH_ID	7U

/*
 * What the stand-in shell and client have seen since the scenario began:
 * the shell's presses, releases and the pointer's last place while it held
 * one, and the client's wl_touch events by opcode.
 *
 * Each scenario starts it from zero (scenario_begin).
 */
struct seen {
	int shell_pressed;
	unsigned shell_presses;
	unsigned shell_releases;
	int32_t press_x;
	int32_t press_y;
	int32_t motion_x;
	int32_t motion_y;
	unsigned downs;
	unsigned ups;
	unsigned motions;
	unsigned cancels;
};

/* The checks that failed, and those that ran. */
static int failures;
static int checks;

/* The compositor's state as touch.c sees it; the test is its only user. */
static struct kwl_server server;

/* The touch screen's evdev node, whose frame each report fills. */
static struct kwl_input_device input;

/* The client with wl_touch, its wl_touch and its desktop surface over the whole output. */
static struct kwl_client files;
static struct kwl_object files_touch;
static struct kwl_object desktop;

/* What the shell and the client have seen in the scenario running. */
static struct seen seen;

/* The compositor's clock in milliseconds, which the scenario moves with its reports. */
static uint64_t clock_ms;

/* The serials the stand-in seat hands out. */
static uint32_t serial;

/* A motion device for kwl_touch_add (never read: the test makes no motions, so the shell follows the reports). */
static int motion_device_token;

static void check(int condition, const char *what);
static void scenario_begin(const char *name);
static void report_add(uint16_t type, uint16_t code, int32_t value);
static void report_send(uint32_t time);
static void finger_down(unsigned slot, int32_t tracking, int32_t x, int32_t y);
static void finger_move(unsigned slot, int32_t x, int32_t y);
static void finger_up(unsigned slot);
static void test_right_pair(void);
static void test_left_pair_staggered(void);
static void test_bottom_pair(void);
static void test_pinch_near_edge(void);
static void test_far_pair(void);
static void test_middle_pair(void);
static void test_joined_shell(void);
static void test_late_finger(void);
static void test_single_strip(void);
static void test_lift_before_in(void);
static void test_other_in_middle(void);
static void test_joined_far(void);
static void test_top_pair(void);
static void test_top_in_bar(void);

/* Runs every scenario and reports how many checks failed. */
int
main(
	void)
{
	int error;

	/* The output and the glass look with windows, as the UAT's desktop was. */
	server.width = OUTPUT_WIDTH;
	server.height = OUTPUT_HEIGHT;
	server.glass = 1;
	server.windowed = 1;

	/* The client with wl_touch and its desktop surface over the whole output. */
	files.number = 3;
	files.server = &server;
	files.objects = &files_touch;
	files_touch.client = &files;
	files_touch.kind = KWL_TOUCH;
	files_touch.id = TOUCH_ID;
	desktop.client = &files;
	desktop.kind = KWL_SURFACE;
	desktop.id = DESKTOP_ID;

	/* The touch screen node. */
	input.fd = 3;
	snprintf(input.path, sizeof(input.path), "/dev/input/event3");
	error = kwl_touch_add(&input);
	check(error == 0, "the touch screen is taken");

	/* Each scenario of the UAT's swipes, and what must stay as it was. */
	test_right_pair();
	test_left_pair_staggered();
	test_bottom_pair();
	test_pinch_near_edge();
	test_far_pair();
	test_middle_pair();
	test_joined_shell();
	test_late_finger();
	test_single_strip();
	test_lift_before_in();
	test_other_in_middle();
	test_joined_far();
	test_top_pair();
	test_top_in_bar();

	/* The touch screen goes. */
	kwl_touch_remove(&server, &input, 1);

	/* A failed check fails the test. */
	if (failures != 0) {
		printf("host-touch-group: FAIL (%d of %d checks)\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("host-touch-group: ok (%d checks)\n", checks);
	return 0;
}

/* Counts a check, and reports one that failed. */
static void
check(
	int condition,
	const char *what)
{
	/* Every check counts. */
	checks++;

	/* A failed check says what it was. */
	if (!condition) {
		failures++;
		printf("FAIL: %s\n", what);
	}
}

/* Starts a scenario: nothing seen, no finger down (each scenario lifts its fingers). */
static void
scenario_begin(
	const char *name)
{
	/* What the shell and the client see starts again. */
	memset(&seen, 0, sizeof(seen));
	printf("== %s\n", name);
}

/* Adds one event to the report being made. */
static void
report_add(
	uint16_t type,
	uint16_t code,
	int32_t value)
{
	struct input_event *event;

	/* A report longer than the frame is a mistake of the test. */
	if (input.frame_count >= KWL_INPUT_FRAME_MAX) {
		check(0, "a report fits the frame");
		return;
	}

	/* Succeeded: the event is the report's next. */
	event = &input.frame[input.frame_count];
	memset(event, 0, sizeof(*event));
	event->type = type;
	event->code = code;
	event->value = value;
	input.frame_count++;
}

/* Applies the report made so far at a time (milliseconds), as the evdev frame would. */
static void
report_send(
	uint32_t time)
{
	/* The compositor's clock follows the report. */
	clock_ms = time;
	input.frame_time_us = (uint64_t)time * 1000U;

	/* The report's end, and the next starts empty. */
	report_add(EV_SYN, SYN_REPORT, 0);
	kwl_touch_frame(&server, &input, time);
	input.frame_count = 0;
}

/* Adds a finger touching in a slot to the report being made. */
static void
finger_down(
	unsigned slot,
	int32_t tracking,
	int32_t x,
	int32_t y)
{
	/* The slot, its new finger and its place. */
	report_add(EV_ABS, ABS_MT_SLOT, (int32_t)slot);
	report_add(EV_ABS, ABS_MT_TRACKING_ID, tracking);
	report_add(EV_ABS, ABS_MT_POSITION_X, x);
	report_add(EV_ABS, ABS_MT_POSITION_Y, y);
}

/* Adds a finger's new place to the report being made. */
static void
finger_move(
	unsigned slot,
	int32_t x,
	int32_t y)
{
	/* The slot and its place. */
	report_add(EV_ABS, ABS_MT_SLOT, (int32_t)slot);
	report_add(EV_ABS, ABS_MT_POSITION_X, x);
	report_add(EV_ABS, ABS_MT_POSITION_Y, y);
}

/* Adds a finger's lift to the report being made. */
static void
finger_up(
	unsigned slot)
{
	/* The slot and no finger. */
	report_add(EV_ABS, ABS_MT_SLOT, (int32_t)slot);
	report_add(EV_ABS, ABS_MT_TRACKING_ID, -1);
}

/*
 * The UAT's line 1537: two fingers at the right edge (1897, 600) and
 * (1901, 827), neither in the 16-pixel strip, touch in one report and
 * swipe left: the desktops' swipe, led by the finger nearest the edge.
 */
static void
test_right_pair(
	void)
{
	unsigned step;

	/* Both fingers touch together; the client hears both. */
	scenario_begin("right edge, two fingers in one report");
	finger_down(0, 101, 1897, 600);
	finger_down(1, 102, 1901, 827);
	report_send(1000);
	check(seen.downs == 2, "right: the client hears both fingers touch");
	check(seen.shell_presses == 0, "right: the shell has nothing yet");

	/* They move left 20 pixels a report. */
	for (step = 1; step <= 10; step++) {
		finger_move(0, 1897 - 20 * (int32_t)step, 600);
		finger_move(1, 1901 - 20 * (int32_t)step, 827);
		report_send(1000 + 16 * step);
	}

	/* The shell took them as the swipe from the edge, at the leader's row. */
	check(seen.cancels == 1, "right: the client hears cancel");
	check(seen.shell_presses == 1, "right: the shell hears one press");
	check(seen.press_x == OUTPUT_WIDTH - 1, "right: the press is on the right edge");
	check(seen.press_y == 827, "right: the press is the nearest finger's row");
	check(seen.motion_x == OUTPUT_WIDTH - 1 - 200, "right: the pointer went the leader's 200 pixels in");

	/* They lift: the shell hears the release. */
	finger_up(0);
	finger_up(1);
	report_send(1200);
	check(seen.shell_releases == 1, "right: the shell hears the release");
	check(seen.ups == 0, "right: the cancelled client hears no up");
}

/*
 * The UAT's line 1454: a finger at (21, 751) on the left, the second at
 * (167, 738) 40 ms later, both swiping right: the second is within reach,
 * the first leads.
 */
static void
test_left_pair_staggered(
	void)
{
	unsigned step;

	/* The first finger, then the second. */
	scenario_begin("left edge, the second finger 40 ms later");
	finger_down(0, 111, 21, 751);
	report_send(2000);
	finger_move(0, 25, 751);
	finger_down(1, 112, 167, 738);
	report_send(2040);
	check(seen.downs == 2, "left: the client hears both fingers touch");

	/* Both move right. */
	for (step = 1; step <= 8; step++) {
		finger_move(0, 25 + 15 * (int32_t)step, 751 + (int32_t)step);
		finger_move(1, 167 + 15 * (int32_t)step, 738);
		report_send(2040 + 16 * step);
	}

	/* The shell's press is on the left edge at the first finger's row, then follows it. */
	check(seen.cancels == 1, "left: the client hears cancel");
	check(seen.shell_presses == 1, "left: the shell hears one press");
	check(seen.press_x == 0, "left: the press is on the left edge");
	check(seen.press_y == 751, "left: the press is the first finger's row");
	check(seen.motion_x == 25 + 120 - 21, "left: the pointer is the leader's way in from the edge");

	/* They lift. */
	finger_up(0);
	finger_up(1);
	report_send(2300);
	check(seen.shell_releases == 1, "left: the shell hears the release");
}

/* The UAT's line 908: two fingers at the bottom, (929, 1258) and (1067, 1259), swipe up: App Home's swipe. */
static void
test_bottom_pair(
	void)
{
	unsigned step;

	/* Both touch. */
	scenario_begin("bottom edge, two fingers");
	finger_down(0, 121, 929, 1258);
	finger_down(1, 122, 1067, 1259);
	report_send(3000);

	/* Both move up. */
	for (step = 1; step <= 6; step++) {
		finger_move(0, 929, 1258 - 30 * (int32_t)step);
		finger_move(1, 1067 + (int32_t)step, 1259 - 30 * (int32_t)step);
		report_send(3000 + 16 * step);
	}

	/* The shell's press is on the bottom edge under the nearest finger. */
	check(seen.shell_presses == 1, "bottom: the shell hears one press");
	check(seen.press_x == 1067, "bottom: the press is the nearest finger's column");
	check(seen.press_y == OUTPUT_HEIGHT - 1, "bottom: the press is on the bottom edge");
	check(seen.motion_y == OUTPUT_HEIGHT - 1 - 180, "bottom: the pointer went the leader's way up");

	/* They lift. */
	finger_up(0);
	finger_up(1);
	report_send(3200);
	check(seen.shell_releases == 1, "bottom: the shell hears the release");
}

/* Two fingers near the right edge that spread apart (a pinch) stay the client's. */
static void
test_pinch_near_edge(
	void)
{
	unsigned step;

	/* Both touch near the edge. */
	scenario_begin("pinch near the right edge");
	finger_down(0, 131, 1880, 500);
	finger_down(1, 132, 1860, 560);
	report_send(4000);

	/* They spread up and down. */
	for (step = 1; step <= 8; step++) {
		finger_move(0, 1880, 500 - 10 * (int32_t)step);
		finger_move(1, 1860, 560 + 10 * (int32_t)step);
		report_send(4000 + 16 * step);
	}

	/* The client heard all of it. */
	check(seen.shell_presses == 0, "pinch: the shell hears no press");
	check(seen.cancels == 0, "pinch: the client is not cancelled");
	check(seen.motions == 16, "pinch: the client hears every motion");

	/* They lift; the client hears both. */
	finger_up(0);
	finger_up(1);
	report_send(4200);
	check(seen.ups == 2, "pinch: the client hears both lift");
}

/*
 * The UAT's line 852: (1786, 515) and (1744, 900), 133 pixels and more
 * from the edge, swipe left: too far from the edge for its swipe.
 */
static void
test_far_pair(
	void)
{
	unsigned step;

	/* Both touch, then move left. */
	scenario_begin("two fingers too far from the right edge");
	finger_down(0, 141, 1786, 515);
	finger_down(1, 142, 1744, 900);
	report_send(5000);
	for (step = 1; step <= 6; step++) {
		finger_move(0, 1786 - 20 * (int32_t)step, 515);
		finger_move(1, 1744 - 20 * (int32_t)step, 900);
		report_send(5000 + 16 * step);
	}

	/* The client keeps them. */
	check(seen.shell_presses == 0, "far: the shell hears no press");
	check(seen.cancels == 0, "far: the client is not cancelled");

	/* They lift. */
	finger_up(0);
	finger_up(1);
	report_send(5200);
	check(seen.ups == 2, "far: the client hears both lift");
}

/* The UAT's line 1062: two fingers in the middle, (849, 541) and (873, 913), stay the client's. */
static void
test_middle_pair(
	void)
{
	unsigned step;

	/* Both touch, then move right. */
	scenario_begin("two fingers in the middle");
	finger_down(0, 151, 849, 541);
	finger_down(1, 152, 873, 913);
	report_send(6000);
	for (step = 1; step <= 6; step++) {
		finger_move(0, 849 + 20 * (int32_t)step, 541);
		finger_move(1, 873 + 20 * (int32_t)step, 913);
		report_send(6000 + 16 * step);
	}

	/* The client keeps them. */
	check(seen.shell_presses == 0, "middle: the shell hears no press");
	check(seen.motions == 12, "middle: the client hears every motion");

	/* They lift. */
	finger_up(0);
	finger_up(1);
	report_send(6200);
	check(seen.ups == 2, "middle: the client hears both lift");
}

/*
 * The UAT's line 1577: the outer finger lands in the strip (1915, 676) and
 * the shell takes it; the inner one (1805, 662) 30 ms later was a tap on
 * the desktop (ZFILES TOUCH tap).  It now goes nowhere.
 */
static void
test_joined_shell(
	void)
{
	unsigned step;

	/* The outer finger: the shell's. */
	scenario_begin("outer finger in the strip, inner one joins");
	finger_down(0, 161, 1915, 676);
	report_send(7000);
	check(seen.shell_presses == 1, "joined: the shell takes the outer finger");
	check(seen.press_x == 1915, "joined: the press is the finger's own point");

	/* The inner finger 30 ms later: nobody hears it. */
	finger_down(1, 162, 1805, 662);
	report_send(7030);
	check(seen.downs == 0, "joined: the client hears no down");

	/* Both move left and lift. */
	for (step = 1; step <= 5; step++) {
		finger_move(0, 1915 - 30 * (int32_t)step, 676);
		finger_move(1, 1805 - 30 * (int32_t)step, 662);
		report_send(7030 + 16 * step);
	}

	/* The inner finger lifts, then the outer one. */
	finger_up(1);
	report_send(7200);
	finger_up(0);
	report_send(7210);
	check(seen.downs == 0, "joined: the client never hears the inner finger");
	check(seen.motion_x == 1915 - 150, "joined: the shell follows the outer finger");
	check(seen.shell_releases == 1, "joined: the shell hears the release");
}

/* A second finger near the edge 200 ms after the first is another finger: no group. */
static void
test_late_finger(
	void)
{
	unsigned step;

	/* The first finger near the left edge, the second much later. */
	scenario_begin("second finger too late");
	finger_down(0, 171, 30, 700);
	report_send(8000);
	finger_down(1, 172, 60, 800);
	report_send(8200);

	/* Both move right. */
	for (step = 1; step <= 6; step++) {
		finger_move(0, 30 + 20 * (int32_t)step, 700);
		finger_move(1, 60 + 20 * (int32_t)step, 800);
		report_send(8200 + 16 * step);
	}

	/* The client keeps them. */
	check(seen.shell_presses == 0, "late: the shell hears no press");
	check(seen.downs == 2, "late: the client hears both");

	/* They lift. */
	finger_up(0);
	finger_up(1);
	report_send(8400);
	check(seen.ups == 2, "late: the client hears both lift");
}

/* One finger in the left strip is the shell's swipe as before (no group needed). */
static void
test_single_strip(
	void)
{
	unsigned step;

	/* The finger touches in the strip and moves right. */
	scenario_begin("one finger in the strip");
	finger_down(0, 181, 5, 720);
	report_send(9000);
	for (step = 1; step <= 6; step++) {
		finger_move(0, 5 + 30 * (int32_t)step, 720);
		report_send(9000 + 16 * step);
	}

	/* The shell had it all the way, from its own point. */
	check(seen.shell_presses == 1, "single: the shell hears one press");
	check(seen.press_x == 5, "single: the press is the finger's own point");
	check(seen.motion_x == 185, "single: the pointer follows the finger");
	check(seen.downs == 0, "single: the client hears nothing");

	/* It lifts. */
	finger_up(0);
	report_send(9200);
	check(seen.shell_releases == 1, "single: the shell hears the release");
}

/* Of two fingers near the edge, one lifts before they move in: the other stays the client's. */
static void
test_lift_before_in(
	void)
{
	unsigned step;

	/* Both touch near the right edge; one lifts at once. */
	scenario_begin("one finger lifts before moving in");
	finger_down(0, 191, 1890, 600);
	finger_down(1, 192, 1885, 700);
	report_send(10000);
	finger_up(1);
	report_send(10050);

	/* The other moves left. */
	for (step = 1; step <= 6; step++) {
		finger_move(0, 1890 - 20 * (int32_t)step, 600);
		report_send(10050 + 16 * step);
	}

	/* The client keeps it. */
	check(seen.shell_presses == 0, "lift: the shell hears no press");
	check(seen.motions == 6, "lift: the client hears the motions");

	/* It lifts. */
	finger_up(0);
	report_send(10300);
	check(seen.ups == 2, "lift: the client hears both lift");
}

/*
 * The rest of the compositor, as far as touch.c needs it.
 */

/* Reads the touch screen's ranges: one unit a pixel of the output, and slot 0 first. */
int
kl_backend_input_absinfo(
	int descriptor,
	uint32_t axis,
	struct input_absinfo *info)
{
	(void)descriptor;

	/* Each axis touch.c asks for. */
	memset(info, 0, sizeof(*info));
	if (axis == ABS_MT_POSITION_X) {
		info->maximum = OUTPUT_WIDTH - 1;
	} else if (axis == ABS_MT_POSITION_Y) {
		info->maximum = OUTPUT_HEIGHT - 1;
	}

	/* Succeeded: the range. */
	return 0;
}

/* Makes the screen's motion device (a token: the test makes no motions). */
struct kl_motion_device *
kl_motion_device_create(
	void)
{
	/* Succeeded: a pointer touch.c only hands back. */
	return (struct kl_motion_device *)(void *)&motion_device_token;
}

/* Frees the motion device (nothing to free). */
void
kl_motion_device_destroy(
	struct kl_motion_device *device)
{
	(void)device;
}

/* Maps a Scan Time (never asked: the reports carry none). */
int
kl_motion_device_time(
	struct kl_motion_device *device,
	uint64_t host_us,
	uint32_t device_us,
	uint64_t *stamp_us)
{
	(void)device;
	(void)device_us;

	/* Succeeded: the host time. */
	*stamp_us = host_us;
	return 0;
}

/* Makes no motion: the shell follows each report as it comes. */
struct kl_motion *
kl_motion_create(
	struct kl_motion_device *device)
{
	(void)device;

	/* Succeeded: none. */
	return NULL;
}

/* Frees a motion (there are none). */
void
kl_motion_destroy(
	struct kl_motion *motion)
{
	(void)motion;
}

/* Starts a stroke (there are no motions). */
void
kl_motion_begin(
	struct kl_motion *motion)
{
	(void)motion;
}

/* Adds a report to a stroke (there are no motions). */
int
kl_motion_add(
	struct kl_motion *motion,
	uint64_t stamp_us,
	uint64_t arrival_us,
	double x,
	double y)
{
	(void)motion;
	(void)stamp_us;
	(void)arrival_us;
	(void)x;
	(void)y;

	/* Succeeded: nothing to add to. */
	return 0;
}

/* Ends a stroke (there are no motions). */
void
kl_motion_end(
	struct kl_motion *motion)
{
	(void)motion;
}

/* Gives a stroke's point (there are no motions). */
int
kl_motion_point(
	struct kl_motion *motion,
	uint64_t now_us,
	uint32_t extrapolation_us,
	double *x,
	double *y)
{
	(void)motion;
	(void)now_us;
	(void)extrapolation_us;
	*x = 0.0;
	*y = 0.0;

	/* No motion has a point. */
	return 1;
}

/* Redraws the cursor (nothing drawn). */
void
kwl_damage_pointer(
	struct kwl_server *unused,
	int32_t old_x,
	int32_t old_y)
{
	(void)unused;
	(void)old_x;
	(void)old_y;
}

/* Gives a drag up (no drags). */
void
kwl_data_drag_cancel(
	struct kwl_server *unused)
{
	(void)unused;
}

/* Moves a drag (no drags). */
void
kwl_data_drag_motion(
	struct kwl_server *unused,
	uint32_t time)
{
	(void)unused;
	(void)time;
}

/* Drops a drag (no drags). */
void
kwl_data_drag_release(
	struct kwl_server *unused)
{
	(void)unused;
}

/* Finds the desktop's surface: Files' desktop covers the output. */
struct kwl_object *
kwl_desktop_at(
	struct kwl_server *unused,
	int32_t x,
	int32_t y)
{
	(void)unused;
	(void)x;
	(void)y;

	/* Succeeded: the desktop. */
	return &desktop;
}

/* Hears an event for the client: counts its wl_touch events. */
int
kwl_emit(
	struct kwl_client *client,
	uint32_t object,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	(void)client;
	(void)object;
	(void)payload;
	(void)size;

	/* Each wl_touch event the client hears (frames are not counted). */
	switch (opcode) {
	case 0:
		seen.downs++;
		break;
	case 1:
		seen.ups++;
		break;
	case 2:
		seen.motions++;
		break;
	case 4:
		seen.cancels++;
		break;
	default:
		break;
	}

	/* Succeeded: queued. */
	return 0;
}

/* Finds a window's body at a point (none: the desktop is bare). */
struct kwl_object *
kwl_glass_body_at(
	struct kwl_server *unused,
	int32_t x,
	int32_t y)
{
	(void)unused;
	(void)x;
	(void)y;

	/* No window. */
	return NULL;
}

/* Sends a window to the back (no windows). */
void
kwl_glass_lower(
	struct kwl_server *unused,
	struct kwl_object *surface,
	const char *via)
{
	(void)unused;
	(void)surface;
	(void)via;
}

/* Finds a title bar at a point (none). */
struct kwl_object *
kwl_glass_title_at(
	struct kwl_server *unused,
	int32_t x,
	int32_t y)
{
	(void)unused;
	(void)x;
	(void)y;

	/* No title bar. */
	return NULL;
}

/* Tells whether the on-screen keyboard is at a point (it is closed). */
int
kwl_keyboard_at(
	int32_t x,
	int32_t y)
{
	(void)x;
	(void)y;

	/* The keyboard is closed. */
	return 0;
}

/* Cancels a keyboard's press (the keyboard is closed). */
void
kwl_keyboard_touch_cancel(
	struct kwl_server *unused,
	uint32_t id)
{
	(void)unused;
	(void)id;
}

/* Presses the keyboard (it is closed). */
int
kwl_keyboard_touch_down(
	struct kwl_server *unused,
	uint32_t id,
	int32_t x,
	int32_t y,
	uint32_t time)
{
	(void)unused;
	(void)id;
	(void)x;
	(void)y;
	(void)time;

	/* Not taken. */
	return 0;
}

/* Moves a keyboard's press (the keyboard is closed). */
int
kwl_keyboard_touch_motion(
	struct kwl_server *unused,
	uint32_t id,
	int32_t x,
	int32_t y,
	uint32_t time)
{
	(void)unused;
	(void)id;
	(void)x;
	(void)y;
	(void)time;

	/* Not taken. */
	return 0;
}

/* Ends a keyboard's press (the keyboard is closed). */
int
kwl_keyboard_touch_up(
	struct kwl_server *unused,
	uint32_t id,
	int32_t x,
	int32_t y,
	uint32_t time)
{
	(void)unused;
	(void)id;
	(void)x;
	(void)y;
	(void)time;

	/* Not taken. */
	return 0;
}

/* Reads the compositor's clock: the scenario's. */
uint64_t
kwl_milliseconds(
	void)
{
	/* Succeeded: the time of the last report. */
	return clock_ms;
}

/* Hands out the next serial. */
uint32_t
kwl_next_serial(
	struct kwl_server *unused)
{
	(void)unused;

	/* Succeeded: a new serial. */
	serial++;
	return serial;
}

/* Marks the pointer as absolute (nothing to mark). */
void
kwl_pointer_absolute(
	struct kwl_server *unused)
{
	(void)unused;
}

/* Delivers a button through the shell to a client (no client of the pointer here). */
void
kwl_seat_button(
	struct kwl_server *unused,
	uint32_t time,
	uint32_t button,
	uint32_t state)
{
	(void)unused;
	(void)time;
	(void)button;
	(void)state;
}

/* Delivers a button to a client (no client of the pointer here). */
void
kwl_seat_button_deliver(
	struct kwl_server *unused,
	uint32_t time,
	uint32_t button,
	uint32_t state)
{
	(void)unused;
	(void)time;
	(void)button;
	(void)state;
}

/*
 * Passes a button to the stand-in shell: a press in a side strip under the
 * system bar (the desktops' swipe), in the bottom strip (App Home's swipe)
 * or in the system bar (the top band, Wiseview's swipe, BUG-270) is taken,
 * and so is the release of a press taken.
 */
int
kwl_seat_button_shell(
	struct kwl_server *shell,
	uint32_t time,
	uint32_t button,
	uint32_t state)
{
	int32_t x;
	int32_t y;
	int strip;

	/* The time and the button do not matter: every press is the left button's. */
	(void)time;
	(void)button;

	/* A release ends the press the shell took. */
	if (state == 0U) {
		if (!seen.shell_pressed)
			return 0;
		seen.shell_pressed = 0;
		seen.shell_releases++;
		return 1;
	}

	/* Whether the press is in one of the shell's strips. */
	x = shell->pointer_x;
	y = shell->pointer_y;
	strip = 0;
	if (y >= KWL_GLASS_BAR && (x < SHELL_SIDE || x >= OUTPUT_WIDTH - SHELL_SIDE))
		strip = 1;
	if (y >= OUTPUT_HEIGHT - SHELL_BOTTOM)
		strip = 1;
	if (y < KWL_EDGE_BAND_DEEP)
		strip = 1;

	/* Anywhere else the press goes on to what is under it, and the button is held. */
	if (!strip) {
		shell->buttons_down |= 1U;
		return 0;
	}

	/* Succeeded: the shell has the press. */
	seen.shell_pressed = 1;
	seen.shell_presses++;
	seen.press_x = x;
	seen.press_y = y;
	seen.motion_x = x;
	seen.motion_y = y;
	return 1;
}

/* Ends the pointer's events for a client (none here). */
void
kwl_seat_frame(
	struct kwl_server *unused)
{
	(void)unused;
}

/* Moves the pointer through the shell to a client (no client of the pointer here). */
void
kwl_seat_motion(
	struct kwl_server *unused,
	uint32_t time)
{
	(void)unused;
	(void)time;
}

/* Moves the pointer to a client (no client of the pointer here). */
void
kwl_seat_motion_deliver(
	struct kwl_server *unused,
	uint32_t time)
{
	(void)unused;
	(void)time;
}

/* Moves the pointer through the stand-in shell: the swipe it holds follows it. */
int
kwl_seat_motion_shell(
	struct kwl_server *shell,
	uint32_t time)
{
	(void)time;

	/* Only a press the shell holds follows. */
	if (!seen.shell_pressed)
		return 0;

	/* Succeeded: the swipe is where the pointer is. */
	seen.motion_x = shell->pointer_x;
	seen.motion_y = shell->pointer_y;
	return 1;
}

/* Finds the pointer's surface (left to touch.c's own search). */
void
kwl_seat_pointer_update(
	struct kwl_server *shell)
{
	/* No surface: touch.c looks at the finger's point itself. */
	shell->pointer_surface = NULL;
}

/* Finds a sub-surface at a point (none). */
struct kwl_object *
kwl_subsurface_at(
	struct kwl_object *root,
	int32_t x,
	int32_t y)
{
	(void)root;
	(void)x;
	(void)y;

	/* No sub-surface. */
	return NULL;
}

/* Cancels the Windows key's tap (no key). */
void
kwl_super_tap_cancel(
	struct kwl_super_tap *tap)
{
	(void)tap;
}

/* Gives a surface's size: the desktop's is the output's. */
void
kwl_surface_size(
	const struct kwl_object *surface,
	uint32_t *width,
	uint32_t *height)
{
	(void)surface;

	/* Succeeded: the output's size. */
	*width = OUTPUT_WIDTH;
	*height = OUTPUT_HEIGHT;
}

/*
 * The user's decision (2026-10-08): with the outermost finger at the edge,
 * the others may be anywhere.  (1890, 600) at the right edge and
 * (900, 700) in the middle swipe left: the right edge's swipe, led by the
 * outer finger.
 */
static void
test_other_in_middle(
	void)
{
	unsigned step;

	/* Both touch; the client hears both. */
	scenario_begin("outer finger at the right edge, the other in the middle");
	finger_down(0, 201, 900, 700);
	finger_down(1, 202, 1890, 600);
	report_send(11000);
	check(seen.downs == 2, "middle-other: the client hears both fingers touch");

	/* Both move left. */
	for (step = 1; step <= 6; step++) {
		finger_move(0, 900 - 25 * (int32_t)step, 700);
		finger_move(1, 1890 - 25 * (int32_t)step, 600);
		report_send(11000 + 16 * step);
	}

	/* The shell took them, from the outer finger's row on the edge. */
	check(seen.cancels == 1, "middle-other: the client hears cancel");
	check(seen.shell_presses == 1, "middle-other: the shell hears one press");
	check(seen.press_x == OUTPUT_WIDTH - 1, "middle-other: the press is on the right edge");
	check(seen.press_y == 600, "middle-other: the press is the outer finger's row");
	check(seen.motion_x == OUTPUT_WIDTH - 1 - 150, "middle-other: the pointer went the outer finger's way");

	/* They lift. */
	finger_up(0);
	finger_up(1);
	report_send(11200);
	check(seen.shell_releases == 1, "middle-other: the shell hears the release");
}

/* The outer finger in the strip, the shell's; a finger soon after in the middle goes nowhere. */
static void
test_joined_far(
	void)
{
	/* The outer finger: the shell's. */
	scenario_begin("outer finger in the strip, the other in the middle");
	finger_down(0, 211, 3, 700);
	report_send(12000);
	check(seen.shell_presses == 1, "joined-far: the shell takes the outer finger");

	/* A finger in the middle 40 ms later: nobody hears it. */
	finger_down(1, 212, 900, 650);
	report_send(12040);
	check(seen.downs == 0, "joined-far: the client hears no down");

	/* Both lift. */
	finger_up(1);
	finger_up(0);
	report_send(12200);
	check(seen.downs == 0, "joined-far: the client never hears the middle finger");
	check(seen.shell_releases == 1, "joined-far: the shell hears the release");
}

/*
 * BUG-270: the outer finger just under the system bar (900, 50) and
 * another (950, 400) swipe down: the top edge's swipe, the press at the
 * top of the outer finger's column (where Wiseview's band takes it).
 */
static void
test_top_pair(
	void)
{
	unsigned step;

	/* Both touch; the client hears both. */
	scenario_begin("top edge, two fingers");
	finger_down(0, 221, 950, 400);
	finger_down(1, 222, 900, 50);
	report_send(13000);
	check(seen.downs == 2, "top: the client hears both fingers touch");

	/* Both move down. */
	for (step = 1; step <= 6; step++) {
		finger_move(0, 950, 400 + 25 * (int32_t)step);
		finger_move(1, 900 + (int32_t)step, 50 + 25 * (int32_t)step);
		report_send(13000 + 16 * step);
	}

	/* The shell took them from the top edge. */
	check(seen.cancels == 1, "top: the client hears cancel");
	check(seen.shell_presses == 1, "top: the shell hears one press");
	check(seen.press_x == 900 && seen.press_y == 0, "top: the press is on the top edge at the outer finger's column");
	check(seen.motion_y == 150, "top: the pointer went the outer finger's way down");

	/* They lift. */
	finger_up(0);
	finger_up(1);
	report_send(13200);
	check(seen.shell_releases == 1, "top: the shell hears the release");
}

/* BUG-270: the outer finger in the system bar (the shell's band) and another soon after anywhere: the other goes nowhere. */
static void
test_top_in_bar(
	void)
{
	unsigned step;

	/* The finger in the bar: the shell's. */
	scenario_begin("top: outer finger in the bar, the other joins");
	finger_down(0, 231, 1000, 20);
	report_send(14000);
	check(seen.shell_presses == 1 && seen.press_y == 20, "top-bar: the shell takes the finger in the bar");

	/* Another finger 30 ms later: nobody hears it. */
	finger_down(1, 232, 1100, 300);
	report_send(14030);
	check(seen.downs == 0, "top-bar: the client hears no down");

	/* Both move down. */
	for (step = 1; step <= 5; step++) {
		finger_move(0, 1000, 20 + 30 * (int32_t)step);
		finger_move(1, 1100, 300 + 30 * (int32_t)step);
		report_send(14030 + 16 * step);
	}

	/* They lift: the shell followed the bar's finger, the client heard nothing. */
	finger_up(1);
	finger_up(0);
	report_send(14200);
	check(seen.motion_y == 170, "top-bar: the shell follows the finger in the bar");
	check(seen.shell_releases == 1 && seen.downs == 0, "top-bar: the shell hears the release, the client nothing");
}
