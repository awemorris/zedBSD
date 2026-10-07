/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws170-p000: draws Phone's view (userland/desktop/phone/view.c) on the
 * host into pictures, and drives it with the pointer and the keys as the
 * window would: the first contact's timeline, another contact chosen by a
 * click, a message written and sent, a call, Japanese from an input method
 * and the flick keyboard in the message field (BUG-203, BUG-204), the
 * search, and a narrow window's list and timeline.  The "PHONE" lines the
 * view logs are checked: sending and calling report that there is no
 * backend.
 *
 *     host-phone FONT FALLBACK PREFIX FOLDER   (the folder of the test data's store, made empty)
 *
 * Writes PREFIX-NAME.ppm for each picture and prints "PASS name" or
 * "FAIL name ..." for each check; exits with 1 when one failed.
 */

#include "userland/desktop/phone/phone.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The wide window's size, and the narrow one's. */
#define TEST_WIDTH		980
#define TEST_HEIGHT		660
#define TEST_NARROW_WIDTH	420
#define TEST_NARROW_HEIGHT	760

/* The keys of the letters typed (evdev codes). */
#define TEST_KEY_E		18U
#define TEST_KEY_H		35U
#define TEST_KEY_L		38U
#define TEST_KEY_N		49U
#define TEST_KEY_O		24U

/* The log the view wrote, for the checks. */
static char test_log[16384];
static size_t test_log_length;

/* The checks that failed. */
static int test_failures;

/* The frame's time, moved on between frames so that clicks are not double ones. */
static uint64_t test_now = 1000000U;

int main(int argc, char **argv);
int test_phone_data_load(const char *folder);
unsigned kl_appearance_get(const struct kl_appearance *appearance);
static void test_frame(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, int width, int height);
static void test_click(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, int width, int height, int x, int y);
static void test_type(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, int width, int height, const uint32_t *keys, size_t count);
static void test_text(struct kl_ui *ui, unsigned kind, const char *text, uint32_t before);
static void test_field(const char *name, const struct kl_field *field, const char *expected);
static void test_check(const char *name, const char *expected);
static int test_save(const struct kl_canvas *canvas, const char *prefix, const char *name);
static int test_save_glass(struct ph_view *view, const struct kl_canvas *canvas, const char *prefix, const char *name);
static void test_hover_part(struct ph_view *view, struct kl_ui *ui, const struct kl_style *style, struct kl_canvas *canvas, int glass);

/*
 * Draws and drives the view and checks what it logged.
 */
int
main(
	int argc,
	char **argv)
{
	static const uint32_t hello[] = { TEST_KEY_H, TEST_KEY_E, TEST_KEY_L, TEST_KEY_L, TEST_KEY_O };
	static const uint32_t lena[] = { TEST_KEY_L, TEST_KEY_E, TEST_KEY_N };
	struct kl_canvas canvas;
	struct kl_canvas narrow;
	struct kl_style style;
	struct kl_text text;
	struct ph_view view;
	struct kl_rect caret;
	struct kl_ui *ui;
	uint32_t *pixels;
	int wanted;
	uint32_t *narrow_pixels;
	int error;

	/* The fonts and the prefix of the pictures. */
	if (argc != 5) {
		fprintf(stderr, "usage: host-phone FONT FALLBACK PREFIX FOLDER\n");
		return 2;
	}

	/* The fonts. */
	kl_text_companions("userland/desktop/fonts/Mahora-Bold.ttf", "userland/desktop/fonts/JetBrainsMono-Regular.ttf");
	error = kl_text_open(&text, argv[1], argv[2]);
	if (error != 0) {
		fprintf(stderr, "host-phone: fonts error=%d\n", error);
		return 2;
	}

	/* The wide window's frame. */
	pixels = calloc((size_t)TEST_WIDTH * TEST_HEIGHT, sizeof(pixels[0]));
	if (pixels == NULL)
		return 2;

	/* The narrow window's frame. */
	narrow_pixels = calloc((size_t)TEST_NARROW_WIDTH * TEST_NARROW_HEIGHT, sizeof(narrow_pixels[0]));
	if (narrow_pixels == NULL)
		return 2;

	/* The canvases on them. */
	error = kl_canvas_init(&canvas, pixels, TEST_WIDTH, TEST_WIDTH, TEST_HEIGHT);
	if (error != 0)
		return 2;
	error = kl_canvas_init(&narrow, narrow_pixels, TEST_NARROW_WIDTH, TEST_NARROW_WIDTH, TEST_NARROW_HEIGHT);
	if (error != 0)
		return 2;

	/* The input and the style. */
	ui = kl_ui_create();
	if (ui == NULL)
		return 2;
	style.canvas = &canvas;
	style.text = &text;
	style.theme = kl_theme_default();
	style.glass = 0;

	/* The view at the start: the test data in a store in a folder, the first contact's timeline, at its end. */
	error = test_phone_data_load(argv[4]);
	if (error != 0) {
		fprintf(stderr, "host-phone: store error=%d\n", error);
		return 2;
	}
	error = ph_view_init(&view);
	if (error != 0)
		return 2;
	test_frame(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT);
	test_frame(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT);
	(void)test_save(&canvas, argv[3], "start");
	test_check("start", "SELECT contact=0");

	/* Kenji's row clicked (the third): his VoIP calls and file. */
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 160, 96 + 2 * 68 + 32);
	(void)test_save(&canvas, argv[3], "kenji");
	test_check("select-kenji", "SELECT contact=2");

	/* Ben's row: SMS in green and a missed call. */
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 160, 96 + 68 + 32);
	(void)test_save(&canvas, argv[3], "ben");
	test_check("select-ben", "SELECT contact=1");

	/* A message written in the field and sent: a request for the window. */
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 600, 630);
	test_type(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, hello, sizeof(hello) / sizeof(hello[0]));
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 952, 630);
	(void)test_save(&canvas, argv[3], "send");
	test_check("send", "REQUEST action=send contact=1 length=5");

	/* The call button: a request for the window. */
	test_now += 5000000U;
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 942, 32);
	(void)test_save(&canvas, argv[3], "call");
	test_check("call", "REQUEST action=call contact=1");

	/* The message field with the keyboard asks for an input method's text, with its caret (BUG-203). */
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 600, 630);
	kl_field_set(&view.message, "");
	test_frame(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT);
	wanted = kl_ui_text_wanted(ui, &caret);
	if (wanted && caret.y > 600 && caret.height > 0) {
		printf("PASS ime-wanted\n");
	} else {
		printf("FAIL ime-wanted wanted=%d y=%d height=%d\n", wanted, caret.y, caret.height);
		test_failures++;
	}

	/* A word being composed shows underlined at the caret, and is not yet the field's text. */
	test_text(ui, KL_WINDOW_TEXT_PREEDIT, "\xe3\x81\xab\xe3\x81\xbb\xe3\x82\x93", 0U);
	test_frame(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT);
	(void)test_save(&canvas, argv[3], "preedit");
	test_field("ime-preedit", &view.message, "");

	/* The conversion committed: the composition goes and the kanji are written. */
	test_text(ui, KL_WINDOW_TEXT_COMMIT, "\xe6\x97\xa5\xe6\x9c\xac", 0U);
	test_text(ui, KL_WINDOW_TEXT_PREEDIT, "", 0U);
	test_frame(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT);
	(void)test_save(&canvas, argv[3], "commit");
	test_field("ime-commit", &view.message, "\xe6\x97\xa5\xe6\x9c\xac");

	/* The flick keyboard's kana, then its voiced form in place of it (the kana before deleted, BUG-204). */
	test_text(ui, KL_WINDOW_TEXT_COMMIT, "\xe3\x81\x8b", 0U);
	test_frame(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT);
	test_text(ui, KL_WINDOW_TEXT_DELETE, "", 3U);
	test_text(ui, KL_WINDOW_TEXT_COMMIT, "\xe3\x81\x8c", 0U);
	test_frame(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT);
	test_field("flick-voiced", &view.message, "\xe6\x97\xa5\xe6\x9c\xac\xe3\x81\x8c");

	/* Without a field with the keyboard the text input is not asked for. */
	kl_ui_clear_focus(ui);
	test_frame(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT);
	wanted = kl_ui_text_wanted(ui, NULL);
	if (!wanted) {
		printf("PASS ime-unwanted\n");
	} else {
		printf("FAIL ime-unwanted wanted=%d\n", wanted);
		test_failures++;
	}
	kl_field_set(&view.message, "");

	/* The search: "len" leaves Lena alone, and her row shows her. */
	test_now += 5000000U;
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 160, 68);
	test_type(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, lena, sizeof(lena) / sizeof(lena[0]));
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 160, 96 + 32);
	(void)test_save(&canvas, argv[3], "search");
	test_check("search", "SELECT contact=5");

	/* A narrow window: the list alone, then a row opens its timeline with a back button. */
	ph_view_release(&view);
	error = ph_view_init(&view);
	if (error != 0)
		return 2;
	style.canvas = &narrow;
	test_frame(&view, ui, &style, TEST_NARROW_WIDTH, TEST_NARROW_HEIGHT);
	test_frame(&view, ui, &style, TEST_NARROW_WIDTH, TEST_NARROW_HEIGHT);
	(void)test_save(&narrow, argv[3], "narrow-list");
	test_click(&view, ui, &style, TEST_NARROW_WIDTH, TEST_NARROW_HEIGHT, 210, 96 + 3 * 68 + 32);
	(void)test_save(&narrow, argv[3], "narrow-timeline");
	test_check("narrow-open", "SELECT contact=3");

	/* The back button: the list again. */
	test_click(&view, ui, &style, TEST_NARROW_WIDTH, TEST_NARROW_HEIGHT, 26, 32);
	(void)test_save(&narrow, argv[3], "narrow-back");
	if (view.opened) {
		printf("FAIL narrow-back opened=%d\n", view.opened);
		test_failures++;
	} else {
		printf("PASS narrow-back\n");
	}

	/* A key no widget takes: Down moves to the next contact. */
	kl_ui_clear_focus(ui);
	(void)kl_ui_key(ui, KL_KEY_DOWN, 1, 0U);
	test_frame(&view, ui, &style, TEST_NARROW_WIDTH, TEST_NARROW_HEIGHT);
	test_check("key-down", "SELECT contact=4");

	/* On glass: the cards with the desktop between, written with their alpha and their panels for the script to lay on a wallpaper. */
	ph_view_release(&view);
	error = ph_view_init(&view);
	if (error != 0)
		return 2;
	view.glass = 1;
	style.glass = 1;
	style.canvas = &canvas;
	test_frame(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT);
	test_frame(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT);
	(void)test_save_glass(&view, &canvas, argv[3], "glass");
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 160, 108 + 1 * 68 + 32);
	(void)test_save_glass(&view, &canvas, argv[3], "glass-ben");
	test_check("glass-ben", "SELECT contact=1");

	/* BUG-226 (ws090-p018): the lit part drawn alone, opaque and on glass. */
	test_hover_part(&view, ui, &style, &canvas, 0);
	test_hover_part(&view, ui, &style, &canvas, 1);

	/* "+": the form of a new contact; Save with a number asks the window. */
	test_now += 5000000U;
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 301, 30);
	test_check("add-open", "ADD open");
	kl_field_set(&view.new_name, "Sam Lee");
	kl_field_set(&view.new_number, "+44 20 5555 0175");
	test_frame(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT);
	(void)test_save_glass(&view, &canvas, argv[3], "glass-add");
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 333 + 8 + (980 - 333 - 8 - 420) / 2 + 420 - 45, 70 + 24 + 42 + 52 + 17);
	test_check("add-save", "REQUEST action=save name=7 number=16");

	/* Everything goes. */
	ph_view_release(&view);
	kl_ui_destroy(ui);
	kl_canvas_release(&canvas);
	kl_canvas_release(&narrow);
	free(pixels);
	free(narrow_pixels);
	kl_text_close(&text);

	/* Reports whether every check passed. */
	if (test_failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/*
 * Reports the light appearance: the host has no desktop to ask (the
 * library's own, appearance.c, needs Wayland), and the pictures are the
 * light theme's.
 */
unsigned
kl_appearance_get(
	const struct kl_appearance *appearance)
{
	(void)appearance;

	/* The light appearance. */
	return KL_APPEARANCE_LIGHT;
}

/*
 * Keeps a log line of the view for the checks (the window's own writes it
 * on standard error).
 */
void
ph_log(
	const char *format,
	...)
{
	va_list arguments;
	int length;

	/* The line, after the others. */
	va_start(arguments, format);
	length = vsnprintf(test_log + test_log_length, sizeof(test_log) - test_log_length - 1U, format, arguments);
	va_end(arguments);
	if (length < 0 || (size_t)length >= sizeof(test_log) - test_log_length - 1U)
		return;

	/* The line's end. */
	test_log_length += (size_t)length;
	test_log[test_log_length] = '\n';
	test_log_length++;
	test_log[test_log_length] = '\0';
}

/*
 * BUG-226: moves the pointer over the window in steps; each step that lights
 * another widget is drawn within the part kl_ui_take_damage gives, and is
 * then drawn whole at the same time: the two pictures are the same.
 */
static void
test_hover_part(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	struct kl_canvas *canvas,
	int glass)
{
	struct kl_style lit;
	struct kl_rect part;
	uint32_t *kept;
	size_t size;
	int step;
	int redraw;
	int placed;
	int parts;
	int differs;
	int same;

	/* A copy of the frame. */
	size = (size_t)canvas->stride * (size_t)canvas->height * sizeof(uint32_t);
	kept = malloc(size);
	if (kept == NULL) {
		printf("FAIL hover-part memory\n");
		test_failures++;
		return;
	}

	/* The style asked for, and a whole frame to start from. */
	lit = *style;
	lit.glass = glass;
	test_frame(view, ui, &lit, canvas->width, canvas->height);

	/* Each step that lights another widget: its part, then the whole frame at the same time. */
	parts = 0;
	differs = 0;
	for (step = 0; step < 60; step++) {
		redraw = kl_ui_pointer_motion(ui, 40.0 + (double)((step * 37) % (canvas->width - 80)), 60.0 + (double)((step * 53) % (canvas->height - 120)));
		if (!redraw)
			continue;
		placed = kl_ui_take_damage(ui, &part);
		if (!placed)
			continue;
		parts++;

		/* The part alone. */
		kl_canvas_clip_push(canvas, &part);
		test_frame(view, ui, &lit, canvas->width, canvas->height);
		kl_canvas_clip_pop(canvas);
		memcpy(kept, canvas->pixels, size);

		/* The whole frame, at the same time. */
		test_now -= 16000U;
		test_frame(view, ui, &lit, canvas->width, canvas->height);
		same = memcmp(kept, canvas->pixels, size);
		if (same != 0)
			differs++;
	}

	/* Some parts drawn, all of them the whole frame's picture. */
	if (parts > 0 && differs == 0) {
		printf("PASS hover-part glass=%d parts=%d\n", glass, parts);
	} else {
		printf("FAIL hover-part glass=%d parts=%d differs=%d\n", glass, parts, differs);
		test_failures++;
	}

	/* The copy goes. */
	free(kept);
}

/*
 * Draws one frame of the view as the window does, and gives the keys no
 * widget took to the view.
 */
static void
test_frame(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	int width,
	int height)
{
	struct kl_event event;
	int taken;

	/* The frame, a frame's time after the last. */
	test_now += 16000U;
	kl_ui_begin(ui, test_now);
	ph_view_draw(view, ui, style, width, height, test_now);
	(void)kl_ui_end(ui, test_now);

	/* The keys no widget took. */
	for (;;) {
		taken = kl_ui_take(ui, &event);
		if (!taken)
			break;

		/* A key is the view's. */
		if (event.kind == KL_EVENT_KEY)
			ph_view_key(view, event.code, event.modifiers, test_now);
	}
}

/*
 * Clicks at a point: the pointer goes there, presses and releases, and
 * two frames are drawn (the click taken, then its result shown).
 */
static void
test_click(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	int width,
	int height,
	int x,
	int y)
{
	/* A second after the last click, so that it is a single one. */
	test_now += 1000000U;
	(void)kl_ui_pointer_motion(ui, (double)x, (double)y);
	(void)kl_ui_pointer_button(ui, 1, test_now);
	(void)kl_ui_pointer_button(ui, 0, test_now + 50000U);
	test_frame(view, ui, style, width, height);
	test_frame(view, ui, style, width, height);
}

/*
 * Types keys into the widget with the focus, a frame after each.
 */
static void
test_type(
	struct ph_view *view,
	struct kl_ui *ui,
	const struct kl_style *style,
	int width,
	int height,
	const uint32_t *keys,
	size_t count)
{
	size_t i;

	/* Each key pressed and released. */
	for (i = 0; i < count; i++) {
		(void)kl_ui_key(ui, keys[i], 1, 0U);
		(void)kl_ui_key(ui, keys[i], 0, 0U);
		test_frame(view, ui, style, width, height);
	}
}

/*
 * Gives the input a text of the window's text input, as the window does
 * with what an input method or the on-screen keyboard sent.
 */
static void
test_text(
	struct kl_ui *ui,
	unsigned kind,
	const char *text,
	uint32_t before)
{
	struct kl_window_event event;

	/* The input, with the composed text's cursor at its end. */
	memset(&event, 0, sizeof(event));
	event.kind = kind;
	snprintf(event.text, sizeof(event.text), "%s", text);
	event.begin = (int32_t)strlen(text);
	event.end = event.begin;
	event.before = before;
	(void)kl_ui_text(ui, &event);
}

/*
 * Checks that a field holds a text.
 */
static void
test_field(
	const char *name,
	const struct kl_field *field,
	const char *expected)
{
	int same;

	/* The field's text against the one expected. */
	same = strcmp(field->text, expected);
	if (same == 0) {
		printf("PASS %s\n", name);
		return;
	}

	/* Another text. */
	printf("FAIL %s text=%s expected=%s\n", name, field->text, expected);
	test_failures++;
}

/*
 * Checks that the view logged a line, and empties the log.
 */
static void
test_check(
	const char *name,
	const char *expected)
{
	const char *found;

	/* The line among those logged. */
	found = strstr(test_log, expected);
	if (found == NULL) {
		printf("FAIL %s expected \"%s\" in:\n%s", name, expected, test_log);
		test_failures++;
	} else {
		printf("PASS %s\n", name);
	}

	/* The next check reads only what comes after. */
	test_log_length = 0;
	test_log[0] = '\0';
}

/*
 * Writes a canvas as a PPM picture PREFIX-NAME.ppm; nonzero when it
 * cannot.
 */
static int
test_save(
	const struct kl_canvas *canvas,
	const char *prefix,
	const char *name)
{
	char path[512];
	FILE *file;
	uint32_t pixel;
	int x;
	int y;

	/* The file. */
	(void)snprintf(path, sizeof(path), "%s-%s.ppm", prefix, name);
	file = fopen(path, "wb");
	if (file == NULL)
		return -1;

	/* The header and each pixel's red, green and blue. */
	fprintf(file, "P6\n%d %d\n255\n", canvas->width, canvas->height);
	for (y = 0; y < canvas->height; y++) {
		for (x = 0; x < canvas->width; x++) {
			pixel = canvas->pixels[(size_t)y * canvas->stride + (size_t)x];
			fputc((int)((pixel >> 16) & 0xffU), file);
			fputc((int)((pixel >> 8) & 0xffU), file);
			fputc((int)(pixel & 0xffU), file);
		}
	}

	/* Succeeded: the picture is written. */
	fclose(file);
	return 0;
}

/*
 * Writes a frame on glass: PREFIX-NAME.pam with its alpha (premultiplied
 * RGBA) and PREFIX-NAME.panels with a line "x y width height radius" for
 * each glass panel; nonzero when it cannot.
 */
static int
test_save_glass(
	struct ph_view *view,
	const struct kl_canvas *canvas,
	const char *prefix,
	const char *name)
{
	struct kl_glass_panel panels[4];
	char path[512];
	FILE *file;
	uint32_t pixel;
	size_t count;
	size_t i;
	int x;
	int y;

	/* The picture's file. */
	(void)snprintf(path, sizeof(path), "%s-%s.pam", prefix, name);
	file = fopen(path, "wb");
	if (file == NULL)
		return -1;

	/* The header and each pixel's red, green, blue and alpha. */
	fprintf(file, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", canvas->width, canvas->height);
	for (y = 0; y < canvas->height; y++) {
		for (x = 0; x < canvas->width; x++) {
			pixel = canvas->pixels[(size_t)y * canvas->stride + (size_t)x];
			fputc((int)((pixel >> 16) & 0xffU), file);
			fputc((int)((pixel >> 8) & 0xffU), file);
			fputc((int)(pixel & 0xffU), file);
			fputc((int)((pixel >> 24) & 0xffU), file);
		}
	}

	/* The picture is written. */
	fclose(file);

	/* The panels' file. */
	(void)snprintf(path, sizeof(path), "%s-%s.panels", prefix, name);
	file = fopen(path, "w");
	if (file == NULL)
		return -1;

	/* A line each. */
	count = ph_view_panels(view, canvas->width, canvas->height, panels, 4U);
	for (i = 0; i < count; i++)
		fprintf(file, "%d %d %d %d %d\n", (int)panels[i].x, (int)panels[i].y, (int)panels[i].width, (int)panels[i].height, (int)panels[i].radius);

	/* Succeeded: both are written. */
	fclose(file);
	return 0;
}
