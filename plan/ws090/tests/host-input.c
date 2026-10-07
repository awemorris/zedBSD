/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host tests of libkeiland's scroll, input and text view touch (ws090-p003),
 * built on Linux with libkeiland's scroller, gestures and touch motion:
 * times are given, so every position is checked against the formulas.
 *
 *   host-input
 */

#include <keiland/keiland.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The fake text view: a monospaced grid of cells, and the text's lines. */
#define VIEW_CELL_WIDTH		10
#define VIEW_LINE_HEIGHT	20
#define VIEW_COLUMNS		80
#define VIEW_LINES		100

/* A second, in microseconds. */
#define SECOND			1000000U

/* The tests run and failed. */
static int test_count;
static int test_failed;

/* The time of the tests' clock, in microseconds. */
static uint64_t test_now;

static void check(int condition, const char *what);
static int near(double value, double expected, double within);
static size_t view_position_at(void *data, double x, double y);
static void view_caret_rect(void *data, size_t position, struct kl_rect *rect);
static void view_word_at(void *data, size_t position, size_t *start, size_t *end);
static void test_scroll(void);
static void test_scroll_touch(void);
static void test_scroll_axis(void);
static void test_scroll_bounds(void);
unsigned kl_appearance_get(const struct kl_appearance *appearance);
static void test_pointer(void);
static void test_touch(void);
static void test_text(void);
static void frame(struct kl_ui *ui, const struct kl_rect *widget, struct kl_scroll *scroll, const struct kl_rect *region, struct kl_text_touch *text, unsigned *state);
static void finger(struct kl_ui *ui, int32_t id, double x, double y, int down);
static void swipe(struct kl_ui *ui, int32_t id, double x, double y, double dx, double dy, int steps, struct kl_scroll *scroll, const struct kl_rect *region, struct kl_text_touch *text);

/* The fake view's answers. */
static const struct kl_text_view view_answers = {
	view_position_at,
	view_caret_rect,
	view_word_at
};

/*
 * Runs the tests.
 */
int
main(void)
{
	/* Each part. */
	test_now = 10U * SECOND;
	test_scroll();
	test_scroll_touch();
	test_scroll_axis();
	test_scroll_bounds();
	test_pointer();
	test_touch();
	test_text();

	/* The outcome. */
	printf("host-input: %d/%d passed\n", test_count - test_failed, test_count);
	if (test_failed != 0)
		return 1;
	return 0;
}

/* Records one check. */
static void
check(
	int condition,
	const char *what)
{
	/* Counted, and a failure said. */
	test_count++;
	if (condition)
		return;
	test_failed++;
	printf("FAIL: %s\n", what);
}

/* Tells whether a value is within a distance of the one expected. */
static int
near(
	double value,
	double expected,
	double within)
{
	double distance;

	/* The distance. */
	distance = fabs(value - expected);
	if (distance <= within)
		return 1;
	printf("  (%.3f, expected %.3f)\n", value, expected);
	return 0;
}

/* The fake view: the position nearest a point (cell by cell, a line of VIEW_COLUMNS characters and its newline). */
static size_t
view_position_at(
	void *data,
	double x,
	double y)
{
	long line;
	long column;

	/* The line and the cell boundary nearest. */
	(void)data;
	line = (long)floor(y / VIEW_LINE_HEIGHT);
	column = (long)floor(x / VIEW_CELL_WIDTH + 0.5);
	if (line < 0)
		line = 0;
	if (line >= VIEW_LINES)
		line = VIEW_LINES - 1;
	if (column < 0)
		column = 0;
	if (column > VIEW_COLUMNS)
		column = VIEW_COLUMNS;

	/* The position. */
	return (size_t)(line * (VIEW_COLUMNS + 1) + column);
}

/* The fake view: the caret's rectangle at a position. */
static void
view_caret_rect(
	void *data,
	size_t position,
	struct kl_rect *rect)
{
	/* The line and column of the position. */
	(void)data;
	rect->x = (int)(position % (VIEW_COLUMNS + 1U)) * VIEW_CELL_WIDTH;
	rect->y = (int)(position / (VIEW_COLUMNS + 1U)) * VIEW_LINE_HEIGHT;
	rect->width = 2;
	rect->height = VIEW_LINE_HEIGHT;
}

/* The fake view: words are runs of 5 characters and a space (columns 0-4, 6-10, ...). */
static void
view_word_at(
	void *data,
	size_t position,
	size_t *start,
	size_t *end)
{
	size_t line;
	size_t column;

	/* The word the column falls in. */
	(void)data;
	line = position / (VIEW_COLUMNS + 1U);
	column = position % (VIEW_COLUMNS + 1U);
	*start = line * (VIEW_COLUMNS + 1U) + column / 6U * 6U;
	*end = *start + 5U;
}

