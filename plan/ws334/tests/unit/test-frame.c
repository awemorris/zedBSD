/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the window frame geometry (win32/frame.h): the dpi
 * scaling, the caption button rectangles, and the hit test step by step,
 * with expected values worked out by hand from the rules of the header,
 * then a fixed-seed sweep of random frames and points that checks only the
 * promises that hold for every input.
 */

#include "check.h"

#include "userland/desktop/libkeiland/win32/frame.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The number of random points the sweep tries. */
#define SWEEP_POINTS	100000U

/*
 * One point of a hit test and the answer worked out by hand for it.
 */
struct hit_case {
	int32_t x;
	int32_t y;
	unsigned expected;
};

/* The state of the sweep's random numbers: a 64 bit linear congruential generator, seeded the same on every run. */
static uint64_t sweep_state;

static uint32_t sweep_next(void);
static uint32_t sweep_range(uint32_t low, uint32_t high);
static struct keiui_win32_frame make_frame(uint32_t width, uint32_t height, unsigned dpi, int maximized, int fullscreen, uint32_t caption_height);
static void run_cases(const char *name, const struct keiui_win32_frame *frame, const struct hit_case *cases, size_t count);
static void check_rect(const struct keiui_win32_rect *rect, int32_t x, uint32_t width, uint32_t height);
static void test_scale(void);
static void test_buttons(void);
static void test_hit_plain_96(void);
static void test_hit_plain_144(void);
static void test_hit_other_dpi(void);
static void test_hit_maximized(void);
static void test_hit_fullscreen(void);
static void test_hit_no_caption(void);
static void test_hit_controls(void);
static void test_hit_small_windows(void);
static void test_hit_null_and_range(void);
static void test_sweep(void);

/*
 * Runs every group of checks and prints the summary.
 */
int
main(
	int argc,
	char **argv)
{
	int status;

	/* The runner passes the system the test is built for. */
	if (argc < 2) {
		printf("test-frame: FAIL (no system argument)\n");
		return 1;
	}
	if (strcmp(argv[1], "posix") != 0 && strcmp(argv[1], "win32") != 0) {
		printf("test-frame: FAIL (unknown system %s)\n", argv[1]);
		return 1;
	}

	/* Runs the groups of checks. */
	test_scale();
	test_buttons();
	test_hit_plain_96();
	test_hit_plain_144();
	test_hit_other_dpi();
	test_hit_maximized();
	test_hit_fullscreen();
	test_hit_no_caption();
	test_hit_controls();
	test_hit_small_windows();
	test_hit_null_and_range();
	test_sweep();

	/* Prints the summary line. */
	status = check_finish("frame");

	/* Reports the status the summary decided. */
	return status;
}

/*
 * Gives the next 32 random bits of the sweep.
 */
static uint32_t
sweep_next(void)
{
	/* Steps the generator with Knuth's constants. */
	sweep_state = sweep_state * 6364136223846793005ULL + 1442695040888963407ULL;

	/* Succeeded: the high bits are the better ones. */
	return (uint32_t)(sweep_state >> 32);
}

/*
 * Gives a random number from low to high, both included.
 */
static uint32_t
sweep_range(
	uint32_t low,
	uint32_t high)
{
	uint32_t span;
	uint32_t number;

	/* Draws a number and folds it into the range. */
	span = high - low + 1U;
	number = sweep_next();
	number = low + number % span;

	/* Succeeded: the number in the range. */
	return number;
}

/*
 * Fills in a frame without controls.
 */
static struct keiui_win32_frame
make_frame(
	uint32_t width,
	uint32_t height,
	unsigned dpi,
	int maximized,
	int fullscreen,
	uint32_t caption_height)
{
	struct keiui_win32_frame frame;

	/* Sets every field, so a test never reads one it did not choose. */
	memset(&frame, 0, sizeof(frame));
	frame.width = width;
	frame.height = height;
	frame.dpi = dpi;
	frame.maximized = maximized;
	frame.fullscreen = fullscreen;
	frame.caption_height = caption_height;
	frame.controls = NULL;
	frame.control_count = 0;

	/* Succeeded: the frame. */
	return frame;
}

/*
 * Checks the answer for each point of a table.
 */
static void
run_cases(
	const char *name,
	const struct keiui_win32_frame *frame,
	const struct hit_case *cases,
	size_t count)
{
	size_t index;
	unsigned actual;

	/* Asks the hit test for each point and compares it with the hand-worked answer. */
	for (index = 0; index < count; index++) {
		actual = keiui_win32_frame_hit(frame, cases[index].x, cases[index].y);
		if (actual != cases[index].expected) {
			printf("%s: point (%d, %d) is %u, expected %u\n", name, (int)cases[index].x, (int)cases[index].y, actual, cases[index].expected);
		}
		CHECK_EQ(actual, cases[index].expected);
	}
}

/*
 * Checks one button rectangle: its left edge, its size, and a top of 0.
 */
static void
check_rect(
	const struct keiui_win32_rect *rect,
	int32_t x,
	uint32_t width,
	uint32_t height)
{
	/* Compares each field with the hand-worked one. */
	CHECK_EQ(rect->x, x);
	CHECK_EQ(rect->y, 0);
	CHECK_EQ(rect->width, width);
	CHECK_EQ(rect->height, height);
}

/*
 * Checks the scaling of a length at 96 dpi.
 */
