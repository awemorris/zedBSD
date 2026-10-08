/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws190-p002: the host test of the fingers' selection of the library's
 * field and text area (field.c, text-area.c, text-select.c and ui.c,
 * KL_VERSION 74; plan/ws190/phase001/phase.md sections 1.1, 1.3, 2.3 and
 * 2.4), driven through kl_ui's fingers frame by frame, the window's
 * clipboard a buffer of the test's:
 *
 *   - the word (keiui_select_word) at and around its ends, on a space, and
 *     in Japanese;
 *   - a double tap selects the word and puts the field in the mode, a
 *     mouse's double click still selects the whole text;
 *   - a finger on a handle drags that end, a tap on it changes nothing;
 *   - the bar's Copy, Cut (undone by Ctrl+Z), Paste and Select All, none
 *     of its keys reaching the application;
 *   - a tap elsewhere, a key, a text the program sets and a field no
 *     longer drawn end the mode, and a field freed after its last frame is
 *     not read (ASan);
 *   - a dialog drawn over the field takes its handles away;
 *   - kl_ui_set_text_bar(…, 0) keeps a double tap's whole text;
 *   - a text area's word, a handle dragged to the next line, Select All.
 *
 *   host-touch-select FONT
 */

#include <keiland/keiland.h>

#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The frame's size. */
#define TEST_WIDTH	600
#define TEST_HEIGHT	400

/* The widgets' ids. */
#define TEST_FIELD	1U
#define TEST_AREA	2U
#define TEST_DIALOG	3U

/* The field's place, its text's left edge (FIELD_SIDE in), the text's size, and the area's place. */
#define TEST_FIELD_X	10
#define TEST_FIELD_Y	200
#define TEST_FIELD_W	300
#define TEST_FIELD_H	32
#define TEST_TEXT_X	(TEST_FIELD_X + 12)
#define TEST_TEXT_SIZE	14U
#define TEST_AREA_Y	260
#define TEST_AREA_H	100

/* The evdev codes of the keys the test presses: Z, A and an ordinary letter. */
#define TEST_KEY_Z	44U
#define TEST_KEY_Q	16U

/*
 * The test's page: its pixels, canvas, font and style, the window's input,
 * the widgets (the field on the heap, so that a field freed after its last
 * frame is seen by ASan), what is drawn, and the clock of the fingers.  It
 * lives for the whole run.
 */
struct test_page {
	uint32_t pixels[TEST_WIDTH * TEST_HEIGHT];
	struct kl_canvas canvas;
	struct kl_text text;
	struct kl_style style;
	struct kl_ui *ui;
	struct kl_field *field;
	struct kl_text_area area;
	int show_field;
	int show_area;
	int show_dialog;
	uint64_t now_us;
	unsigned keys_heard;
};

/* The page. */
static struct test_page test_page;

/* The clipboard's text and its length, written by the stand-in copy. */
static char test_clipboard[1024];
static size_t test_clipboard_length;

/* The checks that failed, and all the checks. */
static unsigned test_failed;
static unsigned test_count;

unsigned kl_appearance_get(const struct kl_appearance *appearance);
static void check(int condition, const char *what);
static void frame(void);
static void tick(unsigned milliseconds);
static void tap(double x, double y);
static void drag(double x1, double y1, double x2, double y2);
static double text_x(const char *text, size_t length);
static void handle_centre(size_t position, double *x, double *y);
static int bar_cell(unsigned button, double *x, double *y);
static void test_copy(struct kl_window *window, const char *text, size_t length);
static size_t test_paste(struct kl_window *window, char *text, size_t size);
static int test_can_paste(const struct kl_window *window);
static void test_words(void);
static void test_field_select(void);
static void test_field_bar(void);
static void test_field_end(void);
static void test_field_off(void);
static void test_area_select(void);

/*
 * Runs the checks; the exit status says whether they all held.
 */
