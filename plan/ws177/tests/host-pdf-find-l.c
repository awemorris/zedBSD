/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of case L of WS177 (ws177-p032 to p035): libpdf's page
 * text with the forms' text, and PDF Viewer's find and selection (find.c
 * with the viewer's core), on the PDFs of make-find-l.py.
 *
 *   forms   forms.pdf: the forms' text after the page's lines, a nested
 *           form's, a turned form's corners, not a Type 3 glyph's text
 *   find    find.pdf: a word across a line's end and a hyphen, spaces,
 *           full and half width, voiced kana, Greek and Cyrillic cases,
 *           ligatures, a sharp s, Japanese across a line's end; the count
 *           of the places found as the ticks read the pages; the words
 *           that cannot be read
 *   select  across pages, a word and a line by the clicks, all of the
 *           document, a word by a long press and its handles, the marks
 *           of a turned page's characters
 *   bar     the find field inside the window (no titlebar)
 *
 *   host-pdf-find-l FONT SAMPLES OUTDIR
 */

#include "../../../userland/desktop/pdfviewer/viewer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The window's size in the test. */
#define TEST_WIDTH	1000
#define TEST_HEIGHT	760

/* The viewer, its font and its frame's pixels, for the whole run. */
static struct pv_app app;
static struct pv_text text;
static uint32_t pixels[TEST_WIDTH * TEST_HEIGHT];

/* Where the samples are, and where the frames go. */
static const char *samples;
static const char *out_dir;

/* The checks that failed, and those that held. */
static int failures;
static int passes;

static void test_forms(void);
static void lines_of(const struct pdf_page_text *page_text, char *out, size_t size);
static void frame(const char *name);
static void key(uint32_t code, uint32_t modifiers);
static void button(int x, int y, int pressed);
static void motion(int x, int y);
static void screen_of(size_t page, double x, double y, int *screen_x, int *screen_y);
static void check(int condition, const char *what);

/*
 * Runs the groups; the exit status says whether every check held.
 */
int
main(
	int argc,
	char **argv)
{
	int error;

	/* The font, the samples and the folder of the frames. */
	if (argc != 4) {
		fprintf(stderr, "usage: host-pdf-find-l FONT SAMPLES OUTDIR\n");
		return 2;
	}
	samples = argv[2];
	out_dir = argv[3];

	/* The viewer's font. */
	error = pv_text_open(&text, argv[1]);
	check(error == 0, "the font opens");
	if (error != 0)
		return 1;

	/* The groups. */
	test_forms();

	/* The viewer's font goes. */
	pv_text_close(&text);

	/* The verdict. */
	printf("host-pdf-find-l: %d passed, %d failed\n", passes, failures);
	if (failures != 0)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/*
 * forms.pdf (ws177-p032): the page's own lines, then the forms' text in
 * the order shown, each run of a form's text on lines of its own; the
 * nested form's; the turned form's characters turned; not the text inside
 * the Type 3 glyph's procedure.
 */
static void
test_forms(void)
{
	struct pdf_document *document;
	struct pdf_page_text *page_text;
	const double *quad;
	char path[1024];
	char lines[1024];
	char *found;
	size_t at;
	int error;

	/* The document. */
	(void)snprintf(path, sizeof(path), "%s/forms.pdf", samples);
	error = pdf_document_open(path, &document);
	check(error == 0, "forms: forms.pdf opens");
	if (error != 0)
		return;

	/* The page's text. */
	error = pdf_page_text_open(document, 0, &page_text);
	check(error == 0 && page_text != NULL, "forms: the page's text is read");
	if (error != 0) {
		pdf_document_close(document);
		return;
	}

	/* The lines in order: the page's two, then the forms'. */
	lines_of(page_text, lines, sizeof(lines));
	printf("forms: lines \"%s\"\n", lines);
	check(strcmp(lines, "Page line|a|Form one|Nested two|after nested a|Turned text|") == 0,
	      "forms: the page's lines, then each form's text on its lines (nested, after it, a glyph apart, turned)");

	/* The Type 3 glyph's own text is not the page's. */
	found = strstr(lines, "zz");
	check(found == NULL, "forms: the text inside a Type 3 glyph's procedure is not read");

	/* The turned form's T: its baseline runs down the page (its corners from top to bottom along y). */
	quad = NULL;
	for (at = 0; at < page_text->count; at++) {
		if (page_text->characters[at].character == 'T') {
			quad = page_text->characters[at].quad;
			break;
		}
	}
	check(quad != NULL, "forms: the turned form's T is there");
	if (quad != NULL) {
		printf("forms: T quad %.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f\n", quad[0], quad[1], quad[2], quad[3], quad[4], quad[5],
		       quad[6], quad[7]);
		check(fabs(quad[6] - quad[4]) < 0.01 && fabs(quad[7] - quad[5]) > 5.0,
		      "forms: the turned form's T advances along y, not x");
	}

	/* The text and the document go. */
	pdf_page_text_close(page_text);
	pdf_document_close(document);
}

/* Writes a page's text as its lines, each ended by '|' (ASCII; another character as '?'). */
static void
lines_of(
	const struct pdf_page_text *page_text,
	char *out,
	size_t size)
{
	size_t length;
	size_t at;
	uint32_t character;

	/* Each character, and '|' after a line's last. */
	length = 0;
	for (at = 0; at < page_text->count && length + 2U < size; at++) {
		character = page_text->characters[at].character;
		out[length] = '?';
		if (character < 0x80U)
			out[length] = (char)character;
		length++;
		if ((page_text->characters[at].flags & PDF_TEXT_LINE_END) != 0U) {
			out[length] = '|';
			length++;
		}
	}

	/* Ended. */
	out[length] = '\0';
}

/* Draws a frame and writes it as OUTDIR/NAME.ppm. */
static void
frame(
	const char *name)
{
	struct pv_canvas canvas;
	char path[1024];
	FILE *file;
	uint32_t pixel;
	size_t at;

	/* The frame. */
	canvas.pixels = pixels;
	canvas.stride = TEST_WIDTH;
	canvas.width = TEST_WIDTH;
	canvas.height = TEST_HEIGHT;
	pv_draw(&app, &canvas);

	/* The PPM. */
	(void)snprintf(path, sizeof(path), "%s/%s.ppm", out_dir, name);
	file = fopen(path, "wb");
	if (file == NULL)
		return;
	fprintf(file, "P6\n%d %d\n255\n", TEST_WIDTH, TEST_HEIGHT);
	for (at = 0; at < (size_t)TEST_WIDTH * TEST_HEIGHT; at++) {
		pixel = pixels[at];
		fputc((int)((pixel >> 16) & 0xffU), file);
		fputc((int)((pixel >> 8) & 0xffU), file);
		fputc((int)(pixel & 0xffU), file);
	}
	fclose(file);
}

/* Presses a key. */
static void
key(
	uint32_t code,
	uint32_t modifiers)
{
	struct pv_event event;

	/* A press. */
	memset(&event, 0, sizeof(event));
	event.type = PV_EVENT_KEY;
	event.key = code;
	event.pressed = 1;
	event.modifiers = modifiers;
	event.time = app.now;
	pv_app_event(&app, &event);
}

/* Presses or lets go the left button at a point of the window. */
static void
button(
	int x,
	int y,
	int pressed)
{
	struct pv_event event;

	/* The button, a little later than the last input. */
	memset(&event, 0, sizeof(event));
	event.type = PV_EVENT_BUTTON;
	event.button = PV_BUTTON_LEFT;
	event.pressed = pressed;
	event.x = x;
	event.y = y;
	app.now += 20U;
	event.time = app.now;
	pv_app_event(&app, &event);
}

/* Moves the pointer to a point of the window. */
static void
motion(
	int x,
	int y)
{
	struct pv_event event;

	/* The motion, a little later than the last input. */
	memset(&event, 0, sizeof(event));
	event.type = PV_EVENT_MOTION;
	event.x = x;
	event.y = y;
	app.now += 20U;
	event.time = app.now;
	pv_app_event(&app, &event);
}

/* Gives the window's point of a point of a page (points from its top left). */
static void
screen_of(
	size_t page,
	double x,
	double y,
	int *screen_x,
	int *screen_y)
{
	double scale;

	/* Through the page's place and scale in the view (no sidebar). */
	scale = pv_app_scale(&app, page);
	*screen_x = (int)(pv_app_page_left(&app, page) + x * scale);
	*screen_y = (int)(pv_app_page_top(&app, page) + y * scale - app.scroll_y);
}

/* Counts one check and prints it. */
static void
check(
	int condition,
	const char *what)
{
	/* Held. */
	if (condition) {
		passes++;
		printf("ok %s\n", what);
		return;
	}

	/* Failed. */
	failures++;
	printf("FAILED %s\n", what);
}