static void
test_scale(void)
{
	/* At 96 dpi, and at 0 which means 96, a length is unchanged. */
	CHECK_EQ(keiui_win32_scale(8U, 96U), 8);
	CHECK_EQ(keiui_win32_scale(46U, 96U), 46);
	CHECK_EQ(keiui_win32_scale(16U, 96U), 16);
	CHECK_EQ(keiui_win32_scale(8U, 0U), 8);
	CHECK_EQ(keiui_win32_scale(46U, 0U), 46);
	CHECK_EQ(keiui_win32_scale(16U, 0U), 16);
	CHECK_EQ(keiui_win32_scale(0U, 96U), 0);
	CHECK_EQ(keiui_win32_scale(0U, 4000U), 0);

	/* 125 %: 8 * 120 / 96 = 10, 46 * 120 / 96 = 57.5 rounds up to 58, 16 * 120 / 96 = 20. */
	CHECK_EQ(keiui_win32_scale(8U, 120U), 10);
	CHECK_EQ(keiui_win32_scale(46U, 120U), 58);
	CHECK_EQ(keiui_win32_scale(16U, 120U), 20);

	/* 150 %: 12, 69 and 24 exactly. */
	CHECK_EQ(keiui_win32_scale(8U, 144U), 12);
	CHECK_EQ(keiui_win32_scale(46U, 144U), 69);
	CHECK_EQ(keiui_win32_scale(16U, 144U), 24);

	/* 200 %: 16, 92 and 32 exactly. */
	CHECK_EQ(keiui_win32_scale(8U, 192U), 16);
	CHECK_EQ(keiui_win32_scale(46U, 192U), 92);
	CHECK_EQ(keiui_win32_scale(16U, 192U), 32);

	/* Rounding to the nearest, halves up: 1.5 and 4.5 and 0.5 go up, 1.25 and 0.49 go down. */
	CHECK_EQ(keiui_win32_scale(1U, 144U), 2);
	CHECK_EQ(keiui_win32_scale(3U, 144U), 5);
	CHECK_EQ(keiui_win32_scale(1U, 48U), 1);
	CHECK_EQ(keiui_win32_scale(1U, 47U), 0);
	CHECK_EQ(keiui_win32_scale(1U, 120U), 1);
	CHECK_EQ(keiui_win32_scale(1U, 96U), 1);
	CHECK_EQ(keiui_win32_scale(2U, 143U), 3);
	CHECK_EQ(keiui_win32_scale(2U, 142U), 3);
	CHECK_EQ(keiui_win32_scale(2U, 120U), 3);

	/* Large values: the product is taken in 64 bits and a result above 32 bits saturates. */
	CHECK_EQ(keiui_win32_scale(UINT32_MAX, 96U), UINT32_MAX);
	CHECK_EQ(keiui_win32_scale(UINT32_MAX, 0U), UINT32_MAX);
	CHECK_EQ(keiui_win32_scale(UINT32_MAX, 192U), UINT32_MAX);
	CHECK_EQ(keiui_win32_scale(UINT32_MAX, 97U), UINT32_MAX);
	CHECK_EQ(keiui_win32_scale(UINT32_MAX, UINT32_MAX), UINT32_MAX);
	CHECK_EQ(keiui_win32_scale(0x80000000U, 144U), 0xC0000000U);
	CHECK_EQ(keiui_win32_scale(0x80000000U, 192U), UINT32_MAX);
	CHECK_EQ(keiui_win32_scale(0x80000000U, 191U), 4272597675U);

	/* The largest dpi on a small value: 4294967295 / 96 = 44739242.66..., which rounds up. */
	CHECK_EQ(keiui_win32_scale(1U, UINT32_MAX), 44739243);
}

/*
 * Checks the caption button rectangles.
 */
static void
test_buttons(void)
{
	struct keiui_win32_frame frame;
	struct keiui_win32_rect buttons[3];
	struct keiui_win32_rect untouched[3];
	struct keiui_win32_rect controls[1];

	/* 800 wide, caption 32, 96 dpi: close 46 wide at 754, maximize at 708, minimize at 662, each 32 tall. */
	frame = make_frame(800U, 600U, 96U, 0, 0, 32U);
	keiui_win32_frame_buttons(&frame, buttons);
	check_rect(&buttons[KEIUI_WIN32_BUTTON_CLOSE_INDEX], 754, 46U, 32U);
	check_rect(&buttons[KEIUI_WIN32_BUTTON_MAXIMIZE_INDEX], 708, 46U, 32U);
	check_rect(&buttons[KEIUI_WIN32_BUTTON_MINIMIZE_INDEX], 662, 46U, 32U);

	/* The indexes are the order from the left. */
	CHECK_EQ(KEIUI_WIN32_BUTTON_MINIMIZE_INDEX, 0);
	CHECK_EQ(KEIUI_WIN32_BUTTON_MAXIMIZE_INDEX, 1);
	CHECK_EQ(KEIUI_WIN32_BUTTON_CLOSE_INDEX, 2);

	/* Maximized buttons are in the same place. */
	frame = make_frame(800U, 600U, 96U, 1, 0, 32U);
	keiui_win32_frame_buttons(&frame, buttons);
	check_rect(&buttons[2], 754, 46U, 32U);
	check_rect(&buttons[1], 708, 46U, 32U);
	check_rect(&buttons[0], 662, 46U, 32U);

	/* dpi 0 is 96. */
	frame = make_frame(800U, 600U, 0U, 0, 0, 32U);
	keiui_win32_frame_buttons(&frame, buttons);
	check_rect(&buttons[2], 754, 46U, 32U);

	/* 144 dpi: 69 wide, 48 tall; close at 800 - 69 = 731, maximize 662, minimize 593. */
	frame = make_frame(800U, 600U, 144U, 0, 0, 32U);
	keiui_win32_frame_buttons(&frame, buttons);
	check_rect(&buttons[2], 731, 69U, 48U);
	check_rect(&buttons[1], 662, 69U, 48U);
	check_rect(&buttons[0], 593, 69U, 48U);

	/* 120 dpi: 58 wide, 40 tall; close at 742, maximize 684, minimize 626. */
	frame = make_frame(800U, 600U, 120U, 0, 0, 32U);
	keiui_win32_frame_buttons(&frame, buttons);
	check_rect(&buttons[2], 742, 58U, 40U);
	check_rect(&buttons[1], 684, 58U, 40U);
	check_rect(&buttons[0], 626, 58U, 40U);

	/* A taller caption makes taller buttons and the same width: 40 at 96 dpi. */
	frame = make_frame(500U, 300U, 96U, 0, 0, 40U);
	keiui_win32_frame_buttons(&frame, buttons);
	check_rect(&buttons[2], 454, 46U, 40U);
	check_rect(&buttons[1], 408, 46U, 40U);
	check_rect(&buttons[0], 362, 46U, 40U);

	/* Full screen and a caption of 0 give three empty rectangles. */
	frame = make_frame(800U, 600U, 96U, 0, 1, 32U);
	memset(buttons, 0xAB, sizeof(buttons));
	keiui_win32_frame_buttons(&frame, buttons);
	check_rect(&buttons[0], 0, 0U, 0U);
	check_rect(&buttons[1], 0, 0U, 0U);
	check_rect(&buttons[2], 0, 0U, 0U);
	frame = make_frame(800U, 600U, 96U, 0, 0, 0U);
	memset(buttons, 0xAB, sizeof(buttons));
	keiui_win32_frame_buttons(&frame, buttons);
	check_rect(&buttons[0], 0, 0U, 0U);
	check_rect(&buttons[1], 0, 0U, 0U);
	check_rect(&buttons[2], 0, 0U, 0U);
	frame = make_frame(800U, 600U, 96U, 1, 1, 0U);
	memset(buttons, 0xAB, sizeof(buttons));
	keiui_win32_frame_buttons(&frame, buttons);
	check_rect(&buttons[0], 0, 0U, 0U);
	check_rect(&buttons[2], 0, 0U, 0U);

	/* A window narrower than the three buttons keeps the layout, and the left ones go negative: 100 - 138 = -38. */
	frame = make_frame(100U, 100U, 96U, 0, 0, 32U);
	keiui_win32_frame_buttons(&frame, buttons);
	check_rect(&buttons[2], 54, 46U, 32U);
	check_rect(&buttons[1], 8, 46U, 32U);
	check_rect(&buttons[0], -38, 46U, 32U);
	frame = make_frame(0U, 0U, 96U, 0, 0, 32U);
	keiui_win32_frame_buttons(&frame, buttons);
	check_rect(&buttons[2], -46, 46U, 32U);
	check_rect(&buttons[1], -92, 46U, 32U);
	check_rect(&buttons[0], -138, 46U, 32U);

	/* The controls do not move the buttons. */
	memset(controls, 0, sizeof(controls));
	controls[0].x = 700;
	controls[0].y = 0;
	controls[0].width = 90U;
	controls[0].height = 30U;
	frame = make_frame(800U, 600U, 96U, 0, 0, 32U);
	frame.controls = controls;
	frame.control_count = 1U;
	keiui_win32_frame_buttons(&frame, buttons);
	check_rect(&buttons[2], 754, 46U, 32U);

	/* A width at the end of the range: the position stays inside int32_t instead of wrapping. */
	frame = make_frame(UINT32_MAX, 100U, 96U, 0, 0, 32U);
	keiui_win32_frame_buttons(&frame, buttons);
	check_rect(&buttons[2], INT32_MAX, 46U, 32U);
	check_rect(&buttons[1], INT32_MAX, 46U, 32U);
	check_rect(&buttons[0], INT32_MAX, 46U, 32U);

	/* Right at the end of int32_t: a close position of exactly INT32_MAX stays, one more clamps to it. */
	frame = make_frame(0x7FFFFFFFU + 46U, 100U, 96U, 0, 0, 32U);
	keiui_win32_frame_buttons(&frame, buttons);
	CHECK_EQ(buttons[2].x, INT32_MAX);
	CHECK_EQ(buttons[1].x, INT32_MAX - 46);
	frame = make_frame(0x80000000U + 46U, 100U, 96U, 0, 0, 32U);
	keiui_win32_frame_buttons(&frame, buttons);
	CHECK_EQ(buttons[2].x, INT32_MAX);
	CHECK_EQ(buttons[1].x, INT32_MAX - 45);

	/* At 2000000000 dpi a button is 958333333 wide: the minimize position of a 727516351 wide window is exactly INT32_MIN, of one narrower by 1 it is below and clamps. */
	frame = make_frame(727516351U, 100U, 2000000000U, 0, 0, 32U);
	keiui_win32_frame_buttons(&frame, buttons);
	CHECK_EQ(buttons[0].width, 958333333U);
	CHECK_EQ(buttons[0].x, INT32_MIN);
	frame = make_frame(727516350U, 100U, 2000000000U, 0, 0, 32U);
	keiui_win32_frame_buttons(&frame, buttons);
	CHECK_EQ(buttons[0].x, INT32_MIN);
	CHECK_EQ(buttons[1].x, 727516350 - 2 * 958333333);

	/* A huge dpi makes each button 2058005162 wide; the two left ones do not fit int32_t and clamp to the lowest. */
	frame = make_frame(800U, 600U, UINT32_MAX, 0, 0, 32U);
	keiui_win32_frame_buttons(&frame, buttons);
	CHECK_EQ(buttons[2].x, 800 - 2058005162);
	CHECK_EQ(buttons[1].x, INT32_MIN);
	CHECK_EQ(buttons[0].x, INT32_MIN);
	CHECK_EQ(buttons[2].width, 2058005162U);

	/* NULL frame or NULL buttons: nothing is written. */
	memset(untouched, 0x5A, sizeof(untouched));
	memcpy(buttons, untouched, sizeof(buttons));
	keiui_win32_frame_buttons(NULL, buttons);
	CHECK(memcmp(buttons, untouched, sizeof(buttons)) == 0);
	frame = make_frame(800U, 600U, 96U, 0, 0, 32U);
	keiui_win32_frame_buttons(&frame, NULL);
	CHECK(frame.width == 800U);
	keiui_win32_frame_buttons(NULL, NULL);
}

