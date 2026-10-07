/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws169-p000: draws Mail's view (userland/desktop/mailer/view.c) on the
 * host into pictures, and drives it with the pointer and the keys as the
 * window would: the inbox with the sign-in code, a message with a file,
 * the Work account, the search, a new message written and sent, a reply,
 * Get Mail, a narrow window's list and message, and the panes on glass.
 * The "MAIL" lines the view logs are checked: sending and getting mail
 * report that there is no backend.  BUG-226: a frame drawn within the part
 * a change of the lit widget needs (kl_ui_take_damage) is the whole
 * frame's picture.
 *
 *     host-mailer FONT FALLBACK PREFIX
 *
 * Writes PREFIX-NAME.ppm for each picture (PREFIX-NAME.pam and .panels on
 * glass) and prints "PASS name" or "FAIL name ..." for each check; exits
 * with 1 when one failed.
 */

#include "userland/desktop/mailer/mailer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The wide window's size, and the narrow one's. */
#define TEST_WIDTH		1180
#define TEST_HEIGHT		740
#define TEST_NARROW_WIDTH	520
#define TEST_NARROW_HEIGHT	760

/* The keys of the letters typed (evdev codes). */
#define TEST_KEY_B		48U
#define TEST_KEY_E		18U
#define TEST_KEY_H		35U
#define TEST_KEY_L		38U
#define TEST_KEY_N		49U
#define TEST_KEY_O		24U
#define TEST_KEY_C		46U
#define TEST_KEY_D		32U

/* The log the view wrote, for the checks. */
static char test_log[16384];
static size_t test_log_length;

/* The checks that failed. */
static int test_failures;

/* The frame's time, moved on between frames so that clicks are not double ones. */
static uint64_t test_now = 1000000U;

int main(int argc, char **argv);
void test_mailer_data_load(void);
unsigned kl_appearance_get(const struct kl_appearance *appearance);
static void test_frame(struct ml_view *view, struct kl_ui *ui, const struct kl_style *style, int width, int height);
static void test_click(struct ml_view *view, struct kl_ui *ui, const struct kl_style *style, int width, int height, int x, int y);
static void test_type(struct ml_view *view, struct kl_ui *ui, const struct kl_style *style, int width, int height, const uint32_t *keys, size_t count);
static void test_check(const char *name, const char *expected);
static void test_hover_part(struct ml_view *view, struct kl_ui *ui, const struct kl_style *style, struct kl_canvas *canvas, int glass);
static int test_save(const struct kl_canvas *canvas, const char *prefix, const char *name);
static int test_save_glass(struct ml_view *view, const struct kl_canvas *canvas, const char *prefix, const char *name);

/*
 * Draws and drives the view and checks what it logged.
 */
