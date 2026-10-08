/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws190-p002: the host test of the bar of editing buttons and of the
 * fingers' selection's bar (libkeiland's text-bar.c and text-touch.c with
 * ui.c, KL_VERSION 74; plan/ws190/phase001/phase.md sections 1.2 to 1.4,
 * 2.1 and 2.2):
 *
 *   - kl_text_bar_buttons chooses Cut, Copy, Paste and Select All from
 *     what the text is;
 *   - kl_text_bar_layout puts the bar above the selection, below it, over
 *     it, within the bounds' sides, and none when the selection is out of
 *     sight or the bounds are too low;
 *   - kl_text_touch shows the bar after a double tap and after a drag's
 *     selection, hides it while a finger drags, after a tap, the keys'
 *     selection and a long press, and keiui_text_touch_hold drags a named
 *     end;
 *   - kl_text_bar_hit reports a finger's tap and a click on a button, a
 *     press on the bar keeps the keyboard's focus, and a drag that starts
 *     on it does nothing.
 *
 *   host-text-bar FONT
 */

#include <keiland/keiland.h>

#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The frame's size. */
#define TEST_WIDTH	600
#define TEST_HEIGHT	400

/* The widgets' ids: a field that has the focus, and the bar. */
#define TEST_FIELD	1U
#define TEST_BAR	2U

/* A content's character width and line height for the view of test_view. */
#define TEST_CHAR	10.0
#define TEST_LINE	20

/*
 * The test's page: its pixels, canvas, font and style, and the window's
 * input.  It lives for the whole run.
 */
struct test_page {
	uint32_t pixels[TEST_WIDTH * TEST_HEIGHT];
	struct kl_canvas canvas;
	struct kl_text text;
	struct kl_style style;
	struct kl_ui *ui;
	struct kl_field field;
};

/* The page. */
static struct test_page test_page;

/* The checks that failed, and all the checks. */
static unsigned test_failed;
static unsigned test_count;

/* The text the view of test_view stands for: one line, a character every TEST_CHAR pixels. */
static const char test_text[] = "hello world";

unsigned kl_appearance_get(const struct kl_appearance *appearance);
static void check(int condition, const char *what);
static void test_buttons(void);
static void test_layout(void);
static void test_touch(void);
static void test_hit(void);
static unsigned frame(const struct kl_text_bar *bar, unsigned *held);
static size_t view_position(void *data, double x, double y);
static void view_caret(void *data, size_t position, struct kl_rect *rect);
static void view_word(void *data, size_t position, size_t *start, size_t *end);

/* The view of one line of test_text. */
static const struct kl_text_view test_view = {
	view_position,
	view_caret,
	view_word
};

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
		fprintf(stderr, "usage: host-text-bar FONT\n");
		return 2;
	}

	/* The font opened. */
	error = kl_text_open(&test_page.text, argv[1], NULL);
	if (error != 0) {
		fprintf(stderr, "host-text-bar: %s: error %d\n", argv[1], error);
		return 2;
	}

	/* The canvas, the style and the window's input. */
	error = kl_canvas_init(&test_page.canvas, test_page.pixels, TEST_WIDTH, TEST_WIDTH, TEST_HEIGHT);
	if (error != 0)
		return 2;
	test_page.style.canvas = &test_page.canvas;
	test_page.style.text = &test_page.text;
	test_page.style.theme = kl_theme_default();
	test_page.ui = kl_ui_create();
	if (test_page.ui == NULL)
		return 2;

	/* The checks. */
	test_buttons();
	test_layout();
	test_touch();
	test_hit();

	/* Reports the checks. */
	kl_ui_destroy(test_page.ui);
	kl_text_close(&test_page.text);
	printf("host-text-bar: %u of %u held\n", test_count - test_failed, test_count);
	if (test_failed != 0U) {
		printf("host-text-bar: FAIL\n");
		return 1;
	}

	/* Succeeded: every check held. */
	printf("host-text-bar: PASS\n");
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