/*
 * Checks the hit test of a normal window at 96 dpi, step by step.
 *
 * 800 by 600, caption 32: the band is 8, the corner reach 16, the top band
 * over a button 4; the buttons are minimize 662..707, maximize 708..753
 * and close 754..799, all from y 0 to 31.
 */
static void
test_hit_plain_96(void)
{
	static const struct hit_case cases[] = {
		/* Step 3, the corners: the diagonal resize at each corner pixel. */
		{ 0, 0, KEIUI_WIN32_HIT_TOP_LEFT },
		{ 799, 0, KEIUI_WIN32_HIT_TOP_RIGHT },
		{ 0, 599, KEIUI_WIN32_HIT_BOTTOM_LEFT },
		{ 799, 599, KEIUI_WIN32_HIT_BOTTOM_RIGHT },

		/* The band is 8 deep: the last pixel of each side is in, the next is out. */
		{ 7, 300, KEIUI_WIN32_HIT_LEFT },
		{ 8, 300, KEIUI_WIN32_HIT_CLIENT },
		{ 792, 300, KEIUI_WIN32_HIT_RIGHT },
		{ 791, 300, KEIUI_WIN32_HIT_CLIENT },
		{ 592, 592, KEIUI_WIN32_HIT_BOTTOM },
		{ 400, 592, KEIUI_WIN32_HIT_BOTTOM },
		{ 400, 591, KEIUI_WIN32_HIT_CLIENT },
		{ 400, 599, KEIUI_WIN32_HIT_BOTTOM },
		{ 400, 0, KEIUI_WIN32_HIT_TOP },
		{ 400, 7, KEIUI_WIN32_HIT_TOP },
		{ 400, 8, KEIUI_WIN32_HIT_CAPTION },
		{ 0, 300, KEIUI_WIN32_HIT_LEFT },
		{ 799, 300, KEIUI_WIN32_HIT_RIGHT },

		/* The corner reach is 16 along an edge: the top edge. */
		{ 15, 0, KEIUI_WIN32_HIT_TOP_LEFT },
		{ 16, 0, KEIUI_WIN32_HIT_TOP },
		{ 15, 7, KEIUI_WIN32_HIT_TOP_LEFT },
		{ 16, 7, KEIUI_WIN32_HIT_TOP },
		{ 783, 0, KEIUI_WIN32_HIT_TOP },
		{ 784, 0, KEIUI_WIN32_HIT_TOP_RIGHT },

		/* The corner reach along the left edge. */
		{ 3, 15, KEIUI_WIN32_HIT_TOP_LEFT },
		{ 3, 16, KEIUI_WIN32_HIT_LEFT },
		{ 3, 583, KEIUI_WIN32_HIT_LEFT },
		{ 3, 584, KEIUI_WIN32_HIT_BOTTOM_LEFT },

		/* The corner reach along the right edge. */
		{ 796, 15, KEIUI_WIN32_HIT_TOP_RIGHT },
		{ 796, 16, KEIUI_WIN32_HIT_RIGHT },
		{ 796, 583, KEIUI_WIN32_HIT_RIGHT },
		{ 796, 584, KEIUI_WIN32_HIT_BOTTOM_RIGHT },

		/* The corner reach along the bottom edge. */
		{ 15, 596, KEIUI_WIN32_HIT_BOTTOM_LEFT },
		{ 16, 596, KEIUI_WIN32_HIT_BOTTOM },
		{ 783, 596, KEIUI_WIN32_HIT_BOTTOM },
		{ 784, 596, KEIUI_WIN32_HIT_BOTTOM_RIGHT },

		/* A point in both bands near the corner but beyond the reach of one axis is still the corner. */
		{ 7, 7, KEIUI_WIN32_HIT_TOP_LEFT },
		{ 792, 592, KEIUI_WIN32_HIT_BOTTOM_RIGHT },

		/* The top band over a button is 4 deep: the first 4 rows are TOP, then the button. */
		{ 760, 3, KEIUI_WIN32_HIT_TOP },
		{ 760, 4, KEIUI_WIN32_HIT_CLOSE },
		{ 720, 3, KEIUI_WIN32_HIT_TOP },
		{ 720, 4, KEIUI_WIN32_HIT_MAXIMIZE },
		{ 680, 3, KEIUI_WIN32_HIT_TOP },
		{ 680, 4, KEIUI_WIN32_HIT_MINIMIZE },

		/* Just left of the leftmost button the band is the full 8 deep. */
		{ 661, 7, KEIUI_WIN32_HIT_TOP },
		{ 661, 8, KEIUI_WIN32_HIT_CAPTION },
		{ 662, 7, KEIUI_WIN32_HIT_MINIMIZE },
		{ 662, 3, KEIUI_WIN32_HIT_TOP },

		/* Step 4, the buttons: their first and last pixels. */
		{ 754, 4, KEIUI_WIN32_HIT_CLOSE },
		{ 753, 4, KEIUI_WIN32_HIT_MAXIMIZE },
		{ 708, 4, KEIUI_WIN32_HIT_MAXIMIZE },
		{ 707, 4, KEIUI_WIN32_HIT_MINIMIZE },
		{ 662, 4, KEIUI_WIN32_HIT_MINIMIZE },
		{ 661, 20, KEIUI_WIN32_HIT_CAPTION },
		{ 754, 31, KEIUI_WIN32_HIT_CLOSE },
		{ 708, 31, KEIUI_WIN32_HIT_MAXIMIZE },
		{ 662, 31, KEIUI_WIN32_HIT_MINIMIZE },
		{ 790, 20, KEIUI_WIN32_HIT_CLOSE },

		/* A button's row below the caption is not the button: the 1 px under it is the client. */
		{ 754, 32, KEIUI_WIN32_HIT_CLIENT },
		{ 708, 32, KEIUI_WIN32_HIT_CLIENT },
		{ 662, 32, KEIUI_WIN32_HIT_CLIENT },

		/* The right band wins over the close button's last columns, and the corner reach over the band. */
		{ 792, 20, KEIUI_WIN32_HIT_RIGHT },
		{ 791, 20, KEIUI_WIN32_HIT_CLOSE },
		{ 792, 15, KEIUI_WIN32_HIT_TOP_RIGHT },

		/* Step 6, the caption: the whole bar apart from the buttons, bands and controls. */
		{ 400, 8, KEIUI_WIN32_HIT_CAPTION },
		{ 400, 31, KEIUI_WIN32_HIT_CAPTION },
		{ 9, 8, KEIUI_WIN32_HIT_CAPTION },
		{ 8, 8, KEIUI_WIN32_HIT_CAPTION },
		{ 8, 31, KEIUI_WIN32_HIT_CAPTION },

		/* Step 7, the client: from the caption's height down, inside the bands. */
		{ 400, 32, KEIUI_WIN32_HIT_CLIENT },
		{ 400, 300, KEIUI_WIN32_HIT_CLIENT },
		{ 8, 32, KEIUI_WIN32_HIT_CLIENT },
		{ 791, 591, KEIUI_WIN32_HIT_CLIENT }
	};
	struct keiui_win32_frame frame;

	/* Runs the table for a plain window. */
	frame = make_frame(800U, 600U, 96U, 0, 0, 32U);
	run_cases("plain96", &frame, cases, sizeof(cases) / sizeof(cases[0]));

	/* dpi 0 is taken as 96: the same table holds. */
	frame = make_frame(800U, 600U, 0U, 0, 0, 32U);
	run_cases("plain-dpi0", &frame, cases, sizeof(cases) / sizeof(cases[0]));
}

