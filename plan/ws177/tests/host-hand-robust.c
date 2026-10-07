/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the handwriting's robustness (ws177-p009, backlog-p2
 * lines 55 to 60), with the compositor's keyboard-hand.c and hand-cloud.c:
 *
 *   - a tap, and ink of a pixel or two, answer no candidates but the note
 *     "Too small to read" (55);
 *   - a missing templates' file, an empty one, one of comments only, one
 *     with a broken line and one too large fail the read, and the
 *     recognition answers the note "No handwriting data" (56);
 *   - a dakuten or handakuten written in the body of its kana or at its
 *     top left still makes the voiced kana the first candidate, counted
 *     over every voiced kana of the templates (58);
 *   - the templates read on the reader thread (kwl_hand_preload) are used
 *     by the next recognition, and after a failed read the note is given
 *     at once (59);
 *   - ink of more than HAND_CLOUD_INPUT_MAX points, a voiced kana whose
 *     mark is written last, keeps its mark (60).
 *
 *   host-hand-robust TEMPLATES WORK
 *
 * TEMPLATES is the package hand-hershey's file; WORK a new directory for
 * the broken files.  The program is built with KEILAND_DATADIR naming a
 * directory whose keiland/hand/hershey.txt is TEMPLATES, so that the
 * reader thread reads the real file at its real path.
 */

#include "keyboard.h"
#include "hand-cloud.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The writing area's height, in pixels: a character is written on it as
 * its template is on the Hershey glyphs' area (y from -16 to 16).  The
 * dense ink of the last check is written at TEST_DENSE_SCALE pixels a
 * unit on an area of 32 units.
 */
#define TEST_AREA		300
#define TEST_DENSE_SCALE	50.0f

/* The most points and strokes of a template read here. */
#define TEST_POINTS		4096U
#define TEST_STROKES		64U

/* The share of displaced marks that must still give the voiced kana first. */
#define TEST_MARK_GOAL		0.90

/*
 * One template as its strokes, in Hershey's units: its character, its
 * points, and for each point whether it starts a stroke.  The test's
 * tables of them live for the whole run.
 */
struct test_glyph {
	uint32_t code;
	size_t count;
	size_t strokes;
	float x[TEST_POINTS];
	float y[TEST_POINTS];
	unsigned char starts[TEST_POINTS];
};

/* The checks that failed, counted for the exit status. */
static unsigned test_failures;

/* The templates' text, read once. */
static char test_text[1024U * 1024U];
static size_t test_length;

/* The ink a check writes; it is too large for the stack. */
static struct kwl_hand_ink test_ink;

static void test_check(int condition, const char *what);
static int test_read(const char *path);
static int test_write_file(const char *path, const char *text, size_t length);
static const char *test_find(uint32_t code);
static int test_glyph_read(uint32_t code, struct test_glyph *glyph);
static void test_ink_glyph(const struct test_glyph *glyph, size_t from, size_t to, float dx, float dy, struct kwl_hand_ink *ink);
static void test_ink_dense(const struct test_glyph *glyph, size_t stroke, unsigned points, struct kwl_hand_ink *ink);
static size_t test_stroke_begin(const struct test_glyph *glyph, size_t stroke);
static void test_utf8(uint32_t code, char *text);
static void test_scant(void);
static void test_broken(const char *work);
static void test_marks(void);
static void test_preload(const char *templates);
static void test_dense(void);

/*
 * Runs the checks; the exit status says whether they all held.
 */