/* The buttons chosen from what the text is (section 1.2). */
static void
test_buttons(void)
{
	unsigned buttons;
	unsigned all;

	/* A selection in a plain text with a clipboard that has text: all four. */
	all = KL_TEXT_BAR_CUT | KL_TEXT_BAR_COPY | KL_TEXT_BAR_PASTE | KL_TEXT_BAR_SELECT_ALL;
	buttons = kl_text_bar_buttons(KL_TEXT_BAR_SELECTED | KL_TEXT_BAR_CLIPBOARD | KL_TEXT_BAR_CAN_PASTE);
	check(buttons == all, "buttons: a selection with a full clipboard has all four");

	/* No selection: no Cut or Copy. */
	buttons = kl_text_bar_buttons(KL_TEXT_BAR_CLIPBOARD | KL_TEXT_BAR_CAN_PASTE);
	check(buttons == (KL_TEXT_BAR_PASTE | KL_TEXT_BAR_SELECT_ALL), "buttons: a caret has Paste and Select All");

	/* An empty clipboard: no Paste. */
	buttons = kl_text_bar_buttons(KL_TEXT_BAR_SELECTED | KL_TEXT_BAR_CLIPBOARD);
	check(buttons == (KL_TEXT_BAR_CUT | KL_TEXT_BAR_COPY | KL_TEXT_BAR_SELECT_ALL), "buttons: an empty clipboard has no Paste");

	/* A secret field: no Cut or Copy, Paste still. */
	buttons = kl_text_bar_buttons(KL_TEXT_BAR_SELECTED | KL_TEXT_BAR_SECRET | KL_TEXT_BAR_CLIPBOARD | KL_TEXT_BAR_CAN_PASTE);
	check(buttons == (KL_TEXT_BAR_PASTE | KL_TEXT_BAR_SELECT_ALL), "buttons: a secret field has Paste and Select All");

	/* A read-only text: Copy, no Cut or Paste. */
	buttons = kl_text_bar_buttons(KL_TEXT_BAR_SELECTED | KL_TEXT_BAR_READ_ONLY | KL_TEXT_BAR_CLIPBOARD | KL_TEXT_BAR_CAN_PASTE);
	check(buttons == (KL_TEXT_BAR_COPY | KL_TEXT_BAR_SELECT_ALL), "buttons: a read-only text has Copy and Select All");

	/* The whole text selected: no Select All. */
	buttons = kl_text_bar_buttons(KL_TEXT_BAR_SELECTED | KL_TEXT_BAR_WHOLE | KL_TEXT_BAR_CLIPBOARD);
	check(buttons == (KL_TEXT_BAR_CUT | KL_TEXT_BAR_COPY), "buttons: the whole text selected has no Select All");

	/* No window's clipboard: Select All alone. */
	buttons = kl_text_bar_buttons(KL_TEXT_BAR_SELECTED | KL_TEXT_BAR_CAN_PASTE);
	check(buttons == KL_TEXT_BAR_SELECT_ALL, "buttons: without a clipboard only Select All");

	/* An empty text and nothing to paste: no button. */
	buttons = kl_text_bar_buttons(KL_TEXT_BAR_EMPTY | KL_TEXT_BAR_CLIPBOARD);
	check(buttons == 0U, "buttons: an empty text and clipboard have none");
}