/* The scroll: the wheel's glide, the ends, reveal, keys, sizes and the bars. */
static void
test_scroll(void)
{
	static uint32_t pixels[200 * 200];
	struct kl_scroll scroll;
	struct kl_canvas canvas;
	struct kl_rect rect;
	int moving;
	int error;
	int drawn;
	int index;

	/* A tall content in a smaller viewport. */
	error = kl_scroll_init(&scroll, KL_SCROLL_Y);
	check(error == 0, "scroll init");
	kl_scroll_set_size(&scroll, 400.0, 5000.0, 400.0, 300.0);
	check(near(kl_scroll_limit_y(&scroll), 4700.0, 0.0), "limit is the content less the viewport");
	check(kl_scroll_limit_x(&scroll) == 0.0, "no horizontal scroll");

	/* The wheel glides: after one time constant 1 - 1/e of the way (Text Editor's 70 ms). */
	kl_scroll_wheel(&scroll, 0.0, 100.0, test_now);
	moving = kl_scroll_step(&scroll, test_now + KL_SCROLL_GLIDE_US);
	check(moving == 1 && near(scroll.y, 100.0 * (1.0 - exp(-1.0)), 0.01), "glide after one time constant");
	moving = kl_scroll_step(&scroll, test_now + SECOND);
	check(moving == 0 && scroll.y == 100.0 && !scroll.gliding, "glide arrives and stops");

	/* Two turns add up: the second from the first's target. */
	test_now += 2U * SECOND;
	kl_scroll_wheel(&scroll, 0.0, 100.0, test_now);
	kl_scroll_wheel(&scroll, 0.0, 100.0, test_now + 10000U);
	(void)kl_scroll_step(&scroll, test_now + SECOND);
	check(scroll.y == 300.0, "two turns glide 200");

	/* Past the end: the end. */
	kl_scroll_wheel(&scroll, 0.0, 1.0e6, test_now + SECOND);
	(void)kl_scroll_step(&scroll, test_now + 3U * SECOND);
	check(scroll.y == 4700.0, "the wheel stops at the end");
	kl_scroll_move_to(&scroll, 0.0, -50.0, 0, test_now);
	check(scroll.y == 0.0, "a move before the start is the start");

	/* Reveal: a row below the viewport glides just into view; one above glides to its top. */
	rect.x = 0;
	rect.y = 1000;
	rect.width = 10;
	rect.height = 20;
	kl_scroll_reveal(&scroll, &rect, test_now);
	(void)kl_scroll_step(&scroll, test_now + SECOND);
	check(scroll.y == 720.0, "reveal below");
	rect.y = 100;
	kl_scroll_reveal(&scroll, &rect, test_now + SECOND);
	(void)kl_scroll_step(&scroll, test_now + 3U * SECOND);
	check(scroll.y == 100.0, "reveal above");

	/* Keys: a line, a page less a line, the end and the start. */
	test_now += 4U * SECOND;
	(void)kl_scroll_key(&scroll, KL_KEY_DOWN, 0U, 20.0, test_now);
	(void)kl_scroll_step(&scroll, test_now + SECOND);
	check(scroll.y == 120.0, "down a line");
	(void)kl_scroll_key(&scroll, KL_KEY_PAGEDOWN, 0U, 20.0, test_now + SECOND);
	(void)kl_scroll_step(&scroll, test_now + 2U * SECOND);
	check(scroll.y == 400.0, "down a page less a line");
	(void)kl_scroll_key(&scroll, KL_KEY_END, KL_MOD_CTRL, 20.0, test_now + 2U * SECOND);
	(void)kl_scroll_step(&scroll, test_now + 3U * SECOND);
	check(scroll.y == 4700.0, "the end");
	index = kl_scroll_key(&scroll, 30U, 0U, 20.0, test_now);
	check(index == 0, "a letter is not a scrolling key");
	(void)kl_scroll_key(&scroll, KL_KEY_HOME, KL_MOD_CTRL, 20.0, test_now + 3U * SECOND);
	(void)kl_scroll_step(&scroll, test_now + 4U * SECOND);
	check(scroll.y == 0.0, "the start");

	/* A smaller content keeps the position within the new end. */
	kl_scroll_move_to(&scroll, 0.0, 4000.0, 0, test_now);
	kl_scroll_set_size(&scroll, 400.0, 500.0, 400.0, 300.0);
	check(scroll.y == 200.0, "a shorter content moves the position back");

	/* The bars show after a move and are gone after the fade. */
	kl_canvas_init(&canvas, pixels, 200, 200, 200);
	kl_scroll_set_size(&scroll, 400.0, 5000.0, 200.0, 200.0);
	kl_scroll_move_to(&scroll, 0.0, 1000.0, 0, test_now + 5U * SECOND);
	rect.x = 0;
	rect.y = 0;
	rect.width = 200;
	rect.height = 200;
	moving = kl_scroll_draw_bars(&scroll, &canvas, &rect, kl_theme_default(), test_now + 5U * SECOND + 100000U);
	drawn = 0;
	for (index = 0; index < 200 * 200; index++) {
		if (pixels[index] != 0U)
			drawn++;
	}

	/* Something of the bar was drawn. */
	check(moving == 1 && drawn > 50, "the bar shows after a move");
	moving = kl_scroll_draw_bars(&scroll, &canvas, &rect, kl_theme_default(), test_now + 7U * SECOND);
	check(moving == 0, "the bar is gone after the fade");
	kl_canvas_release(&canvas);

	/* An axis that does not scroll ignores the wheel on it. */
	kl_scroll_release(&scroll);
	error = kl_scroll_init(&scroll, KL_SCROLL_X);
	kl_scroll_set_size(&scroll, 2000.0, 1000.0, 400.0, 300.0);
	kl_scroll_wheel(&scroll, 50.0, 100.0, test_now);
	(void)kl_scroll_step(&scroll, test_now + SECOND);
	check(error == 0 && scroll.x == 50.0 && scroll.y == 0.0, "only the scroll's own axis moves");
	kl_scroll_release(&scroll);
	error = kl_scroll_init(&scroll, 0U);
	check(error != 0, "a scroll with no axis is refused");
	test_now += 10U * SECOND;
}

/* The scroll under a finger follows libkeiland's scroller exactly. */
static void
test_scroll_touch(void)
{
	struct kl_scroller *reference;
	struct kl_scroll scroll;
	double x;
	double y;
	int moving;
	int same;
	int step;

	/* The scroll and a bare scroller with the same bounds, both at 1000. */
	(void)kl_scroll_init(&scroll, KL_SCROLL_Y);
	kl_scroll_set_size(&scroll, 400.0, 5000.0, 400.0, 300.0);
	kl_scroll_move_to(&scroll, 0.0, 1000.0, 0, test_now);
	reference = kl_scroller_create();
	(void)kl_scroller_set_bounds(reference, 0.0, 0.0, 0.0, 4700.0, 400.0, 300.0);
	kl_scroller_set_position(reference, 0.0, 1000.0);

	/* A finger drags up by 200 and flings up at 2000 px/s. */
	(void)kl_scroll_press(&scroll, test_now);
	(void)kl_scroller_press(reference, test_now);
	kl_scroll_drag(&scroll, 0.0, -200.0);
	kl_scroller_drag(reference, 0.0, -200.0);
	(void)kl_scroll_step(&scroll, test_now + 50000U);
	check(scroll.y == 1200.0, "the content follows the finger");
	kl_scroll_fling(&scroll, 0.0, -2000.0, test_now + 60000U);
	kl_scroller_release(reference, test_now + 60000U, 0.0, -2000.0);

	/* Frame by frame both are at the same place until they rest. */
	same = 1;
	moving = 1;
	for (step = 1; step < 400 && moving; step++) {
		moving = kl_scroll_step(&scroll, test_now + 60000U + (uint64_t)step * 16667U);
		(void)kl_scroller_step(reference, test_now + 60000U + (uint64_t)step * 16667U, &x, &y);
		if (scroll.y != y)
			same = 0;
	}

	/* The same all the way, and at rest. */
	check(same, "the flight is the scroller's, frame by frame");
	check(scroll.y > 1300.0 && !moving && !scroll.touched, "the content flew on and rests");
	kl_scroller_destroy(reference);
	kl_scroll_release(&scroll);
	test_now += 10U * SECOND;
}