int
main(
	int argc,
	char **argv)
{
	int error;

	/* Refuses a call without the templates and the work directory. */
	if (argc < 3) {
		fprintf(stderr, "usage: host-hand-robust TEMPLATES WORK\n");
		return 2;
	}

	/* Reads the templates' text for writing their characters as ink. */
	error = test_read(argv[1]);
	if (error != 0) {
		fprintf(stderr, "host-hand-robust: %s: error %d\n", argv[1], error);
		return 2;
	}

	/* The reader thread first, while no template is read yet (59). */
	test_preload(argv[1]);

	/* Then each row of the backlog. */
	test_scant();
	test_marks();
	test_dense();
	test_broken(argv[2]);

	/* Reports the checks that failed. */
	if (test_failures != 0U) {
		printf("host-hand-robust: %u FAILED\n", test_failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("host-hand-robust: PASS\n");
	return 0;
}

/* Counts and reports a check that does not hold; a check that holds is said too. */
static void
test_check(
	int condition,
	const char *what)
{
	/* A check that holds. */
	if (condition) {
		printf("ok: %s\n", what);
		return;
	}

	/* A failure. */
	test_failures++;
	printf("FAIL: %s\n", what);
}

/* Reads the templates' text into test_text.  Returns 0 or an errno value. */
static int
test_read(
	const char *path)
{
	FILE *file;

	/* Opens the file. */
	file = fopen(path, "r");
	if (file == NULL)
		return errno;

	/* Reads it whole, ended by a zero. */
	test_length = fread(test_text, 1U, sizeof(test_text) - 1U, file);
	fclose(file);
	test_text[test_length] = '\0';

	/* Succeeded: the text. */
	return 0;
}

/* Writes a file of a text.  Returns 0 or an errno value. */
static int
test_write_file(
	const char *path,
	const char *text,
	size_t length)
{
	FILE *file;
	size_t written;

	/* Creates the file. */
	file = fopen(path, "w");
	if (file == NULL)
		return errno;

	/* Writes the text. */
	written = fwrite(text, 1U, length, file);
	fclose(file);
	if (written != length)
		return EIO;

	/* Succeeded: written. */
	return 0;
}

/* Finds a template's line in the text, or NULL. */
static const char *
test_find(
	uint32_t code)
{
	char key[16];
	const char *found;

	/* Looks for the line that begins with the code point. */
	(void)snprintf(key, sizeof(key), "\nU+%04X ", (unsigned)code);
	found = strstr(test_text, key);
	if (found == NULL)
		return NULL;

	/* Succeeded: the line, past its newline. */
	return found + 1;
}

/* Reads a template's strokes.  Returns 0, or -1 when the character has no template. */
static int
test_glyph_read(
	uint32_t code,
	struct test_glyph *glyph)
{
	const char *line;
	const char *word;
	char *end;
	int fresh;

	/* The template's line, past the code point and the Hershey number. */
	line = test_find(code);
	if (line == NULL)
		return -1;
	word = strchr(line, ' ');
	word = strchr(word + 1, ' ');

	/* Reads its points; "/" starts a stroke. */
	glyph->code = code;
	glyph->count = 0U;
	glyph->strokes = 0U;
	fresh = 1;
	while (*word != '\n' && *word != '\0' && glyph->count < TEST_POINTS) {
		/* The spaces between the words. */
		if (*word == ' ') {
			word++;
			continue;
		}

		/* A stroke's end. */
		if (*word == '/') {
			fresh = 1;
			word++;
			continue;
		}

		/* A point. */
		glyph->x[glyph->count] = strtof(word, &end);
		glyph->y[glyph->count] = strtof(end + 1, &end);
		word = end;
		glyph->starts[glyph->count] = (unsigned char)fresh;
		if (fresh)
			glyph->strokes++;
		fresh = 0;
		glyph->count++;
	}

	/* Succeeded: the strokes. */
	return 0;
}

/* Gives the index of a glyph's stroke's first point (the count for the stroke after the last). */
static size_t
test_stroke_begin(
	const struct test_glyph *glyph,
	size_t stroke)
{
	size_t index;
	size_t seen;

	/* Counts the strokes' starts up to the one asked for. */
	seen = 0U;
	for (index = 0U; index < glyph->count; index++) {
		if (!glyph->starts[index])
			continue;
		if (seen == stroke)
			return index;
		seen++;
	}

	/* The stroke after the last. */
	return glyph->count;
}

/*
 * Adds a glyph's strokes from..to (not including to) to ink, on the
 * writing area of TEST_AREA pixels for the Hershey glyphs' 32 units, each
 * point moved by dx, dy units.
 */
static void
test_ink_glyph(
	const struct test_glyph *glyph,
	size_t from,
	size_t to,
	float dx,
	float dy,
	struct kwl_hand_ink *ink)
{
	size_t point;
	size_t first;
	size_t last;
	float scale;
	int32_t x;
	int32_t y;

	/* The points of the strokes asked for. */
	scale = (float)TEST_AREA / 32.0f;
	first = test_stroke_begin(glyph, from);
	last = test_stroke_begin(glyph, to);
	for (point = first; point < last; point++) {
		x = (int32_t)lroundf(40.0f + (glyph->x[point] + dx + 16.0f) * scale);
		y = (int32_t)lroundf((glyph->y[point] + dy + 16.0f) * scale);

		/* A stroke's first point begins it; the others follow it. */
		if (glyph->starts[point])
			(void)kwl_hand_begin(ink, x, y);
		else
			(void)kwl_hand_add(ink, x, y);
	}
}

/*
 * Adds one stroke of a glyph to ink as points of it, along it at
 * TEST_DENSE_SCALE pixels a unit, every other one a pixel aside so that
 * each moves.
 */
static void
test_ink_dense(
	const struct test_glyph *glyph,
	size_t stroke,
	unsigned points,
	struct kwl_hand_ink *ink)
{
	size_t first;
	size_t last;
	size_t segment;
	unsigned point;
	float along;
	float share;
	float scale;
	float x;
	float y;

	/* The stroke's points in the glyph. */
	first = test_stroke_begin(glyph, stroke);
	last = test_stroke_begin(glyph, stroke + 1U);
	scale = TEST_DENSE_SCALE;

	/* Spreads the points evenly over the stroke's segments. */
	for (point = 0U; point < points; point++) {
		along = (float)point * (float)(last - first - 1U) / (float)(points - 1U);
		segment = first + (size_t)along;
		if (segment >= last - 1U)
			segment = last - 2U;
		share = along - (float)(segment - first);
		x = glyph->x[segment] + share * (glyph->x[segment + 1U] - glyph->x[segment]);
		y = glyph->y[segment] + share * (glyph->y[segment + 1U] - glyph->y[segment]);
		x = 40.0f + (x + 16.0f) * scale + (float)(point & 1U);
		y = (y + 16.0f) * scale;

		/* A stroke's first point begins it; the others follow it. */
		if (point == 0U)
			(void)kwl_hand_begin(ink, (int32_t)x, (int32_t)y);
		else
			(void)kwl_hand_add(ink, (int32_t)x, (int32_t)y);
	}
}

/* Writes a code point as UTF-8 (one to three bytes are enough here). */
static void
test_utf8(
	uint32_t code,
	char *text)
{
	/* One byte. */
	if (code < 0x80U) {
		text[0] = (char)code;
		text[1] = '\0';
		return;
	}

	/* Two bytes. */
	if (code < 0x800U) {
		text[0] = (char)(0xc0U | (code >> 6));
		text[1] = (char)(0x80U | (code & 0x3fU));
		text[2] = '\0';
		return;
	}

	/* Three bytes. */
	text[0] = (char)(0xe0U | (code >> 12));
	text[1] = (char)(0x80U | ((code >> 6) & 0x3fU));
	text[2] = (char)(0x80U | (code & 0x3fU));
	text[3] = '\0';
}

/* Checks that a tap, and ink of a pixel or two, are not read (backlog-p2 line 55). */
static void
test_scant(void)
{
	struct hand_cloud_input input;
	struct hand_frame frame;
	struct kwl_hand_result result;
	int none;
	int scant;

	/* A stroke list without points is too little ink for the recognizer itself. */
	memset(&input, 0, sizeof(input));
	frame.top = 0.0f;
	frame.height = (float)TEST_AREA;
	scant = hand_ink_scant(&input, &frame);
	test_check(scant, "an empty stroke list is too little ink");

	/* A tap: one point. */
	kwl_hand_clear(&test_ink);
	(void)kwl_hand_begin(&test_ink, 100, 250);
	kwl_hand_recognize_on(&test_ink, 0, TEST_AREA, &result);
	none = 0;
	if (result.count == 0U && strcmp(result.note, "Too small to read") == 0)
		none = 1;
	test_check(none, "a tap is too small to read");

	/* A tap that slipped two pixels. */
	(void)kwl_hand_add(&test_ink, 101, 251);
	(void)kwl_hand_add(&test_ink, 102, 251);
	kwl_hand_recognize_on(&test_ink, 0, TEST_AREA, &result);
	none = 0;
	if (result.count == 0U && strcmp(result.note, "Too small to read") == 0)
		none = 1;
	test_check(none, "a tap that slipped is too small to read");

	/* Three taps apart: strokes that never move. */
	kwl_hand_clear(&test_ink);
	(void)kwl_hand_begin(&test_ink, 100, 250);
	(void)kwl_hand_begin(&test_ink, 150, 100);
	(void)kwl_hand_begin(&test_ink, 200, 100);
	kwl_hand_recognize_on(&test_ink, 0, TEST_AREA, &result);
	none = 0;
	if (result.count == 0U && strcmp(result.note, "Too small to read") == 0)
		none = 1;
	test_check(none, "taps apart are too small to read");

	/* A short line is a character's. */
	kwl_hand_clear(&test_ink);
	(void)kwl_hand_begin(&test_ink, 100, 60);
	(void)kwl_hand_add(&test_ink, 100, 160);
	(void)kwl_hand_add(&test_ink, 100, 240);
	kwl_hand_recognize_on(&test_ink, 0, TEST_AREA, &result);
	test_check(result.count > 0U, "a line is read");
}

/*
 * Checks that a voiced kana's mark written in its body or at its top left
 * still gives the voiced kana first (backlog-p2 line 58), over all the
 * voiced kana of the templates: a voiced template is its kana's strokes
 * and the mark's after them.
 */
static void
test_marks(void)
{
	static struct test_glyph voiced;
	static struct test_glyph plain;
	static const char *const places[3] = { "top right (as written)", "in the body", "top left" };
	struct kwl_hand_result result;
	char expected[8];
	char what[96];
	uint32_t code;
	uint32_t base;
	size_t kana;
	size_t strokes;
	size_t point;
	size_t first;
	unsigned place;
	unsigned hits[3];
	float mark_x;
	float mark_y;
	float body_left;
	float body_top;
	float body_x;
	float body_y;
	float dx;
	float dy;
	int read;
	int same;

	/* Nothing counted yet. */
	kana = 0U;
	memset(hits, 0, sizeof(hits));

	/* Each voiced kana: the code point before it (dakuten) or two before (handakuten) is its kana. */
	for (code = 0x3041U; code <= 0x30f6U; code++) {
		read = test_glyph_read(code, &voiced);
		if (read != 0)
			continue;

		/* Finds its kana: a template whose strokes begin this one's. */
		for (base = code - 1U; base + 2U >= code; base--) {
			read = test_glyph_read(base, &plain);
			if (read != 0)
				continue;
			if (plain.strokes >= voiced.strokes)
				continue;
			same = memcmp(plain.x, voiced.x, plain.count * sizeof(float));
			if (same == 0)
				break;
		}

		/* Not a voiced kana. */
		if (base + 2U < code)
			continue;
		kana++;

		/* The mark's middle, and the body's box. */
		first = test_stroke_begin(&voiced, plain.strokes);
		mark_x = 0.0f;
		mark_y = 0.0f;
		for (point = first; point < voiced.count; point++) {
			mark_x += voiced.x[point];
			mark_y += voiced.y[point];
		}
		mark_x /= (float)(voiced.count - first);
		mark_y /= (float)(voiced.count - first);
		body_left = plain.x[0];
		body_top = plain.y[0];
		body_x = 0.0f;
		body_y = 0.0f;
		for (point = 0U; point < plain.count; point++) {
			body_left = fminf(body_left, plain.x[point]);
			body_top = fminf(body_top, plain.y[point]);
			body_x += plain.x[point];
			body_y += plain.y[point];
		}
		body_x /= (float)plain.count;
		body_y /= (float)plain.count;

		/* Each place: as written, in the body's middle, at its top left. */
		for (place = 0U; place < 3U; place++) {
			dx = 0.0f;
			dy = 0.0f;
			if (place == 1U) {
				dx = body_x - mark_x;
				dy = body_y - mark_y;
			} else if (place == 2U) {
				dx = body_left - mark_x;
				dy = body_top - mark_y;
			}

			/* Writes the kana, then its mark at the place. */
			kwl_hand_clear(&test_ink);
			strokes = voiced.strokes;
			test_ink_glyph(&voiced, 0U, plain.strokes, 0.0f, 0.0f, &test_ink);
			test_ink_glyph(&voiced, plain.strokes, strokes, dx, dy, &test_ink);
			kwl_hand_recognize_on(&test_ink, 0, TEST_AREA, &result);

			/* Counts it when the voiced kana is the first candidate, and says which were not. */
			test_utf8(code, expected);
			if (result.count > 0U && strcmp(result.candidates[0], expected) == 0) {
				hits[place]++;
				continue;
			}
			printf("  %s %s: %s\n", expected, places[place], result.count > 0U ? result.candidates[0] : "(none)");
		}
	}

	/* Reports each place's share. */
	for (place = 0U; place < 3U; place++) {
		(void)snprintf(what, sizeof(what), "the mark %s: %u of %lu voiced kana first", places[place], hits[place], (unsigned long)kana);
		test_check(kana != 0U && (double)hits[place] >= TEST_MARK_GOAL * (double)kana, what);
	}
}

/*
 * Checks the reader thread (backlog-p2 line 59): the templates it reads
 * at their real path are used by the next recognition; a second start is
 * nothing more.
 */
static void
test_preload(
	const char *templates)
{
	static struct test_glyph glyph;
	struct kwl_hand_result result;
	int read;
	int first;

	/* Starts the reader, twice: the second start finds it running. */
	memset(&result, 0, sizeof(result));
	kwl_hand_preload(&result);
	kwl_hand_preload(&result);
	test_check(result.note[0] == '\0', "the reader starts without a note");

	/* Writes あ; the recognition takes the reader's templates. */
	read = test_glyph_read(0x3042U, &glyph);
	kwl_hand_clear(&test_ink);
	if (read == 0)
		test_ink_glyph(&glyph, 0U, glyph.strokes, 0.0f, 0.0f, &test_ink);
	kwl_hand_recognize_on(&test_ink, 0, TEST_AREA, &result);
	first = 0;
	if (result.count > 0U && strcmp(result.candidates[0], "あ") == 0)
		first = 1;
	test_check(first, "the templates read on the reader thread recognize あ");

	/* A start after the read is nothing more. */
	memset(&result, 0, sizeof(result));
	kwl_hand_preload(&result);
	test_check(result.note[0] == '\0', "a start after the read gives no note");
	printf("  (templates %s)\n", templates);
}

/*
 * Checks ink of more points than the recognizer takes (backlog-p2 line
 * 60): が with its three strokes written over five times each and its
 * mark's two strokes last, 512 points each: 8704 points.
 */
static void
test_dense(void)
{
	static struct test_glyph glyph;
	struct kwl_hand_result result;
	size_t stroke;
	unsigned again;
	unsigned total;
	int read;
	int first;

	/* The template of が. */
	read = test_glyph_read(0x304cU, &glyph);
	test_check(read == 0 && glyph.strokes == 5U, "が has three strokes and a mark of two");
	if (read != 0)
		return;

	/* Its strokes written over, then the mark. */
	kwl_hand_clear(&test_ink);
	for (again = 0U; again < 5U; again++) {
		for (stroke = 0U; stroke < 3U; stroke++)
			test_ink_dense(&glyph, stroke, KWL_HAND_POINTS, &test_ink);
	}
	test_ink_dense(&glyph, 3U, KWL_HAND_POINTS, &test_ink);
	test_ink_dense(&glyph, 4U, KWL_HAND_POINTS, &test_ink);
	total = kwl_hand_points(&test_ink);
	printf("  dense ink: %u strokes, %u points\n", test_ink.count, total);
	test_check(total > HAND_CLOUD_INPUT_MAX, "the ink has more points than the recognizer takes");

	/* Recognized with its mark. */
	kwl_hand_recognize_on(&test_ink, 0, (int32_t)(32.0f * TEST_DENSE_SCALE), &result);
	first = 0;
	if (result.count > 0U && strcmp(result.candidates[0], "が") == 0)
		first = 1;
	printf("  dense ink first: %s\n", result.count > 0U ? result.candidates[0] : "(none)");
	test_check(first, "ink of more points keeps its last strokes: が");
}

/*
 * Checks templates' files that do not read (backlog-p2 line 56): each
 * fails the read and the recognition gives the note instead.
 */
static void
test_broken(
	const char *work)
{
	static const char comments[] = "# no template here\n\n# nor here\n";
	static const char broken[] = "U+0041 501 0,-12 -8,9 / -5,2 5,\n";
	static const char points_none[] = "U+0041 501\n";
	static const char word_bad[] = "U+0041 501 0,-12 -8,9x\n";
	struct kwl_hand_result result;
	char path[1024];
	char *large;
	int error;
	int noted;

	/* A missing file. */
	(void)snprintf(path, sizeof(path), "%s/missing.txt", work);
	error = kwl_hand_load(path);
	test_check(error == ENOENT, "a missing file fails the read");
	kwl_hand_clear(&test_ink);
	(void)kwl_hand_begin(&test_ink, 100, 60);
	(void)kwl_hand_add(&test_ink, 100, 240);
	kwl_hand_recognize_on(&test_ink, 0, TEST_AREA, &result);
	noted = 0;
	if (result.count == 0U && strcmp(result.note, "No handwriting data") == 0)
		noted = 1;
	test_check(noted, "without templates the recognition says No handwriting data");

	/* After a failed read, showing the face gives the note at once. */
	memset(&result, 0, sizeof(result));
	kwl_hand_preload(&result);
	test_check(strcmp(result.note, "No handwriting data") == 0, "the face shown after a failed read gives the note");

	/* An empty file. */
	(void)snprintf(path, sizeof(path), "%s/empty.txt", work);
	(void)test_write_file(path, "", 0U);
	error = kwl_hand_load(path);
	test_check(error == ENOENT, "an empty file fails the read");

	/* A file of comments only. */
	(void)snprintf(path, sizeof(path), "%s/comments.txt", work);
	(void)test_write_file(path, comments, sizeof(comments) - 1U);
	error = kwl_hand_load(path);
	test_check(error == ENOENT, "a file of comments only fails the read");

	/* A line cut in a point. */
	(void)snprintf(path, sizeof(path), "%s/broken.txt", work);
	(void)test_write_file(path, broken, sizeof(broken) - 1U);
	error = kwl_hand_load(path);
	test_check(error == EINVAL, "a line cut in a point fails the read");

	/* A line without points. */
	(void)snprintf(path, sizeof(path), "%s/points-none.txt", work);
	(void)test_write_file(path, points_none, sizeof(points_none) - 1U);
	error = kwl_hand_load(path);
	test_check(error == EINVAL, "a line without points fails the read");

	/* A point with a stray letter. */
	(void)snprintf(path, sizeof(path), "%s/word-bad.txt", work);
	(void)test_write_file(path, word_bad, sizeof(word_bad) - 1U);
	error = kwl_hand_load(path);
	test_check(error == EINVAL, "a point with a stray letter fails the read");

	/* A file larger than the reader takes: the real templates padded with comments past 1 MiB. */
	large = malloc(1024U * 1024U + 2U);
	if (large == NULL) {
		test_check(0, "room for the large file");
		return;
	}
	memset(large, '#', 1024U * 1024U + 1U);
	memcpy(large, test_text, test_length);
	large[test_length] = '\n';
	(void)snprintf(path, sizeof(path), "%s/large.txt", work);
	(void)test_write_file(path, large, 1024U * 1024U + 1U);
	free(large);
	error = kwl_hand_load(path);
	test_check(error == EFBIG, "a file too large fails the read");

	/* The real file still reads. */
	(void)snprintf(path, sizeof(path), "%s/share/keiland/hand/hershey.txt", work);
	error = kwl_hand_load(path);
	test_check(error == 0, "the real file reads again");
}
