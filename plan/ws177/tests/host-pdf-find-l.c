/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of case L of WS177 (ws177-p040 to p043): libpdf's page
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
static void test_find(void);
static void test_select(void);
static void test_turned(void);
static int open_sample(const char *name);
static void find_expect(const char *query, size_t page, const char *expected, const char *what);
static void matched(char *out, size_t size);
static void characters_of(size_t page, size_t from, size_t count, char *out, size_t size);
static void tick_until_counted(void);
static void point_of(size_t page, size_t index, int *x, int *y);
static void click(int x, int y);
static size_t utf8_put(uint32_t character, char *out);
static size_t line_start(size_t page, size_t line);
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

	/* Where the samples are and the frames go. */
	samples = argv[2];
	out_dir = argv[3];

	/* The viewer's font. */
	error = pv_text_open(&text, argv[1]);
	check(error == 0, "the font opens");
	if (error != 0)
		return 1;

	/* The groups. */
	test_forms();
	test_find();
	test_select();
	test_turned();

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
 * forms.pdf (ws177-p040): the page's own lines, then the forms' text in
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

	/* The turned form's T: its baseline runs 30 degrees up the page. */
	quad = NULL;
	for (at = 0; at < page_text->count; at++) {
		if (page_text->characters[at].character == 'T') {
			quad = page_text->characters[at].quad;
			break;
		}
	}

	/* Its corners. */
	check(quad != NULL, "forms: the turned form's T is there");
	if (quad != NULL) {
		printf("forms: T quad %.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f\n", quad[0], quad[1], quad[2], quad[3], quad[4], quad[5],
		       quad[6], quad[7]);
		check(fabs(quad[4] - quad[6]) > 1.0 && fabs(fabs((quad[5] - quad[7]) / (quad[4] - quad[6])) - 0.577) < 0.05,
		      "forms: the turned form's T advances 30 degrees off the line");
	}

	/* The text and the document go. */
	pdf_page_text_close(page_text);
	pdf_document_close(document);
}

/*
 * find.pdf (ws177-p041): each rule of the keys finds its line, the count
 * of the places grows as the ticks read the pages, and words found nowhere
 * say that some characters could not be read.
 */
static void
test_find(void)
{
	char status[128];
	int opened;

	/* The document, its first page in view. */
	opened = open_sample("find.pdf");
	if (!opened)
		return;

	/* A word broken by a hyphen at a line's end, with the hyphen and without. */
	find_expect("international", 0, "inter-national", "find: \"international\" across the hyphen at a line's end");
	find_expect("inter-national", 0, "inter-national", "find: \"inter-national\" with the hyphen");
	find_expect("well-known", 0, "well-known", "find: \"well-known\" across the line's end");
	find_expect("wellknown", 0, "well-known", "find: \"wellknown\" passing over the hyphen");

	/* Spaces: one in the words is a run of them on the page. */
	find_expect("many spaces here", 0, "Many    spaces   here", "find: one space for a run of spaces, and the case");

	/* Full width letters, half width kana with their voiced marks. */
	find_expect("full width abc", 0, "\xef\xbc\xa6\xef\xbc\xb5\xef\xbc\xac\xef\xbc\xac \xef\xbd\x97\xef\xbd\x89\xef\xbd\x84\xef\xbd\x94\xef\xbd\x88 "
		    "\xef\xbc\xa1\xef\xbc\xa2\xef\xbc\xa3", "find: ASCII finds the full width letters");
	find_expect("\xe3\x82\xac\xe3\x82\xae", 0, "\xef\xbd\xb6\xef\xbe\x9e\xef\xbd\xb7\xef\xbe\x9e", "find: \"ga gi\" finds the half width kana with marks");

	/* A kana with a spacing and with a combining voiced mark, both "ga". */
	find_expect("\xe3\x81\x8c", 0, "\xe3\x81\x8b\xe3\x82\x9b", "find: \"ga\" finds ka with a spacing voiced mark");
	pv_find_next(&app, 1);
	matched(status, sizeof(status));
	check(app.find_found && strcmp(status, "\xe3\x81\x8b\xe3\x82\x99") == 0, "find: F3 finds ka with a combining voiced mark");

	/* Greek (a final sigma) and Cyrillic without their cases; ligatures and a sharp s spelled out. */
	find_expect("\xce\xb1\xce\xb8\xce\xb7\xce\xbd\xce\xb1 \xcf\x83\xce\xbf\xcf\x86\xce\xbf\xcf\x82", 0,
		    "\xce\x91\xce\x98\xce\x97\xce\x9d\xce\x91 \xce\xa3\xce\x9f\xce\xa6\xce\x9f\xce\xa3", "find: Greek in small letters, a final sigma");
	find_expect("\xd0\xbc\xd0\xbe\xd1\x81\xd0\xba\xd0\xb2\xd0\xb0", 0, "\xd0\x9c\xd0\x9e\xd0\xa1\xd0\x9a\xd0\x92\xd0\x90", "find: Cyrillic in small letters");
	find_expect("find the flow", 0, "\xef\xac\x81nd the \xef\xac\x82ow", "find: the fi and fl ligatures");
	find_expect("STRASSE", 0, "Stra\xc3\x9f" "e", "find: a sharp s as ss");

	/* Japanese across a line's end, without a space. */
	find_expect("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe6\x96\x87\xe7\xab\xa0", 0,
		    "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe6\x96\x87\xe7\xab\xa0", "find: Japanese across a line's end");

	/* Words found nowhere, some characters unreadable (page 2's). */
	pv_find_text(&app, "zebra");
	check(!app.find_found && strcmp(app.message, "Not found (some text could not be read)") == 0,
	      "find: nothing found says some text could not be read");

	/* The count: "lazy" on page 1, page 2 and the 30 after, numbered once every page is counted. */
	pv_app_go_to(&app, 0);
	app.scroll_y = 0.0;
	pv_app_clamp(&app);
	pv_find_text(&app, "lazy");
	check(app.find_found && app.find_page == 0, "find: \"lazy\" on page 1");
	tick_until_counted();
	check(app.find_counted && app.find_total == 32U, "find: the ticks count 32 places");
	pv_find_status(&app, status, sizeof(status));
	printf("find: status \"%s\" message \"%s\"\n", status, app.message);
	check(strcmp(status, "Match 1 of 32") == 0 && strcmp(app.message, "Match 1 of 32") == 0, "find: \"Match 1 of 32\"");
	key(PV_KEY_F3, 0);
	check(app.find_page == 1 && strcmp(app.message, "Match 2 of 32") == 0, "find: F3: \"Match 2 of 32\" on page 2");
	frame("find-01-match-2");

	/* A copy of the whole document counts the three unreadable characters. */
	key(PV_KEY_A, PV_MOD_CTRL);
	key(PV_KEY_C, PV_MOD_CTRL);
	check(app.copy_text != NULL && strcmp(app.message, "3 characters could not be read") == 0,
	      "find: Ctrl+A, Ctrl+C: the copy tells 3 characters could not be read");
	free(app.copy_text);
	app.copy_text = NULL;

	/* The document goes. */
	pv_app_release(&app);
}