/* The bar's place (section 1.4). */
static void
test_layout(void)
{
	static const struct kl_rect bounds = { 0, 0, TEST_WIDTH, TEST_HEIGHT };
	struct kl_text_bar bar;
	struct kl_rect selection;
	struct kl_rect visible;
	struct kl_rect narrow;
	unsigned all;
	int shown;
	int width;

	/* A selection in the middle: above it, across its middle. */
	all = KL_TEXT_BAR_CUT | KL_TEXT_BAR_COPY | KL_TEXT_BAR_PASTE | KL_TEXT_BAR_SELECT_ALL;
	selection.x = 250;
	selection.y = 200;
	selection.width = 100;
	selection.height = 20;
	visible = bounds;
	shown = kl_text_bar_layout(&bar, &test_page.text, all, &selection, &visible, &bounds);
	check(shown == 1 && bar.count == 4U, "layout: four buttons laid out");
	check(bar.rect.y == 200 - KL_TEXT_BAR_GAP - KL_TEXT_BAR_HEIGHT, "layout: above the selection");
	check(bar.rect.x + bar.rect.width / 2 >= 299 && bar.rect.x + bar.rect.width / 2 <= 301, "layout: across the selection's middle");
	check(bar.kinds[0] == KL_TEXT_BAR_CUT && bar.kinds[3] == KL_TEXT_BAR_SELECT_ALL, "layout: Cut first, Select All last");
	check(bar.cells[1].x == bar.cells[0].x + bar.cells[0].width, "layout: the cells follow each other");
	width = bar.rect.width;

	/* No room above: below, past the knobs. */
	selection.y = 10;
	shown = kl_text_bar_layout(&bar, &test_page.text, all, &selection, &visible, &bounds);
	check(shown == 1 && bar.rect.y == 10 + 20 + KL_TEXT_HANDLE + KL_TEXT_BAR_GAP, "layout: below a selection at the top");

	/* No room above or below: over the selection's top. */
	selection.y = 20;
	selection.height = 360;
	shown = kl_text_bar_layout(&bar, &test_page.text, all, &selection, &visible, &bounds);
	check(shown == 1 && bar.rect.y == 20 + KL_TEXT_BAR_GAP, "layout: over a selection as tall as the bounds");

	/* At the left and right edges: kept within the bounds. */
	selection.y = 200;
	selection.height = 20;
	selection.x = 0;
	selection.width = 10;
	shown = kl_text_bar_layout(&bar, &test_page.text, all, &selection, &visible, &bounds);
	check(shown == 1 && bar.rect.x == KL_TEXT_BAR_GAP, "layout: kept off the left edge");
	selection.x = TEST_WIDTH - 10;
	shown = kl_text_bar_layout(&bar, &test_page.text, all, &selection, &visible, &bounds);
	check(shown == 1 && bar.rect.x + width == TEST_WIDTH - KL_TEXT_BAR_GAP, "layout: kept off the right edge");

	/* Bounds narrower than the bar: from their left edge. */
	narrow.x = 50;
	narrow.y = 0;
	narrow.width = width - 10;
	narrow.height = TEST_HEIGHT;
	selection.x = 100;
	shown = kl_text_bar_layout(&bar, &test_page.text, all, &selection, &visible, &narrow);
	check(shown == 1 && bar.rect.x == 50, "layout: wider than the bounds starts at their left");

	/* Bounds too low: no bar. */
	narrow.x = 0;
	narrow.y = 0;
	narrow.width = TEST_WIDTH;
	narrow.height = KL_TEXT_BAR_HEIGHT + KL_TEXT_BAR_GAP;
	shown = kl_text_bar_layout(&bar, &test_page.text, all, &selection, &visible, &narrow);
	check(shown == 0 && bar.count == 0U, "layout: no bar in bounds too low");

	/* The keyboard covers the bottom: a selection near it gets the bar above it. */
	narrow.height = 250;
	selection.y = 230;
	shown = kl_text_bar_layout(&bar, &test_page.text, all, &selection, &visible, &narrow);
	check(shown == 1 && bar.rect.y + KL_TEXT_BAR_HEIGHT <= 230, "layout: above a selection near the keyboard");

	/* The selection out of sight: no bar. */
	visible.x = 0;
	visible.y = 0;
	visible.width = TEST_WIDTH;
	visible.height = 100;
	selection.y = 200;
	shown = kl_text_bar_layout(&bar, &test_page.text, all, &selection, &visible, &bounds);
	check(shown == 0, "layout: no bar for a selection out of sight");

	/* Half in sight: by the part in sight. */
	selection.y = 90;
	selection.height = 40;
	shown = kl_text_bar_layout(&bar, &test_page.text, all, &selection, &visible, &bounds);
	check(shown == 1 && bar.rect.y == 90 - KL_TEXT_BAR_GAP - KL_TEXT_BAR_HEIGHT, "layout: by the part of the selection in sight");

	/* A caret (no width) has its bar too. */
	selection.width = 0;
	shown = kl_text_bar_layout(&bar, &test_page.text, KL_TEXT_BAR_PASTE, &selection, &visible, &bounds);
	check(shown == 1 && bar.count == 1U, "layout: a caret's bar with Paste alone");

	/* No button: no bar. */
	shown = kl_text_bar_layout(&bar, &test_page.text, 0U, &selection, &visible, &bounds);
	check(shown == 0, "layout: no button, no bar");
}