int
main(
	int argc,
	char **argv)
{
	static const uint32_t ben[] = { TEST_KEY_B, TEST_KEY_E, TEST_KEY_N };
	static const uint32_t hello[] = { TEST_KEY_H, TEST_KEY_E, TEST_KEY_L, TEST_KEY_L, TEST_KEY_O };
	static const uint32_t code[] = { TEST_KEY_C, TEST_KEY_O, TEST_KEY_D, TEST_KEY_E };
	struct kl_canvas canvas;
	struct kl_canvas narrow;
	struct kl_style style;
	struct kl_text text;
	struct ml_view view;
	struct kl_ui *ui;
	struct kl_glass_panel panels[4];
	uint32_t *pixels;
	uint32_t *narrow_pixels;
	size_t panel_count;
	int error;

	/* The fonts and the prefix of the pictures. */
	if (argc != 4) {
		fprintf(stderr, "usage: host-mailer FONT FALLBACK PREFIX\n");
		return 2;
	}

	/* The fonts. */
	kl_text_companions("userland/desktop/fonts/Mahora-Bold.ttf", "userland/desktop/fonts/JetBrainsMono-Regular.ttf");
	error = kl_text_open(&text, argv[1], argv[2]);
	if (error != 0) {
		fprintf(stderr, "host-mailer: fonts error=%d\n", error);
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

	/* The view at the start: the Personal inbox, its first message (the sign-in code). */
	test_mailer_data_load();
	error = ml_view_init(&view);
	if (error != 0)
		return 2;
	test_frame(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT);
	test_frame(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT);
	(void)test_save(&canvas, argv[3], "start");
	test_check("start", "OPEN message=0");

	/* The second row: Aiko's message with a file. */
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 390, 104 + 78 + 38);
	(void)test_save(&canvas, argv[3], "attachment");
	test_check("open", "OPEN message=1");

	/* Reply: the fields filled, the message quoted. */
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 600, 28);
	(void)test_save(&canvas, argv[3], "reply");
	test_check("reply", "COMPOSE kind=reply");

	/* A new message: To "ben", the words "hello", Send: a request for the window. */
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 110, 71);
	test_check("new", "COMPOSE kind=new");
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 800, 82);
	test_type(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, ben, sizeof(ben) / sizeof(ben[0]));
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 800, 300);
	test_type(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, hello, sizeof(hello) / sizeof(hello[0]));
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 1124, 28);
	(void)test_save(&canvas, argv[3], "send");
	test_check("send", "REQUEST action=send to=3 subject=0 body=5");

	/* Get Mail: a request for the window. */
	test_now += 5000000U;
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 110, TEST_HEIGHT - 48);
	test_check("get", "REQUEST action=get");

	/* Add Account: the form; the browser's switch; Sign In asks the window; Cancel closes the form. */
	test_now += 5000000U;
	(void)snprintf(view.status, sizeof(view.status), "Updated 09:41");
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 110, TEST_HEIGHT - 88);
	test_check("setup", "SETUP open");
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 950, 410);
	test_check("codes", "CODES allowed=1");
	(void)test_save(&canvas, argv[3], "setup");
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 925, 347);
	test_check("sign-in", "REQUEST action=sign-in address=0 imap=0 smtp=0");
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 807, 347);
	test_check("setup-cancel", "COMPOSE kind=cancel");

	/* The Work account's inbox. */
	test_now += 5000000U;
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 110, 369);
	test_check("folder", "FOLDER account=1 folder=Inbox");
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 390, 104 + 38);
	(void)test_save(&canvas, argv[3], "work");
	test_check("work", "OPEN message=8");

	/* The search, in the Personal inbox: "code" leaves the bank's message. */
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 110, 163);
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 390, 81);
	test_type(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, code, sizeof(code) / sizeof(code[0]));
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 390, 104 + 38);
	(void)test_save(&canvas, argv[3], "search");
	test_check("search", "OPEN message=0");

	/* A narrow window: the list alone, then a row opens the message with a back button. */
	ml_view_release(&view);
	error = ml_view_init(&view);
	if (error != 0)
		return 2;
	style.canvas = &narrow;
	test_frame(&view, ui, &style, TEST_NARROW_WIDTH, TEST_NARROW_HEIGHT);
	test_frame(&view, ui, &style, TEST_NARROW_WIDTH, TEST_NARROW_HEIGHT);
	(void)test_save(&narrow, argv[3], "narrow-list");
	test_click(&view, ui, &style, TEST_NARROW_WIDTH, TEST_NARROW_HEIGHT, 260, 104 + 2 * 78 + 38);
	(void)test_save(&narrow, argv[3], "narrow-message");
	test_check("narrow-open", "OPEN message=2");

	/* On glass: the panes as cards with the desktop between. */
	ml_view_release(&view);
	error = ml_view_init(&view);
	if (error != 0)
		return 2;
	view.glass = 1;
	style.glass = 1;
	style.canvas = &canvas;
	test_frame(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT);
	test_frame(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT);
	(void)test_save_glass(&view, &canvas, argv[3], "glass");

	/*
	 * On glass, Add Account: the form on one card in the list's and the
	 * message's place, so the glass under it is one panel (the UAT of
	 * 2026-10-07: two panels showed their gap through the form).
	 */
	test_click(&view, ui, &style, TEST_WIDTH, TEST_HEIGHT, 110, TEST_HEIGHT - 88);
	test_check("glass-setup", "SETUP open");
	(void)test_save_glass(&view, &canvas, argv[3], "glass-setup");
	panel_count = ml_view_panels(&view, TEST_WIDTH, TEST_HEIGHT, panels, sizeof(panels) / sizeof(panels[0]));
	if (panel_count == 2U && panels[1].x + panels[1].width == TEST_WIDTH) {
		printf("PASS glass-setup-panels\n");
	} else {
		printf("FAIL glass-setup-panels count=%zu\n", panel_count);
		test_failures++;
	}

	/* BUG-226: the lit part drawn alone, opaque and on glass. */
	test_hover_part(&view, ui, &style, &canvas, 0);
	test_hover_part(&view, ui, &style, &canvas, 1);

	/* Everything goes. */
	ml_view_release(&view);
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
 * BUG-226: moves the pointer over the window in steps; each step that lights
 * another widget is drawn within the part kl_ui_take_damage gives, and is
 * then drawn whole at the same time: the two pictures are the same.
 */
static void
test_hover_part(
	struct ml_view *view,
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

	/* A copy of the frame, and the style opaque or on glass, drawn whole once. */
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
 * Keeps a log line of the view for the checks (the window's own writes it
 * on standard error).
 */
void
ml_log(
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
 * Draws one frame of the view as the window does, and gives the keys no
 * widget took to the view.
 */
static void
test_frame(
	struct ml_view *view,
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
	ml_view_draw(view, ui, style, width, height, test_now);
	(void)kl_ui_end(ui, test_now);

	/* The keys no widget took. */
	for (;;) {
		taken = kl_ui_take(ui, &event);
		if (!taken)
			break;

		/* A key is the view's. */
		if (event.kind == KL_EVENT_KEY)
			ml_view_key(view, event.code, event.modifiers, test_now);
	}
}

/*
 * Clicks at a point: the pointer goes there, presses and releases, and
 * two frames are drawn (the click taken, then its result shown).
 */
static void
test_click(
	struct ml_view *view,
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
	struct ml_view *view,
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
	struct ml_view *view,
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
	count = ml_view_panels(view, canvas->width, canvas->height, panels, 4U);
	for (i = 0; i < count; i++)
		fprintf(file, "%d %d %d %d %d\n", (int)panels[i].x, (int)panels[i].y, (int)panels[i].width, (int)panels[i].height, (int)panels[i].radius);

	/* Succeeded: both are written. */
	fclose(file);
	return 0;
}