int
main(
	int argc,
	char **argv)
{
	int error;

	/* The font. */
	if (argc < 2) {
		fprintf(stderr, "usage: host-touch-select FONT\n");
		return 2;
	}

	/* The font opened. */
	error = kl_text_open(&test_page.text, argv[1], NULL);
	if (error != 0) {
		fprintf(stderr, "host-touch-select: %s: error %d\n", argv[1], error);
		return 2;
	}

	/* The canvas, the style, the field and the window's input, tied to the stand-in clipboard. */
	error = kl_canvas_init(&test_page.canvas, test_page.pixels, TEST_WIDTH, TEST_WIDTH, TEST_HEIGHT);
	if (error != 0)
		return 2;
	test_page.style.canvas = &test_page.canvas;
	test_page.style.text = &test_page.text;
	test_page.style.theme = kl_theme_default();
	test_page.field = calloc(1, sizeof(*test_page.field));
	if (test_page.field == NULL)
		return 2;
	test_page.ui = kl_ui_create();
	if (test_page.ui == NULL)
		return 2;
	keiui_ui_set_window(test_page.ui, (struct kl_window *)(void *)&test_page, test_copy, test_paste);
	keiui_ui_set_window_extras(test_page.ui, test_can_paste, NULL);
	test_page.now_us = 1000000U;
	test_page.show_field = 1;

	/* The checks. */
	test_words();
	test_field_select();
	test_field_bar();
	test_field_end();
	test_field_off();
	test_area_select();

	/* Reports the checks. */
	kl_ui_destroy(test_page.ui);
	free(test_page.field);
	kl_text_close(&test_page.text);
	printf("host-touch-select: %u of %u held\n", test_count - test_failed, test_count);
	if (test_failed != 0U) {
		printf("host-touch-select: FAIL\n");
		return 1;
	}

	/* Succeeded: every check held. */
	printf("host-touch-select: PASS\n");
	return 0;
}

/*
 * Reports the light appearance: the host has no desktop to ask (the
 * library's own, appearance.c, needs Wayland).
 */
unsigned
kl_appearance_get(
	const struct kl_appearance *appearance)
{
	(void)appearance;

	/* The light appearance. */
	return KL_APPEARANCE_LIGHT;
}

/* Counts a check, and says one that does not hold. */
static void
check(
	int condition,
	const char *what)
{
	/* Counted. */
	test_count++;
	if (condition) {
		printf("ok: %s\n", what);
		return;
	}

	/* A failure. */
	test_failed++;
	printf("FAIL: %s\n", what);
}

/* Draws one frame: the field, the area and a dialog over the field, as the page shows them; counts the keys the application hears. */
static void
frame(void)
{
	static const struct kl_rect field = { TEST_FIELD_X, TEST_FIELD_Y, TEST_FIELD_W, TEST_FIELD_H };
	static const struct kl_rect area = { TEST_FIELD_X, TEST_AREA_Y, TEST_FIELD_W, TEST_AREA_H };
	static const struct kl_rect dialog = { 0, 150, TEST_WIDTH, 120 };
	struct kl_event event;
	int taken;

	/* The widgets, the dialog last. */
	kl_ui_begin(test_page.ui, test_page.now_us);
	if (test_page.show_field)
		(void)kl_field(test_page.ui, &test_page.style, TEST_FIELD, &field, test_page.field, "Field");
	if (test_page.show_area)
		(void)kl_text_area(test_page.ui, &test_page.style, TEST_AREA, &area, &test_page.area, "Area");
	if (test_page.show_dialog)
		(void)keiui_ui_widget(test_page.ui, TEST_DIALOG, 0U, &dialog, KEIUI_MODAL);
	(void)kl_ui_end(test_page.ui, test_page.now_us);

	/* What no widget took: the keys the application hears. */
	for (;;) {
		taken = kl_ui_take(test_page.ui, &event);
		if (!taken)
			break;
		if (event.kind == KL_EVENT_KEY)
			test_page.keys_heard++;
	}
}

/* Moves the clock on. */
static void
tick(
	unsigned milliseconds)
{
	/* Later. */
	test_page.now_us += (uint64_t)milliseconds * 1000U;
}