/*
 * A touch pad's two fingers (BUG-211, BUG-218): their moves move the
 * content at once, their velocity is the track's, it flies on when they
 * lift, fingers that rested throw nothing, and a wheel still glides.
 */
static void
test_scroll_axis(void)
{
	struct kl_axis_track track;
	struct kl_window_event event;
	struct kl_scroller *scroller;
	struct kl_scroll scroll;
	struct kl_rect widget;
	struct kl_rect region;
	struct kl_ui *ui;
	unsigned state[2];
	int taken;
	double vx;
	double vy;
	double x;
	double y;
	int caught;
	int flung;
	int moving;
	int step;
	int index;

	/* The track: ten moves of 30 px every 10 ms are 3000 px/s; a single move, or a rest, none. */
	kl_axis_track_reset(&track);
	kl_axis_track_add(&track, 0.0, 30.0, test_now);
	kl_axis_track_velocity(&track, test_now + 5000U, &vx, &vy);
	check(vx == 0.0 && vy == 0.0, "track: one move has no velocity");
	for (index = 1; index < 10; index++)
		kl_axis_track_add(&track, 0.0, 30.0, test_now + (uint64_t)index * 10000U);
	kl_axis_track_velocity(&track, test_now + 95000U, &vx, &vy);
	check(near(vy, 3000.0, 1.0) && vx == 0.0, "track: the velocity of the last moves");
	kl_axis_track_velocity(&track, test_now + 90000U + KL_AXIS_TRACK_REST_US, &vx, &vy);
	check(vy == 0.0, "track: fingers that rested throw nothing");

	/* The fingers scroll the content at once, as far as they moved. */
	(void)kl_scroll_init(&scroll, KL_SCROLL_Y);
	kl_scroll_set_size(&scroll, 400.0, 5000.0, 400.0, 300.0);
	kl_scroll_move_to(&scroll, 0.0, 1000.0, 0, test_now);
	for (index = 0; index < 10; index++)
		kl_scroll_axis(&scroll, 0.0, 30.0, KL_AXIS_SOURCE_FINGER, test_now + (uint64_t)index * 10000U);
	(void)kl_scroll_step(&scroll, test_now + 90000U);
	check(scroll.y == 1300.0 && kl_scroller_axis_holding(scroll.scroller), "axis: the fingers move the content at once");

	/* They lift moving: the content flies on further down, and rests. */
	kl_scroll_axis_stop(&scroll, test_now + 95000U);
	moving = 1;
	for (step = 1; step < 400 && moving; step++)
		moving = kl_scroll_step(&scroll, test_now + 95000U + (uint64_t)step * 16667U);
	check(scroll.y > 1600.0 && !moving && !scroll.touched && !kl_scroller_axis_holding(scroll.scroller), "axis: the content flies on when the fingers lift");
	test_now += 10U * SECOND;

	/* Fingers that rest before lifting leave the content where it is. */
	kl_scroll_move_to(&scroll, 0.0, 1000.0, 0, test_now);
	for (index = 0; index < 10; index++)
		kl_scroll_axis(&scroll, 0.0, 30.0, KL_AXIS_SOURCE_FINGER, test_now + (uint64_t)index * 10000U);
	kl_scroll_axis_stop(&scroll, test_now + 200000U);
	for (step = 1; step < 400; step++)
		(void)kl_scroll_step(&scroll, test_now + 200000U + (uint64_t)step * 16667U);
	check(scroll.y == 1300.0 && !scroll.touched, "axis: rested fingers throw nothing");
	test_now += 10U * SECOND;

	/* A wheel's axis glides as kl_scroll_wheel does. */
	kl_scroll_axis(&scroll, 0.0, 100.0, KL_AXIS_SOURCE_WHEEL, test_now);
	check(scroll.gliding && !kl_scroller_axis_holding(scroll.scroller) && scroll.to_y == 1400.0, "axis: a wheel glides");
	kl_scroll_release(&scroll);
	test_now += 10U * SECOND;

	/*
	 * The bare scroller (ws090-p019, what the touch screens' programs keep):
	 * the fingers' moves at the compositor's times, 5 s behind the steps'
	 * clock, fling it from the steps' time; another move catches the flight.
	 */
	scroller = kl_scroller_create();
	check(scroller != NULL, "scroller: made");
	(void)kl_scroller_set_bounds(scroller, 0.0, 0.0, 0.0, 4000.0, 400.0, 300.0);
	kl_scroller_set_position(scroller, 0.0, 1000.0);
	caught = 0;
	for (index = 0; index < 10; index++)
		caught |= kl_scroller_axis(scroller, 0.0, 30.0, test_now + (uint64_t)index * 10000U, test_now + 5U * SECOND + (uint64_t)index * 10000U);
	(void)kl_scroller_step(scroller, test_now + 5U * SECOND + 90000U, &x, &y);
	check(!caught && y == 1300.0 && kl_scroller_axis_holding(scroller), "scroller: the fingers drag it");
	flung = kl_scroller_axis_stop(scroller, test_now + 95000U, test_now + 5U * SECOND + 95000U, &vx, &vy);
	check(flung && near(vy, 3000.0, 1.0) && !kl_scroller_axis_holding(scroller), "scroller: the fingers' lift flings it at their velocity");
	moving = kl_scroller_step(scroller, test_now + 5U * SECOND + 300000U, &x, &y);
	check(moving && y > 1500.0, "scroller: it flies on the steps' clock");
	caught = kl_scroller_axis(scroller, 0.0, 1.0, test_now + 400000U, test_now + 5U * SECOND + 400000U);
	check(caught, "scroller: another move catches the flight");
	flung = kl_scroller_axis_stop(scroller, test_now + 400000U + KL_AXIS_TRACK_REST_US, test_now + 5U * SECOND + 500000U, NULL, NULL);
	check(!flung, "scroller: rested fingers throw nothing");
	kl_scroller_destroy(scroller);
	test_now += 10U * SECOND;

	/* Through the window's events: the fingers' moves reach the scroll under the pointer, their end lets it fly. */
	ui = kl_ui_create();
	(void)kl_scroll_init(&scroll, KL_SCROLL_Y);
	kl_scroll_set_size(&scroll, 300.0, 3000.0, 300.0, 300.0);
	widget.x = 10;
	widget.y = 10;
	widget.width = 100;
	widget.height = 30;
	region.x = 200;
	region.y = 0;
	region.width = 300;
	region.height = 300;
	frame(ui, &widget, &scroll, &region, NULL, state);
	(void)kl_ui_pointer_motion(ui, 300.0, 100.0);
	memset(&event, 0, sizeof(event));
	event.kind = KL_WINDOW_AXIS;
	event.axis_source = KL_AXIS_SOURCE_FINGER;
	event.dy = 40.0;
	for (index = 0; index < 5; index++) {
		event.time_us = test_now + (uint64_t)index * 10000U;
		event.arrival_us = event.time_us;
		taken = kl_ui_axis(ui, &event);
	}
	(void)kl_scroll_step(&scroll, test_now + 40000U);
	check(taken == 1 && scroll.y == 200.0, "ui axis: the fingers scroll the region under the pointer");
	event.kind = KL_WINDOW_AXIS_STOP;
	event.time_us = test_now + 45000U;
	taken = kl_ui_axis(ui, &event);
	moving = kl_scroll_step(&scroll, test_now + 60000U);
	check(taken == KL_UI_AXIS_FLUNG && moving && scroll.y > 200.0, "ui axis: the end lets the content fly");
	kl_scroll_release(&scroll);
	kl_ui_destroy(ui);
	test_now += 10U * SECOND;
}