/*
 * The selection (ws177-p042) on find.pdf: a drag from page 2 onto page 3,
 * two and three clicks, a long press and its end's handle moved.
 */
static void
test_select(void)
{
	char words[256];
	int from_x;
	int from_y;
	int to_x;
	int to_y;
	int handle;
	int opened;
	int taken;
	int copied;

	/* The document; the words of pages 1 to 3 read (Find reads them on its way to page 3's). */
	opened = open_sample("find.pdf");
	if (!opened)
		return;
	pv_find_text(&app, "page 3");
	check(app.find_found && app.find_page == 2, "select: \"page 3\" is on page 3");
	pv_find_clear(&app);

	/* A drag from page 2's "page" (after the three unreadable characters) to page 3's "y" of "lazy". */
	pv_app_go_to(&app, 1);
	point_of(1, 3, &from_x, &from_y);
	point_of(2, 3, &to_x, &to_y);
	button(from_x, from_y, 1);
	motion(from_x + 30, from_y);
	motion(to_x, to_y);
	button(to_x, to_y, 0);
	check(app.has_selection && app.select_page == 1 && app.select_anchor == 3 && app.select_caret_page == 2 && app.select_caret == 3,
	      "select: a drag from page 2 onto page 3");
	key(PV_KEY_C, PV_MOD_CTRL);
	copied = 0;
	if (app.copy_text != NULL)
		copied = !strcmp(app.copy_text, "page two has a lazy dog\nlazy");
	check(copied, "select: the copy of both pages' parts");
	free(app.copy_text);
	app.copy_text = NULL;

	/* Two clicks on page 1's "quick" (its 15th line, "The quick brown fox ...": q at 4) select it, three its line. */
	pv_app_go_to(&app, 0);
	point_of(0, line_start(0, 14) + 5U, &from_x, &from_y);
	click(from_x, from_y);
	click(from_x, from_y);
	characters_of(app.select_page, app.select_anchor, app.select_caret - app.select_anchor + 1U, words, sizeof(words));
	check(app.has_selection && strcmp(words, "quick") == 0, "select: two clicks select the word \"quick\"");
	click(from_x, from_y);
	characters_of(app.select_page, app.select_anchor, app.select_caret - app.select_anchor + 1U, words, sizeof(words));
	check(app.has_selection && strcmp(words, "The quick brown fox jumps over the lazy dog") == 0, "select: three clicks select the line");
	frame("select-01-line");

	/* A long press on "brown" selects it with the handles; the end's handle moved onto "fox" takes it in. */
	app.now += 1000U;
	point_of(0, line_start(0, 14) + 11U, &from_x, &from_y);
	taken = pv_select_word_at(&app, from_x, from_y);
	characters_of(app.select_page, app.select_anchor, app.select_caret - app.select_anchor + 1U, words, sizeof(words));
	check(taken && app.select_handles && strcmp(words, "brown") == 0, "select: a long press selects \"brown\" with handles");
	frame("select-02-handles");
	point_of(0, line_start(0, 14) + 14U, &to_x, &to_y);
	handle = pv_select_handle_at(&app, to_x, to_y + 2);
	check(handle == 2, "select: the end's handle is under the finger at the word's end");
	point_of(0, line_start(0, 14) + 18U, &to_x, &to_y);
	pv_select_handle_move(&app, handle, to_x, to_y);
	characters_of(app.select_page, app.select_anchor, app.select_caret - app.select_anchor + 1U, words, sizeof(words));
	check(strcmp(words, "brown fox") == 0, "select: the handle moved takes in \"fox\"");

	/* The start's handle moved past the end becomes the end. */
	point_of(0, line_start(0, 14) + 22U, &to_x, &to_y);
	pv_select_handle_move(&app, 1, to_x, to_y);
	characters_of(app.select_page, app.select_anchor, app.select_caret - app.select_anchor + 1U, words, sizeof(words));
	check(strcmp(words, "x jum") == 0, "select: the start's handle moved past the end swaps them");

	/* A tap lets it go. */
	pv_select_clear(&app);
	check(!app.has_selection && !app.select_handles, "select: a tap lets the selection go");

	/* The document goes. */
	pv_app_release(&app);
}