/* A finger's tap at a point, with a frame after it. */
static void
tap(
	double x,
	double y)
{
	/* Down, a moment, up. */
	(void)kl_ui_touch_down(test_page.ui, 1, test_page.now_us, test_page.now_us, x, y);
	tick(40U);
	(void)kl_ui_touch_up(test_page.ui, 1, test_page.now_us, test_page.now_us);
	frame();
	tick(100U);
}

/* A finger's drag from a point to another, a frame at each step. */
static void
drag(
	double x1,
	double y1,
	double x2,
	double y2)
{
	int step;
	double x;
	double y;

	/* Down. */
	(void)kl_ui_touch_down(test_page.ui, 2, test_page.now_us, test_page.now_us, x1, y1);
	frame();

	/* Eight steps along the line, a frame after each. */
	for (step = 1; step <= 8; step++) {
		tick(16U);
		x = x1 + (x2 - x1) * (double)step / 8.0;
		y = y1 + (y2 - y1) * (double)step / 8.0;
		(void)kl_ui_touch_motion(test_page.ui, 2, test_page.now_us, test_page.now_us, x, y);
		frame();
	}

	/* Still a moment, then up. */
	tick(100U);
	frame();
	(void)kl_ui_touch_up(test_page.ui, 2, test_page.now_us, test_page.now_us);
	frame();
	tick(100U);
}

/* Reports where a text's first bytes end across the field, in the window. */
static double
text_x(
	const char *text,
	size_t length)
{
	int width;

	/* The width of the bytes, from the text's left edge. */
	width = kl_text_width(&test_page.text, text, length, TEST_TEXT_SIZE, 0);

	/* Reports the place. */
	return (double)(TEST_TEXT_X + width);
}

/* Gives the window's point of the knob under a position of the selection's widget. */
static void
handle_centre(
	size_t position,
	double *x,
	double *y)
{
	struct keiui_select *select;
	struct kl_rect place;

	/* The caret's rectangle in the widget's content, moved into the window, and the knob under it. */
	select = keiui_ui_select(test_page.ui);
	select->touch.view->caret_rect(select->touch.data, position, &place);
	*x = (double)select->box.x - select->scroll.x + (double)place.x;
	*y = (double)select->box.y - select->scroll.y + (double)(place.y + place.height) + (double)KL_TEXT_HANDLE / 2.0;
}

/*
 * Gives the window's point in the middle of a button of the selection's
 * bar, laid out as kl_ui lays it out (the canvas for its bounds).  Returns
 * 1 when the bar has that button.
 */