/*
 * Checks the hit test at 144 dpi, where every length is 1.5 times.
 *
 * Band 12, top band over a button 6, corner reach 24, caption 48, buttons
 * 69 wide: minimize 593..661, maximize 662..730, close 731..799.
 */
static void
test_hit_plain_144(void)
{
	static const struct hit_case cases[] = {
		{ 0, 0, KEIUI_WIN32_HIT_TOP_LEFT },
		{ 799, 599, KEIUI_WIN32_HIT_BOTTOM_RIGHT },
		{ 11, 300, KEIUI_WIN32_HIT_LEFT },
		{ 12, 300, KEIUI_WIN32_HIT_CLIENT },
		{ 788, 300, KEIUI_WIN32_HIT_RIGHT },
		{ 787, 300, KEIUI_WIN32_HIT_CLIENT },
		{ 400, 11, KEIUI_WIN32_HIT_TOP },
		{ 400, 12, KEIUI_WIN32_HIT_CAPTION },
		{ 400, 47, KEIUI_WIN32_HIT_CAPTION },
		{ 400, 48, KEIUI_WIN32_HIT_CLIENT },
		{ 400, 588, KEIUI_WIN32_HIT_BOTTOM },
		{ 400, 587, KEIUI_WIN32_HIT_CLIENT },

		/* The corner reach is 24. */
		{ 23, 0, KEIUI_WIN32_HIT_TOP_LEFT },
		{ 24, 0, KEIUI_WIN32_HIT_TOP },
		{ 775, 0, KEIUI_WIN32_HIT_TOP },
		{ 776, 0, KEIUI_WIN32_HIT_TOP_RIGHT },
		{ 5, 23, KEIUI_WIN32_HIT_TOP_LEFT },
		{ 5, 24, KEIUI_WIN32_HIT_LEFT },
		{ 5, 575, KEIUI_WIN32_HIT_LEFT },
		{ 5, 576, KEIUI_WIN32_HIT_BOTTOM_LEFT },
		{ 795, 575, KEIUI_WIN32_HIT_RIGHT },
		{ 795, 576, KEIUI_WIN32_HIT_BOTTOM_RIGHT },
		{ 23, 595, KEIUI_WIN32_HIT_BOTTOM_LEFT },
		{ 24, 595, KEIUI_WIN32_HIT_BOTTOM },

		/* The top band over a button is 6 deep. */
		{ 740, 5, KEIUI_WIN32_HIT_TOP },
		{ 740, 6, KEIUI_WIN32_HIT_CLOSE },
		{ 700, 5, KEIUI_WIN32_HIT_TOP },
		{ 700, 6, KEIUI_WIN32_HIT_MAXIMIZE },
		{ 593, 5, KEIUI_WIN32_HIT_TOP },
		{ 593, 6, KEIUI_WIN32_HIT_MINIMIZE },
		{ 592, 11, KEIUI_WIN32_HIT_TOP },
		{ 592, 12, KEIUI_WIN32_HIT_CAPTION },

		/* The buttons' edges. */
		{ 731, 6, KEIUI_WIN32_HIT_CLOSE },
		{ 730, 6, KEIUI_WIN32_HIT_MAXIMIZE },
		{ 662, 6, KEIUI_WIN32_HIT_MAXIMIZE },
		{ 661, 6, KEIUI_WIN32_HIT_MINIMIZE },
		{ 593, 47, KEIUI_WIN32_HIT_MINIMIZE },
		{ 593, 48, KEIUI_WIN32_HIT_CLIENT },
		{ 592, 47, KEIUI_WIN32_HIT_CAPTION },
		{ 787, 20, KEIUI_WIN32_HIT_CLOSE },
		{ 788, 24, KEIUI_WIN32_HIT_RIGHT },
		{ 788, 23, KEIUI_WIN32_HIT_TOP_RIGHT }
	};
	struct keiui_win32_frame frame;

	/* Runs the table. */
	frame = make_frame(800U, 600U, 144U, 0, 0, 32U);
	run_cases("plain144", &frame, cases, sizeof(cases) / sizeof(cases[0]));
}