/*
 * forms.pdf (ws177-p042): the turned form's characters are marked as they
 * lie: the middle of its T changes when all is selected, a corner of the
 * box around it (outside the turned glyph) does not.
 */
static void
test_turned(void)
{
	static uint32_t before[TEST_WIDTH * TEST_HEIGHT];
	const struct pdf_page_text *page_text;
	const double *quad;
	double left;
	double top;
	double right;
	double bottom;
	size_t at;
	int middle_x;
	int middle_y;
	int corner_x;
	int corner_y;
	int opened;
	unsigned corner;

	/* The document, its text read. */
	opened = open_sample("forms.pdf");
	if (!opened)
		return;
	pv_find_text(&app, "Turned");
	check(app.find_found, "turned: the turned words are found");
	pv_find_clear(&app);

	/* The T and the box around its corners, in points. */
	page_text = app.document.pages[0].text;
	quad = NULL;
	for (at = 0; page_text != NULL && at < page_text->count; at++) {
		if (page_text->characters[at].character == 'T') {
			quad = page_text->characters[at].quad;
			break;
		}
	}

	/* The T, found. */
	check(quad != NULL, "turned: the T is there");
	if (quad == NULL) {
		pv_app_release(&app);
		return;
	}

	/* The box around its corners. */
	left = quad[0];
	right = quad[0];
	top = quad[1];
	bottom = quad[1];
	for (corner = 1; corner < 4U; corner++) {
		left = fmin(left, quad[corner * 2U]);
		right = fmax(right, quad[corner * 2U]);
		top = fmin(top, quad[corner * 2U + 1U]);
		bottom = fmax(bottom, quad[corner * 2U + 1U]);
	}

	/* Its middle and the box's top left corner, a point inward, in the window. */
	screen_of(0, (quad[0] + quad[4]) / 2.0, (quad[1] + quad[5]) / 2.0, &middle_x, &middle_y);
	screen_of(0, left + 0.6, top + 0.6, &corner_x, &corner_y);

	/* The frame without, then with the selection of everything. */
	frame("turned-00-plain");
	memcpy(before, pixels, sizeof(before));
	key(PV_KEY_A, PV_MOD_CTRL);
	frame("turned-01-all");
	check(middle_x >= 0 && middle_x < TEST_WIDTH && middle_y >= 0 && middle_y < TEST_HEIGHT, "turned: the T is in the window");
	if (middle_x >= 0 && middle_x < TEST_WIDTH && middle_y >= 0 && middle_y < TEST_HEIGHT) {
		check(pixels[middle_y * TEST_WIDTH + middle_x] != before[middle_y * TEST_WIDTH + middle_x], "turned: the T's middle is marked");
		check(pixels[corner_y * TEST_WIDTH + corner_x] == before[corner_y * TEST_WIDTH + corner_x],
		      "turned: the box's corner outside the turned T is not");
	}

	/* The document goes. */
	pv_app_release(&app);
}