static int
bar_cell(
	unsigned button,
	double *x,
	double *y)
{
	struct keiui_select *select;
	struct kl_text_bar bar;
	struct kl_rect first;
	struct kl_rect second;
	struct kl_rect selection;
	struct kl_rect visible;
	struct kl_rect bounds;
	unsigned facts;
	size_t start;
	size_t end;
	size_t length;
	size_t index;
	int shown;

	/* The selection's ends in order, and the text's length. */
	select = keiui_ui_select(test_page.ui);
	start = select->touch.anchor;
	end = select->touch.caret;
	if (start > end) {
		start = select->touch.caret;
		end = select->touch.anchor;
	}

	/* The text's length. */
	length = select->field.length;
	if (select->kind == KEIUI_SELECT_AREA)
		length = select->area.length;

	/* What the text is, as kl_ui sees it. */
	facts = KL_TEXT_BAR_CLIPBOARD;
	if (start != end)
		facts |= KL_TEXT_BAR_SELECTED;
	if (start == 0U && end == length && length != 0U)
		facts |= KL_TEXT_BAR_WHOLE;
	if (length == 0U)
		facts |= KL_TEXT_BAR_EMPTY;
	if (select->kind == KEIUI_SELECT_FIELD && select->field.secret)
		facts |= KL_TEXT_BAR_SECRET;
	if (test_clipboard_length != 0U)
		facts |= KL_TEXT_BAR_CAN_PASTE;

	/* The selection in the window, one line or across the box. */
	select->touch.view->caret_rect(select->touch.data, start, &first);
	select->touch.view->caret_rect(select->touch.data, end, &second);
	selection.x = (int)((double)select->box.x - select->scroll.x + (double)first.x);
	selection.y = (int)((double)select->box.y - select->scroll.y + (double)first.y);
	selection.width = second.x - first.x;
	selection.height = first.height;
	if (second.y != first.y) {
		selection.x = select->box.x;
		selection.width = select->box.width;
		selection.height = second.y + second.height - first.y;
	}

	/* Laid out in the canvas. */
	shown = keiui_rect_intersect(&select->clip, &select->box, &visible);
	if (!shown)
		return 0;
	bounds.x = 0;
	bounds.y = 0;
	bounds.width = TEST_WIDTH;
	bounds.height = TEST_HEIGHT;
	shown = kl_text_bar_layout(&bar, &test_page.text, kl_text_bar_buttons(facts), &selection, &visible, &bounds);
	if (!shown)
		return 0;

	/* The button's cell. */
	for (index = 0; index < bar.count; index++) {
		if (bar.kinds[index] != button)
			continue;
		*x = (double)bar.cells[index].x + (double)bar.cells[index].width / 2.0;
		*y = (double)bar.cells[index].y + (double)bar.cells[index].height / 2.0;
		return 1;
	}

	/* No such button. */
	return 0;
}

/* The stand-in for the window's copy: the test's buffer. */
static void
test_copy(
	struct kl_window *window,
	const char *text,
	size_t length)
{
	(void)window;

	/* The text, as much as fits. */
	if (length > sizeof(test_clipboard) - 1U)
		length = sizeof(test_clipboard) - 1U;
	memcpy(test_clipboard, text, length);
	test_clipboard[length] = '\0';
	test_clipboard_length = length;
}

/* The stand-in for the window's paste: the test's buffer. */
static size_t
test_paste(
	struct kl_window *window,
	char *text,
	size_t size)
{
	size_t length;

	/* The window is the test's. */
	(void)window;

	/* As much as fits. */
	length = test_clipboard_length;
	if (length > size)
		length = size;
	memcpy(text, test_clipboard, length);

	/* Reports the bytes. */
	return length;
}

/* The stand-in for whether the clipboard has text. */
static int
test_can_paste(
	const struct kl_window *window)
{
	(void)window;

	/* Text when the buffer has some. */
	if (test_clipboard_length != 0U)
		return 1;
	return 0;
}

/* The word of the fingers' selection (section 2.4). */
static void
test_words(void)
{
	static const char text[] = "hello world";
	static const char japanese[] = "abc\xe6\x97\xa5\xe6\x9c\xac\xe3\x80\x81\xe3\x81\xa7\xe3\x81\x99";
	size_t start;
	size_t end;

	/* Inside "hello", and at its end (the word left of a space). */
	keiui_select_word(text, 11U, 2U, &start, &end);
	check(start == 0U && end == 5U, "word: inside hello");
	keiui_select_word(text, 11U, 5U, &start, &end);
	check(start == 0U && end == 5U, "word: at hello's end, not both words");

	/* At "world"'s start and the text's end. */
	keiui_select_word(text, 11U, 6U, &start, &end);
	check(start == 6U && end == 11U, "word: at world's start");
	keiui_select_word(text, 11U, 11U, &start, &end);
	check(start == 6U && end == 11U, "word: at the text's end");

	/* Between two spaces: nothing. */
	keiui_select_word("a  b", 4U, 2U, &start, &end);
	check(start == 2U && end == 2U, "word: none between two spaces");

	/* ASCII and Japanese are two words; the Japanese comma parts words. */
	keiui_select_word(japanese, sizeof(japanese) - 1U, 1U, &start, &end);
	check(start == 0U && end == 3U, "word: abc apart from the Japanese");
	keiui_select_word(japanese, sizeof(japanese) - 1U, 6U, &start, &end);
	check(start == 3U && end == 9U, "word: the Japanese up to its comma");
	keiui_select_word(japanese, sizeof(japanese) - 1U, 13U, &start, &end);
	check(start == 12U && end == 18U, "word: the Japanese after its comma");
}

