/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the grid track sizing algorithm against cases whose answers the
 * specification (and the web platform tests built from it) fix.
 */

#include "layout/grid-track.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How many cases failed. */
static int failures;

/* The minimum contribution of the case's item when it differs from its content (-1: the same). */
static int minimum_override = -1;

/* What the items of a case contribute. */
struct case_items {
	layout_unit min_content[8];
	layout_unit max_content[8];
	layout_unit minimum[8];
};

/* Answers the sizing with a case's numbers (pixels times 64). */
static layout_unit
answer(
	void *context,
	size_t item,
	int kind,
	int automatic,
	layout_unit *fixed)
{
	struct case_items *items;

	/* The case's table. */
	(void)automatic;
	*fixed = -1;
	items = context;
	if (kind == GRID_CONTRIBUTION_MIN_CONTENT)
		return items->min_content[item] * LAYOUT_UNIT;
	if (kind == GRID_CONTRIBUTION_MAX_CONTENT)
		return items->max_content[item] * LAYOUT_UNIT;
	return items->minimum[item] * LAYOUT_UNIT;
}

/* Parses a track list like "1fr 100 auto mc xc m(25,1fr) fit(30)" into specs. */
static int
parse_tracks(
	const char *text,
	struct grid_track_spec *tracks)
{
	char word[64];
	int count;
	int used;
	float number;
	int lower;

	/* Each word. */
	count = 0;
	while (sscanf(text, " %63s%n", word, &used) == 1) {
		text += used;
		memset(&tracks[count], 0, sizeof(tracks[count]));
		tracks[count].minimum = GRID_TRACK_AUTO;
		if (strcmp(word, "auto") == 0) {
			tracks[count].maximum = GRID_TRACK_AUTO;
		} else if (strcmp(word, "mc") == 0) {
			tracks[count].minimum = GRID_TRACK_AUTO;
			tracks[count].maximum = GRID_TRACK_MIN_CONTENT;
		} else if (strcmp(word, "xc") == 0) {
			tracks[count].maximum = GRID_TRACK_MAX_CONTENT;
		} else if (sscanf(word, "fit(%f)", &number) == 1) {
			tracks[count].maximum = GRID_TRACK_FIT_CONTENT;
			tracks[count].maximum_length = (layout_unit)(number * LAYOUT_UNIT);
		} else if (sscanf(word, "%ffr", &number) == 1 && strstr(word, "fr") != NULL && word[0] != 'm') {
			tracks[count].maximum = GRID_TRACK_FLEX;
			tracks[count].flex = number;
		} else if (sscanf(word, "m(%d,%ffr)", &lower, &number) == 2) {
			tracks[count].minimum = GRID_TRACK_LENGTH;
			tracks[count].minimum_length = lower * LAYOUT_UNIT;
			tracks[count].maximum = GRID_TRACK_FLEX;
			tracks[count].flex = number;
		} else if (sscanf(word, "mx(%d,%d)", &lower, &used) == 2) {
			tracks[count].minimum = GRID_TRACK_LENGTH;
			tracks[count].minimum_length = lower * LAYOUT_UNIT;
			tracks[count].maximum = GRID_TRACK_LENGTH;
			tracks[count].maximum_length = used * LAYOUT_UNIT;
		} else if (sscanf(word, "mxc(%d)", &lower) == 1) {
			tracks[count].minimum = GRID_TRACK_MAX_CONTENT;
			tracks[count].maximum = GRID_TRACK_LENGTH;
			tracks[count].maximum_length = lower * LAYOUT_UNIT;
		} else if (sscanf(word, "ma(%d)", &lower) == 1) {
			tracks[count].minimum = GRID_TRACK_AUTO;
			tracks[count].maximum = GRID_TRACK_LENGTH;
			tracks[count].maximum_length = lower * LAYOUT_UNIT;
		} else {
			tracks[count].minimum = GRID_TRACK_LENGTH;
			tracks[count].maximum = GRID_TRACK_LENGTH;
			tracks[count].minimum_length = (layout_unit)(atof(word) * LAYOUT_UNIT);
			tracks[count].maximum_length = tracks[count].minimum_length;
		}
		count++;
	}
	return count;
}

/* Runs one case and compares the sizes (in pixels, space separated) with the expected text. */
static void
run(
	const char *name,
	const char *tracks_text,
	int constraint,
	int available,
	int span_start,
	int span,
	int content,
	const char *expected)
{
	struct grid_track_spec tracks[16];
	struct grid_track_item item;
	struct grid_track_input input;
	struct case_items items;
	layout_unit sizes[16];
	char got[256];
	int count;
	int index;
	int length;

	/* The tracks and the one item every case has. */
	count = parse_tracks(tracks_text, tracks);
	item.start = span_start;
	item.span = span;
	memset(&items, 0, sizeof(items));
	items.min_content[0] = content;
	items.max_content[0] = content;
	items.minimum[0] = minimum_override >= 0 ? minimum_override : content;
	items.min_content[0] = minimum_override >= 0 ? minimum_override : content;

	/* The input. */
	memset(&input, 0, sizeof(input));
	input.tracks = tracks;
	input.count = count;
	input.items = &item;
	input.item_count = content >= 0 ? 1U : 0U;
	input.constraint = constraint;
	input.available = available * LAYOUT_UNIT;
	input.container_minimum = -1;
	input.container_maximum = -1;
	input.stretch_auto = 1;
	input.contribution = answer;
	input.context = &items;
	if (grid_track_size(&input, sizes) != 0) {
		printf("FAIL %s: error\n", name);
		failures++;
		return;
	}

	/* The sizes in pixels. */
	length = 0;
	got[0] = '\0';
	for (index = 0; index < count; index++) {
		length += snprintf(got + length, sizeof(got) - (size_t)length, index ? " %g" : "%g",
		    (double)sizes[index] / LAYOUT_UNIT);
	}
	if (strcmp(got, expected) != 0) {
		printf("FAIL %s: tracks [%s] got [%s] expected [%s]\n", name, tracks_text, got, expected);
		failures++;
	}
}