/*
 * The scroll's own ends (kl_scroll_set_bounds, KL_VERSION 61, ws090-p015):
 * a view whose position runs below 0 (Terminal's scrollback), its rubber
 * band against a size of its own frame by frame as a bare scroller's, the
 * place handed over under a finger and under a touch pad's fingers, the
 * keys' ends, and the sizes' ends again.
 */
static void
test_scroll_bounds(void)
{
	struct kl_scroller *reference;
	struct kl_scroll scroll;
	double x;
	double y;
	int moving;
	int flung;
	int error;
	int same;
	int step;

	/* Ends from -2000 to 0 down, none across, the band the size of a 300-pixel grid. */
	(void)kl_scroll_init(&scroll, KL_SCROLL_Y);
	error = kl_scroll_set_bounds(&scroll, 0.0, 0.0, -2000.0, 0.0, 1.0, 300.0);
	check(error == 0 && kl_scroll_limit_y(&scroll) == 0.0, "bounds: the ends hold");
	error = kl_scroll_set_bounds(&scroll, 0.0, 0.0, 10.0, 0.0, 1.0, 300.0);
	check(error != 0, "bounds: a maximum below its minimum is refused");
	error = kl_scroll_set_bounds(&scroll, 0.0, 0.0, -2000.0, 0.0, 1.0, 0.0);
	check(error != 0, "bounds: a band without a size is refused");
	kl_scroll_move_to(&scroll, 0.0, -500.0, 0, test_now);
	check(scroll.y == -500.0, "bounds: a place below 0 is kept");
	kl_scroll_move_to(&scroll, 0.0, -5000.0, 0, test_now);
	check(scroll.y == -2000.0, "bounds: a place past the minimum stops at it");

	/* A bare scroller with the same ends, both at -500. */
	kl_scroll_move_to(&scroll, 0.0, -500.0, 0, test_now);
	reference = kl_scroller_create();
	(void)kl_scroller_set_bounds(reference, 0.0, 0.0, -2000.0, 0.0, 1.0, 300.0);
	kl_scroller_set_position(reference, 0.0, -500.0);

	/* A finger drags 800 the way that passes the maximum, and lets go slowly: the band and its spring back are the scroller's. */
	(void)kl_scroll_press(&scroll, test_now);
	(void)kl_scroller_press(reference, test_now);
	kl_scroll_drag(&scroll, 0.0, -800.0);
	kl_scroller_drag(reference, 0.0, -800.0);
	(void)kl_scroll_step(&scroll, test_now + 50000U);
	(void)kl_scroller_step(reference, test_now + 50000U, &x, &y);
	check(scroll.y > 0.0 && scroll.y < 300.0 && scroll.y == y, "bounds: past the maximum the band stretches as the scroller's");
	flung = kl_scroll_fling(&scroll, 0.0, 0.0, test_now + 60000U);
	(void)kl_scroller_release(reference, test_now + 60000U, 0.0, 0.0);
	same = 1;
	moving = 1;
	for (step = 1; step < 400 && moving; step++) {
		moving = kl_scroll_step(&scroll, test_now + 60000U + (uint64_t)step * 16667U);
		(void)kl_scroller_step(reference, test_now + 60000U + (uint64_t)step * 16667U, &x, &y);
		if (scroll.y != y)
			same = 0;
	}

	/* No flight, the same all the way, and at rest at the maximum. */
	check(!flung && same && scroll.y == 0.0 && !moving && !scroll.touched, "bounds: it springs back to the maximum, frame by frame as the scroller");
	kl_scroller_destroy(reference);
	test_now += 10U * SECOND;

	/* A fling from the middle flies and says so. */
	kl_scroll_move_to(&scroll, 0.0, -1000.0, 0, test_now);
	(void)kl_scroll_press(&scroll, test_now);
	kl_scroll_drag(&scroll, 0.0, -100.0);
	(void)kl_scroll_step(&scroll, test_now + 50000U);
	flung = kl_scroll_fling(&scroll, 0.0, -1500.0, test_now + 60000U);
	moving = kl_scroll_step(&scroll, test_now + 200000U);
	check(flung && moving && scroll.y > -900.0, "bounds: a fling flies and says so");
	for (step = 1; step < 400 && moving; step++)
		moving = kl_scroll_step(&scroll, test_now + 200000U + (uint64_t)step * 16667U);
	check(!moving && scroll.y <= 0.0 && !scroll.touched, "bounds: the flight rests within the ends");
	test_now += 10U * SECOND;

	/* A place handed over under a finger (the program moved its view): pressed again, the drag goes on from there. */
	kl_scroll_move_to(&scroll, 0.0, -1000.0, 0, test_now);
	(void)kl_scroll_press(&scroll, test_now);
	kl_scroll_drag(&scroll, 0.0, -50.0);
	(void)kl_scroll_step(&scroll, test_now + 20000U);
	check(scroll.y == -950.0, "hand-over: the finger drags");
	kl_scroll_move_to(&scroll, 0.0, -1500.0, 0, test_now + 30000U);
	check(scroll.y == -1500.0 && !scroll.touched, "hand-over: the new place, the finger let go");
	(void)kl_scroll_press(&scroll, test_now + 40000U);
	kl_scroll_drag(&scroll, 0.0, -100.0);
	(void)kl_scroll_step(&scroll, test_now + 50000U);
	check(scroll.y == -1400.0, "hand-over: the next press drags on from the new place");
	(void)kl_scroll_fling(&scroll, 0.0, 0.0, test_now + 60000U);
	for (step = 1; step < 400; step++)
		(void)kl_scroll_step(&scroll, test_now + 60000U + (uint64_t)step * 16667U);
	test_now += 10U * SECOND;

	/* A touch pad's fingers holding the content take a place handed over at their next move. */
	kl_scroll_move_to(&scroll, 0.0, -1000.0, 0, test_now);
	(void)kl_scroll_axis_at(&scroll, 0.0, 30.0, KL_AXIS_SOURCE_FINGER, test_now, test_now);
	(void)kl_scroll_step(&scroll, test_now + 10000U);
	check(scroll.y == -970.0 && kl_scroll_axis_holding(&scroll), "hand-over: the touch pad's fingers move it");
	kl_scroll_move_to(&scroll, 0.0, -1800.0, 0, test_now + 20000U);
	(void)kl_scroll_axis_at(&scroll, 0.0, 30.0, KL_AXIS_SOURCE_FINGER, test_now + 30000U, test_now + 30000U);
	(void)kl_scroll_step(&scroll, test_now + 40000U);
	check(scroll.y == -1770.0, "hand-over: their next move goes on from the new place");
	(void)kl_scroll_axis_stop_at(&scroll, test_now + 400000U, test_now + 400000U, NULL, NULL);
	for (step = 1; step < 400; step++)
		(void)kl_scroll_step(&scroll, test_now + 400000U + (uint64_t)step * 16667U);
	check(!kl_scroll_axis_holding(&scroll) && !scroll.touched, "hand-over: they lift");
	test_now += 10U * SECOND;

	/* Home glides to the minimum, End to the maximum. */
	(void)kl_scroll_key(&scroll, KL_KEY_HOME, KL_MOD_CTRL, 20.0, test_now);
	(void)kl_scroll_step(&scroll, test_now + SECOND);
	check(scroll.y == -2000.0, "bounds: Home goes to the minimum");
	(void)kl_scroll_key(&scroll, KL_KEY_END, KL_MOD_CTRL, 20.0, test_now + SECOND);
	(void)kl_scroll_step(&scroll, test_now + 2U * SECOND);
	check(scroll.y == 0.0, "bounds: End goes to the maximum");

	/* The sizes' ends again: 0 to the content less the viewport. */
	kl_scroll_set_size(&scroll, 400.0, 1000.0, 400.0, 300.0);
	kl_scroll_move_to(&scroll, 0.0, -100.0, 0, test_now + 3U * SECOND);
	check(scroll.y == 0.0 && kl_scroll_limit_y(&scroll) == 700.0, "bounds: the sizes' ends come back");
	kl_scroll_release(&scroll);
	test_now += 10U * SECOND;
}