/* A double tap's word and the handles (sections 1.1 and 2.3). */
static void
test_field_select(void)
{
	struct keiui_select *select;
	double x;
	double y;
	int owned;

	/* "hello world" in the field, which has the keyboard. */
	kl_field_set(test_page.field, "hello world");
	kl_ui_set_focus(test_page.ui, TEST_FIELD, 0U);
	frame();

	/* A double tap in "world": its word, in the fingers' selection. */
	x = text_x("hello wo", 8U);
	y = (double)(TEST_FIELD_Y + TEST_FIELD_H / 2);
	tap(x, y);
	tap(x, y);
	owned = keiui_ui_select_owned(test_page.ui, TEST_FIELD, 0U);
	check(owned, "field: a double tap puts the field in the fingers' selection");
	check(test_page.field->anchor == 6U && test_page.field->caret == 11U, "field: the word selected");
	select = keiui_ui_select(test_page.ui);
	check(select->touch.handles == 1 && select->touch.bar == 1, "field: handles and the bar");

	/* A tap on the anchor's knob changes nothing. */
	handle_centre(6U, &x, &y);
	tap(x, y);
	check(test_page.field->anchor == 6U && test_page.field->caret == 11U, "field: a tap on a handle keeps the selection");
	owned = keiui_ui_select_owned(test_page.ui, TEST_FIELD, 0U);
	check(owned, "field: a tap on a handle keeps the mode");

	/* The anchor's knob dragged to the text's start: the ends change places. */
	drag(x, y, (double)TEST_TEXT_X - 2.0, y);
	check(test_page.field->anchor == 11U && test_page.field->caret == 0U, "field: the start's handle dragged to the start");
	check(select->touch.bar == 1, "field: the bar back after the drag");
}

/* The bar's commands (section 1.3). */
static void
test_field_bar(void)
{
	struct keiui_select *select;
	double x;
	double y;
	int found;
	int owned;

	/* Copy: the whole text to the clipboard, the bar hidden, the handles kept. */
	select = keiui_ui_select(test_page.ui);
	test_page.keys_heard = 0;
	found = bar_cell(KL_TEXT_BAR_COPY, &x, &y);
	check(found, "bar: Copy is there");
	tap(x, y);
	frame();
	check(strcmp(test_clipboard, "hello world") == 0, "bar: Copy copied the selection");
	check(select->touch.bar == 0 && select->touch.handles == 1, "bar: Copy hid the bar, kept the handles");
	owned = keiui_ui_select_owned(test_page.ui, TEST_FIELD, 0U);
	check(owned, "bar: Copy kept the mode");

	/* A tap within the selection shows the bar again. */
	tap(text_x("hel", 3U), (double)(TEST_FIELD_Y + TEST_FIELD_H / 2));
	check(select->touch.bar == 1, "bar: a tap in the selection shows the bar");

	/* A double tap on "hello", then Cut: gone from the text, in the clipboard, the mode over; Ctrl+Z brings it back. */
	x = text_x("hell", 4U);
	y = (double)(TEST_FIELD_Y + TEST_FIELD_H / 2);
	tap(x, y);
	tap(x, y);
	check(test_page.field->anchor == 0U && test_page.field->caret == 5U, "bar: hello selected");
	found = bar_cell(KL_TEXT_BAR_CUT, &x, &y);
	check(found, "bar: Cut is there");
	tap(x, y);
	frame();
	check(strcmp(test_page.field->text, " world") == 0 && strcmp(test_clipboard, "hello") == 0, "bar: Cut cut hello");
	owned = keiui_ui_select_owned(test_page.ui, TEST_FIELD, 0U);
	check(!owned, "bar: Cut ended the mode");
	(void)kl_ui_key(test_page.ui, TEST_KEY_Z, 1, KL_MOD_CTRL);
	(void)kl_ui_key(test_page.ui, TEST_KEY_Z, 0, KL_MOD_CTRL);
	frame();
	check(strcmp(test_page.field->text, "hello world") == 0, "bar: Ctrl+Z undid the cut");

	/* A double tap on "world", then Paste: hello in its place. */
	x = text_x("hello wo", 8U);
	y = (double)(TEST_FIELD_Y + TEST_FIELD_H / 2);
	tap(x, y);
	tap(x, y);
	found = bar_cell(KL_TEXT_BAR_PASTE, &x, &y);
	check(found, "bar: Paste is there");
	tap(x, y);
	frame();
	check(strcmp(test_page.field->text, "hello hello") == 0, "bar: Paste put the clipboard in place of world");

	/* A double tap, then Select All: the whole text with the bar. */
	x = text_x("he", 2U);
	y = (double)(TEST_FIELD_Y + TEST_FIELD_H / 2);
	tap(x, y);
	tap(x, y);
	found = bar_cell(KL_TEXT_BAR_SELECT_ALL, &x, &y);
	check(found, "bar: Select All is there");
	tap(x, y);
	frame();
	check(test_page.field->anchor == 0U && test_page.field->caret == 11U, "bar: Select All selected the whole text");
	check(select->touch.bar == 1 && select->touch.handles == 1, "bar: Select All kept the bar and the handles");

	/* None of the bar's keys reached the application. */
	check(test_page.keys_heard == 0U, "bar: no key of the bar's reached the application");
}

