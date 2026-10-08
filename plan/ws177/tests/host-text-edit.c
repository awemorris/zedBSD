/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p013: the host test of the text widgets' editing commands
 * (libkeiland's field.c and text-area.c with ui.c, KL_VERSION 67): a field
 * and a text area are drawn frame by frame and given keys through kl_ui,
 * the window's clipboard a buffer of the test's (keiui_ui_set_window):
 *
 *   - Ctrl+Left and Ctrl+Right move a word, with Shift the selection;
 *   - Ctrl+Z takes back one change a key, Ctrl+Shift+Z and Ctrl+Y do it
 *     again, and a change after an undo forgets the changes taken back;
 *   - Ctrl+C copies the selection, Ctrl+X cuts it, Ctrl+V pastes (in place
 *     of the selection), each undone in one step; a secret field copies
 *     nothing;
 *   - a text the application sets is not undone into;
 *   - a text area pastes a text longer than an input method's commit.
 *
 *   host-text-edit FONT
 */

#include <keiland/keiland.h>

#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The frame's size. */
#define TEST_WIDTH	600
#define TEST_HEIGHT	300

/* The widgets' ids. */
#define TEST_FIELD	1U
#define TEST_AREA	2U
#define TEST_SECRET	3U

/* The evdev codes of the keys the test presses. */
#define KEY_A		30U
#define KEY_C		46U
#define KEY_V		47U
#define KEY_X		45U
#define KEY_Y		21U
#define KEY_Z		44U

/*
 * The test's page: its pixels, canvas, font and style, the window's input,
 * the widgets' states, and the clipboard standing in for the window's.
 * It lives for the whole run.
 */
struct test_page {
	uint32_t pixels[TEST_WIDTH * TEST_HEIGHT];
	struct kl_canvas canvas;
	struct kl_text text;
	struct kl_style style;
	struct kl_ui *ui;
	struct kl_field field;
	struct kl_field secret;
	struct kl_text_area area;
};

/* The page. */
static struct test_page test_page;

/* The clipboard's text and its length, written by the stand-in copy. */
static char test_clipboard[16384];
static size_t test_clipboard_length;

/* The checks that failed, and all the checks. */
static unsigned test_failed;
static unsigned test_count;

unsigned kl_appearance_get(const struct kl_appearance *appearance);
static void check(int condition, const char *what);
static void frame(void);
static void press(uint32_t code, unsigned modifiers);
static void type(const char *text);
static void focus(uint32_t id);
static void test_copy(struct kl_window *window, const char *text, size_t length);
static size_t test_paste(struct kl_window *window, char *text, size_t size);
static void test_field(void);
static void test_area(void);

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
		fprintf(stderr, "usage: host-text-edit FONT\n");
		return 2;
	}
	error = kl_text_open(&test_page.text, argv[1], NULL);
	if (error != 0) {
		fprintf(stderr, "host-text-edit: %s: error %d\n", argv[1], error);
		return 2;
	}

	/* The canvas, the style and the window's input, tied to the stand-in clipboard. */
	error = kl_canvas_init(&test_page.canvas, test_page.pixels, TEST_WIDTH, TEST_WIDTH, TEST_HEIGHT);
	if (error != 0)
		return 2;
	test_page.style.canvas = &test_page.canvas;
	test_page.style.text = &test_page.text;
	test_page.style.theme = kl_theme_default();
	test_page.ui = kl_ui_create();
	if (test_page.ui == NULL)
		return 2;
	keiui_ui_set_window(test_page.ui, (struct kl_window *)(void *)&test_page, test_copy, test_paste);
	test_page.secret.secret = 1;
	frame();

	/* The field, then the text area. */
	test_field();
	test_area();

	/* Reports the checks. */
	kl_ui_destroy(test_page.ui);
	kl_text_close(&test_page.text);
	printf("host-text-edit: %u of %u held\n", test_count - test_failed, test_count);
	if (test_failed != 0U) {
		printf("host-text-edit: FAIL\n");
		return 1;
	}

	/* Succeeded: every check held. */
	printf("host-text-edit: PASS\n");
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