/* The appearance the theme asks for: the light one (the tests draw nothing that depends on it). */
unsigned
kl_appearance_get(
	const struct kl_appearance *appearance)
{
	(void)appearance;

	/* The light appearance. */
	return KL_APPEARANCE_LIGHT;
}

/* Draws one frame of the pointer and touch tests: a widget, a scroll's region (or a text view's), and a widget over the region. */
static void
frame(
	struct kl_ui *ui,
	const struct kl_rect *widget,
	struct kl_scroll *scroll,
	const struct kl_rect *region,
	struct kl_text_touch *text,
	unsigned *state)
{
	struct kl_rect row;

	/* The frame's time. */
	kl_ui_begin(ui, test_now);

	/* The region, then a widget: the first alone, and a row in the region. */
	if (text != NULL)
		kl_ui_text_region(ui, 20U, region, scroll, text);
	else if (scroll != NULL)
		kl_ui_scroll_region(ui, 10U, region, scroll);
	state[0] = kl_ui_hit(ui, 1U, 0U, widget);
	state[1] = 0;
	if (scroll != NULL && text == NULL) {
		row.x = region->x;
		row.y = region->y + 40 - (int)scroll->y;
		row.width = region->width;
		row.height = 28;
		state[1] = kl_ui_hit(ui, 2U, 7U, &row);
	}

	/* The frame is drawn. */
	(void)kl_ui_end(ui, test_now);
}