/* What ends the fingers' selection (section 1.1). */
static void
test_field_end(void)
{
	struct kl_field *old;
	double x;
	double y;
	int owned;

	/* A tap off the field ends it, the selection kept. */
	tap(500.0, 50.0);
	owned = keiui_ui_select_owned(test_page.ui, TEST_FIELD, 0U);
	check(!owned, "end: a tap elsewhere");

	/* A key ends it. */
	kl_ui_set_focus(test_page.ui, TEST_FIELD, 0U);
	frame();
	x = text_x("he", 2U);
	y = (double)(TEST_FIELD_Y + TEST_FIELD_H / 2);
	tap(x, y);
	tap(x, y);
	(void)kl_ui_key(test_page.ui, TEST_KEY_Q, 1, 0U);
	(void)kl_ui_key(test_page.ui, TEST_KEY_Q, 0, 0U);
	frame();
	owned = keiui_ui_select_owned(test_page.ui, TEST_FIELD, 0U);
	check(!owned, "end: a key");

	/* A text the program sets ends it. */
	kl_field_set(test_page.field, "hello world");
	frame();
	tap(x, y);
	tap(x, y);
	kl_field_set(test_page.field, "another text");
	frame();
	owned = keiui_ui_select_owned(test_page.ui, TEST_FIELD, 0U);
	check(!owned, "end: a text the program set");

	/* A dialog drawn over the field: its handle no longer drags. */
	kl_field_set(test_page.field, "hello world");
	frame();
	tap(x, y);
	tap(x, y);
	handle_centre(0U, &x, &y);
	test_page.show_dialog = 1;
	frame();
	drag(x, y, x + 60.0, y);
	check(test_page.field->anchor == 0U && test_page.field->caret == 5U, "end: no handle under a dialog");
	test_page.show_dialog = 0;
	frame();

	/* A field freed after its last frame: a drag from its handle reads nothing of it, and the mode ends. */
	old = test_page.field;
	test_page.field = calloc(1, sizeof(*test_page.field));
	if (test_page.field == NULL)
		return;
	free(old);
	test_page.show_field = 0;
	(void)kl_ui_touch_down(test_page.ui, 3, test_page.now_us, test_page.now_us, x, y);
	tick(16U);
	(void)kl_ui_touch_motion(test_page.ui, 3, test_page.now_us, test_page.now_us, x + 40.0, y);
	tick(16U);
	(void)kl_ui_touch_motion(test_page.ui, 3, test_page.now_us, test_page.now_us, x + 80.0, y);
	frame();
	(void)kl_ui_touch_up(test_page.ui, 3, test_page.now_us, test_page.now_us);
	frame();
	owned = keiui_ui_select_owned(test_page.ui, TEST_FIELD, 0U);
	check(!owned, "end: a field no longer drawn");
	test_page.show_field = 1;
}