/* The touch's bar (section 2.1). */
static void
test_touch(void)
{
	struct kl_text_touch touch;
	unsigned changes;

	/* A double tap on "world": the word, its handles and the bar. */
	kl_text_touch_init(&touch, &test_view, NULL);
	kl_text_touch_tap(&touch, 7.0 * TEST_CHAR, 5.0, 1);
	changes = kl_text_touch_take(&touch);
	check(touch.anchor == 6U && touch.caret == 11U, "touch: a double tap selects the word");
	check(touch.handles == 1 && touch.bar == 1, "touch: the word has its handles and the bar");
	check((changes & KL_TEXT_TOUCH_BAR) != 0U, "touch: the bar's coming is told");

	/* A drag hides it; its end shows it again over the selection. */
	kl_text_touch_drag_begin(&touch, 30.0, -40.0);
	check(touch.bar == 0, "touch: a drag hides the bar");
	kl_text_touch_drag(&touch, 110.0, 5.0);
	kl_text_touch_drag_end(&touch);
	check(touch.bar == 1 && touch.handles == 1, "touch: a drag's selection shows the bar");

	/* A drag that leaves a caret has no bar. */
	kl_text_touch_drag_begin(&touch, 30.0, -40.0);
	kl_text_touch_drag_end(&touch);
	check(touch.bar == 0, "touch: a drag's caret has no bar");

	/* A tap, the keys' selection and a long press take it away. */
	kl_text_touch_tap(&touch, 7.0 * TEST_CHAR, 5.0, 1);
	kl_text_touch_tap(&touch, 20.0, 5.0, 0);
	check(touch.bar == 0, "touch: a tap takes the bar away");
	kl_text_touch_tap(&touch, 7.0 * TEST_CHAR, 5.0, 1);
	kl_text_touch_set_selection(&touch, 0U, 3U);
	check(touch.bar == 0, "touch: the keys' selection takes the bar away");
	kl_text_touch_tap(&touch, 7.0 * TEST_CHAR, 5.0, 1);
	kl_text_touch_long_press(&touch, 70.0, 5.0);
	check(touch.bar == 0, "touch: a long press takes the bar away");

	/* A double tap off the words: a caret, with the bar (Paste, Select All). */
	kl_text_touch_tap(&touch, 5.0 * TEST_CHAR, 5.0, 1);
	check(touch.anchor == touch.caret && touch.handles == 0 && touch.bar == 1, "touch: a double tap on the space has the bar at a caret");

	/* Select All's selection, hiding and toggling. */
	kl_text_touch_select(&touch, 0U, 11U);
	check(touch.anchor == 0U && touch.caret == 11U && touch.handles == 1 && touch.bar == 1, "touch: the selection put with the bar");
	kl_text_touch_hide_bar(&touch);
	check(touch.bar == 0 && touch.handles == 1, "touch: the bar hidden, the handles kept");
	kl_text_touch_toggle_bar(&touch);
	check(touch.bar == 1, "touch: a toggle shows it");
	kl_text_touch_toggle_bar(&touch);
	check(touch.bar == 0, "touch: a toggle hides it");

	/* A named end's drag: the anchor's handle swaps the ends and moves. */
	kl_text_touch_select(&touch, 6U, 11U);
	keiui_text_touch_hold(&touch, KL_TEXT_HANDLE_ANCHOR, 60.0, 30.0);
	check(touch.anchor == 11U && touch.caret == 6U && touch.selecting == 1 && touch.bar == 0, "touch: the anchor's handle held");
	kl_text_touch_drag(&touch, 0.0, 30.0);
	kl_text_touch_drag_end(&touch);
	check(touch.anchor == 11U && touch.caret == 0U && touch.bar == 1, "touch: the anchor's end dragged to the start");
}