/* The pointer: hover, click, double click, the wheel and what no part takes. */
static void
test_pointer(void)
{
	struct kl_scroll scroll;
	struct kl_event event;
	struct kl_rect widget;
	struct kl_rect region;
	struct kl_ui *ui;
	unsigned state[2];
	int redraw;
	int taken;

	/* A widget and a scroll's region beside it. */
	ui = kl_ui_create();
	check(ui != NULL, "ui create");
	(void)kl_scroll_init(&scroll, KL_SCROLL_Y);
	kl_scroll_set_size(&scroll, 300.0, 3000.0, 300.0, 300.0);
	widget.x = 10;
	widget.y = 10;
	widget.width = 100;
	widget.height = 30;
	region.x = 200;
	region.y = 0;
	region.width = 300;
	region.height = 300;
	frame(ui, &widget, &scroll, &region, NULL, state);

	/* Hover: onto the widget draws, moving within it does not, leaving it does. */
	redraw = kl_ui_pointer_motion(ui, 20.0, 20.0);
	check(redraw == 1, "the pointer onto a widget redraws");
	redraw = kl_ui_pointer_motion(ui, 30.0, 25.0);
	check(redraw == 0, "moving within the widget does not redraw");
	frame(ui, &widget, &scroll, &region, NULL, state);
	check((state[0] & KL_HIT_HOT) != 0U, "the widget is lit");
	redraw = kl_ui_pointer_motion(ui, 150.0, 100.0);
	check(redraw == 1, "leaving the widget redraws");

	/* A press and a release on it click it, for one frame. */
	(void)kl_ui_pointer_motion(ui, 20.0, 20.0);
	(void)kl_ui_pointer_button(ui, 1, test_now);
	frame(ui, &widget, &scroll, &region, NULL, state);
	check((state[0] & KL_HIT_ACTIVE) != 0U && (state[0] & KL_HIT_CLICKED) == 0U, "held, not yet clicked");
	(void)kl_ui_pointer_button(ui, 0, test_now);
	frame(ui, &widget, &scroll, &region, NULL, state);
	check((state[0] & KL_HIT_CLICKED) != 0U && (state[0] & KL_HIT_DOUBLE) == 0U, "clicked");
	frame(ui, &widget, &scroll, &region, NULL, state);
	check((state[0] & KL_HIT_CLICKED) == 0U, "a click is seen by one frame");

	/* A second click soon after is a double click. */
	(void)kl_ui_pointer_button(ui, 1, test_now + 200000U);
	(void)kl_ui_pointer_button(ui, 0, test_now + 250000U);
	frame(ui, &widget, &scroll, &region, NULL, state);
	check((state[0] & KL_HIT_DOUBLE) != 0U, "double click");

	/* A press on the widget released elsewhere clicks nothing. */
	test_now += SECOND;
	(void)kl_ui_pointer_button(ui, 1, test_now);
	(void)kl_ui_pointer_motion(ui, 150.0, 20.0);
	(void)kl_ui_pointer_button(ui, 0, test_now);
	frame(ui, &widget, &scroll, &region, NULL, state);
	check((state[0] & KL_HIT_CLICKED) == 0U, "released elsewhere: no click");

	/* The wheel over the region glides its scroll. */
	(void)kl_ui_pointer_motion(ui, 300.0, 200.0);
	(void)kl_ui_wheel(ui, 0.0, 120.0, test_now);
	test_now += SECOND;
	frame(ui, &widget, &scroll, &region, NULL, state);
	check(scroll.y == 120.0, "the wheel over a scroll glides it");

	/* A press and the wheel over nothing are the application's. */
	(void)kl_ui_pointer_motion(ui, 150.0, 350.0);
	(void)kl_ui_wheel(ui, 0.0, 30.0, test_now);
	(void)kl_ui_pointer_button(ui, 1, test_now);
	taken = kl_ui_take(ui, &event);
	check(taken == 1 && event.kind == KL_EVENT_WHEEL && event.dy == 30.0, "an unclaimed wheel");
	taken = kl_ui_take(ui, &event);
	check(taken == 1 && event.kind == KL_EVENT_PRESS && event.x == 150.0 && event.region == 0U, "an unclaimed press");
	taken = kl_ui_take(ui, &event);
	check(taken == 0, "no more");

	/* A press in the scroll region on no widget names the region. */
	(void)kl_ui_pointer_button(ui, 0, test_now);
	(void)kl_ui_take(ui, &event);
	(void)kl_ui_pointer_motion(ui, 300.0, 250.0);
	(void)kl_ui_pointer_button(ui, 1, test_now);
	taken = kl_ui_take(ui, &event);
	check(taken == 1 && event.kind == KL_EVENT_PRESS && event.region == 10U, "a press over a region names it");
	(void)kl_ui_pointer_button(ui, 0, test_now);
	(void)kl_ui_take(ui, &event);

	/* The scroll and the input go. */
	/* The scroll and the input go. */
	/* The scroll and the input go. */
	kl_scroll_release(&scroll);
	kl_ui_destroy(ui);
	test_now += 10U * SECOND;
}

/* One finger down or up at a place, at the test's time. */
static void
finger(
	struct kl_ui *ui,
	int32_t id,
	double x,
	double y,
	int down)
{
	/* Down, or up. */
	if (down)
		(void)kl_ui_touch_down(ui, id, test_now, test_now, x, y);
	else
		(void)kl_ui_touch_up(ui, id, test_now, test_now);
}

/* A finger that is down moves by dx, dy in steps of a frame (16.7 ms), a frame drawn after each. */
static void
swipe(
	struct kl_ui *ui,
	int32_t id,
	double x,
	double y,
	double dx,
	double dy,
	int steps,
	struct kl_scroll *scroll,
	const struct kl_rect *region,
	struct kl_text_touch *text)
{
	static const struct kl_rect nowhere = { -100, -100, 1, 1 };
	unsigned state[2];
	int step;