/* Opens a sample in the viewer, fitted to the window's width, its first page at the top; returns 1 when it opened. */
static int
open_sample(
	const char *name)
{
	char path[1024];
	int error;

	/* A fresh viewer on the document. */
	memset(&app, 0, sizeof(app));
	pv_app_init(&app, &text, TEST_WIDTH, TEST_HEIGHT);
	app.now = 1000;
	(void)snprintf(path, sizeof(path), "%s/%s", samples, name);
	error = pv_app_open(&app, path);
	check(error == 0 && app.has_document, name);
	if (error != 0)
		return 0;

	/* Opened. */
	return 1;
}

/* Types words in the find field and checks the place found: its page and its characters. */
static void
find_expect(
	const char *query,
	size_t page,
	const char *expected,
	const char *what)
{
	char words[256];
	int same;

	/* The words from the first page. */
	pv_app_go_to(&app, 0);
	pv_find_text(&app, query);
	matched(words, sizeof(words));
	same = strcmp(words, expected);
	if (!app.find_found || app.find_page != page || same != 0)
		printf("find: \"%s\" found=%d page=%lu words \"%s\"\n", query, app.find_found, (unsigned long)app.find_page, words);
	check(app.find_found && app.find_page == page && same == 0, what);
}

/* Writes the characters of the place found as UTF-8. */
static void
matched(
	char *out,
	size_t size)
{
	/* Nothing found. */
	out[0] = '\0';
	if (!app.find_found)
		return;

	/* Its characters. */
	characters_of(app.find_page, app.find_from, app.find_length, out, size);
}

/* Writes characters of a page's text as UTF-8 (no line ends). */
static void
characters_of(
	size_t page,
	size_t from,
	size_t count,
	char *out,
	size_t size)
{
	const struct pdf_page_text *page_text;
	size_t length;
	size_t at;

	/* The page's text. */
	out[0] = '\0';
	page_text = app.document.pages[page].text;
	if (page_text == NULL)
		return;

	/* Each character within the room. */
	length = 0;
	for (at = from; at < from + count && at < page_text->count && length + 5U < size; at++)
		length += utf8_put(page_text->characters[at].character, out + length);
	out[length] = '\0';
}

/* Moves time on, ticking the viewer, until every page is counted (or a bound). */
static void
tick_until_counted(void)
{
	unsigned ticks;

	/* Ticks a millisecond apart. */
	for (ticks = 0; ticks < 1000U && !app.find_counted; ticks++) {
		app.now++;
		(void)pv_app_tick(&app, app.now);
	}
}

/* Gives the window's point at the middle of a character of a page (read already). */
static void
point_of(
	size_t page,
	size_t index,
	int *x,
	int *y)
{
	const double *quad;

	/* The character's middle. */
	quad = app.document.pages[page].text->characters[index].quad;
	screen_of(page, (quad[0] + quad[4]) / 2.0, (quad[1] + quad[5]) / 2.0, x, y);
}

/* Presses and lets go the left button at a point. */
static void
click(
	int x,
	int y)
{
	/* Down, then up. */
	button(x, y, 1);
	button(x, y, 0);
}

/* Writes a character as UTF-8; returns its bytes. */
static size_t
utf8_put(
	uint32_t character,
	char *out)
{
	/* One byte. */
	if (character < 0x80U) {
		out[0] = (char)character;
		return 1;
	}

	/* Two bytes. */
	if (character < 0x800U) {
		out[0] = (char)(0xc0U | (character >> 6));
		out[1] = (char)(0x80U | (character & 0x3fU));
		return 2;
	}

	/* Three bytes. */
	if (character < 0x10000U) {
		out[0] = (char)(0xe0U | (character >> 12));
		out[1] = (char)(0x80U | ((character >> 6) & 0x3fU));
		out[2] = (char)(0x80U | (character & 0x3fU));
		return 3;
	}

	/* Four bytes. */
	out[0] = (char)(0xf0U | (character >> 18));
	out[1] = (char)(0x80U | ((character >> 12) & 0x3fU));
	out[2] = (char)(0x80U | ((character >> 6) & 0x3fU));
	out[3] = (char)(0x80U | (character & 0x3fU));
	return 4;
}

/* Gives the index of a page's character that starts a line (counted from 0). */
static size_t
line_start(
	size_t page,
	size_t line)
{
	const struct pdf_page_text *page_text;
	size_t at;
	size_t seen;

	/* Each line's end until the line asked for starts. */
	page_text = app.document.pages[page].text;
	seen = 0;
	for (at = 0; page_text != NULL && at < page_text->count; at++) {
		if (seen == line)
			return at;
		if ((page_text->characters[at].flags & PDF_TEXT_LINE_END) != 0U)
			seen++;
	}

	/* Past the last. */
	return 0;
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

	/* Written. */
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