/*
 * Checks the hit test at 120 and 192 dpi.
 */
static void
test_hit_other_dpi(void)
{
	/*
	 * 192 dpi: band 16, top band over a button 8, corner reach 32, caption
	 * 64, buttons 92 wide: minimize 524..615, maximize 616..707, close
	 * 708..799.
	 */
	static const struct hit_case cases192[] = {
		{ 15, 300, KEIUI_WIN32_HIT_LEFT },
		{ 16, 300, KEIUI_WIN32_HIT_CLIENT },
		{ 650, 7, KEIUI_WIN32_HIT_TOP },
		{ 650, 8, KEIUI_WIN32_HIT_MAXIMIZE },
		{ 523, 15, KEIUI_WIN32_HIT_TOP },
		{ 523, 16, KEIUI_WIN32_HIT_CAPTION },
		{ 31, 0, KEIUI_WIN32_HIT_TOP_LEFT },
		{ 32, 0, KEIUI_WIN32_HIT_TOP },
		{ 524, 63, KEIUI_WIN32_HIT_MINIMIZE },
		{ 524, 64, KEIUI_WIN32_HIT_CLIENT },
		{ 523, 63, KEIUI_WIN32_HIT_CAPTION },
		{ 400, 63, KEIUI_WIN32_HIT_CAPTION },
		{ 400, 64, KEIUI_WIN32_HIT_CLIENT }
	};

	/*
	 * 120 dpi: band 10, top band over a button 5, corner reach 20, caption
	 * 40, buttons 58 wide: minimize 626..683, maximize 684..741, close
	 * 742..799.
	 */
	static const struct hit_case cases120[] = {
		{ 9, 300, KEIUI_WIN32_HIT_LEFT },
		{ 10, 300, KEIUI_WIN32_HIT_CLIENT },
		{ 700, 4, KEIUI_WIN32_HIT_TOP },
		{ 700, 5, KEIUI_WIN32_HIT_MAXIMIZE },
		{ 625, 9, KEIUI_WIN32_HIT_TOP },
		{ 625, 10, KEIUI_WIN32_HIT_CAPTION },
		{ 19, 0, KEIUI_WIN32_HIT_TOP_LEFT },
		{ 20, 0, KEIUI_WIN32_HIT_TOP },
		{ 742, 39, KEIUI_WIN32_HIT_CLOSE },
		{ 742, 40, KEIUI_WIN32_HIT_CLIENT }
	};
	struct keiui_win32_frame frame;

	/* Runs both tables. */
	frame = make_frame(800U, 600U, 192U, 0, 0, 32U);
	run_cases("plain192", &frame, cases192, sizeof(cases192) / sizeof(cases192[0]));
	frame = make_frame(800U, 600U, 120U, 0, 0, 32U);
	run_cases("plain120", &frame, cases120, sizeof(cases120) / sizeof(cases120[0]));
}

/*
 * Checks a maximized window: no resize bands, the buttons reach the top.
 */
static void
test_hit_maximized(void)
{
	static const struct hit_case cases[] = {
		/* Where the bands would be, the top row is the caption and the sides are client. */
		{ 0, 0, KEIUI_WIN32_HIT_CAPTION },
		{ 400, 0, KEIUI_WIN32_HIT_CAPTION },
		{ 3, 300, KEIUI_WIN32_HIT_CLIENT },
		{ 0, 599, KEIUI_WIN32_HIT_CLIENT },
		{ 799, 599, KEIUI_WIN32_HIT_CLIENT },
		{ 400, 599, KEIUI_WIN32_HIT_CLIENT },
		{ 799, 300, KEIUI_WIN32_HIT_CLIENT },
		{ 0, 32, KEIUI_WIN32_HIT_CLIENT },
		{ 0, 31, KEIUI_WIN32_HIT_CAPTION },

		/* The buttons reach the very top row. */
		{ 799, 0, KEIUI_WIN32_HIT_CLOSE },
		{ 754, 0, KEIUI_WIN32_HIT_CLOSE },
		{ 753, 0, KEIUI_WIN32_HIT_MAXIMIZE },
		{ 708, 0, KEIUI_WIN32_HIT_MAXIMIZE },
		{ 707, 0, KEIUI_WIN32_HIT_MINIMIZE },
		{ 662, 0, KEIUI_WIN32_HIT_MINIMIZE },
		{ 661, 0, KEIUI_WIN32_HIT_CAPTION },
		{ 799, 31, KEIUI_WIN32_HIT_CLOSE },
		{ 799, 32, KEIUI_WIN32_HIT_CLIENT }
	};
	static const struct hit_case cases144[] = {
		{ 0, 0, KEIUI_WIN32_HIT_CAPTION },
		{ 731, 0, KEIUI_WIN32_HIT_CLOSE },
		{ 730, 0, KEIUI_WIN32_HIT_MAXIMIZE },
		{ 593, 0, KEIUI_WIN32_HIT_MINIMIZE },
		{ 592, 0, KEIUI_WIN32_HIT_CAPTION },
		{ 400, 47, KEIUI_WIN32_HIT_CAPTION },
		{ 400, 48, KEIUI_WIN32_HIT_CLIENT },
		{ 5, 300, KEIUI_WIN32_HIT_CLIENT }
	};
	struct keiui_win32_frame frame;

	/* Runs the tables at 96 and 144 dpi. */
	frame = make_frame(800U, 600U, 96U, 1, 0, 32U);
	run_cases("maximized96", &frame, cases, sizeof(cases) / sizeof(cases[0]));
	frame = make_frame(800U, 600U, 144U, 1, 0, 32U);
	run_cases("maximized144", &frame, cases144, sizeof(cases144) / sizeof(cases144[0]));
}

/*
 * Checks full screen: everything inside is client, whatever else is set.
 */
