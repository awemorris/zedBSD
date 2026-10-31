/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The track sizing algorithm of CSS Grid Layout for one axis, written over
 * plain numbers so that it can be tried without a document: the caller
 * describes the tracks and the items placed in them, and answers what each
 * item contributes when asked.
 */

#ifndef KEILAND_BROWSER_GRID_TRACK_H
#define KEILAND_BROWSER_GRID_TRACK_H

#include "layout/layout.h"

#include <stddef.h>

/* The kinds of track sizing function (a minimum is never flexible or fit-content). */
enum grid_track_function {
	GRID_TRACK_LENGTH,
	GRID_TRACK_AUTO,
	GRID_TRACK_MIN_CONTENT,
	GRID_TRACK_MAX_CONTENT,
	GRID_TRACK_FIT_CONTENT,
	GRID_TRACK_FLEX
};

/* What the size of the axis is bound by: a definite size, or the content's min-content or max-content size. */
enum grid_track_constraint {
	GRID_TRACK_DEFINITE,
	GRID_TRACK_MIN_CONSTRAINT,
	GRID_TRACK_MAX_CONSTRAINT
};

/* The contributions an item can be asked for. */
enum grid_track_contribution {
	GRID_CONTRIBUTION_MIN_CONTENT,
	GRID_CONTRIBUTION_MAX_CONTENT,
	GRID_CONTRIBUTION_MINIMUM
};

/*
 * One track's sizing functions.  A length (or the limit of fit-content())
 * is already resolved to layout units; percentages of an indefinite size
 * are the caller's to turn into auto before asking.  A collapsed track is
 * a zero-sized track whose gaps collapse with it.
 */
struct grid_track_spec {
	int minimum;
	layout_unit minimum_length;
	int maximum;
	layout_unit maximum_length;
	float flex;
	int collapsed;
};

/* The tracks an item covers: the first one and how many. */
struct grid_track_item {
	int start;
	int span;
};

/*
 * Answers what an item contributes in this axis (its outer size, margins
 * included).  For the minimum contribution, automatic is nonzero when the
 * item's automatic minimum size applies (it spans a track with an auto
 * minimum), and the callback sets *fixed (-1 for none) to the part of the
 * answer that cannot shrink (margins, borders and padding) when the answer
 * is an automatic minimum that a fixed maximum may cap.
 */
typedef layout_unit (*grid_track_contribution_fn)(void *context, size_t item, int kind, int automatic, layout_unit *fixed);

/* The input of one sizing: the tracks, the items, the space and who answers for the items. */
struct grid_track_input {
	const struct grid_track_spec *tracks;
	int count;
	const struct grid_track_item *items;
	size_t item_count;
	layout_unit gap;
	int constraint;
	/* The inner size of the container when it is definite. */
	layout_unit available;
	/* The container's own minimum and maximum inner size in this axis (-1 for none). */
	layout_unit container_minimum;
	layout_unit container_maximum;
	/* Whether content distribution stretches the tracks with an auto maximum. */
	int stretch_auto;
	grid_track_contribution_fn contribution;
	void *context;
};

int grid_track_size(const struct grid_track_input *input, layout_unit *sizes);

#endif
