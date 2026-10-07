/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the handwriting recognizer (ws165-p002,
 * userland/desktop/wayland/hand-cloud.c, plan/ws165/phase001/phase.md
 * section 5): from each template of the hand-hershey file, SAMPLES written
 * characters are made by hand of a machine: turned up to 10 degrees,
 * stretched up to 15% each way, every point moved by a normal error of 2%
 * of the character's size, the strokes in another order and some of them
 * the other way round, and the end of a stroke cut off.  Each is
 * recognized; the share whose character is the first candidate (top-1)
 * and among the first four (top-4) and the time a recognition takes are
 * printed, and the characters most often missed.  The share is the upper
 * mark: the samples come from the same data as the templates.
 *
 *   host-hand TEMPLATES [SAMPLES [SEED]]
 *
 * Some characters differ from others only by their size or their place
 * (c and C, o and the degree sign, l and |): with the writing alone the
 * recognizer cannot tell them apart.  Each sample is written on an area
 * as the templates are on the Hershey glyphs' (y from -16 to 16), a
 * little larger or smaller (10%) and higher or lower (5% of the area)
 * besides the changes above, and recognized with the area
 * (hand_recognize_framed, ws165-p005), which tells them apart by the
 * size and the place.  The share of top-1 is also printed with each such
 * group counted as one character ("same shape").  HOST_HAND_UNFRAMED
 * recognizes without the area (p002's way).
 *
 * The exit status is 0 when top-1 >= 90% and top-4 >= 98% (the user's
 * goal H1, plan/ws165/phase002/phase.md, without counting the groups as
 * one: p005), 1 otherwise.
 */

#include "userland/desktop/wayland/hand-cloud.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The longest file, the most points of a template, and the most strokes. */
#define TEST_FILE_MAX		(1024U * 1024U)
#define TEST_POINTS		4096U
#define TEST_STROKES		64U

/* A template's raw strokes as the file has them. */
struct test_glyph {
	uint32_t code;
	size_t count;
	float x[TEST_POINTS];
	float y[TEST_POINTS];
	unsigned char starts[TEST_POINTS];
};

/* The random numbers' state (xorshift). */
static uint64_t test_state;

static uint32_t test_random(void);
static float test_uniform(float low, float high);
static float test_normal(void);
static int test_glyph_read(const char *line, struct test_glyph *glyph);
static void test_sample(const struct test_glyph *glyph, struct test_glyph *sample);
static double test_now(void);
static void test_utf8(uint32_t code, char *text);
static int test_same_shape(uint32_t one, uint32_t other);

/* The groups of characters that differ only by size or place (a group's code points, 0 after the last). */
static const uint32_t test_shapes[][5] = {
	{ 'c', 'C', 0 },
	{ 'o', 'O', '0', 0xb0, 0 },
	{ 's', 'S', 0 },
	{ 'v', 'V', 0 },
	{ 'w', 'W', 0 },
	{ 'x', 'X', 0xd7, 0 },
	{ 'z', 'Z', 0 },
	{ 'l', '|', '1', 'I', 0 },
	{ '.', 0xb7, 0 },
	{ 'p', 'P', 0 },
	{ '/', 0x30ce, 0 },
};

/*
 * Runs the test; the exit status says whether the goal was reached.
 */
int
main(
	int argc,
	char **argv)
{
	static char text[TEST_FILE_MAX];
	static struct test_glyph glyphs[400];
	static struct test_glyph sample;
	static unsigned missed[400];
	struct hand_templates templates;
	struct hand_cloud_input input;
	struct hand_frame frame;
	uint32_t codes[4];
	float distances[4];
	FILE *file;
	size_t length;
	size_t glyph_count;
	size_t found;
	size_t index;
	size_t place;
	char *line;
	char *next;
	char name[8];
	unsigned samples;
	unsigned sample_index;
	unsigned total;
	unsigned first;
	unsigned first_shape;
	unsigned four;
	double started;
	double spent;
	double slowest;
	double one;
	const char *confusion;
	const char *unframed;
	int error;
	int read;
	int same;

	/* The file, the templates and the raw glyphs. */
	if (argc < 2) {
		fprintf(stderr, "usage: host-hand TEMPLATES [SAMPLES [SEED]]\n");
		return 2;
	}

	/* How many samples, and the seed. */
	samples = 20U;
	if (argc > 2)
		samples = (unsigned)atoi(argv[2]);
	test_state = 0x9e3779b97f4a7c15ULL;
	if (argc > 3)
		test_state ^= (uint64_t)strtoull(argv[3], NULL, 10);
	file = fopen(argv[1], "r");
	if (file == NULL) {
		perror(argv[1]);
		return 2;
	}

	/* Its text, and the templates of it. */
	length = fread(text, 1U, sizeof(text) - 1U, file);
	fclose(file);
	text[length] = '\0';
	error = hand_templates_parse(&templates, text, length);
	if (error != 0) {
		fprintf(stderr, "parse: %d\n", error);
		return 2;
	}

	/* The raw glyphs, a line each. */
	glyph_count = 0U;
	for (line = text; line != NULL && *line != '\0'; line = next) {
		next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';
		if (line[0] != 'U' || glyph_count >= sizeof(glyphs) / sizeof(glyphs[0]))
			continue;
		read = test_glyph_read(line, &glyphs[glyph_count]);
		if (read == 0)
			glyph_count++;
	}

	/* What was read. */
	printf("host-hand: templates=%lu glyphs=%lu samples=%u\n", (unsigned long)templates.count, (unsigned long)glyph_count, samples);

	/* Each glyph's samples, recognized (HOST_HAND_CONFUSION prints each miss). */
	confusion = getenv("HOST_HAND_CONFUSION");
	unframed = getenv("HOST_HAND_UNFRAMED");
	frame.top = -16.0f;
	frame.height = 32.0f;
	if (unframed != NULL)
		frame.height = 0.0f;
	total = 0U;
	first = 0U;
	first_shape = 0U;
	four = 0U;
	spent = 0.0;
	slowest = 0.0;
	for (index = 0U; index < glyph_count; index++) {
		for (sample_index = 0U; sample_index < samples; sample_index++) {
			test_sample(&glyphs[index], &sample);
			input.x = sample.x;
			input.y = sample.y;
			input.starts = sample.starts;
			input.count = sample.count;
			started = test_now();
			found = hand_recognize_framed(&templates, &input, &frame, codes, distances, 4U);
			one = test_now() - started;
			spent += one;
			if (one > slowest)
				slowest = one;
			total++;
			if (found > 0U && codes[0] == glyphs[index].code)
				first++;
			same = 0;
			if (found > 0U)
				same = test_same_shape(codes[0], glyphs[index].code);
			if (same)
				first_shape++;
			for (place = 0U; place < found; place++) {
				if (codes[place] == glyphs[index].code) {
					four++;
					break;
				}
			}

			/* A miss is counted, and told when asked. */
			if (found == 0U || codes[0] != glyphs[index].code) {
				missed[index]++;
				if (confusion != NULL && found > 0U) {
					test_utf8(glyphs[index].code, name);
					printf("confused %s ->", name);
					test_utf8(codes[0], name);
					printf(" %s\n", name);
				}
			}
		}
	}

	/* The characters missed most at top-1. */
	printf("host-hand: missed at top-1:");
	for (index = 0U; index < glyph_count; index++) {
		if (missed[index] * 2U < samples)
			continue;
		test_utf8(glyphs[index].code, name);
		printf(" %s(%u)", name, missed[index]);
	}

	/* The line's end. */
	printf("\n");

	/* The result. */
	printf("host-hand: top-1 %.1f%% (same shape %.1f%%) top-4 %.1f%% of %u, recognition mean %.2f ms slowest %.2f ms\n",
	    100.0 * first / total, 100.0 * first_shape / total, 100.0 * four / total, total, 1000.0 * spent / total, 1000.0 * slowest);
	hand_templates_free(&templates);
	if (first * 100U >= total * 90U && four * 100U >= total * 98U) {
		printf("host-hand: PASS\n");
		return 0;
	}

	/* Short of the goal. */
	printf("host-hand: FAIL\n");
	return 1;
}

/* Gives the next random number. */
static uint32_t
test_random(void)
{
	/* xorshift64. */
	test_state ^= test_state << 13;
	test_state ^= test_state >> 7;
	test_state ^= test_state << 17;
	return (uint32_t)(test_state >> 32);
}

/* Gives a number evenly between low and high. */
static float
test_uniform(
	float low,
	float high)
{
	/* Scaled. */
	return low + (high - low) * ((float)test_random() / 4294967296.0f);
}

/* Gives a number of the standard normal distribution (Box-Muller). */
static float
test_normal(void)
{
	float u;
	float v;

	/* Two uniform numbers, the first not zero. */
	u = test_uniform(1.0e-6f, 1.0f);
	v = test_uniform(0.0f, 1.0f);
	return sqrtf(-2.0f * logf(u)) * cosf(6.2831853f * v);
}

/* Reads a template line's strokes; returns 0, or -1 when it does not read. */
static int
test_glyph_read(
	const char *line,
	struct test_glyph *glyph)
{
	char copy[16384];
	char *word;
	char *end;
	int fresh;
	int separator;

	/* The code point, the number, then the points. */
	snprintf(copy, sizeof(copy), "%s", line);
	glyph->code = (uint32_t)strtoul(copy + 2, &end, 16);
	word = strtok(end, " ");
	word = strtok(NULL, " ");
	glyph->count = 0U;
	fresh = 1;
	while (word != NULL && glyph->count < TEST_POINTS) {
		separator = strcmp(word, "/");
		if (separator == 0) {
			fresh = 1;
		} else {
			glyph->x[glyph->count] = strtof(word, &end);
			glyph->y[glyph->count] = strtof(end + 1, NULL);
			glyph->starts[glyph->count] = (unsigned char)fresh;
			fresh = 0;
			glyph->count++;
		}

		/* The next word. */
		word = strtok(NULL, " ");
	}

	/* A glyph without points does not read. */
	if (glyph->count == 0U)
		return -1;
	return 0;
}

/*
 * Makes a sample of a glyph: turned, stretched, shaken, the strokes
 * reordered and reversed, a stroke's end cut, written larger or smaller,
 * higher or lower and anywhere across.
 */
static void
test_sample(
	const struct test_glyph *glyph,
	struct test_glyph *sample)
{
	size_t begins[TEST_STROKES + 1U];
	size_t order[TEST_STROKES];
	size_t strokes;
	size_t index;
	size_t from;
	size_t to;
	size_t point;
	size_t swap;
	size_t cut;
	float angle;
	float grown;
	float lift;
	float across;
	float scale_x;
	float scale_y;
	float size;
	float min_x;
	float max_x;
	float min_y;
	float max_y;
	float x;
	float y;
	int reverse;

	/* The strokes. */
	strokes = 0U;
	for (index = 0U; index < glyph->count && strokes < TEST_STROKES; index++) {
		if (glyph->starts[index])
			begins[strokes++] = index;
	}

	/* The last one's end. */
	begins[strokes] = glyph->count;

	/* Their order shuffled. */
	for (index = 0U; index < strokes; index++)
		order[index] = index;
	for (index = strokes; index > 1U; index--) {
		swap = test_random() % index;
		point = order[index - 1U];
		order[index - 1U] = order[swap];
		order[swap] = point;
	}

	/* The size, for the error. */
	min_x = max_x = glyph->x[0];
	min_y = max_y = glyph->y[0];
	for (index = 1U; index < glyph->count; index++) {
		min_x = fminf(min_x, glyph->x[index]);
		max_x = fmaxf(max_x, glyph->x[index]);
		min_y = fminf(min_y, glyph->y[index]);
		max_y = fmaxf(max_y, glyph->y[index]);
	}

	/* The changes drawn. */
	size = fmaxf(max_x - min_x, max_y - min_y);
	angle = test_uniform(-10.0f, 10.0f) * 3.14159265f / 180.0f;
	scale_x = test_uniform(0.85f, 1.15f);
	scale_y = test_uniform(0.85f, 1.15f);
	cut = test_random() % (strokes + 1U);
	grown = test_uniform(0.9f, 1.1f);
	lift = test_uniform(-1.6f, 1.6f);
	across = test_uniform(-40.0f, 40.0f);

	/* Each stroke in its new order, maybe backwards, a tenth of one cut off its end. */
	sample->code = glyph->code;
	sample->count = 0U;
	for (index = 0U; index < strokes; index++) {
		from = begins[order[index]];
		to = begins[order[index] + 1U];
		if (index == cut && to - from > 10U)
			to -= (to - from) / 10U;
		reverse = (test_random() & 1U) != 0U;
		for (point = 0U; point < to - from; point++) {
			swap = from + point;
			if (reverse)
				swap = to - 1U - point;
			x = glyph->x[swap] * scale_x;
			y = glyph->y[swap] * scale_y;
			sample->x[sample->count] = grown * (x * cosf(angle) - y * sinf(angle) + 0.02f * size * test_normal()) + across;
			sample->y[sample->count] = grown * (x * sinf(angle) + y * cosf(angle) + 0.02f * size * test_normal()) + lift;
			sample->starts[sample->count] = (unsigned char)(point == 0U);
			sample->count++;
		}
	}
}

/* Gives the time in seconds. */
static double
test_now(void)
{
	struct timespec now;

	/* The monotonic clock. */
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* Writes a code point as UTF-8. */
static void
test_utf8(
	uint32_t code,
	char *text)
{
	/* One to three bytes are enough here. */
	if (code < 0x80U) {
		text[0] = (char)code;
		text[1] = '\0';
	} else if (code < 0x800U) {
		text[0] = (char)(0xc0U | (code >> 6));
		text[1] = (char)(0x80U | (code & 0x3fU));
		text[2] = '\0';
	} else {
		text[0] = (char)(0xe0U | (code >> 12));
		text[1] = (char)(0x80U | ((code >> 6) & 0x3fU));
		text[2] = (char)(0x80U | (code & 0x3fU));
		text[3] = '\0';
	}
}

/* Tells whether two characters are the same or of one same-shape group. */
static int
test_same_shape(
	uint32_t one,
	uint32_t other)
{
	size_t group;
	size_t a;
	size_t b;

	/* The same. */
	if (one == other)
		return 1;

	/* Both in one group. */
	for (group = 0U; group < sizeof(test_shapes) / sizeof(test_shapes[0]); group++) {
		for (a = 0U; test_shapes[group][a] != 0U; a++) {
			if (test_shapes[group][a] != one)
				continue;
			for (b = 0U; test_shapes[group][b] != 0U; b++) {
				if (test_shapes[group][b] == other)
					return 1;
			}
		}
	}

	/* Different shapes. */
	return 0;
}