/* Draws one frame: the field, the secret field and the text area take the keys given since the last. */
static void
frame(void)
{
	static const struct kl_rect field = { 10, 10, 400, 32 };
	static const struct kl_rect secret = { 10, 50, 400, 32 };
	static const struct kl_rect area = { 10, 90, 400, 180 };
	struct kl_event event;
	int taken;

	/* The widgets. */
	kl_ui_begin(test_page.ui, 0U);
	(void)kl_field(test_page.ui, &test_page.style, TEST_FIELD, &field, &test_page.field, "Field");
	(void)kl_field(test_page.ui, &test_page.style, TEST_SECRET, &secret, &test_page.secret, "Secret");
	(void)kl_text_area(test_page.ui, &test_page.style, TEST_AREA, &area, &test_page.area, "Area");
	(void)kl_ui_end(test_page.ui, 0U);

	/* What no widget took. */
	for (;;) {
		taken = kl_ui_take(test_page.ui, &event);
		if (!taken)
			break;
	}
}

/* A key pressed and released, with a frame after it. */
static void
press(
	uint32_t code,
	unsigned modifiers)
{
	/* Down and up. */
	(void)kl_ui_key(test_page.ui, code, 1, modifiers);
	(void)kl_ui_key(test_page.ui, code, 0, modifiers);
	frame();
}

/* Types ASCII letters, digits and spaces, a key each (a frame each). */
static void
type(
	const char *text)
{
	static const char letters[] = "qwertyuiop";
	static const uint32_t row1[] = { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25 };
	static const char letters2[] = "asdfghjkl";
	static const uint32_t row2[] = { 30, 31, 32, 33, 34, 35, 36, 37, 38 };
	static const char letters3[] = "zxcvbnm";
	static const uint32_t row3[] = { 44, 45, 46, 47, 48, 49, 50 };
	const char *found;
	uint32_t code;

	/* Each character's key. */
	for (; *text != '\0'; text++) {
		code = 57U;
		found = strchr(letters, *text);
		if (found != NULL)
			code = row1[found - letters];
		found = strchr(letters2, *text);
		if (found != NULL)
			code = row2[found - letters2];
		found = strchr(letters3, *text);
		if (found != NULL)
			code = row3[found - letters3];
		press(code, 0U);
	}
}

/* Gives the keyboard to a widget. */
static void
focus(
	uint32_t id)
{
	/* The focus, and a frame. */
	kl_ui_set_focus(test_page.ui, id, 0U);
	frame();
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
	if (length > sizeof(test_clipboard))
		length = sizeof(test_clipboard);
	memcpy(test_clipboard, text, length);
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

	(void)window;

	/* As much as fits. */
	length = test_clipboard_length;
	if (length > size)
		length = size;
	memcpy(text, test_clipboard, length);
	return length;
}