static void
test_hit_fullscreen(void)
{
	static const struct hit_case cases[] = {
		{ 0, 0, KEIUI_WIN32_HIT_CLIENT },
		{ 799, 0, KEIUI_WIN32_HIT_CLIENT },
		{ 0, 599, KEIUI_WIN32_HIT_CLIENT },
		{ 799, 599, KEIUI_WIN32_HIT_CLIENT },
		{ 400, 10, KEIUI_WIN32_HIT_CLIENT },
		{ 760, 10, KEIUI_WIN32_HIT_CLIENT },
		{ 3, 300, KEIUI_WIN32_HIT_CLIENT },

		/* Outside is still nowhere. */
		{ -1, 10, KEIUI_WIN32_HIT_NOWHERE },
		{ 10, -1, KEIUI_WIN32_HIT_NOWHERE },
		{ 800, 10, KEIUI_WIN32_HIT_NOWHERE },
		{ 10, 600, KEIUI_WIN32_HIT_NOWHERE }
	};
	struct keiui_win32_frame frame;

	/* Full screen, both not maximized and maximized, with a caption set. */
	frame = make_frame(800U, 600U, 96U, 0, 1, 32U);
	run_cases("fullscreen", &frame, cases, sizeof(cases) / sizeof(cases[0]));
	frame = make_frame(800U, 600U, 144U, 1, 1, 32U);
	run_cases("fullscreen-maximized", &frame, cases, sizeof(cases) / sizeof(cases[0]));
}

/*
 * Checks a window without a title bar: no buttons, the top band is full depth.
 */
static void
test_hit_no_caption(void)
{
	static const struct hit_case cases[] = {
		/* The top band is 8 deep everywhere, since there is no button to thin it. */
		{ 400, 7, KEIUI_WIN32_HIT_TOP },
		{ 400, 8, KEIUI_WIN32_HIT_CLIENT },
		{ 760, 4, KEIUI_WIN32_HIT_TOP },
		{ 760, 7, KEIUI_WIN32_HIT_TOP },
		{ 760, 8, KEIUI_WIN32_HIT_CLIENT },
		{ 710, 10, KEIUI_WIN32_HIT_CLIENT },
		{ 0, 0, KEIUI_WIN32_HIT_TOP_LEFT },
		{ 799, 0, KEIUI_WIN32_HIT_TOP_RIGHT },
		{ 5, 300, KEIUI_WIN32_HIT_LEFT }
	};
	static const struct hit_case maximized[] = {
		/* Maximized with no title bar: all client. */
		{ 0, 0, KEIUI_WIN32_HIT_CLIENT },
		{ 760, 4, KEIUI_WIN32_HIT_CLIENT },
		{ 799, 0, KEIUI_WIN32_HIT_CLIENT },
		{ 5, 300, KEIUI_WIN32_HIT_CLIENT }
	};
	struct keiui_win32_frame frame;

	/* Runs the tables. */
	frame = make_frame(800U, 600U, 96U, 0, 0, 0U);
	run_cases("no-caption", &frame, cases, sizeof(cases) / sizeof(cases[0]));
	frame = make_frame(800U, 600U, 96U, 1, 0, 0U);
	run_cases("no-caption-maximized", &frame, maximized, sizeof(maximized) / sizeof(maximized[0]));
}

/*
 * Checks the application's controls (step 5).
 */
static void
test_hit_controls(void)
{
	static const struct hit_case cases[] = {
		/* A control on the title bar: inside is the client, the edges are the first pixel in and out. */
		{ 100, 8, KEIUI_WIN32_HIT_CLIENT },
		{ 399, 27, KEIUI_WIN32_HIT_CLIENT },
		{ 250, 15, KEIUI_WIN32_HIT_CLIENT },
		{ 400, 10, KEIUI_WIN32_HIT_CAPTION },
		{ 99, 10, KEIUI_WIN32_HIT_CAPTION },
		{ 100, 28, KEIUI_WIN32_HIT_CAPTION },

		/* A resize band comes before the controls. */
		{ 399, 7, KEIUI_WIN32_HIT_TOP },
		{ 100, 7, KEIUI_WIN32_HIT_TOP },

		/* A control that reaches under the caption buttons: the buttons come first, the rest of it is client. */
		{ 700, 10, KEIUI_WIN32_HIT_MINIMIZE },
		{ 710, 10, KEIUI_WIN32_HIT_MAXIMIZE },
		{ 780, 10, KEIUI_WIN32_HIT_CLOSE },
		{ 661, 10, KEIUI_WIN32_HIT_CLIENT },
		{ 600, 10, KEIUI_WIN32_HIT_CLIENT },
		{ 599, 10, KEIUI_WIN32_HIT_CAPTION },

		/* A control outside the caption is client, and one that reaches a band does not hide the band. */
		{ 500, 100, KEIUI_WIN32_HIT_CLIENT },
		{ 3, 110, KEIUI_WIN32_HIT_LEFT },
		{ 9, 110, KEIUI_WIN32_HIT_CLIENT },

		/* Later controls of the list are looked at too. */
		{ 450, 15, KEIUI_WIN32_HIT_CLIENT },
		{ 459, 29, KEIUI_WIN32_HIT_CLIENT },
		{ 449, 15, KEIUI_WIN32_HIT_CAPTION },
		{ 460, 15, KEIUI_WIN32_HIT_CAPTION },

		/* The last control of the list is empty (width 0) and contains nothing. */
		{ 500, 15, KEIUI_WIN32_HIT_CAPTION },
		{ 499, 15, KEIUI_WIN32_HIT_CAPTION }
	};
	struct keiui_win32_rect controls[5];
	struct keiui_win32_frame frame;

	/* Builds the controls: one in the title bar, a narrow one, one below the title bar, one under the buttons, and an empty one. */
	memset(controls, 0, sizeof(controls));
	controls[0].x = 100;
	controls[0].y = 8;
	controls[0].width = 300U;
	controls[0].height = 20U;
	controls[1].x = 450;
	controls[1].y = 0;
	controls[1].width = 10U;
	controls[1].height = 30U;
	controls[2].x = 0;
	controls[2].y = 100;
	controls[2].width = 600U;
	controls[2].height = 50U;
	controls[3].x = 600;
	controls[3].y = 10;
	controls[3].width = 190U;
	controls[3].height = 20U;
	controls[4].x = 500;
	controls[4].y = 10;
	controls[4].width = 0U;
	controls[4].height = 10U;

	/* Runs the table with all five controls. */
	frame = make_frame(800U, 600U, 96U, 0, 0, 32U);
	frame.controls = controls;
	frame.control_count = 5U;
	run_cases("controls", &frame, cases, sizeof(cases) / sizeof(cases[0]));

	/* The count limits the list: with 3 the control under the buttons is not read. */
	frame.control_count = 3U;
	CHECK_EQ(keiui_win32_frame_hit(&frame, 600, 10), KEIUI_WIN32_HIT_CAPTION);
	CHECK_EQ(keiui_win32_frame_hit(&frame, 450, 15), KEIUI_WIN32_HIT_CLIENT);

	/* With a count of 0 the controls are not read. */
	frame.control_count = 0U;
	CHECK_EQ(keiui_win32_frame_hit(&frame, 250, 15), KEIUI_WIN32_HIT_CAPTION);

	/* A NULL list with a count is treated as no controls. */
	frame.controls = NULL;
	frame.control_count = 5U;
	CHECK_EQ(keiui_win32_frame_hit(&frame, 250, 15), KEIUI_WIN32_HIT_CAPTION);

	/* Controls with edges at the end of the range: the comparison does not wrap. */
	controls[0].x = INT32_MAX;
	controls[0].y = 0;
	controls[0].width = UINT32_MAX;
	controls[0].height = UINT32_MAX;
	controls[1].x = INT32_MIN;
	controls[1].y = INT32_MIN;
	controls[1].width = 5U;
	controls[1].height = 5U;
	frame.controls = controls;
	frame.control_count = 2U;
	CHECK_EQ(keiui_win32_frame_hit(&frame, 250, 15), KEIUI_WIN32_HIT_CAPTION);
	CHECK_EQ(keiui_win32_frame_hit(&frame, 400, 40), KEIUI_WIN32_HIT_CLIENT);

	/* A control with the whole range as its width and a left edge before the point does contain it. */
	controls[0].x = INT32_MIN;
	controls[0].y = INT32_MIN;
	controls[0].width = UINT32_MAX;
	controls[0].height = UINT32_MAX;
	CHECK_EQ(keiui_win32_frame_hit(&frame, 250, 15), KEIUI_WIN32_HIT_CLIENT);
	CHECK_EQ(keiui_win32_frame_hit(&frame, 100, 15), KEIUI_WIN32_HIT_CLIENT);
}