/* The bar's input (section 2.2). */
static void
test_hit(void)
{
	static const struct kl_rect bounds = { 0, 0, TEST_WIDTH, TEST_HEIGHT };
	struct kl_text_bar bar;
	struct kl_rect selection;
	unsigned pressed;
	unsigned held;
	double x;
	double y;
	int focused;
	int shown;

	/* The bar over a selection, and the field with the focus. */
	selection.x = 250;
	selection.y = 200;
	selection.width = 100;
	selection.height = 20;
	shown = kl_text_bar_layout(&bar, &test_page.text, KL_TEXT_BAR_COPY | KL_TEXT_BAR_SELECT_ALL, &selection, &bounds, &bounds);
	check(shown == 1, "hit: the bar laid out");
	kl_ui_set_focus(test_page.ui, TEST_FIELD, 0U);
	(void)frame(&bar, &held);

	/* A finger's tap on Copy is reported, and the field keeps the focus. */
	x = (double)bar.cells[0].x + (double)bar.cells[0].width / 2.0;
	y = (double)bar.cells[0].y + (double)bar.cells[0].height / 2.0;
	(void)kl_ui_touch_down(test_page.ui, 1, 1000000U, 1000000U, x, y);
	(void)kl_ui_touch_up(test_page.ui, 1, 1040000U, 1040000U);
	pressed = frame(&bar, &held);
	check(pressed == KL_TEXT_BAR_COPY, "hit: a tap on Copy");
	focused = kl_ui_has_focus(test_page.ui, TEST_FIELD, 0U);
	check(focused, "hit: the tap keeps the field's focus");

	/* A click on Select All with the pointer, held between press and release, and the focus kept. */
	x = (double)bar.cells[1].x + (double)bar.cells[1].width / 2.0;
	(void)kl_ui_pointer_motion(test_page.ui, x, y);
	(void)kl_ui_pointer_button(test_page.ui, 1, 2000000U);
	pressed = frame(&bar, &held);
	check(pressed == 0U && held == KL_TEXT_BAR_SELECT_ALL, "hit: Select All held by the pointer");
	focused = kl_ui_has_focus(test_page.ui, TEST_FIELD, 0U);
	check(focused, "hit: the press keeps the field's focus");
	(void)kl_ui_pointer_button(test_page.ui, 0, 2050000U);
	pressed = frame(&bar, &held);
	check(pressed == KL_TEXT_BAR_SELECT_ALL, "hit: a click on Select All");

	/* A drag that starts on the bar does nothing: no button, no drag of the application's. */
	x = (double)bar.cells[0].x + 4.0;
	(void)kl_ui_touch_down(test_page.ui, 2, 3000000U, 3000000U, x, y);
	(void)kl_ui_touch_motion(test_page.ui, 2, 3020000U, 3020000U, x + 60.0, y + 40.0);
	(void)kl_ui_touch_motion(test_page.ui, 2, 3040000U, 3040000U, x + 120.0, y + 80.0);
	(void)kl_ui_touch_up(test_page.ui, 2, 3060000U, 3060000U);
	pressed = frame(&bar, &held);
	check(pressed == 0U, "hit: a drag from the bar presses nothing");
}

/* Draws one frame with the field and the bar; reports the bar's button pressed, and the one held. */
static unsigned
frame(
	const struct kl_text_bar *bar,
	unsigned *held)
{
	static const struct kl_rect field = { 10, 10, 300, 32 };
	struct kl_event event;
	unsigned pressed;
	int taken;

	/* The field, then the bar over everything. */
	kl_ui_begin(test_page.ui, 0U);
	(void)kl_field(test_page.ui, &test_page.style, TEST_FIELD, &field, &test_page.field, "Field");
	pressed = kl_text_bar_hit(test_page.ui, TEST_BAR, bar, held);
	kl_text_bar_draw(bar, &test_page.style, *held);
	(void)kl_ui_end(test_page.ui, 0U);

	/* What no widget took: a drag from the bar is not the application's. */
	for (;;) {
		taken = kl_ui_take(test_page.ui, &event);
		if (!taken)
			break;
		check(event.kind != KL_EVENT_DRAG_BEGIN, "hit: no drag reaches the application");
	}

	/* Reports the button. */
	return pressed;
}

/* The position nearest a point of the line: a character every TEST_CHAR pixels. */
static size_t
view_position(
	void *data,
	double x,
	double y)
{
	double place;

	/* The view has no data of its own, and one line. */
	(void)data;
	(void)y;

	/* The nearest boundary, within the text. */
	place = x / TEST_CHAR + 0.5;
	if (place < 0.0)
		return 0;
	if (place > (double)(sizeof(test_text) - 1U))
		return sizeof(test_text) - 1U;

	/* The boundary. */
	return (size_t)place;
}

/* The caret's rectangle at a position: its x, the line's top and height. */
static void
view_caret(
	void *data,
	size_t position,
	struct kl_rect *rect)
{
	/* The view has no data of its own. */
	(void)data;

	/* A thin caret on the one line. */
	rect->x = (int)((double)position * TEST_CHAR);
	rect->y = 0;
	rect->width = 2;
	rect->height = TEST_LINE;
}

/* The word around a position: the letters on either side, nothing on a space. */
static void
view_word(
	void *data,
	size_t position,
	size_t *start,
	size_t *end)
{
	size_t length;

	/* The view has no data of its own. */
	(void)data;

	/* A position on the space (or the end) has no word. */
	length = sizeof(test_text) - 1U;
	*start = position;
	*end = position;
	if (position >= length || test_text[position] == ' ')
		return;

	/* The letters before and after. */
	while (*start > 0U && test_text[*start - 1U] != ' ')
		(*start)--;
	while (*end < length && test_text[*end] != ' ')
		(*end)++;
}