	/* Each step: the report, then a frame. */
	for (step = 1; step <= steps; step++) {
		test_now += 16667U;
		(void)kl_ui_touch_motion(ui, id, test_now, test_now, x + dx * step / steps, y + dy * step / steps);
		frame(ui, &nowhere, scroll, region, text, state);
	}
}

/* A finger: a tap on a widget, a drag and a fling of a scroll, a tap that catches it, and a drag no part takes. */
static void
test_touch(void)
{
	static const struct kl_rect nowhere = { -100, -100, 1, 1 };
	struct kl_scroll scroll;
	struct kl_event event;
	struct kl_rect widget;
	struct kl_rect region;
	struct kl_ui *ui;
	unsigned state[2];
	double before;
	double distance;
	double dx;
	double dy;
	int taken;
	int error;
	int step;

	/* A widget and a scroll's region with a row in it. */
	ui = kl_ui_create();
	(void)kl_scroll_init(&scroll, KL_SCROLL_Y);
	kl_scroll_set_size(&scroll, 300.0, 3000.0, 300.0, 300.0);
	widget.x = 10;
	widget.y = 10;
	widget.width = 100;
	widget.height = 30;
	region.x = 200;
	region.y = 0;
	region.width = 300;
	region.height = 300;
	frame(ui, &widget, &scroll, &region, NULL, state);

	/* A tap on the widget clicks it. */
	finger(ui, 1, 30.0, 20.0, 1);
	test_now += 60000U;
	finger(ui, 1, 30.0, 20.0, 0);
	frame(ui, &widget, &scroll, &region, NULL, state);
	check((state[0] & KL_HIT_CLICKED) != 0U, "a tap clicks the widget");

	/* A tap on the row in the region clicks the row. */
	test_now += SECOND;
	finger(ui, 1, 300.0, 50.0, 1);
	test_now += 60000U;
	finger(ui, 1, 300.0, 50.0, 0);
	frame(ui, &widget, &scroll, &region, NULL, state);
	check((state[1] & KL_HIT_CLICKED) != 0U, "a tap clicks a row in a scroll");
	check(!scroll.touched && scroll.y == 0.0, "the tap leaves the scroll where it was");

	/* A drag over the row scrolls the region (the row does not take a drag), and a fast lift flies on. */
	test_now += SECOND;
	finger(ui, 1, 300.0, 250.0, 1);
	swipe(ui, 1, 300.0, 250.0, 0.0, -200.0, 8, &scroll, &region, NULL);
	before = scroll.y;
	check(before > 100.0, "a drag moves the scroll with the finger");
	finger(ui, 1, 300.0, 50.0, 0);
	for (step = 0; step < 10; step++) {
		test_now += 16667U;
		frame(ui, &nowhere, &scroll, &region, NULL, state);
	}

	/* Farther than the finger took it, and still flying. */
	check(scroll.y > before + 50.0 && scroll.touched, "the content flies on after the lift");

	/* A tap while it flies catches it and clicks nothing. */
	finger(ui, 1, 300.0, 60.0, 1);
	test_now += 60000U;
	finger(ui, 1, 300.0, 60.0, 0);
	frame(ui, &widget, &scroll, &region, NULL, state);
	before = scroll.y;
	for (step = 0; step < 10; step++) {
		test_now += 16667U;
		frame(ui, &nowhere, &scroll, &region, NULL, state);
	}

	/* Stopped where it was caught, and no click. */
	distance = fabs(scroll.y - before);
	check((state[1] & KL_HIT_CLICKED) == 0U && distance < 1.0, "a tap stops the flight without a click");

	/* A drag over nothing is the application's, with its offset. */
	test_now += SECOND;
	for (;;) {
		taken = kl_ui_take(ui, &event);
		if (taken == 0)
			break;
	}

	/* A finger far from every part drags. */
	finger(ui, 1, 150.0, 400.0, 1);
	swipe(ui, 1, 150.0, 400.0, 60.0, 0.0, 6, &scroll, &region, NULL);
	taken = kl_ui_take(ui, &event);
	check(taken == 1 && event.kind == KL_EVENT_DRAG_BEGIN, "an unclaimed drag begins");
	error = kl_ui_drag_offset(ui, test_now, &dx, &dy);
	check(error == 0 && dx > 40.0, "its offset");
	finger(ui, 1, 210.0, 400.0, 0);
	taken = kl_ui_take(ui, &event);
	check(taken == 1 && event.kind == KL_EVENT_DRAG_END, "and ends");

	/* The scroll and the input go. */
	kl_scroll_release(&scroll);
	kl_ui_destroy(ui);
	test_now += 10U * SECOND;
}