/*
 * Checks windows smaller than the bands and the buttons.
 */
static void
test_hit_small_windows(void)
{
	static const struct hit_case narrow[] = {
		/* Width 100: minimize -38..7, maximize 8..53, close 54..99, maximized so the bands do not interfere. */
		{ 0, 0, KEIUI_WIN32_HIT_MINIMIZE },
		{ 7, 0, KEIUI_WIN32_HIT_MINIMIZE },
		{ 8, 0, KEIUI_WIN32_HIT_MAXIMIZE },
		{ 53, 0, KEIUI_WIN32_HIT_MAXIMIZE },
		{ 54, 0, KEIUI_WIN32_HIT_CLOSE },
		{ 99, 31, KEIUI_WIN32_HIT_CLOSE },
		{ 30, 40, KEIUI_WIN32_HIT_CLIENT },
		{ 100, 0, KEIUI_WIN32_HIT_NOWHERE }
	};

	/*
	 * 20 by 20, no title bar, not maximized: band 8, reach 16.  Where the
	 * bands of opposite sides overlap, the nearer edge decides, and the
	 * low edge on a tie.
	 */
	static const struct hit_case tiny[] = {
		{ 5, 5, KEIUI_WIN32_HIT_TOP_LEFT },
		{ 14, 5, KEIUI_WIN32_HIT_TOP_RIGHT },
		{ 5, 14, KEIUI_WIN32_HIT_BOTTOM_LEFT },
		{ 14, 14, KEIUI_WIN32_HIT_BOTTOM_RIGHT },
		{ 9, 9, KEIUI_WIN32_HIT_CLIENT },
		{ 9, 10, KEIUI_WIN32_HIT_CLIENT },
		{ 10, 5, KEIUI_WIN32_HIT_TOP_RIGHT },
		{ 5, 10, KEIUI_WIN32_HIT_BOTTOM_LEFT },
		{ 9, 5, KEIUI_WIN32_HIT_TOP_LEFT },
		{ 5, 9, KEIUI_WIN32_HIT_TOP_LEFT },
		{ 10, 10, KEIUI_WIN32_HIT_CLIENT },
		{ 12, 12, KEIUI_WIN32_HIT_BOTTOM_RIGHT }
	};

	/* A window of 1 by 1 and an empty one. */
	static const struct hit_case one[] = {
		{ 0, 0, KEIUI_WIN32_HIT_TOP_LEFT },
		{ 1, 0, KEIUI_WIN32_HIT_NOWHERE },
		{ 0, 1, KEIUI_WIN32_HIT_NOWHERE }
	};
	static const struct hit_case empty[] = {
		{ 0, 0, KEIUI_WIN32_HIT_NOWHERE }
	};
	struct keiui_win32_frame frame;

	/* Runs the tables. */
	frame = make_frame(100U, 100U, 96U, 1, 0, 32U);
	run_cases("narrow", &frame, narrow, sizeof(narrow) / sizeof(narrow[0]));
	frame = make_frame(20U, 20U, 96U, 0, 0, 0U);
	run_cases("tiny", &frame, tiny, sizeof(tiny) / sizeof(tiny[0]));
	frame = make_frame(1U, 1U, 96U, 0, 0, 0U);
	run_cases("one", &frame, one, sizeof(one) / sizeof(one[0]));
	frame = make_frame(0U, 0U, 96U, 0, 0, 32U);
	run_cases("empty", &frame, empty, sizeof(empty) / sizeof(empty[0]));
}

/*
 * Checks step 1: no frame, and points outside the window.
 */
static void
test_hit_null_and_range(void)
{
	static const struct hit_case cases[] = {
		{ -1, 10, KEIUI_WIN32_HIT_NOWHERE },
		{ 10, -1, KEIUI_WIN32_HIT_NOWHERE },
		{ -1, -1, KEIUI_WIN32_HIT_NOWHERE },
		{ 800, 10, KEIUI_WIN32_HIT_NOWHERE },
		{ 10, 600, KEIUI_WIN32_HIT_NOWHERE },
		{ 800, 600, KEIUI_WIN32_HIT_NOWHERE },
		{ 799, 600, KEIUI_WIN32_HIT_NOWHERE },
		{ 800, 599, KEIUI_WIN32_HIT_NOWHERE },
		{ INT32_MIN, 10, KEIUI_WIN32_HIT_NOWHERE },
		{ 10, INT32_MIN, KEIUI_WIN32_HIT_NOWHERE },
		{ INT32_MAX, 10, KEIUI_WIN32_HIT_NOWHERE },
		{ 10, INT32_MAX, KEIUI_WIN32_HIT_NOWHERE },
		{ INT32_MAX, INT32_MAX, KEIUI_WIN32_HIT_NOWHERE }
	};
	struct keiui_win32_frame frame;

	/* A NULL frame answers NOWHERE. */
	CHECK_EQ(keiui_win32_frame_hit(NULL, 10, 10), KEIUI_WIN32_HIT_NOWHERE);
	CHECK_EQ(keiui_win32_frame_hit(NULL, 0, 0), KEIUI_WIN32_HIT_NOWHERE);
	CHECK_EQ(keiui_win32_frame_hit(NULL, -1, -1), KEIUI_WIN32_HIT_NOWHERE);

	/* Out of range points answer NOWHERE in every state of the window. */
	frame = make_frame(800U, 600U, 96U, 0, 0, 32U);
	run_cases("range-plain", &frame, cases, sizeof(cases) / sizeof(cases[0]));
	frame = make_frame(800U, 600U, 96U, 1, 0, 32U);
	run_cases("range-maximized", &frame, cases, sizeof(cases) / sizeof(cases[0]));
	frame = make_frame(800U, 600U, 96U, 0, 1, 32U);
	run_cases("range-fullscreen", &frame, cases, sizeof(cases) / sizeof(cases[0]));

	/* The largest windows hold the largest points: INT32_MAX is inside a window of INT32_MAX + 1. */
	frame = make_frame(0x80000000U, 0x80000000U, 96U, 1, 0, 32U);
	CHECK_EQ(keiui_win32_frame_hit(&frame, INT32_MAX, INT32_MAX), KEIUI_WIN32_HIT_CLIENT);
	CHECK_EQ(keiui_win32_frame_hit(&frame, INT32_MAX, 0), KEIUI_WIN32_HIT_CLOSE);
	frame = make_frame(UINT32_MAX, UINT32_MAX, 96U, 0, 0, 32U);
	CHECK_EQ(keiui_win32_frame_hit(&frame, INT32_MAX, INT32_MAX), KEIUI_WIN32_HIT_CLIENT);
	CHECK_EQ(keiui_win32_frame_hit(&frame, 0, 0), KEIUI_WIN32_HIT_TOP_LEFT);
}