/* The program turns the fingers' selection off (kl_ui_set_text_bar). */
static void
test_field_off(void)
{
	double x;
	double y;
	int owned;

	/* A double tap then selects the whole text, as a click's. */
	kl_field_set(test_page.field, "hello world");
	kl_ui_set_focus(test_page.ui, TEST_FIELD, 0U);
	kl_ui_set_text_bar(test_page.ui, NULL, 0);
	frame();
	x = text_x("he", 2U);
	y = (double)(TEST_FIELD_Y + TEST_FIELD_H / 2);
	tap(x, y);
	tap(x, y);
	owned = keiui_ui_select_owned(test_page.ui, TEST_FIELD, 0U);
	check(!owned && test_page.field->anchor == 0U && test_page.field->caret == 11U, "off: a double tap selects the whole text");

	/* A mouse's double click does the same with the selection on. */
	kl_ui_set_text_bar(test_page.ui, NULL, 1);
	(void)kl_ui_pointer_motion(test_page.ui, x, y);
	(void)kl_ui_pointer_button(test_page.ui, 1, test_page.now_us);
	(void)kl_ui_pointer_button(test_page.ui, 0, test_page.now_us);
	frame();
	tick(100U);
	(void)kl_ui_pointer_button(test_page.ui, 1, test_page.now_us);
	(void)kl_ui_pointer_button(test_page.ui, 0, test_page.now_us);
	frame();
	owned = keiui_ui_select_owned(test_page.ui, TEST_FIELD, 0U);
	check(!owned && test_page.field->anchor == 0U && test_page.field->caret == 11U, "off: a mouse's double click selects the whole text");
}

/* A text area's word, a handle dragged to the next line, and Select All. */
static void
test_area_select(void)
{
	struct keiui_select *select;
	double x;
	double y;
	double end_x;
	double end_y;
	int found;
	int owned;

	/* Two lines in the area, which has the keyboard. */
	test_page.show_field = 0;
	test_page.show_area = 1;
	kl_text_area_set(&test_page.area, "one two\nthree");
	kl_ui_set_focus(test_page.ui, TEST_AREA, 0U);
	frame();

	/* A double tap on "two". */
	x = (double)(TEST_FIELD_X + 12) + (double)kl_text_width(&test_page.text, "one t", 5U, TEST_TEXT_SIZE, 0);
	y = (double)(TEST_AREA_Y + 8 + 10);
	tap(x, y);
	tap(x, y);
	owned = keiui_ui_select_owned(test_page.ui, TEST_AREA, 0U);
	check(owned, "area: a double tap puts the area in the fingers' selection");
	check(test_page.area.anchor == 4U && test_page.area.caret == 7U, "area: two selected");

	/* The end's knob dragged past "three" on the next line. */
	handle_centre(7U, &x, &y);
	end_x = (double)(TEST_FIELD_X + 12) + (double)kl_text_width(&test_page.text, "three", 5U, TEST_TEXT_SIZE, 0) + 4.0;
	end_y = y + 20.0;
	drag(x, y, end_x, end_y);
	check(test_page.area.anchor == 4U && test_page.area.caret == 13U, "area: the end dragged to the next line's end");

	/* Select All. */
	select = keiui_ui_select(test_page.ui);
	found = bar_cell(KL_TEXT_BAR_SELECT_ALL, &x, &y);
	check(found, "area: Select All is there");
	tap(x, y);
	frame();
	check(test_page.area.anchor == 0U && test_page.area.caret == 13U && select->touch.bar == 1, "area: Select All");
}
