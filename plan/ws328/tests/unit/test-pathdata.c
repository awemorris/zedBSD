/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of svg-pathdata: the grammar of path data (SVG 2 §9.3.9),
 * the commands and their meaning (§9.3.3 to §9.3.8), the reflected control
 * points (§9.5.2) and the handling of errors (§9.5.4).  Every expected
 * path is written by hand from the standard, or built by a generator that
 * writes path data from a model of the commands and builds the expected
 * path with the path builder (the reverse direction of the reader).  The
 * last groups feed random text and every prefix of valid text, each string
 * in a block of exactly its size, so that the sanitizers see a read past
 * the NUL.
 */

#include "check.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "svg/svg-pathdata.h"
#include "vector/vec-path.h"

/* The size of the text a dump of a path is written to. */
#define DUMP_SIZE	4096

/* Copies text into a block of exactly its size (with the NUL), so that a read past the end is caught. */
static char *
exact_copy(
	const char *text)
{
	char *copy;
	size_t size;

	size = strlen(text) + 1;
	copy = malloc(size);
	if (copy == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	memcpy(copy, text, size);
	return copy;
}

/*
 * Writes a path as text, one word per verb: M x,y  L x,y  Q x1,y1,x,y
 * C x1,y1,x2,y2,x,y  Z, separated by spaces.  Output beyond the room is
 * cut (the text still ends with a NUL).
 */
static void
dump_path(
	const struct vec_path *path,
	char *out,
	size_t size)
{
	size_t verb;
	size_t point;
	size_t used;
	size_t count;
	size_t i;
	int written;

	out[0] = '\0';
	used = 0;
	point = 0;
	for (verb = 0; verb < path->verb_count; verb++) {
		/* Writes the letter of the verb and the number of points it takes. */
		count = 0;
		written = 0;
		if (used + 64 > size)
			return;
		if (verb != 0)
			out[used++] = ' ';
		switch (path->verbs[verb]) {
		case VEC_VERB_MOVE:
			out[used++] = 'M';
			count = 1;
			break;
		case VEC_VERB_LINE:
			out[used++] = 'L';
			count = 1;
			break;
		case VEC_VERB_QUAD:
			out[used++] = 'Q';
			count = 2;
			break;
		case VEC_VERB_CUBIC:
			out[used++] = 'C';
			count = 3;
			break;
		default:
			out[used++] = 'Z';
			break;
		}
		out[used] = '\0';

		/* Writes the points. */
		for (i = 0; i < count; i++) {
			if (point >= path->point_count)
				return;
			if (used + 64 > size)
				return;
			written = snprintf(out + used, size - used, "%s%g,%g", i == 0 ? "" : ",",
			    (double)path->points[point].x, (double)path->points[point].y);
			if (written < 0)
				return;
			used += (size_t)written;
			point++;
		}
	}
}

/* Tells whether two paths hold the same verbs and the same points, exactly. */
static int
path_equal(
	const struct vec_path *a,
	const struct vec_path *b)
{
	size_t i;

	if (a->verb_count != b->verb_count)
		return 0;
	if (a->point_count != b->point_count)
		return 0;
	if (a->verb_count != 0 && memcmp(a->verbs, b->verbs, a->verb_count) != 0)
		return 0;
	for (i = 0; i < a->point_count; i++) {
		if (a->points[i].x != b->points[i].x)
			return 0;
		if (a->points[i].y != b->points[i].y)
			return 0;
	}
	return 1;
}

/* Parses text (in an exact-size block) into an empty path and checks what the path holds and the flag. */
static void
check_parse(
	const char *text,
	const char *expected,
	int expected_complete,
	int line)
{
	struct vec_path path;
	char dump[DUMP_SIZE];
	char *copy;
	int complete;
	int error;

	vec_path_init(&path);
	copy = exact_copy(text);
	complete = -1;
	error = svg_path_data_parse(copy, &path, &complete);
	check_record_long(error, 0, __FILE__, line, text);
	check_record_long(complete, expected_complete, __FILE__, line, text);
	dump_path(&path, dump, sizeof(dump));
	check_record(strcmp(dump, expected) == 0, __FILE__, line, text);
	if (strcmp(dump, expected) != 0)
		printf("  text '%s'\n  got      '%s'\n  expected '%s'\n", text, dump, expected);
	free(copy);
	vec_path_release(&path);
}

/* Parses text and checks that the path equals a reference path built by hand with the builder. */
static void
check_parse_path(
	const char *text,
	const struct vec_path *reference,
	int expected_complete,
	int line)
{
	struct vec_path path;
	char *copy;
	char dump[DUMP_SIZE];
	char dump_reference[DUMP_SIZE];
	int complete;
	int error;
	int equal;

	vec_path_init(&path);
	copy = exact_copy(text);
	complete = -1;
	error = svg_path_data_parse(copy, &path, &complete);
	check_record_long(error, 0, __FILE__, line, text);
	check_record_long(complete, expected_complete, __FILE__, line, text);
	equal = path_equal(&path, reference);
	check_record(equal, __FILE__, line, text);
	if (!equal) {
		dump_path(&path, dump, sizeof(dump));
		dump_path(reference, dump_reference, sizeof(dump_reference));
		printf("  text '%s'\n  got      '%s'\n  expected '%s'\n", text, dump, dump_reference);
	}
	free(copy);
	vec_path_release(&path);
}

#define PARSE(text, expected, complete)	check_parse((text), (expected), (complete), __LINE__)

/* The commands one by one, absolute and relative (SVG 2 §9.3.3 to §9.3.7). */
static void
test_commands(void)
{
	/* A lone moveto, and the pairs after it that are implicit linetos (§9.3.3). */
	PARSE("M 10 20", "M10,20", 1);
	PARSE("M10,20 30,40", "M10,20 L30,40", 1);
	PARSE("M 1 2 3 4 5 6", "M1,2 L3,4 L5,6", 1);

	/* A relative moveto that starts the data is absolute; the implicit linetos after it are relative (§9.3.3). */
	PARSE("m 10 20 30 40", "M10,20 L40,60", 1);
	PARSE("m 10 20 30 40 5 6", "M10,20 L40,60 L45,66", 1);

	/* A relative moveto later in the data is from the current point. */
	PARSE("M 10 10 m 5 5", "M10,10 M15,15", 1);
	PARSE("M 10 10 L 20 20 m 5 5", "M10,10 L20,20 M25,25", 1);

	/* Lineto, absolute and relative, with several pairs (§9.3.5). */
	PARSE("M 1 2 L 3 4", "M1,2 L3,4", 1);
	PARSE("M 1 2 L 3 4 5 6", "M1,2 L3,4 L5,6", 1);
	PARSE("M 1 2 l 3 4", "M1,2 L4,6", 1);
	PARSE("M 1 2 l 3 4 5 6", "M1,2 L4,6 L9,12", 1);

	/* Horizontal and vertical lines, absolute and relative (§9.3.5). */
	PARSE("M 1 2 H 5", "M1,2 L5,2", 1);
	PARSE("M 1 2 V 7", "M1,2 L1,7", 1);
	PARSE("M 1 2 h 5", "M1,2 L6,2", 1);
	PARSE("M 1 2 v 7", "M1,2 L1,9", 1);
	PARSE("M 1 2 H 5 V 7 h 2 v -3", "M1,2 L5,2 L5,7 L7,7 L7,4", 1);
	PARSE("M 1 2 H 5 6", "M1,2 L5,2 L6,2", 1);
	PARSE("M 1 2 h 1 2", "M1,2 L2,2 L4,2", 1);
	PARSE("M 1 2 V 5 6", "M1,2 L1,5 L1,6", 1);
	PARSE("M 1 2 v 1 2", "M1,2 L1,3 L1,5", 1);

	/* Cubic curves, absolute and relative, one and several sets (§9.3.6). */
	PARSE("M 0 0 C 1 2 3 4 5 6", "M0,0 C1,2,3,4,5,6", 1);
	PARSE("M 10 10 c 1 2 3 4 5 6", "M10,10 C11,12,13,14,15,16", 1);
	PARSE("M 0 0 C 1 2 3 4 5 6 7 8 9 10 11 12", "M0,0 C1,2,3,4,5,6 C7,8,9,10,11,12", 1);
	PARSE("M 0 0 c 1 2 3 4 5 6 1 1 2 2 3 3", "M0,0 C1,2,3,4,5,6 C6,7,7,8,8,9", 1);

	/* Quadratic curves (§9.3.7). */
	PARSE("M 0 0 Q 1 2 3 4", "M0,0 Q1,2,3,4", 1);
	PARSE("M 10 10 q 1 2 3 4", "M10,10 Q11,12,13,14", 1);
	PARSE("M 0 0 Q 1 2 3 4 5 6 7 8", "M0,0 Q1,2,3,4 Q5,6,7,8", 1);
	PARSE("M 0 0 q 1 2 3 4 1 1 2 2", "M0,0 Q1,2,3,4 Q4,5,5,6", 1);

	/* Lower case and upper case letters need no space around them. */
	PARSE("M1 2L3 4Z", "M1,2 L3,4 Z", 1);
	PARSE("M1 2l3 4z", "M1,2 L4,6 Z", 1);
}

/* Closepath, and where the next subpath starts (§9.3.4). */
static void
test_close(void)
{
	/* A closed triangle. */
	PARSE("M 1 2 L 5 2 L 5 6 Z", "M1,2 L5,2 L5,6 Z", 1);
	PARSE("M 1 2 L 5 2 L 5 6 z", "M1,2 L5,2 L5,6 Z", 1);

	/* After a close, a drawing command starts at the subpath's first point. */
	PARSE("M 1 2 L 5 2 Z L 3 3", "M1,2 L5,2 Z M1,2 L3,3", 1);
	PARSE("M 1 2 L 5 2 Z l 3 3", "M1,2 L5,2 Z M1,2 L4,5", 1);
	PARSE("M 1 2 L 5 2 z h 3", "M1,2 L5,2 Z M1,2 L4,2", 1);
	PARSE("M 1 2 L 5 2 z v 3", "M1,2 L5,2 Z M1,2 L1,5", 1);

	/* After a close, a relative moveto is from the first point of the closed subpath. */
	PARSE("M 1 2 L 5 2 z m 3 3", "M1,2 L5,2 Z M4,5", 1);
	PARSE("M 1 2 L 5 2 Z M 7 7", "M1,2 L5,2 Z M7,7", 1);

	/* A close of a close does nothing more. */
	PARSE("M 1 2 L 5 2 Z Z", "M1,2 L5,2 Z", 1);
	PARSE("M 1 2 Z", "M1,2 Z", 1);

	/* The subpath's first point is the last moveto, not an implicit lineto's end. */
	PARSE("M 1 2 3 4 Z l 1 1", "M1,2 L3,4 Z M1,2 L2,3", 1);

	/* A close needs no space before the next command. */
	PARSE("M1 2L5 2ZL3 3", "M1,2 L5,2 Z M1,2 L3,3", 1);
	PARSE("M1 2 L 5 2 z m 1 1 l 1 1", "M1,2 L5,2 Z M2,3 L3,4", 1);
}

/* The smooth curves and the reflected control points (SVG 2 §9.3.6, §9.3.7, §9.5.2). */
static void
test_smooth(void)
{
	/* S after C: the first control point is 2 * current - previous second control point = (7, 8). */
	PARSE("M 0 0 C 1 2 3 4 5 6 S 7 8 9 10", "M0,0 C1,2,3,4,5,6 C7,8,7,8,9,10", 1);

	/* S after S reflects the second control point of the first S. */
	PARSE("M 0 0 C 1 2 3 4 5 6 S 7 8 9 10 S 11 12 13 14",
	    "M0,0 C1,2,3,4,5,6 C7,8,7,8,9,10 C11,12,11,12,13,14", 1);

	/* A relative s: the current point is (5, 6), the first control point (7, 8), the second (6, 7), the end (7, 8). */
	PARSE("M 0 0 C 1 2 3 4 5 6 s 1 1 2 2", "M0,0 C1,2,3,4,5,6 C7,8,6,7,7,8", 1);

	/* S with no command before it, after L, after a quadratic, after M and after Z uses the current point. */
	PARSE("M 5 5 S 7 8 9 10", "M5,5 C5,5,7,8,9,10", 1);
	PARSE("M 5 5 L 6 6 S 7 8 9 10", "M5,5 L6,6 C6,6,7,8,9,10", 1);
	PARSE("M 0 0 Q 1 2 3 4 S 5 6 7 8", "M0,0 Q1,2,3,4 C3,4,5,6,7,8", 1);
	PARSE("M 0 0 C 1 2 3 4 5 6 M 7 8 S 9 10 11 12", "M0,0 C1,2,3,4,5,6 M7,8 C7,8,9,10,11,12", 1);
	PARSE("M 0 0 C 1 2 3 4 5 6 Z S 9 10 11 12", "M0,0 C1,2,3,4,5,6 Z M0,0 C0,0,9,10,11,12", 1);
	PARSE("M 0 0 C 1 2 3 4 5 6 H 9 S 9 10 11 12", "M0,0 C1,2,3,4,5,6 L9,6 C9,6,9,10,11,12", 1);

	/* The reflection is measured from the previous command's second control point, not a first one. */
	PARSE("M 0 0 C 10 0 20 0 30 0 S 50 0 60 0", "M0,0 C10,0,20,0,30,0 C40,0,50,0,60,0", 1);

	/* Several sets of S: the second reflects the first. */
	PARSE("M 0 0 C 1 2 3 4 5 6 S 7 8 9 10 11 12 13 14",
	    "M0,0 C1,2,3,4,5,6 C7,8,7,8,9,10 C11,12,11,12,13,14", 1);

	/* T after Q: the control point is 2 * current - previous control point. */
	PARSE("M 0 0 Q 1 2 3 4 T 5 6", "M0,0 Q1,2,3,4 Q5,6,5,6", 1);

	/* T after T reflects the control point the first T used. */
	PARSE("M 0 0 Q 1 0 2 0 T 4 4 T 6 0", "M0,0 Q1,0,2,0 Q3,0,4,4 Q5,8,6,0", 1);

	/* A relative t. */
	PARSE("M 0 0 Q 1 0 2 0 t 2 2", "M0,0 Q1,0,2,0 Q3,0,4,2", 1);

	/* T with no quadratic before it, after C, after S, after M and after Z uses the current point. */
	PARSE("M 5 5 T 7 8", "M5,5 Q5,5,7,8", 1);
	PARSE("M 0 0 C 1 1 2 2 3 3 T 4 4", "M0,0 C1,1,2,2,3,3 Q3,3,4,4", 1);
	PARSE("M 0 0 Q 1 2 3 4 Z T 5 6", "M0,0 Q1,2,3,4 Z M0,0 Q0,0,5,6", 1);
	PARSE("M 0 0 Q 1 2 3 4 M 9 9 T 5 6", "M0,0 Q1,2,3,4 M9,9 Q9,9,5,6", 1);

	/* Several pairs of T. */
	PARSE("M 0 0 Q 1 0 2 0 T 4 4 6 0", "M0,0 Q1,0,2,0 Q3,0,4,4 Q5,8,6,0", 1);
}

/* The arcs (SVG 2 §9.3.8): the parameters reach the path builder, which applies §9.5.1. */
static void
test_arcs(void)
{
	struct vec_path reference;
	struct vec_path path;
	float handle;
	int complete;
	int error;

	/* Absolute, with the flags 0 and 1 and separators of every kind. */
	vec_path_init(&reference);
	vec_path_move_to(&reference, 0, 0);
	vec_path_arc_to(&reference, 10, 10, 0, 0, 1, 10, 10);
	check_parse_path("M 0 0 A 10 10 0 0 1 10 10", &reference, 1, __LINE__);
	check_parse_path("M0,0A10,10,0,0,1,10,10", &reference, 1, __LINE__);
	check_parse_path("M0 0A10 10 0 0 1 10 10", &reference, 1, __LINE__);
	check_parse_path("M0 0A10 10 0 01 10 10", &reference, 1, __LINE__);
	check_parse_path("M0 0A10 10 0 01,10,10", &reference, 1, __LINE__);
	check_parse_path("M0 0A10 10 0 0110 10", &reference, 1, __LINE__);
	check_parse_path("M0 0A10 10 0 0110,10", &reference, 1, __LINE__);
	check_parse_path("M0 0a10 10 0 0 1 10 10", &reference, 1, __LINE__);
	vec_path_close(&reference);
	check_parse_path("M 0 0 A 10 10 0 0 1 10 10 Z", &reference, 1, __LINE__);
	vec_path_release(&reference);

	/* The quarter circle from (0, 0) to (10, 10) of radius 10 with the sweep flag 1 has its center at (0, 10); */
	/* its one cubic curve has the control points (4/3 tan(pi/8) * 10 = 5.522847 away from the ends along the tangents). */
	vec_path_init(&path);
	complete = -1;
	error = svg_path_data_parse("M 0 0 A 10 10 0 0 1 10 10", &path, &complete);
	CHECK_LONG(error, 0);
	CHECK_LONG(complete, 1);
	CHECK_LONG(path.verb_count, 2);
	CHECK_LONG(path.verbs[1], VEC_VERB_CUBIC);
	CHECK_LONG(path.point_count, 4);
	handle = 5.522847f;
	if (path.point_count == 4) {
		CHECK_NEAR(path.points[1].x, handle, 1e-4);
		CHECK_NEAR(path.points[1].y, 0.0, 1e-4);
		CHECK_NEAR(path.points[2].x, 10.0, 1e-4);
		CHECK_NEAR(path.points[2].y, 10.0 - handle, 1e-4);
		CHECK_NEAR(path.points[3].x, 10.0, 1e-6);
		CHECK_NEAR(path.points[3].y, 10.0, 1e-6);
	}
	vec_path_release(&path);

	/* With the sweep flag 0 the center is at (10, 0) and the curve bulges the other way. */
	vec_path_init(&path);
	error = svg_path_data_parse("M 0 0 A 10 10 0 0 0 10 10", &path, &complete);
	CHECK_LONG(error, 0);
	CHECK_LONG(path.verb_count, 2);
	CHECK_LONG(path.point_count, 4);
	if (path.point_count == 4) {
		CHECK_NEAR(path.points[1].x, 0.0, 1e-4);
		CHECK_NEAR(path.points[1].y, handle, 1e-4);
		CHECK_NEAR(path.points[2].x, 10.0 - handle, 1e-4);
		CHECK_NEAR(path.points[2].y, 10.0, 1e-4);
	}
	vec_path_release(&path);

	/* The large-arc flag 1 takes the long way round: more than one curve, ending at the same point. */
	vec_path_init(&path);
	error = svg_path_data_parse("M 0 0 A 10 10 0 1 1 10 10", &path, &complete);
	CHECK_LONG(error, 0);
	CHECK_LONG(complete, 1);
	CHECK(path.verb_count > 2);
	if (path.point_count >= 1) {
		CHECK_NEAR(path.points[path.point_count - 1].x, 10.0, 1e-4);
		CHECK_NEAR(path.points[path.point_count - 1].y, 10.0, 1e-4);
	}
	vec_path_release(&path);

	/* The flags without separators, the example of the arc grammar: a1 1 0 00 1 1. */
	vec_path_init(&reference);
	vec_path_move_to(&reference, 2, 3);
	vec_path_arc_to(&reference, 1, 1, 0, 0, 0, 3, 4);
	check_parse_path("M 2 3 a1 1 0 00 1 1", &reference, 1, __LINE__);
	check_parse_path("M 2 3 a1 1 0 0 0 1 1", &reference, 1, __LINE__);
	check_parse_path("M 2 3 a1,1,0,0,0,1,1", &reference, 1, __LINE__);
	check_parse_path("M 2 3 a 1 1 0 0,0 1 1", &reference, 1, __LINE__);
	check_parse_path("M 2 3 a1 1 0 00,1,1", &reference, 1, __LINE__);
	vec_path_release(&reference);

	/* A flag followed by a signed or a fractional number needs no separator. */
	vec_path_init(&reference);
	vec_path_move_to(&reference, 1, 1);
	vec_path_arc_to(&reference, 1, 1, 0, 0, 1, 0.5f, -1);
	check_parse_path("M 1 1 A 1 1 0 01.5-1", &reference, 1, __LINE__);
	check_parse_path("M 1 1 a 1 1 0 01-.5-2", &reference, 1, __LINE__);
	vec_path_release(&reference);

	/* The end point is relative for a, the radii and the rotation are not. */
	vec_path_init(&reference);
	vec_path_move_to(&reference, 5, 5);
	vec_path_arc_to(&reference, 3, 4, 30, 1, 0, 8, 9);
	check_parse_path("M 5 5 A 3 4 30 1 0 8 9", &reference, 1, __LINE__);
	check_parse_path("M 5 5 a 3 4 30 1 0 3 4", &reference, 1, __LINE__);
	vec_path_release(&reference);

	/* A negative rotation (the example arcs01 of SVG 2 §9.3.8) and negative radii (§9.5.1) are read. */
	vec_path_init(&reference);
	vec_path_move_to(&reference, 0, 0);
	vec_path_arc_to(&reference, 25, 50, -30, 0, 1, 50, -25);
	check_parse_path("M 0 0 a25,50 -30 0,1 50,-25", &reference, 1, __LINE__);
	vec_path_release(&reference);
	vec_path_init(&reference);
	vec_path_move_to(&reference, 0, 0);
	vec_path_arc_to(&reference, -5, -6, 0, 0, 1, 10, 0);
	check_parse_path("M 0 0 A -5 -6 0 0 1 10 0", &reference, 1, __LINE__);
	vec_path_release(&reference);

	/* Several arcs in one command, and the radius 0 and the equal end point cases that the builder decides. */
	vec_path_init(&reference);
	vec_path_move_to(&reference, 0, 0);
	vec_path_arc_to(&reference, 5, 5, 0, 0, 1, 10, 0);
	vec_path_arc_to(&reference, 5, 5, 0, 0, 1, 20, 0);
	check_parse_path("M 0 0 A 5 5 0 0 1 10 0 5 5 0 0 1 20 0", &reference, 1, __LINE__);
	check_parse_path("M 0 0 A 5 5 0 0 1 10 0 a 5 5 0 0 1 10 0", &reference, 1, __LINE__);
	check_parse_path("M 0 0 a 5 5 0 0 1 10 0 5 5 0 0 1 10 0", &reference, 1, __LINE__);
	vec_path_release(&reference);
	vec_path_init(&reference);
	vec_path_move_to(&reference, 0, 0);
	vec_path_arc_to(&reference, 0, 5, 0, 0, 1, 10, 0);
	check_parse_path("M 0 0 A 0 5 0 0 1 10 0", &reference, 1, __LINE__);
	vec_path_release(&reference);
	PARSE("M 0 0 A 0 5 0 0 1 10 0", "M0,0 L10,0", 1);
	PARSE("M 3 4 A 5 5 0 0 1 3 4", "M3,4", 1);

	/* An arc resets the reflection of a smooth curve. */
	vec_path_init(&reference);
	vec_path_move_to(&reference, 0, 0);
	vec_path_cubic_to(&reference, 1, 2, 3, 4, 5, 6);
	vec_path_arc_to(&reference, 5, 5, 0, 0, 1, 15, 6);
	vec_path_cubic_to(&reference, 15, 6, 17, 8, 19, 10);
	check_parse_path("M 0 0 C 1 2 3 4 5 6 A 5 5 0 0 1 15 6 S 17 8 19 10", &reference, 1, __LINE__);
	vec_path_release(&reference);

	/* Bad flags: a digit that is not 0 or 1, a sign, a point, a letter, a missing one. */
	PARSE("M 1 1 A 5 5 0 2 0 10 0", "M1,1", 0);
	PARSE("M 1 1 A 5 5 0 0 2 10 0", "M1,1", 0);
	PARSE("M 1 1 A 5 5 0 -1 0 10 0", "M1,1", 0);
	PARSE("M 1 1 A 5 5 0 .5 0 10 0", "M1,1", 0);
	PARSE("M 1 1 A 5 5 0 0 .5 10 0", "M1,1", 0);
	PARSE("M 1 1 A 5 5 0 x 0 10 0", "M1,1", 0);
	PARSE("M 1 1 A 5 5 0 0 1 10", "M1,1", 0);
	PARSE("M 1 1 A 5 5 0 0", "M1,1", 0);
	PARSE("M 1 1 A 5 5 0", "M1,1", 0);
	PARSE("M 1 1 A 5 5", "M1,1", 0);
	PARSE("M 1 1 A", "M1,1", 0);

	/* An arc cut short after a whole one keeps the whole one. */
	vec_path_init(&reference);
	vec_path_move_to(&reference, 0, 0);
	vec_path_arc_to(&reference, 5, 5, 0, 0, 1, 10, 0);
	check_parse_path("M 0 0 A 5 5 0 0 1 10 0 5 5 0 0 1 20", &reference, 0, __LINE__);
	check_parse_path("M 0 0 A 5 5 0 0 1 10 0 5 5 0 0 1", &reference, 0, __LINE__);
	check_parse_path("M 0 0 A 5 5 0 0 1 10 0,", &reference, 0, __LINE__);
	vec_path_release(&reference);
}

/* The grammar's boundaries: separators, signs, points, exponents (SVG 2 §9.3.9). */
static void
test_grammar(void)
{
	/* The longest-match examples of the standard: "M 100-200" and "M 0.6.5". */
	PARSE("M 100-200", "M100,-200", 1);
	PARSE("M 0.6.5", "M0.6,0.5", 1);

	/* A sign separates two numbers. */
	PARSE("M1-2", "M1,-2", 1);
	PARSE("M1+2", "M1,2", 1);
	PARSE("M-1-2", "M-1,-2", 1);
	PARSE("M-1-2-3-4", "M-1,-2 L-3,-4", 1);
	PARSE("M1 2L3-4-5-6", "M1,2 L3,-4 L-5,-6", 1);

	/* A second point begins the next number. */
	PARSE("M.5.5", "M0.5,0.5", 1);
	PARSE("M.5.5.5.5", "M0.5,0.5 L0.5,0.5", 1);
	PARSE("M1.5.5", "M1.5,0.5", 1);
	PARSE("M-.5-.5", "M-0.5,-0.5", 1);
	PARSE("M+.5+.5", "M0.5,0.5", 1);

	/* Exponents. */
	PARSE("M1e1,2e-1", "M10,0.2", 1);
	PARSE("M1E2 3", "M100,3", 1);
	PARSE("M1e+2 3E+0", "M100,3", 1);
	PARSE("M.5e1 5e0", "M5,5", 1);
	PARSE("M1e1-2e1", "M10,-20", 1);
	PARSE("M 25e-1 1.5e1", "M2.5,15", 1);

	/* Leading zeros and a fraction with a leading zero. */
	PARSE("M007 0.5", "M7,0.5", 1);
	PARSE("M 0 -0", "M0,0", 1);

	/* Commas: one between the numbers of a pair, between pairs, with white space around. */
	PARSE("M1,2", "M1,2", 1);
	PARSE("M1 ,2", "M1,2", 1);
	PARSE("M1, 2", "M1,2", 1);
	PARSE("M1 , 2", "M1,2", 1);
	PARSE("M1,2,3,4", "M1,2 L3,4", 1);
	PARSE("M1,2 3,4", "M1,2 L3,4", 1);
	PARSE("M1,2 , 3,4", "M1,2 L3,4", 1);
	PARSE("M1 2 3 4", "M1,2 L3,4", 1);
	PARSE("M1\t2\n3\f4\r5\r\n6", "M1,2 L3,4 L5,6", 1);

	/* White space around the data and between commands, of the five kinds. */
	PARSE("   M 1 2   ", "M1,2", 1);
	PARSE("\tM\n1\f2\r", "M1,2", 1);
	PARSE("\n\r\t\f M 1 2 \n\r\t\f L 3 4 \n\r\t\f", "M1,2 L3,4", 1);
	PARSE("M 1 2\tL\t3\t4", "M1,2 L3,4", 1);

	/* Empty data and white space alone are valid and draw nothing. */
	PARSE("", "", 1);
	PARSE(" ", "", 1);
	PARSE(" \t\r\n\f ", "", 1);

	/* A command's argument list may begin after white space or at once. */
	PARSE("M 1 2 L3 4", "M1,2 L3,4", 1);
	PARSE("M 1 2 L 3 4", "M1,2 L3,4", 1);
	PARSE("M 1 2 H5", "M1,2 L5,2", 1);
	PARSE("M 1 2 H,5", "M1,2", 0);

	/* Commas between the coordinates of H and V sequences. */
	PARSE("M 1 2 H 5 , 6", "M1,2 L5,2 L6,2", 1);
	PARSE("M 1 2 V 5,6 7", "M1,2 L1,5 L1,6 L1,7", 1);

	/* A number may have more than one repetition of its parts: many digits. */
	PARSE("M 123456 7890", "M123456,7890", 1);
	PARSE("M 0.000001 1000000", "M1e-06,1e+06", 1);
}

/* Errors in the data (SVG 2 §9.3.9, §9.5.4): what is kept and the flag. */
static void
test_errors(void)
{
	/* The example of the standard: the line to 20,20 is kept, the odd third number is dropped. */
	PARSE("M 10,10 L 20,20,30", "M10,10 L20,20", 0);

	/* An incomplete set of any command keeps the whole sets before it. */
	PARSE("M 10 10 L 20 20 30 40 50", "M10,10 L20,20 L30,40", 0);
	PARSE("M 1 2 3", "M1,2", 0);
	PARSE("M 1 2 3 4 5", "M1,2 L3,4", 0);
	PARSE("M 1 2 C 1 2 3 4 5", "M1,2", 0);
	PARSE("M 1 2 C 1 2 3 4 5 6 7 8", "M1,2 C1,2,3,4,5,6", 0);
	PARSE("M 1 2 S 1 2 3", "M1,2", 0);
	PARSE("M 1 2 S 1 2 3 4 5", "M1,2 C1,2,1,2,3,4", 0);
	PARSE("M 1 2 Q 1 2 3", "M1,2", 0);
	PARSE("M 1 2 Q 1 2 3 4 5", "M1,2 Q1,2,3,4", 0);
	PARSE("M 1 2 T 3", "M1,2", 0);
	PARSE("M 1 2 T 3 4 5", "M1,2 Q1,2,3,4", 0);
	PARSE("M 1 2 H 3 x", "M1,2 L3,2", 0);
	PARSE("M 1 2 V 3 4 x", "M1,2 L1,3 L1,4", 0);

	/* A command with no argument set at all. */
	PARSE("M", "", 0);
	PARSE("M 1 2 L", "M1,2", 0);
	PARSE("M 1 2 H", "M1,2", 0);
	PARSE("M 1 2 C", "M1,2", 0);
	PARSE("M 1 2 L Z", "M1,2", 0);
	PARSE("M 1 2 L 3 4 L", "M1,2 L3,4", 0);

	/* An error before the first whole moveto leaves nothing. */
	PARSE("M 10", "", 0);
	PARSE("M 10,", "", 0);
	PARSE("M,1 2", "", 0);
	PARSE("M,", "", 0);
	PARSE("M 1,,2", "", 0);

	/* Data that does not start with a moveto draws nothing. */
	PARSE("L 1 2", "", 0);
	PARSE("l 1 2", "", 0);
	PARSE("Z", "", 0);
	PARSE("z", "", 0);
	PARSE("1 2", "", 0);
	PARSE("  L 1 2 M 3 4", "", 0);
	PARSE("H 5", "", 0);
	PARSE("C 1 2 3 4 5 6", "", 0);
	PARSE("A 1 1 0 0 0 1 1", "", 0);
	PARSE("x", "", 0);
	PARSE(",", "", 0);
	PARSE(",M 1 2", "", 0);

	/* An unknown letter, a digit where a command should be, a number after a close. */
	PARSE("M 1 2 X", "M1,2", 0);
	PARSE("M 1 2 L 3 4 X 5 6 L 7 8", "M1,2 L3,4", 0);
	PARSE("M 1 2 Z 3", "M1,2 Z", 0);
	PARSE("M 1 2 Z 3 4", "M1,2 Z", 0);
	PARSE("M 1 2 z, L 3 4", "M1,2 Z", 0);
	PARSE("M 1 2 e", "M1,2", 0);
	PARSE("M 1 2 E5", "M1,2", 0);
	PARSE("M 1 2 B 1", "M1,2", 0);

	/* A comma between commands is an error: the command before it is kept. */
	PARSE("M 1 2 , L 3 4", "M1,2", 0);
	PARSE("M 1 2 L 3 4 , Z", "M1,2 L3,4", 0);

	/* A comma after the last set of a command (with no set after it) is an error. */
	PARSE("M 1 2,", "M1,2", 0);
	PARSE("M 1 2, ", "M1,2", 0);
	PARSE("M 1 2 L 3 4,", "M1,2 L3,4", 0);
	PARSE("M 1 2 L 3 4, Z", "M1,2 L3,4", 0);
	PARSE("M 1 2 H 3,", "M1,2 L3,2", 0);

	/* Two commas in a separator. */
	PARSE("M 1 2 L 3,,4", "M1,2", 0);
	PARSE("M 1 2 L 3, ,4", "M1,2", 0);
	PARSE("M 1 2 L 3 4,,5 6", "M1,2 L3,4", 0);
	PARSE("M 1 2 L 3 4 , , 5 6", "M1,2 L3,4", 0);

	/* A point with no digit after it is not part of a number (SVG 2 §9.3.9 disallows "23."). */
	PARSE("M 5. 5", "", 0);
	PARSE("M 5 5.", "M5,5", 0);
	PARSE("M 5 5. 6 6", "M5,5", 0);
	PARSE("M 1 2 L 3. 4", "M1,2", 0);
	PARSE("M 1 2 L 3 4 5. 6", "M1,2 L3,4", 0);
	PARSE("M 5 .", "", 0);
	PARSE("M . 5", "", 0);
	PARSE("M 1 2 L 3.4.", "M1,2", 0);

	/* A sign with no number after it, a double sign, an exponent with no digit. */
	PARSE("M 1 -", "", 0);
	PARSE("M 1 +", "", 0);
	PARSE("M 1 --2", "", 0);
	PARSE("M 1 -+2", "", 0);
	PARSE("M 1 +-2", "", 0);
	PARSE("M 1 2e", "M1,2", 0);
	PARSE("M 1e 2", "", 0);
	PARSE("M 1 2 L 3 4e+", "M1,2 L3,4", 0);

	/* A vertical tab, a non-breaking space and other bytes are not white space. */
	PARSE("M 1\v2", "", 0);
	PARSE("M 1 2\vL 3 4", "M1,2", 0);
	PARSE("\vM 1 2", "", 0);
	PARSE("M 1 2\xc2\xa0L 3 4", "M1,2", 0);
	PARSE("M 1 2\x80", "M1,2", 0);
	PARSE("M 1 2 L 3 4\xff", "M1,2 L3,4", 0);

	/* A number a float cannot hold ends the data. */
	PARSE("M 1 2 L 1e39 0", "M1,2", 0);
	PARSE("M 1e39 0", "", 0);
	PARSE("M 1 2 L 3 4 L 0 1e400", "M1,2 L3,4", 0);

	/* The words of a later command do not matter after an error. */
	PARSE("M 1 2 L 3 x L 5 6", "M1,2", 0);
}

/* Reading is independent of what the path already holds, and appends. */
static void
test_append(void)
{
	struct vec_path path;
	struct vec_path reference;
	char *copy;
	int complete;
	int error;

	/* Appends the data to the path's contents, keeping them. */
	vec_path_init(&path);
	vec_path_move_to(&path, 100, 100);
	vec_path_line_to(&path, 200, 200);
	copy = exact_copy("M 1 2 L 3 4");
	complete = -1;
	error = svg_path_data_parse(copy, &path, &complete);
	CHECK_LONG(error, 0);
	CHECK_LONG(complete, 1);
	vec_path_init(&reference);
	vec_path_move_to(&reference, 100, 100);
	vec_path_line_to(&reference, 200, 200);
	vec_path_move_to(&reference, 1, 2);
	vec_path_line_to(&reference, 3, 4);
	CHECK(path_equal(&path, &reference));
	vec_path_release(&reference);

	/* Reads a second text into the same path, relative commands measured from the data's own start. */
	free(copy);
	copy = exact_copy("m 5 5 l 1 1");
	error = svg_path_data_parse(copy, &path, &complete);
	CHECK_LONG(error, 0);
	CHECK_LONG(complete, 1);
	vec_path_init(&reference);
	vec_path_move_to(&reference, 100, 100);
	vec_path_line_to(&reference, 200, 200);
	vec_path_move_to(&reference, 1, 2);
	vec_path_line_to(&reference, 3, 4);
	vec_path_move_to(&reference, 5, 5);
	vec_path_line_to(&reference, 6, 6);
	CHECK(path_equal(&path, &reference));
	vec_path_release(&reference);
	free(copy);
	vec_path_release(&path);
}

/* The arguments the function refuses, and a path that already has an error. */
static void
test_arguments(void)
{
	struct vec_path path;
	int complete;
	int error;

	/* Refuses a NULL text, path or flag, setting the flag to 0 when it can. */
	vec_path_init(&path);
	complete = 5;
	error = svg_path_data_parse(NULL, &path, &complete);
	CHECK_LONG(error, EINVAL);
	CHECK_LONG(complete, 0);
	complete = 5;
	error = svg_path_data_parse("M 1 2", NULL, &complete);
	CHECK_LONG(error, EINVAL);
	CHECK_LONG(complete, 0);
	error = svg_path_data_parse("M 1 2", &path, NULL);
	CHECK_LONG(error, EINVAL);
	CHECK_LONG(path.verb_count, 0);

	/* A path with a sticky error keeps it and returns it, with the flag 0. */
	error = vec_path_move_to(&path, (float)NAN, 0.0f);
	CHECK_LONG(error, EINVAL);
	complete = 5;
	error = svg_path_data_parse("M 1 2", &path, &complete);
	CHECK_LONG(error, EINVAL);
	CHECK_LONG(complete, 0);
	CHECK_LONG(vec_path_error(&path), EINVAL);
	CHECK_LONG(path.verb_count, 0);

	/* Empty data with such a path is still valid (nothing is asked of the path). */
	complete = 5;
	error = svg_path_data_parse("", &path, &complete);
	CHECK_LONG(error, 0);
	CHECK_LONG(complete, 1);
	vec_path_release(&path);
}

/* A coordinate the path refuses, and the limits of the path (the path's sticky errors). */
static void
test_path_errors(void)
{
	struct vec_path path;
	char *copy;
	char dump[DUMP_SIZE];
	char *text;
	size_t length;
	size_t count;
	size_t at;
	int complete;
	int error;

	/* A coordinate beyond VEC_COORD_MAX is the path's error; what came before stays, the caller draws nothing. */
	vec_path_init(&path);
	copy = exact_copy("M 1 2 L 1e20 0");
	complete = -1;
	error = svg_path_data_parse(copy, &path, &complete);
	CHECK_LONG(error, EINVAL);
	CHECK_LONG(complete, 0);
	CHECK_LONG(vec_path_error(&path), EINVAL);
	free(copy);
	vec_path_release(&path);

	/* The range's edge itself is allowed, the next float up is not. */
	vec_path_init(&path);
	copy = exact_copy("M 16777216 -16777216");
	complete = -1;
	error = svg_path_data_parse(copy, &path, &complete);
	CHECK_LONG(error, 0);
	CHECK_LONG(complete, 1);
	dump_path(&path, dump, sizeof(dump));
	CHECK(strcmp(dump, "M1.67772e+07,-1.67772e+07") == 0);
	free(copy);
	vec_path_release(&path);
	vec_path_init(&path);
	copy = exact_copy("M 16777218 0");
	complete = -1;
	error = svg_path_data_parse(copy, &path, &complete);
	CHECK_LONG(error, EINVAL);
	CHECK_LONG(complete, 0);
	free(copy);
	vec_path_release(&path);

	/* A relative coordinate that lands beyond the range is refused as an absolute one is. */
	vec_path_init(&path);
	copy = exact_copy("M 16000000 0 l 16000000 0");
	complete = -1;
	error = svg_path_data_parse(copy, &path, &complete);
	CHECK_LONG(error, EINVAL);
	CHECK_LONG(complete, 0);
	free(copy);
	vec_path_release(&path);

	/* A reflected control point beyond the range is refused. */
	vec_path_init(&path);
	copy = exact_copy("M 0 0 C 0 0 0 0 16000000 0 S 16000000 0 0 0");
	complete = -1;
	error = svg_path_data_parse(copy, &path, &complete);
	CHECK_LONG(error, EINVAL);
	CHECK_LONG(complete, 0);
	free(copy);
	vec_path_release(&path);

	/* A control point beyond the range is refused. */
	vec_path_init(&path);
	copy = exact_copy("M 0 0 Q 1e9 0 1 1");
	complete = -1;
	error = svg_path_data_parse(copy, &path, &complete);
	CHECK_LONG(error, EINVAL);
	free(copy);
	vec_path_release(&path);

	/* Builds a text of one moveto and count linetos. */
	count = 1048575;
	length = 4 + count * 4 + 8;
	text = malloc(length + 1);
	if (text == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	memcpy(text, "M0 0L", 5);
	at = 5;
	while (count > 0) {
		memcpy(text + at, "1 1 ", 4);
		at += 4;
		count--;
	}
	text[at] = '\0';

	/* A path of exactly VEC_PATH_VERBS_MAX verbs is accepted. */
	vec_path_init(&path);
	complete = -1;
	error = svg_path_data_parse(text, &path, &complete);
	CHECK_LONG(error, 0);
	CHECK_LONG(complete, 1);
	CHECK_LONG(path.verb_count, VEC_PATH_VERBS_MAX);
	vec_path_release(&path);

	/* One verb more is ENOBUFS, and the path keeps what it held. */
	text[at] = '2';
	text[at + 1] = ' ';
	text[at + 2] = '2';
	text[at + 3] = '\0';
	vec_path_init(&path);
	complete = -1;
	error = svg_path_data_parse(text, &path, &complete);
	CHECK_LONG(error, ENOBUFS);
	CHECK_LONG(complete, 0);
	CHECK_LONG(vec_path_error(&path), ENOBUFS);
	CHECK_LONG(path.verb_count, VEC_PATH_VERBS_MAX);
	vec_path_release(&path);
	free(text);
}

/* ------------------------------------------------------------------ */
/* The generator: writes path data from a model of the commands.      */
/* ------------------------------------------------------------------ */

/* The most text a generated path data takes. */
#define GEN_TEXT_MAX	8192

/*
 * What the generator keeps: the text written so far, and the model of the
 * pen (SVG 2 §9.3) with the path it builds with the path builder.
 */
struct gen {
	uint32_t random;
	char text[GEN_TEXT_MAX];
	size_t length;
	struct vec_path model;
	float current_x;
	float current_y;
	float start_x;
	float start_y;
	int previous;
	float control_x;
	float control_y;
};

/* Appends text to the generated text, within its room. */
static void
gen_append(
	struct gen *gen,
	const char *text)
{
	size_t size;

	size = strlen(text);
	if (gen->length + size + 1 > GEN_TEXT_MAX) {
		fprintf(stderr, "generated text too long\n");
		exit(2);
	}
	memcpy(gen->text + gen->length, text, size + 1);
	gen->length += size;
}

/* A random number below limit. */
static unsigned long
gen_below(
	struct gen *gen,
	unsigned long limit)
{
	return check_random(&gen->random) % limit;
}

/*
 * Appends a separator before a number whose text begins with first: a
 * comma_wsp of some kind, or nothing when the number begins with a sign
 * (a sign ends the number before it) and the grammar allows no separator
 * (every place the generator calls it from).
 */
static void
gen_separator(
	struct gen *gen,
	char first,
	int may_be_empty)
{
	static const char *const nonempty[] = { " ", ",", " , ", "\n", "\t,", ", ", " ,", "\r\n", "  " };
	unsigned long pick;

	pick = gen_below(gen, 11);
	if (may_be_empty && pick >= 9 && (first == '-' || first == '+')) {
		return;
	}
	gen_append(gen, nonempty[pick % 9]);
}

/* Writes the integer n as a number in one of several spellings; returns it in buffer. */
static void
gen_number_text(
	struct gen *gen,
	int n,
	char *buffer,
	size_t size)
{
	unsigned long form;

	form = gen_below(gen, 7);
	if (form == 1 && n >= 0)
		snprintf(buffer, size, "+%d", n);
	else if (form == 2)
		snprintf(buffer, size, "%d.0", n);
	else if (form == 3)
		snprintf(buffer, size, "%de0", n);
	else if (form == 4 && n >= 0)
		snprintf(buffer, size, "%03d", n);
	else if (form == 5)
		snprintf(buffer, size, "%d0e-1", n);
	else if (form == 6)
		snprintf(buffer, size, "%d00e-2", n);
	else
		snprintf(buffer, size, "%d", n);
}

/* A random integer in [low, high]. */
static int
gen_int(
	struct gen *gen,
	int low,
	int high)
{
	return low + (int)gen_below(gen, (unsigned long)(high - low + 1));
}

/*
 * Applies one argument set (the values as written) to the model: the
 * model's own statement of what SVG 2 §9.3 says each command does.
 * letter is the command letter in force.
 */
static void
gen_model_apply(
	struct gen *gen,
	char letter,
	const int *v)
{
	float bx;
	float by;
	float x;
	float y;
	float x1;
	float y1;
	float x2;
	float y2;
	int previous;
	char upper;

	upper = letter;
	if (letter >= 'a' && letter <= 'z')
		upper = (char)(letter - 'a' + 'A');
	bx = 0.0f;
	by = 0.0f;
	if (letter != upper) {
		bx = gen->current_x;
		by = gen->current_y;
	}
	previous = 0;
	x = gen->current_x;
	y = gen->current_y;
	x1 = 0.0f;
	y1 = 0.0f;
	x2 = 0.0f;
	y2 = 0.0f;
	switch (upper) {
	case 'M':
		x = bx + (float)v[0];
		y = by + (float)v[1];
		vec_path_move_to(&gen->model, x, y);
		gen->start_x = x;
		gen->start_y = y;
		break;
	case 'L':
		x = bx + (float)v[0];
		y = by + (float)v[1];
		vec_path_line_to(&gen->model, x, y);
		break;
	case 'H':
		x = bx + (float)v[0];
		vec_path_line_to(&gen->model, x, y);
		break;
	case 'V':
		y = by + (float)v[0];
		vec_path_line_to(&gen->model, x, y);
		break;
	case 'C':
		x1 = bx + (float)v[0];
		y1 = by + (float)v[1];
		x2 = bx + (float)v[2];
		y2 = by + (float)v[3];
		x = bx + (float)v[4];
		y = by + (float)v[5];
		vec_path_cubic_to(&gen->model, x1, y1, x2, y2, x, y);
		previous = 1;
		break;
	case 'S':
		x2 = bx + (float)v[0];
		y2 = by + (float)v[1];
		x = bx + (float)v[2];
		y = by + (float)v[3];
		x1 = gen->current_x;
		y1 = gen->current_y;
		if (gen->previous == 1) {
			x1 = 2.0f * gen->current_x - gen->control_x;
			y1 = 2.0f * gen->current_y - gen->control_y;
		}
		vec_path_cubic_to(&gen->model, x1, y1, x2, y2, x, y);
		previous = 1;
		break;
	case 'Q':
		x1 = bx + (float)v[0];
		y1 = by + (float)v[1];
		x = bx + (float)v[2];
		y = by + (float)v[3];
		vec_path_quad_to(&gen->model, x1, y1, x, y);
		previous = 2;
		break;
	case 'T':
		x = bx + (float)v[0];
		y = by + (float)v[1];
		x1 = gen->current_x;
		y1 = gen->current_y;
		if (gen->previous == 2) {
			x1 = 2.0f * gen->current_x - gen->control_x;
			y1 = 2.0f * gen->current_y - gen->control_y;
		}
		vec_path_quad_to(&gen->model, x1, y1, x, y);
		previous = 2;
		break;
	default:
		x = bx + (float)v[5];
		y = by + (float)v[6];
		vec_path_arc_to(&gen->model, (float)v[0], (float)v[1], (float)v[2], v[3], v[4], x, y);
		break;
	}
	gen->current_x = x;
	gen->current_y = y;
	gen->previous = previous;
	if (previous == 1) {
		gen->control_x = x2;
		gen->control_y = y2;
	} else if (previous == 2) {
		gen->control_x = x1;
		gen->control_y = y1;
	}
}

/* Writes one argument set of a command (random values) to the text and applies it to the model. */
static void
gen_set(
	struct gen *gen,
	char letter,
	int first_set)
{
	static const char *const flag_separators[] = { "", "", " ", ",", " , ", "\n" };
	char upper;
	char number[64];
	int values[7];
	int count;
	int i;
	int flags;

	upper = letter;
	if (letter >= 'a' && letter <= 'z')
		upper = (char)(letter - 'a' + 'A');
	count = 2;
	if (upper == 'H' || upper == 'V')
		count = 1;
	else if (upper == 'S' || upper == 'Q')
		count = 4;
	else if (upper == 'C')
		count = 6;
	else if (upper == 'A')
		count = 7;
	for (i = 0; i < count; i++)
		values[i] = gen_int(gen, -20, 20);
	flags = 0;
	if (upper == 'A') {
		values[0] = gen_int(gen, -6, 6);
		values[1] = gen_int(gen, -6, 6);
		values[2] = gen_int(gen, -90, 90);
		values[3] = gen_int(gen, 0, 1);
		values[4] = gen_int(gen, 0, 1);
		flags = 1;
	}

	/* Writes the numbers with the separators the grammar allows. */
	for (i = 0; i < count; i++) {
		if (flags && (i == 3 || i == 4)) {
			/* A flag is a single digit; the separator before the first is required. */
			if (i == 3) {
				gen_append(gen, " ");
			} else {
				gen_append(gen, flag_separators[gen_below(gen, 6)]);
			}
			snprintf(number, sizeof(number), "%d", values[i]);
			gen_append(gen, number);
			continue;
		}
		gen_number_text(gen, values[i], number, sizeof(number));
		if (i == 0 && first_set) {
			/* The first number after the letter: white space or nothing. */
			if (gen_below(gen, 2) == 0)
				gen_append(gen, " ");
		} else if (i == 0) {
			/* The first number of a further set: a separator, or nothing before a sign. */
			gen_separator(gen, number[0], 1);
		} else if (flags && i == 5) {
			/* The first number after the flags: anything, even nothing. */
			gen_append(gen, flag_separators[gen_below(gen, 6)]);
		} else {
			gen_separator(gen, number[0], 1);
		}
		gen_append(gen, number);
	}

	/* Applies the set to the model. */
	gen_model_apply(gen, letter, values);
}

/* Generates path data and its model: a moveto and a random sequence of commands. */
static void
gen_path_data(
	struct gen *gen,
	unsigned long commands)
{
	static const char letters[] = "MmLlHhVvCcSsQqTtAaZz";
	static const char *const between[] = { "", "", " ", "\n", "  ", "\t" };
	unsigned long n;
	unsigned long sets;
	unsigned long s;
	char letter;
	char active;
	char one[2];

	gen->length = 0;
	gen->text[0] = '\0';
	vec_path_init(&gen->model);
	gen->current_x = 0.0f;
	gen->current_y = 0.0f;
	gen->start_x = 0.0f;
	gen->start_y = 0.0f;
	gen->previous = 0;
	gen->control_x = 0.0f;
	gen->control_y = 0.0f;
	one[1] = '\0';

	/* Leading white space, sometimes. */
	if (gen_below(gen, 3) == 0)
		gen_append(gen, " \n");

	for (n = 0; n < commands; n++) {
		letter = letters[gen_below(gen, (unsigned long)(sizeof(letters) - 1))];
		if (n == 0)
			letter = gen_below(gen, 2) == 0 ? 'M' : 'm';
		one[0] = letter;
		gen_append(gen, one);
		if (letter == 'Z' || letter == 'z') {
			vec_path_close(&gen->model);
			gen->current_x = gen->start_x;
			gen->current_y = gen->start_y;
			gen->previous = 0;
		} else {
			sets = 1 + gen_below(gen, 3);
			active = letter;
			for (s = 0; s < sets; s++) {
				gen_set(gen, active, s == 0);
				if (active == 'M')
					active = 'L';
				if (active == 'm')
					active = 'l';
			}
		}
		if (n + 1 < commands)
			gen_append(gen, between[gen_below(gen, 6)]);
	}

	/* Trailing white space, sometimes. */
	if (gen_below(gen, 3) == 0)
		gen_append(gen, "\r\n ");
}

/* Writes path data from a model and compares what the reader makes of it with the model's path. */
static void
test_generated(void)
{
	struct gen *gen;
	struct vec_path path;
	char dump[DUMP_SIZE];
	char dump_model[DUMP_SIZE];
	char *copy;
	unsigned long iteration;
	int complete;
	int error;
	int equal;

	gen = malloc(sizeof(*gen));
	if (gen == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	gen->random = 0x13579bdfU;

	/* Generates 20000 texts of 1 to 14 commands. */
	for (iteration = 0; iteration < 20000; iteration++) {
		gen_path_data(gen, 1 + gen_below(gen, 14));
		vec_path_init(&path);
		copy = exact_copy(gen->text);
		complete = -1;
		error = svg_path_data_parse(copy, &path, &complete);
		equal = path_equal(&path, &gen->model);
		check_record_long(error, 0, __FILE__, __LINE__, "generated: return");
		check_record_long(complete, 1, __FILE__, __LINE__, "generated: complete");
		check_record(equal, __FILE__, __LINE__, "generated: path");
		if (!equal || error != 0 || complete != 1) {
			dump_path(&path, dump, sizeof(dump));
			dump_path(&gen->model, dump_model, sizeof(dump_model));
			printf("  text '%s'\n  got      '%s'\n  expected '%s'\n", gen->text, dump, dump_model);
		}
		free(copy);
		vec_path_release(&path);
		vec_path_release(&gen->model);
	}
	free(gen);
}

/*
 * Every prefix of generated text is read without error, leaves a path that
 * is well formed, and reports complete only when the prefix is the whole
 * text (up to trailing white space, which the generator may have written).
 */
static void
test_prefixes(void)
{
	struct gen *gen;
	struct vec_path path;
	struct vec_path_view view;
	char *copy;
	size_t length;
	size_t cut;
	unsigned long iteration;
	int complete;
	int error;

	gen = malloc(sizeof(*gen));
	if (gen == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	gen->random = 0x2468ace1U;

	/* Cuts 300 generated texts at every length. */
	for (iteration = 0; iteration < 300; iteration++) {
		gen_path_data(gen, 1 + gen_below(gen, 10));
		length = strlen(gen->text);
		for (cut = 0; cut <= length; cut++) {
			copy = malloc(cut + 1);
			if (copy == NULL) {
				fprintf(stderr, "out of memory\n");
				exit(2);
			}
			memcpy(copy, gen->text, cut);
			copy[cut] = '\0';
			vec_path_init(&path);
			complete = -1;
			error = svg_path_data_parse(copy, &path, &complete);
			CHECK_LONG(error, 0);
			CHECK(complete == 0 || complete == 1);

			/* The path is a well formed one (empty is not a view the checker accepts). */
			if (path.verb_count != 0) {
				vec_path_view_of(&path, &view);
				CHECK_LONG(vec_path_view_check(&view), 0);
			}

			/* The whole text reads as complete and as the model's path. */
			if (cut == length) {
				CHECK_LONG(complete, 1);
				CHECK(path_equal(&path, &gen->model));
			}
			free(copy);
			vec_path_release(&path);
		}
		vec_path_release(&gen->model);
	}
	free(gen);
}

/* Feeds random strings from the characters of path data and from junk; nothing may go wrong. */
static void
test_random(void)
{
	static const char alphabet[] = "MmZzLlHhVvCcSsQqTtAa0123456789+-.,eE \t\n\f\rxX\v\x80(";
	struct vec_path path;
	struct vec_path_view view;
	uint32_t state;
	char *copy;
	size_t length;
	size_t i;
	unsigned long iteration;
	int complete;
	int error;

	state = 0x9e3779b9U;

	/* Feeds 60000 random strings of 0 to 60 characters. */
	for (iteration = 0; iteration < 60000; iteration++) {
		length = check_random(&state) % 61;
		copy = malloc(length + 1);
		if (copy == NULL) {
			fprintf(stderr, "out of memory\n");
			exit(2);
		}
		for (i = 0; i < length; i++)
			copy[i] = alphabet[check_random(&state) % (sizeof(alphabet) - 1)];
		copy[length] = '\0';
		vec_path_init(&path);
		complete = -1;
		error = svg_path_data_parse(copy, &path, &complete);

		/* The reading returns, with the flag set, and an error only from the path's own limits. */
		CHECK(error == 0 || error == EINVAL || error == ENOBUFS || error == ENOMEM);
		CHECK(complete == 0 || complete == 1);
		if (error != 0)
			CHECK_LONG(complete, 0);

		/* Whatever path it leaves is well formed. */
		if (path.verb_count != 0) {
			vec_path_view_of(&path, &view);
			CHECK_LONG(vec_path_view_check(&view), 0);
		}
		free(copy);
		vec_path_release(&path);
	}

	/* Feeds 20000 strings of numbers with random digits, points, signs and exponents after a moveto. */
	for (iteration = 0; iteration < 20000; iteration++) {
		length = check_random(&state) % 40;
		copy = malloc(length + 6);
		if (copy == NULL) {
			fprintf(stderr, "out of memory\n");
			exit(2);
		}
		memcpy(copy, "M 1 1", 5);
		for (i = 0; i < length; i++)
			copy[5 + i] = "0123456789.-+eE, "[check_random(&state) % 17];
		copy[5 + length] = '\0';
		vec_path_init(&path);
		complete = -1;
		error = svg_path_data_parse(copy, &path, &complete);
		CHECK(error == 0 || error == EINVAL);
		CHECK(complete == 0 || complete == 1);
		if (path.verb_count != 0) {
			vec_path_view_of(&path, &view);
			CHECK_LONG(vec_path_view_check(&view), 0);
		}
		free(copy);
		vec_path_release(&path);
	}
}

int
main(void)
{
	/* Runs every group of checks. */
	test_commands();
	test_close();
	test_smooth();
	test_arcs();
	test_grammar();
	test_errors();
	test_append();
	test_arguments();
	test_path_errors();
	test_generated();
	test_prefixes();
	test_random();

	/* Reports the result. */
	return check_finish("pathdata");
}