/* Checks the field's words, undo and redo, and clipboard. */
static void
test_field(void)
{
	struct kl_field *field;
	int same;

	/* Typed: one change a key. */
	field = &test_page.field;
	focus(TEST_FIELD);
	type("hello world");
	same = strcmp(field->text, "hello world");
	check(same == 0, "the field took hello world");

	/* Words: Ctrl+Left to the start of world, again to hello's, Ctrl+Right to hello's end. */
	press(KL_KEY_LEFT, KL_MOD_CTRL);
	check(field->caret == 6U && field->anchor == 6U, "Ctrl+Left: the start of world");
	press(KL_KEY_LEFT, KL_MOD_CTRL);
	check(field->caret == 0U, "Ctrl+Left again: the start of hello");
	press(KL_KEY_RIGHT, KL_MOD_CTRL);
	check(field->caret == 5U, "Ctrl+Right: the end of hello");
	press(KL_KEY_RIGHT, KL_MOD_CTRL | KL_MOD_SHIFT);
	check(field->caret == 11U && field->anchor == 5U, "Ctrl+Shift+Right: world selected with its space");

	/* Undo: the last two keys taken back, then done again. */
	press(KL_KEY_END, 0U);
	press(KEY_Z, KL_MOD_CTRL);
	press(KEY_Z, KL_MOD_CTRL);
	same = strcmp(field->text, "hello wor");
	check(same == 0 && field->caret == 9U, "Ctrl+Z twice: hello wor, the caret at its end");
	press(KEY_Z, KL_MOD_CTRL | KL_MOD_SHIFT);
	same = strcmp(field->text, "hello worl");
	check(same == 0, "Ctrl+Shift+Z: hello worl");
	press(KEY_Y, KL_MOD_CTRL);
	same = strcmp(field->text, "hello world");
	check(same == 0, "Ctrl+Y: hello world");
	press(KEY_Y, KL_MOD_CTRL);
	same = strcmp(field->text, "hello world");
	check(same == 0, "Ctrl+Y with nothing taken back: no change");

	/* A change after an undo forgets what was taken back. */
	press(KEY_Z, KL_MOD_CTRL);
	type("x");
	press(KEY_Y, KL_MOD_CTRL);
	same = strcmp(field->text, "hello worlx");
	check(same == 0, "a key after an undo: no redo of the change taken back");

	/* Copy, cut and paste. */
	press(KEY_A, KL_MOD_CTRL);
	press(KEY_C, KL_MOD_CTRL);
	check(test_clipboard_length == 11U && memcmp(test_clipboard, "hello worlx", 11U) == 0, "Ctrl+A, Ctrl+C: the clipboard has the text");
	press(KL_KEY_HOME, 0U);
	press(KL_KEY_RIGHT, KL_MOD_CTRL | KL_MOD_SHIFT);
	press(KEY_X, KL_MOD_CTRL);
	same = strcmp(field->text, " worlx");
	check(same == 0 && test_clipboard_length == 5U && memcmp(test_clipboard, "hello", 5U) == 0, "Ctrl+X: hello cut to the clipboard");
	press(KL_KEY_END, 0U);
	press(KEY_V, KL_MOD_CTRL);
	same = strcmp(field->text, " worlxhello");
	check(same == 0, "Ctrl+V: hello pasted at the end");
	press(KEY_Z, KL_MOD_CTRL);
	same = strcmp(field->text, " worlx");
	check(same == 0, "Ctrl+Z: the paste taken back in one step");
	press(KEY_Z, KL_MOD_CTRL);
	same = strcmp(field->text, "hello worlx");
	check(same == 0 && field->anchor == 0U && field->caret == 5U, "Ctrl+Z: the cut taken back, hello selected again");

	/* A text the application set is not undone into. */
	kl_field_set(field, "set by the program");
	frame();
	press(KEY_Z, KL_MOD_CTRL);
	same = strcmp(field->text, "set by the program");
	check(same == 0, "Ctrl+Z after kl_field_set: no change");

	/* A secret field copies and cuts nothing. */
	focus(TEST_SECRET);
	type("abc");
	test_clipboard_length = 0;
	press(KEY_A, KL_MOD_CTRL);
	press(KEY_C, KL_MOD_CTRL);
	press(KEY_X, KL_MOD_CTRL);
	same = strcmp(test_page.secret.text, "abc");
	check(same == 0 && test_clipboard_length == 0U, "a secret field: Ctrl+C and Ctrl+X do nothing");
}

/* Checks the text area's undo and a long paste. */
static void
test_area(void)
{
	struct kl_text_area *area;
	char *long_text;
	size_t index;
	int same;

	/* Typed over two lines. */
	area = &test_page.area;
	focus(TEST_AREA);
	type("ab");
	press(KL_KEY_ENTER, 0U);
	type("cd");
	same = strcmp(area->text, "ab\ncd");
	check(same == 0, "the area took ab, a newline, cd");

	/* Three keys taken back. */
	press(KEY_Z, KL_MOD_CTRL);
	press(KEY_Z, KL_MOD_CTRL);
	press(KEY_Z, KL_MOD_CTRL);
	same = strcmp(area->text, "ab");
	check(same == 0, "Ctrl+Z three times: ab");

	/* A paste longer than an input method's commit, in one step. */
	long_text = malloc(2000U);
	if (long_text == NULL) {
		check(0, "memory for the long text");
		return;
	}
	for (index = 0; index < 2000U; index++)
		long_text[index] = (char)('a' + (char)(index % 26U));
	memcpy(test_clipboard, long_text, 2000U);
	test_clipboard_length = 2000U;
	press(KEY_V, KL_MOD_CTRL);
	check(area->length == 2002U && memcmp(area->text + 2, long_text, 2000U) == 0, "Ctrl+V: 2000 bytes pasted whole");
	press(KEY_Z, KL_MOD_CTRL);
	same = strcmp(area->text, "ab");
	check(same == 0, "Ctrl+Z: the long paste taken back in one step");
	free(long_text);

	/* Words in the area. */
	press(KEY_A, KL_MOD_CTRL);
	type("one two");
	press(KL_KEY_LEFT, KL_MOD_CTRL);
	check(area->caret == 4U, "Ctrl+Left in the area: the start of two");
}