/*
 * Sweeps random frames and points for the promises that hold for any input.
 */
static void
test_sweep(void)
{
	struct keiui_win32_rect controls[8];
	struct keiui_win32_frame frame;
	struct keiui_win32_rect buttons[3];
	unsigned point;
	unsigned control;
	unsigned hit;
	unsigned frame_choice;
	int32_t x;
	int32_t y;
	int outside;
	int deep_inside;
	int64_t close_x;
	int64_t band;

	/* Seeds the generator the same on every run. */
	sweep_state = 0x5EEDF00DCAFEULL;

	/* Tries 100000 random points, changing the frame every 16 points. */
	memset(&frame, 0, sizeof(frame));
	for (point = 0; point < SWEEP_POINTS; point++) {
		if (point % 16U == 0U) {
			/* Builds a random frame: size, dpi, mode, caption and up to 8 controls. */
			frame = make_frame(sweep_range(0U, 5000U), sweep_range(0U, 5000U), sweep_range(72U, 480U), 0, 0, sweep_range(0U, 80U));
			frame_choice = sweep_range(0U, 9U);
			if (frame_choice == 0U)
				frame.fullscreen = 1;
			if (frame_choice == 1U || frame_choice == 2U)
				frame.maximized = 1;
			frame.control_count = sweep_range(0U, 8U);
			for (control = 0; control < 8U; control++) {
				controls[control].x = (int32_t)sweep_range(0U, 5200U) - 100;
				controls[control].y = (int32_t)sweep_range(0U, 200U) - 50;
				controls[control].width = sweep_range(0U, 600U);
				controls[control].height = sweep_range(0U, 120U);
			}
			frame.controls = controls;
		}

		/* Draws a point partly inside and partly just outside the window. */
		x = (int32_t)sweep_range(0U, frame.width + 20U) - 10;
		y = (int32_t)sweep_range(0U, frame.height + 20U) - 10;
		hit = keiui_win32_frame_hit(&frame, x, y);
		outside = 0;
		if (x < 0 || (int64_t)x >= (int64_t)frame.width)
			outside = 1;
		if (y < 0 || (int64_t)y >= (int64_t)frame.height)
			outside = 1;

		/* The answer is always one of the 15 values of the header. */
		CHECK(hit == KEIUI_WIN32_HIT_NOWHERE ||
		    hit == KEIUI_WIN32_HIT_CLIENT ||
		    hit == KEIUI_WIN32_HIT_CAPTION ||
		    hit == KEIUI_WIN32_HIT_MINIMIZE ||
		    hit == KEIUI_WIN32_HIT_MAXIMIZE ||
		    hit == KEIUI_WIN32_HIT_LEFT ||
		    hit == KEIUI_WIN32_HIT_RIGHT ||
		    hit == KEIUI_WIN32_HIT_TOP ||
		    hit == KEIUI_WIN32_HIT_TOP_LEFT ||
		    hit == KEIUI_WIN32_HIT_TOP_RIGHT ||
		    hit == KEIUI_WIN32_HIT_BOTTOM ||
		    hit == KEIUI_WIN32_HIT_BOTTOM_LEFT ||
		    hit == KEIUI_WIN32_HIT_BOTTOM_RIGHT ||
		    hit == KEIUI_WIN32_HIT_CLOSE);

		/* Outside the window is NOWHERE, and inside it never is. */
		if (outside != 0)
			CHECK_EQ(hit, KEIUI_WIN32_HIT_NOWHERE);
		if (outside == 0)
			CHECK(hit != KEIUI_WIN32_HIT_NOWHERE);

		/* Full screen is client everywhere inside. */
		if (frame.fullscreen != 0 && outside == 0)
			CHECK_EQ(hit, KEIUI_WIN32_HIT_CLIENT);

		/* A maximized window has no resize part. */
		if (frame.maximized != 0) {
			CHECK(hit != KEIUI_WIN32_HIT_LEFT);
			CHECK(hit != KEIUI_WIN32_HIT_RIGHT);
			CHECK(hit != KEIUI_WIN32_HIT_TOP);
			CHECK(hit != KEIUI_WIN32_HIT_BOTTOM);
			CHECK(hit != KEIUI_WIN32_HIT_TOP_LEFT);
			CHECK(hit != KEIUI_WIN32_HIT_TOP_RIGHT);
			CHECK(hit != KEIUI_WIN32_HIT_BOTTOM_LEFT);
			CHECK(hit != KEIUI_WIN32_HIT_BOTTOM_RIGHT);
		}

		/* A point farther than the band's depth from every edge is on no resize part. */
		band = (int64_t)keiui_win32_scale(KEIUI_WIN32_RESIZE_BORDER, frame.dpi);
		deep_inside = 0;
		if ((int64_t)x >= band && (int64_t)x + band < (int64_t)frame.width &&
		    (int64_t)y >= band && (int64_t)y + band < (int64_t)frame.height)
			deep_inside = 1;
		if (deep_inside != 0) {
			CHECK(hit != KEIUI_WIN32_HIT_LEFT);
			CHECK(hit != KEIUI_WIN32_HIT_RIGHT);
			CHECK(hit != KEIUI_WIN32_HIT_TOP);
			CHECK(hit != KEIUI_WIN32_HIT_BOTTOM);
			CHECK(hit != KEIUI_WIN32_HIT_TOP_LEFT);
			CHECK(hit != KEIUI_WIN32_HIT_TOP_RIGHT);
			CHECK(hit != KEIUI_WIN32_HIT_BOTTOM_LEFT);
			CHECK(hit != KEIUI_WIN32_HIT_BOTTOM_RIGHT);
		}

		/* The buttons agree with the scale: close ends at the right edge. */
		if (point % 16U == 0U) {
			keiui_win32_frame_buttons(&frame, buttons);
			close_x = (int64_t)frame.width - (int64_t)keiui_win32_scale(KEIUI_WIN32_BUTTON_WIDTH, frame.dpi);
			if (frame.fullscreen != 0 || frame.caption_height == 0U)
				close_x = 0;
			CHECK_EQ(buttons[KEIUI_WIN32_BUTTON_CLOSE_INDEX].x, close_x);
			CHECK_EQ(buttons[KEIUI_WIN32_BUTTON_CLOSE_INDEX].y, 0);
		}
	}
}