/* Runs the cases. */
int
main(
	void)
{
	int definite;

	/* The grid is 50px wide and the item is 100px, as in grid-flex-track-intrinsic-sizes-001. */
	definite = GRID_TRACK_DEFINITE;
	run("flex-1", "0fr", definite, 50, 0, 1, 100, "100");
	run("flex-2", "1fr", definite, 50, 0, 1, 100, "100");
	run("flex-3", "2fr", definite, 50, 0, 1, 100, "100");
	run("fixed-flex-1", "m(0,0fr)", definite, 50, 0, 1, 100, "0");
	run("fixed-flex-2", "m(0,.5fr)", definite, 50, 0, 1, 100, "25");
	run("fixed-flex-3", "m(0,1fr)", definite, 50, 0, 1, 100, "50");
	run("fixed-flex-4", "m(0,2fr)", definite, 50, 0, 1, 100, "50");
	run("fixed-flex-5", "m(75,1fr)", definite, 50, 0, 1, 100, "75");
	run("span2-1", "0fr 0fr", definite, 50, 0, 2, 100, "50 50");
	run("span2-2", "0fr 1fr", definite, 50, 0, 2, 100, "0 100");
	run("span2-3", "1fr 0fr", definite, 50, 0, 2, 100, "100 0");
	run("span2-4", "1fr 1fr", definite, 50, 0, 2, 100, "50 50");
	run("span2-5", "1fr 3fr", definite, 50, 0, 2, 100, "25 75");
	run("span2-6", "0fr 0fr 1fr", definite, 50, 0, 2, 100, "50 50 0");
	run("span2-7", "m(0,0fr) m(0,0fr)", definite, 50, 0, 2, 100, "0 0");
	run("span2-8", "m(0,0fr) m(0,1fr)", definite, 50, 0, 2, 100, "0 50");
	run("span2-9", "m(15,0fr) m(0,1fr)", definite, 50, 0, 2, 100, "15 35");
	run("span2-10", "m(20,1fr) m(0,1fr)", definite, 50, 0, 2, 100, "25 25");
	run("span2-11", "m(30,1fr) m(0,1fr)", definite, 50, 0, 2, 100, "30 20");
	run("mixed-1", "0fr m(0,0fr)", definite, 50, 0, 2, 100, "100 0");
	run("mixed-2", "0fr m(0,1fr)", definite, 50, 0, 2, 100, "100 0");
	run("mixed-3", "1fr m(0,1fr)", definite, 50, 0, 2, 100, "100 0");
	run("mixed-4", "1fr m(25,1fr)", definite, 50, 0, 2, 100, "75 25");
	run("mixed-5", "0fr auto", definite, 50, 0, 2, 100, "100 0");
	run("mixed-6", "1fr auto", definite, 50, 0, 2, 100, "100 0");
	run("mixed-7", "1fr xc", definite, 50, 0, 2, 100, "100 0");
	run("mixed-8", "m(0,0fr) auto", definite, 50, 0, 2, 100, "0 50");
	run("mixed-9", "m(0,1fr) auto", definite, 50, 0, 2, 100, "50 0");
	run("mixed-10", "m(25,0fr) auto", definite, 50, 0, 2, 100, "25 25");
	run("mixed-11", "m(25,1fr) auto", definite, 50, 0, 2, 100, "50 0");

	/* Plain cases. */
	run("auto-fills", "auto", definite, 200, 0, 1, 30, "200");
	run("auto-two", "auto auto", definite, 200, 0, 1, 30, "115 85");
	run("max-content", "auto 40", GRID_TRACK_MAX_CONSTRAINT, 0, 0, 1, 30, "30 40");
	run("fr-intrinsic", "1fr 1fr", GRID_TRACK_MAX_CONSTRAINT, 0, 0, 1, 30, "30 30");
	minimum_override = 10;
	run("fit-content", "fit(20)", GRID_TRACK_DEFINITE, 100, 0, 1, 50, "20");
	run("fit-content-small", "fit(80)", GRID_TRACK_DEFINITE, 100, 0, 1, 50, "50");
	minimum_override = -1;

	/* A max-content minimum takes the item's max-content contribution (grid-intrinsic-track-sizes-001). */
	run("max-content-minimum", "mxc(10) mxc(10)", definite, 120, 0, 2, 60, "30 30");
	if (failures != 0) {
		printf("%d failed\n", failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