/* A text view: one finger selects, two scroll, a double tap selects a word, a long press asks for the menu, the handles and the edge. */
static void
test_text(void)
{
	static const struct kl_rect nowhere = { -100, -100, 1, 1 };
	struct kl_text_touch text;
	struct kl_scroll scroll;
	struct kl_rect region;
	struct kl_ui *ui;
	unsigned changes;
	unsigned state[2];
	size_t anchor;
	double before;
	int step;

	/* A text view of 100 lines in a 300-pixel viewport at (0, 0). */
	ui = kl_ui_create();
	(void)kl_scroll_init(&scroll, KL_SCROLL_Y);
	kl_scroll_set_size(&scroll, 800.0, (double)(VIEW_LINES * VIEW_LINE_HEIGHT), 800.0, 300.0);
	kl_text_touch_init(&text, &view_answers, NULL);
	region.x = 0;
	region.y = 0;
	region.width = 800;
	region.height = 300;
	frame(ui, &nowhere, &scroll, &region, &text, state);

	/* A tap puts the caret: line 2, column 3. */
	finger(ui, 1, 31.0, 45.0, 1);
	test_now += 60000U;
	finger(ui, 1, 31.0, 45.0, 0);
	changes = kl_text_touch_take(&text);
	check((changes & KL_TEXT_TOUCH_SELECTION) != 0U && text.caret == 2U * 81U + 3U && text.anchor == text.caret, "a tap puts the caret");

	/* A double tap selects the word there (columns 6 to 11) with handles. */
	test_now += SECOND;
	finger(ui, 1, 72.0, 45.0, 1);
	test_now += 50000U;
	finger(ui, 1, 72.0, 45.0, 0);
	test_now += 100000U;
	finger(ui, 1, 72.0, 45.0, 1);
	test_now += 50000U;
	finger(ui, 1, 72.0, 45.0, 0);
	check(text.anchor == 2U * 81U + 6U && text.caret == 2U * 81U + 11U && text.handles, "a double tap selects a word");

	/* One finger's drag selects from where it touched, and the scroll stays. */
	test_now += SECOND;
	finger(ui, 1, 20.0, 5.0, 1);
	swipe(ui, 1, 20.0, 5.0, 80.0, 40.0, 8, &scroll, &region, &text);
	finger(ui, 1, 100.0, 45.0, 0);
	frame(ui, &nowhere, &scroll, &region, &text, state);
	check(text.anchor == 2U && text.caret == 2U * 81U + 10U, "one finger selects");
	check(scroll.y == 0.0, "one finger does not scroll");
	check(text.handles && !text.selecting, "the selection keeps its handles");

	/* Dragging the caret's handle (under the caret at column 10, line 2) 40 pixels down moves only the caret, two lines. */
	test_now += SECOND;
	anchor = text.anchor;
	finger(ui, 1, 100.0, 66.0, 1);
	swipe(ui, 1, 100.0, 66.0, 0.0, 40.0, 6, &scroll, &region, &text);
	finger(ui, 1, 100.0, 106.0, 0);
	frame(ui, &nowhere, &scroll, &region, &text, state);
	check(text.anchor == anchor && text.caret == 4U * 81U + 10U, "the caret's handle moves the caret, by the caret's line (not the knob's)");

	/* Dragging the anchor's handle moves the other end. */
	test_now += SECOND;
	finger(ui, 1, 20.0, 26.0, 1);
	swipe(ui, 1, 20.0, 26.0, 30.0, 20.0, 6, &scroll, &region, &text);
	finger(ui, 1, 50.0, 46.0, 0);
	frame(ui, &nowhere, &scroll, &region, &text, state);
	check(text.anchor == 4U * 81U + 10U && text.caret == 1U * 81U + 5U, "the anchor's handle moves that end");

	/* Two fingers scroll and leave the selection. */
	test_now += SECOND;
	anchor = text.anchor;
	(void)kl_ui_touch_down(ui, 1, test_now, test_now, 300.0, 250.0);
	(void)kl_ui_touch_down(ui, 2, test_now, test_now, 400.0, 250.0);
	for (step = 1; step <= 8; step++) {
		test_now += 16667U;
		(void)kl_ui_touch_motion(ui, 1, test_now, test_now, 300.0, 250.0 - 15.0 * step);
		(void)kl_ui_touch_motion(ui, 2, test_now, test_now, 400.0, 250.0 - 15.0 * step);
		frame(ui, &nowhere, &scroll, &region, &text, state);
	}

	/* The content moved with them; they lift. */
	before = scroll.y;
	(void)kl_ui_touch_up(ui, 1, test_now, test_now);
	(void)kl_ui_touch_up(ui, 2, test_now, test_now);
	check(before > 60.0 && text.anchor == anchor, "two fingers scroll and keep the selection");
	for (step = 0; step < 200 && scroll.touched; step++) {
		test_now += 16667U;
		frame(ui, &nowhere, &scroll, &region, &text, state);
	}

	/* At rest in the end. */
	check(!scroll.touched, "the two fingers' flight rests");

	/* A long press asks for the menu at the finger. */
	test_now += SECOND;
	(void)kl_text_touch_take(&text);
	finger(ui, 1, 200.0, 150.0, 1);
	test_now += 600000U;
	frame(ui, &nowhere, &scroll, &region, &text, state);
	changes = kl_text_touch_take(&text);
	check((changes & KL_TEXT_TOUCH_MENU) != 0U && text.menu_x == 200.0 && text.menu_y == 150.0, "a long press asks for the menu");
	finger(ui, 1, 200.0, 150.0, 0);
	frame(ui, &nowhere, &scroll, &region, &text, state);
	check(!scroll.touched, "the long press lets the scroll go");

	/* A selecting finger held near the bottom edge scrolls the content by itself and the selection follows. */
	test_now += SECOND;
	kl_scroll_move_to(&scroll, 0.0, 0.0, 0, test_now);
	frame(ui, &nowhere, &scroll, &region, &text, state);
	finger(ui, 1, 50.0, 100.0, 1);
	swipe(ui, 1, 50.0, 100.0, 0.0, 190.0, 6, &scroll, &region, &text);
	before = scroll.y;
	for (step = 0; step < 30; step++) {
		test_now += 16667U;
		(void)kl_ui_touch_motion(ui, 1, test_now, test_now, 50.0, 290.0);
		frame(ui, &nowhere, &scroll, &region, &text, state);
	}

	/* The content moved under the still finger. */
	check(scroll.y > before + 100.0, "the edge scrolls the content");
	check(text.caret >= (size_t)((scroll.y + 280.0) / VIEW_LINE_HEIGHT) * 81U, "the selection follows the content under the finger");
	finger(ui, 1, 50.0, 290.0, 0);
	frame(ui, &nowhere, &scroll, &region, &text, state);

	/* A key's selection takes the handles away. */
	kl_text_touch_set_selection(&text, 3U, 9U);
	check(!text.handles && text.anchor == 3U && text.caret == 9U, "the view's own selection has no handles");

	/* The keys' characters. */
	check(kl_key_character(30U, 0U) == 'a' && kl_key_character(30U, KL_MOD_SHIFT) == 'A', "keys type characters");
	check(kl_key_character(30U, KL_MOD_CTRL) == 0U && kl_key_character(200U, 0U) == 0U, "commands and other keys type none");

	/* The scroll and the input go. */
	kl_scroll_release(&scroll);
	kl_ui_destroy(ui);
}
