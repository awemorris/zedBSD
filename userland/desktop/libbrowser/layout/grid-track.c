/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The track sizing algorithm of CSS Grid Layout (https://drafts.csswg.org/css-grid-2/#algo-track-sizing)
 * for one axis: initialize the tracks, resolve the intrinsic sizes from
 * the items (the non-spanning ones, then the spanning ones in order of
 * span, then the ones crossing flexible tracks), maximize the tracks,
 * expand the flexible tracks and stretch the auto tracks.
 */

#include "layout/grid-track.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A size no track reaches, for a growth limit that is infinite. */
#define GT_INFINITE	(((int64_t)1) << 50)

/* Which size of a track a distribution of extra space grows: its base size or its growth limit. */
#define GT_BASE		0
#define GT_LIMIT	1

/* The tracks that take what is left after the limits: all the affected ones, or the ones with an intrinsic or a max-content maximum. */
#define GT_BEYOND_ALL		0
#define GT_BEYOND_INTRINSIC	1
#define GT_BEYOND_MAX_CONTENT	2

/*
 * The sizing in progress: the tracks' base sizes and growth limits, the
 * extra space planned for them, and the answers already given for the
 * items.
 */
struct gt_state {
	const struct grid_track_input *input;
	int64_t *base;
	int64_t *limit;
	int64_t *planned;
	int64_t *incurred;
	int64_t *room;
	int64_t *grow;
	unsigned char *affected;
	unsigned char *active;
	unsigned char *growable;
	unsigned char *inflexible;
	int64_t *answer;
	unsigned char *known;
	int has_flex;
};

static int gt_allocate(struct gt_state *state);
static void gt_release(struct gt_state *state);
static int64_t gt_gaps(const struct gt_state *state, int first, int span);
static int64_t gt_answer(struct gt_state *state, size_t item, int kind);
static int gt_cap(const struct gt_state *state, size_t item, int fit, int64_t *cap);
static int64_t gt_limited(struct gt_state *state, size_t item, int kind);
static int gt_spans_flex(const struct gt_state *state, size_t item);
static int gt_intrinsic_minimum(int function);
static int gt_intrinsic_maximum(int function);
static int64_t gt_size_of(const struct gt_state *state, int track, int target);
static int64_t gt_share(const int64_t *room, int64_t *grow, const unsigned char *active, int count, int64_t space);
static void gt_spread(struct gt_state *state, size_t item, int64_t contribution, int target, int beyond, int flexible);
static void gt_apply(struct gt_state *state, int target);
static void gt_initialize(struct gt_state *state);
static void gt_single_items(struct gt_state *state);
static void gt_spanning_items(struct gt_state *state);
static void gt_pass(struct gt_state *state, const size_t *group, size_t count, int step, int flexible);
static void gt_flexible_items(struct gt_state *state);
static void gt_close_limits(struct gt_state *state);
static void gt_maximize(struct gt_state *state);
static double gt_find_fr(struct gt_state *state, int first, int last, int64_t space);
static void gt_expand(struct gt_state *state);
static void gt_stretch(struct gt_state *state);
static int gt_compare_items(const void *left, const void *right);

/*
 * The order of the steps of a distribution (see gt_pass): the max-content
 * minimums of auto tracks under a max-content constraint (6) come before
 * the limits are raised (3).
 */
static const int gt_step_order[7] = { 0, 1, 2, 6, 3, 4, 5 };

/* The items by span for qsort (the sort key is read through this while the sort runs). */
static const struct grid_track_item *gt_sort_items;

/*
 * Sizes the tracks of one axis and stores their sizes.
 *
 * The sizes of collapsed tracks are zero; the caller decides what their
 * gaps do.  Returns ENOMEM when the working storage cannot be had.
 */
int
grid_track_size(
	const struct grid_track_input *input,
	layout_unit *sizes)
{
	struct gt_state state;
	int index;
	int error;

	/* Nothing to size. */
	if (input->count <= 0)
		return 0;

	/* The working storage. */
	memset(&state, 0, sizeof(state));
	state.input = input;
	error = gt_allocate(&state);
	if (error != 0)
		return error;

	/* Whether any track is flexible. */
	for (index = 0; index < input->count; index++) {
		if (input->tracks[index].maximum == GRID_TRACK_FLEX && !input->tracks[index].collapsed)
			state.has_flex = 1;
	}

	/* The steps of the algorithm, in order. */
	gt_initialize(&state);
	gt_single_items(&state);
	gt_spanning_items(&state);
	gt_flexible_items(&state);
	gt_close_limits(&state);
	gt_maximize(&state);
	gt_expand(&state);
	gt_stretch(&state);

	/* The base sizes are the used sizes. */
	for (index = 0; index < input->count; index++) {
		if (state.base[index] > INT32_MAX)
			sizes[index] = INT32_MAX;
		else
			sizes[index] = (layout_unit)state.base[index];
	}

	/* Succeeded: the tracks are sized. */
	gt_release(&state);
	return 0;
}

/* Allocates the working arrays in one block. */
static int
gt_allocate(
	struct gt_state *state)
{
	size_t count;
	size_t items;
	size_t bytes;
	char *block;

	/* One block for the arrays of 64-bit sizes and one for the flags. */
	count = (size_t)state->input->count;
	items = state->input->item_count;
	bytes = (count * 7U + items * 3U) * sizeof(int64_t) + count * 5U + items * 3U;
	block = calloc(1U, bytes + 16U);
	if (block == NULL)
		return ENOMEM;

	/* The 64-bit arrays first. */
	state->base = (int64_t *)block;
	state->limit = state->base + count;
	state->planned = state->limit + count;
	state->incurred = state->planned + count;
	state->room = state->incurred + count;
	state->grow = state->room + count;
	state->answer = state->grow + count;
	state->affected = (unsigned char *)(state->answer + items * 3U + count);

	/* The flags after them. */
	state->active = state->affected + count;
	state->growable = state->active + count;
	state->inflexible = state->growable + count;
	state->known = state->inflexible + count;
	return 0;
}

/* Frees the working arrays (the base array is the start of the block). */
static void
gt_release(
	struct gt_state *state)
{
	free(state->base);
}

/* Sums the gaps inside a span of tracks: one between each pair of tracks that take room. */
static int64_t
gt_gaps(
	const struct gt_state *state,
	int first,
	int span)
{
	int live;
	int index;

	/* Counts the tracks that have not collapsed. */
	live = 0;
	for (index = first; index < first + span && index < state->input->count; index++) {
		if (!state->input->tracks[index].collapsed)
			live++;
	}

	/* One fewer gaps than tracks. */
	if (live < 2)
		return 0;
	return (int64_t)state->input->gap * (live - 1);
}

/* Asks the caller for an item's contribution once and keeps the answer. */
static int64_t
gt_answer(
	struct gt_state *state,
	size_t item,
	int kind)
{
	const struct grid_track_input *input;
	int64_t value;
	int64_t cap;
	layout_unit fixed;
	int automatic;
	int index;

	/* A contribution asked for before. */
	input = state->input;
	index = (int)(item * 3U) + kind;
	if (state->known[index])
		return state->answer[index];

	/* The automatic minimum applies to an item that spans a track with an auto minimum, and when it spans several, no flexible one. */
	automatic = 0;
	if (kind == GRID_CONTRIBUTION_MINIMUM) {
		int track;

		for (track = input->items[item].start; track < input->items[item].start + input->items[item].span; track++) {
			if (input->tracks[track].minimum == GRID_TRACK_AUTO)
				automatic = 1;
		}
		if (input->items[item].span > 1 && gt_spans_flex(state, item))
			automatic = 0;
	}

	/* The caller's answer. */
	fixed = -1;
	value = input->contribution(input->context, item, kind, automatic, &fixed);
	if (value < 0)
		value = 0;

	/* An automatic minimum is no more than what fits an item that spans only tracks with a fixed maximum. */
	if (kind == GRID_CONTRIBUTION_MINIMUM && fixed >= 0 && value > fixed && gt_cap(state, item, 0, &cap)) {
		if (cap < fixed)
			cap = fixed;
		if (value > cap)
			value = cap;
	}

	/* Keeps it. */
	state->answer[index] = value;
	state->known[index] = 1;
	return value;
}

/* Finds the most an item spanning only fixed-maximum tracks needs: their maxima and the gaps between. */
static int
gt_cap(
	const struct gt_state *state,
	size_t item,
	int fit,
	int64_t *cap)
{
	const struct grid_track_spec *track;
	int first;
	int span;
	int index;
	int64_t sum;

	/* Each spanned track must have a fixed maximum (a length or the limit of fit-content()). */
	first = state->input->items[item].start;
	span = state->input->items[item].span;
	sum = 0;
	for (index = first; index < first + span; index++) {
		track = &state->input->tracks[index];
		if (track->collapsed)
			continue;
		if (track->maximum != GRID_TRACK_LENGTH && !(fit && track->maximum == GRID_TRACK_FIT_CONTENT))
			return 0;
		sum += track->maximum_length;
	}

	/* The sum with the gaps between. */
	*cap = sum + gt_gaps(state, first, span);
	return 1;
}

/* Finds an item's limited min-content or max-content contribution: capped by fixed maxima, floored by its minimum contribution. */
static int64_t
gt_limited(
	struct gt_state *state,
	size_t item,
	int kind)
{
	int64_t value;
	int64_t cap;
	int64_t minimum;

	/* The contribution, capped. */
	value = gt_answer(state, item, kind);
	if (gt_cap(state, item, 1, &cap)) {
		if (value > cap)
			value = cap;
	}

	/* Never below the item's minimum contribution. */
	minimum = gt_answer(state, item, GRID_CONTRIBUTION_MINIMUM);
	if (value < minimum)
		value = minimum;
	return value;
}

/* Tells whether an item spans a flexible track. */
static int
gt_spans_flex(
	const struct gt_state *state,
	size_t item)
{
	int index;

	/* Each spanned track. */
	for (index = state->input->items[item].start;
	    index < state->input->items[item].start + state->input->items[item].span; index++) {
		if (state->input->tracks[index].maximum == GRID_TRACK_FLEX && !state->input->tracks[index].collapsed)
			return 1;
	}
	return 0;
}

/* Tells whether a minimum sizing function is intrinsic. */
static int
gt_intrinsic_minimum(
	int function)
{
	if (function == GRID_TRACK_AUTO)
		return 1;
	if (function == GRID_TRACK_MIN_CONTENT)
		return 1;
	if (function == GRID_TRACK_MAX_CONTENT)
		return 1;
	return 0;
}

/* Tells whether a maximum sizing function is intrinsic (not a length, not flexible). */
static int
gt_intrinsic_maximum(
	int function)
{
	if (function == GRID_TRACK_AUTO)
		return 1;
	if (function == GRID_TRACK_MIN_CONTENT)
		return 1;
	if (function == GRID_TRACK_MAX_CONTENT)
		return 1;
	if (function == GRID_TRACK_FIT_CONTENT)
		return 1;
	return 0;
}

/* Finds the size of a track that a distribution of extra space measures against. */
static int64_t
gt_size_of(
	const struct gt_state *state,
	int track,
	int target)
{
	/* A base size is its own measure. */
	if (target == GT_BASE)
		return state->base[track];

	/* A growth limit that is infinite, or that may grow without bound this round, counts as the base size. */
	if (state->limit[track] >= GT_INFINITE || state->growable[track])
		return state->base[track];
	return state->limit[track];
}

/*
 * Gives space equally to the active tracks, no more to each than its
 * room, and returns what is left.
 */
static int64_t
gt_share(
	const int64_t *room,
	int64_t *grow,
	const unsigned char *active,
	int count,
	int64_t space)
{
	int64_t share;
	int64_t step;
	int64_t before;
	int open;
	int index;

	/* Rounds of equal shares until the space is gone or no track has room. */
	while (space > 0) {
		/* How many tracks can still take some. */
		open = 0;
		for (index = 0; index < count; index++) {
			if (active[index] && room[index] > grow[index])
				open++;
		}
		if (open == 0)
			break;

		/* The equal share; a share below one unit goes out a unit at a time. */
		share = space / open;
		before = space;
		for (index = 0; index < count && space > 0; index++) {
			if (!active[index] || room[index] <= grow[index])
				continue;
			step = share;
			if (share == 0)
				step = 1;
			if (step > room[index] - grow[index])
				step = room[index] - grow[index];
			if (step > space)
				step = space;
			grow[index] += step;
			space -= step;
		}
		if (space == before)
			break;
	}
	return space;
}

/*
 * Distributes the extra space an item needs across the affected tracks it
 * spans (the affected flags are set by the caller): what the contribution
 * exceeds the spanned tracks' sizes and gaps by.  The item-incurred
 * increases are left in the incurred array.
 */
static void
gt_spread(
	struct gt_state *state,
	size_t item,
	int64_t contribution,
	int target,
	int beyond,
	int flexible)
{
	const struct grid_track_spec *track;
	int64_t space;
	int64_t sum;
	int64_t given;
	int64_t flex_sum;
	int first;
	int span;
	int index;
	int count;
	int chosen;

	/* The tracks the item spans. */
	first = state->input->items[item].start;
	span = state->input->items[item].span;
	for (index = first; index < first + span; index++) {
		state->incurred[index] = 0;
		state->grow[index] = 0;
		state->room[index] = GT_INFINITE;
		state->active[index] = state->affected[index];
	}

	/* The extra space is what the contribution needs beyond what the tracks and gaps already give. */
	sum = gt_gaps(state, first, span);
	for (index = first; index < first + span; index++) {
		if (!state->input->tracks[index].collapsed)
			sum += gt_size_of(state, index, target);
	}
	space = contribution - sum;
	if (space <= 0)
		return;

	/* Flexible tracks get it in proportion to their flex factors, equally when they have none. */
	if (flexible) {
		count = 0;
		flex_sum = 0;
		for (index = first; index < first + span; index++) {
			if (!state->active[index])
				continue;
			count++;
			flex_sum += (int64_t)(state->input->tracks[index].flex * 65536.0f);
		}
		given = 0;
		chosen = 0;
		for (index = first; index < first + span && count > 0; index++) {
			if (!state->active[index])
				continue;
			chosen++;
			if (chosen == count)
				state->incurred[index] = space - given;
			else if (flex_sum > 0)
				state->incurred[index] = space * (int64_t)(state->input->tracks[index].flex * 65536.0f) / flex_sum;
			else
				state->incurred[index] = space / count;
			given += state->incurred[index];
		}
		return;
	}

	/* Up to the limits: a base size to the growth limit, a growth limit to infinity (fit-content() to its argument). */
	for (index = first; index < first + span; index++) {
		track = &state->input->tracks[index];
		if (!state->active[index])
			continue;
		if (target == GT_BASE) {
			if (state->limit[index] < GT_INFINITE)
				state->room[index] = state->limit[index] - state->base[index];
		} else if (track->maximum == GRID_TRACK_FIT_CONTENT) {
			state->room[index] = track->maximum_length - gt_size_of(state, index, target);
		} else if (state->limit[index] < GT_INFINITE && !state->growable[index]) {
			/* A growth limit already finite does not take space before the infinite ones do. */
			state->room[index] = 0;
		}
		if (state->room[index] < 0)
			state->room[index] = 0;
	}
	space = gt_share(state->room + first, state->grow + first, state->active + first, span, space);

	/* Beyond the limits: the rest goes to the tracks that the rule names, or to all the affected ones when none qualifies. */
	if (space > 0) {
		count = 0;
		for (index = first; index < first + span; index++) {
			track = &state->input->tracks[index];
			state->active[index] = 0;
			if (!state->affected[index])
				continue;
			if (beyond == GT_BEYOND_INTRINSIC) {
				if (gt_intrinsic_maximum(track->maximum) &&
				    (track->maximum != GRID_TRACK_FIT_CONTENT ||
				    state->base[index] + state->grow[index] < track->maximum_length))
					state->active[index] = 1;
			} else if (beyond == GT_BEYOND_MAX_CONTENT) {
				if (track->maximum == GRID_TRACK_MAX_CONTENT || track->maximum == GRID_TRACK_AUTO ||
				    (track->maximum == GRID_TRACK_FIT_CONTENT &&
				    state->base[index] + state->grow[index] < track->maximum_length))
					state->active[index] = 1;
			} else {
				state->active[index] = 1;
			}
			if (state->active[index])
				count++;
		}
		if (count == 0) {
			for (index = first; index < first + span; index++) {
				state->active[index] = state->affected[index];
				if (state->active[index])
					count++;
			}
		}
		for (index = first; index < first + span; index++)
			state->room[index] = GT_INFINITE;
		gt_share(state->room + first, state->grow + first, state->active + first, span, space);
	}

	/* The increases. */
	for (index = first; index < first + span; index++)
		state->incurred[index] = state->grow[index];
}

/* Adds the planned increases to the affected sizes and clears them. */
static void
gt_apply(
	struct gt_state *state,
	int target)
{
	int index;

	/* Each track. */
	for (index = 0; index < state->input->count; index++) {
		if (state->planned[index] > 0) {
			if (target == GT_BASE) {
				state->base[index] += state->planned[index];
			} else {
				/* A limit that was infinite becomes finite and may grow further in the next step. */
				if (state->limit[index] >= GT_INFINITE || state->growable[index]) {
					state->limit[index] = state->base[index] + state->planned[index];
					if (state->limit[index] >= GT_INFINITE)
						state->limit[index] = GT_INFINITE - 1;
				} else {
					state->limit[index] += state->planned[index];
				}
			}
		}
		state->planned[index] = 0;
	}
}

/* Initializes the base sizes and growth limits from the tracks' sizing functions. */
static void
gt_initialize(
	struct gt_state *state)
{
	const struct grid_track_spec *track;
	int index;

	/* Each track. */
	for (index = 0; index < state->input->count; index++) {
		track = &state->input->tracks[index];
		state->base[index] = 0;
		state->limit[index] = GT_INFINITE;
		if (track->collapsed) {
			state->limit[index] = 0;
			continue;
		}

		/* A fixed minimum is the base size. */
		if (track->minimum == GRID_TRACK_LENGTH)
			state->base[index] = track->minimum_length;

		/* A fixed maximum is the growth limit; an intrinsic or flexible one is infinite. */
		if (track->maximum == GRID_TRACK_LENGTH)
			state->limit[index] = track->maximum_length;

		/* A limit never falls below the base size. */
		if (state->limit[index] < state->base[index])
			state->limit[index] = state->base[index];
	}
}

/* Sizes the tracks to fit the items that span one track and no flexible one. */
static void
gt_single_items(
	struct gt_state *state)
{
	const struct grid_track_input *input;
	const struct grid_track_spec *track;
	size_t item;
	int64_t value;
	int64_t clamped;
	int index;

	/* Each item of one track that is not flexible. */
	input = state->input;
	for (item = 0; item < input->item_count; item++) {
		if (input->items[item].span != 1)
			continue;
		index = input->items[item].start;
		track = &input->tracks[index];
		if (track->collapsed || track->maximum == GRID_TRACK_FLEX)
			continue;

		/* The base size from the minimum sizing function. */
		if (track->minimum == GRID_TRACK_MIN_CONTENT) {
			value = gt_answer(state, item, GRID_CONTRIBUTION_MIN_CONTENT);
			if (value > state->base[index])
				state->base[index] = value;
		} else if (track->minimum == GRID_TRACK_MAX_CONTENT) {
			value = gt_answer(state, item, GRID_CONTRIBUTION_MAX_CONTENT);
			if (value > state->base[index])
				state->base[index] = value;
		} else if (track->minimum == GRID_TRACK_AUTO) {
			if (input->constraint != GRID_TRACK_DEFINITE)
				value = gt_limited(state, item, GRID_CONTRIBUTION_MIN_CONTENT);
			else
				value = gt_answer(state, item, GRID_CONTRIBUTION_MINIMUM);
			if (value > state->base[index])
				state->base[index] = value;
		}

		/* The growth limit from the maximum sizing function. */
		if (track->maximum == GRID_TRACK_MIN_CONTENT)
			value = gt_answer(state, item, GRID_CONTRIBUTION_MIN_CONTENT);
		else if (track->maximum == GRID_TRACK_MAX_CONTENT || track->maximum == GRID_TRACK_AUTO ||
		    track->maximum == GRID_TRACK_FIT_CONTENT)
			value = gt_answer(state, item, GRID_CONTRIBUTION_MAX_CONTENT);
		else
			continue;
		if (state->limit[index] >= GT_INFINITE || value > state->limit[index])
			clamped = value;
		else
			clamped = state->limit[index];
		if (track->maximum == GRID_TRACK_FIT_CONTENT && clamped > track->maximum_length)
			clamped = track->maximum_length;
		state->limit[index] = clamped;
	}

	/* A limit is never below its base size. */
	for (index = 0; index < input->count; index++) {
		if (state->limit[index] < state->base[index])
			state->limit[index] = state->base[index];
	}
}

/* Orders item indexes by span, then by index. */
static int
gt_compare_items(
	const void *left,
	const void *right)
{
	size_t first;
	size_t second;

	/* By span. */
	first = *(const size_t *)left;
	second = *(const size_t *)right;
	if (gt_sort_items[first].span != gt_sort_items[second].span)
		return gt_sort_items[first].span < gt_sort_items[second].span ? -1 : 1;

	/* Then in the order given, so that the sort is stable. */
	if (first != second)
		return first < second ? -1 : 1;
	return 0;
}

/* Sizes the tracks to fit the items that span several tracks and none flexible, shortest spans first. */
static void
gt_spanning_items(
	struct gt_state *state)
{
	const struct grid_track_input *input;
	size_t *order;
	size_t count;
	size_t item;
	size_t start;
	size_t end;
	int step;

	/* The items that span several tracks, none of them flexible. */
	input = state->input;
	order = malloc((input->item_count + 1U) * sizeof(*order));
	if (order == NULL)
		return;
	count = 0;
	for (item = 0; item < input->item_count; item++) {
		if (input->items[item].span < 2 || gt_spans_flex(state, item))
			continue;
		order[count++] = item;
	}
	gt_sort_items = input->items;
	qsort(order, count, sizeof(*order), gt_compare_items);

	/* Each group of the same span goes through the steps together. */
	for (start = 0; start < count; start = end) {
		for (end = start; end < count && input->items[order[end]].span == input->items[order[start]].span; end++)
			continue;
		for (step = 0; step < 7; step++)
			gt_pass(state, order + start, end - start, gt_step_order[step], 0);
	}
	free(order);
}

/*
 * One step of the spanning items' distributions for a group of items: the
 * steps are the intrinsic minimums (0), the content-based minimums (1),
 * the max-content minimums (2, and 6 for auto minimums under a
 * max-content constraint), the
 * raising of limits that fell below their base sizes (3), the intrinsic
 * maximums (4) and the max-content maximums (5).
 */
static void
gt_pass(
	struct gt_state *state,
	const size_t *group,
	size_t count,
	int step,
	int flexible)
{
	const struct grid_track_input *input;
	const struct grid_track_spec *track;
	size_t index;
	int64_t contribution;
	int target;
	int beyond;
	int track_index;
	int any;
	int fit;
	int first;
	int span;
	size_t item;

	/* Raising the limits needs no item. */
	input = state->input;
	if (step == 3) {
		for (track_index = 0; track_index < input->count; track_index++) {
			if (state->limit[track_index] < state->base[track_index])
				state->limit[track_index] = state->base[track_index];
		}
		return;
	}

	/* An auto minimum takes the limited max-content contribution only under a max-content constraint. */
	if (step == 6 && input->constraint != GRID_TRACK_MAX_CONSTRAINT)
		return;

	/* What is distributed to what. */
	target = GT_BASE;
	beyond = GT_BEYOND_INTRINSIC;
	if (step == 2 || step == 6)
		beyond = GT_BEYOND_MAX_CONTENT;
	if (step == 4 || step == 5) {
		target = GT_LIMIT;
		beyond = GT_BEYOND_ALL;
	}

	/* Each item's increases, the greatest taken for each track. */
	any = 0;
	for (index = 0; index < count; index++) {
		item = group[index];
		first = input->items[item].start;
		span = input->items[item].span;

		/* The tracks this step affects. */
		fit = 0;
		for (track_index = first; track_index < first + span; track_index++) {
			track = &input->tracks[track_index];
			state->affected[track_index] = 0;
			if (track->collapsed)
				continue;
			if (flexible && track->maximum != GRID_TRACK_FLEX)
				continue;
			if (step == 0 && gt_intrinsic_minimum(track->minimum))
				state->affected[track_index] = 1;
			else if (step == 1 && (track->minimum == GRID_TRACK_MIN_CONTENT || track->minimum == GRID_TRACK_MAX_CONTENT))
				state->affected[track_index] = 1;
			else if (step == 2 && track->minimum == GRID_TRACK_MAX_CONTENT)
				state->affected[track_index] = 1;
			else if (step == 6 && track->minimum == GRID_TRACK_AUTO)
				state->affected[track_index] = 1;
			else if (step == 4 && gt_intrinsic_maximum(track->maximum))
				state->affected[track_index] = 1;
			else if (step == 5 && (track->maximum == GRID_TRACK_MAX_CONTENT || track->maximum == GRID_TRACK_AUTO ||
			    track->maximum == GRID_TRACK_FIT_CONTENT))
				state->affected[track_index] = 1;
			if (state->affected[track_index])
				fit = 1;
		}
		if (!fit)
			continue;

		/* The contribution the step accommodates. */
		if (step == 0) {
			if (input->constraint != GRID_TRACK_DEFINITE)
				contribution = gt_limited(state, item, GRID_CONTRIBUTION_MIN_CONTENT);
			else
				contribution = gt_answer(state, item, GRID_CONTRIBUTION_MINIMUM);
		} else if (step == 1 || step == 4) {
			contribution = gt_answer(state, item, GRID_CONTRIBUTION_MIN_CONTENT);
		} else if (step == 6) {
			contribution = gt_limited(state, item, GRID_CONTRIBUTION_MAX_CONTENT);
		} else {
			contribution = gt_answer(state, item, GRID_CONTRIBUTION_MAX_CONTENT);
		}

		/* The increases this item incurs. */
		gt_spread(state, item, contribution, target, beyond, flexible);
		for (track_index = first; track_index < first + span; track_index++) {
			if (state->incurred[track_index] > state->planned[track_index])
				state->planned[track_index] = state->incurred[track_index];
			if (state->incurred[track_index] > 0)
				any = 1;
		}
	}

	/* Adds them in; a limit that became finite may grow without bound in the last step. */
	if (!any)
		return;
	if (step == 4) {
		for (track_index = 0; track_index < input->count; track_index++) {
			if (state->planned[track_index] > 0 && state->limit[track_index] >= GT_INFINITE)
				state->growable[track_index] = 2;
		}
	}
	gt_apply(state, target);
	if (step == 4) {
		for (track_index = 0; track_index < input->count; track_index++) {
			if (state->growable[track_index] == 2)
				state->growable[track_index] = 1;
		}
	}
	if (step == 5) {
		for (track_index = 0; track_index < input->count; track_index++)
			state->growable[track_index] = 0;
	}
}

/* Sizes the tracks to fit the items that cross flexible tracks, together, giving the space to the flexible tracks only. */
static void
gt_flexible_items(
	struct gt_state *state)
{
	const struct grid_track_input *input;
	size_t *order;
	size_t count;
	size_t item;
	int step;

	/* The items that cross a flexible track. */
	input = state->input;
	if (!state->has_flex)
		return;
	order = malloc((input->item_count + 1U) * sizeof(*order));
	if (order == NULL)
		return;
	count = 0;
	for (item = 0; item < input->item_count; item++) {
		if (gt_spans_flex(state, item))
			order[count++] = item;
	}

	/* The base-size steps only: flexible tracks have no growth limit of their own. */
	for (step = 0; step < 4; step++)
		gt_pass(state, order, count, gt_step_order[step], 1);
	free(order);
}

/* Gives a track with a growth limit that is still infinite (no item, or flexible) its base size for one. */
static void
gt_close_limits(
	struct gt_state *state)
{
	int index;

	/* Each track. */
	for (index = 0; index < state->input->count; index++) {
		if (state->limit[index] >= GT_INFINITE || state->limit[index] < state->base[index])
			state->limit[index] = state->base[index];
	}
}

/* Grows the tracks toward their growth limits with the free space. */
static void
gt_maximize(
	struct gt_state *state)
{
	const struct grid_track_input *input;
	int64_t free_space;
	int64_t sum;
	int index;

	/* The free space: the container's size less the tracks, none under a min-content constraint, endless under a max-content one. */
	input = state->input;
	sum = gt_gaps(state, 0, input->count);
	for (index = 0; index < input->count; index++)
		sum += state->base[index];
	if (input->constraint == GRID_TRACK_DEFINITE)
		free_space = (int64_t)input->available - sum;
	else if (input->constraint == GRID_TRACK_MIN_CONSTRAINT)
		free_space = 0;
	else
		free_space = GT_INFINITE;
	if (free_space <= 0)
		return;

	/* Each track may grow to its limit. */
	for (index = 0; index < input->count; index++) {
		state->room[index] = state->limit[index] - state->base[index];
		state->grow[index] = 0;
		state->active[index] = !input->tracks[index].collapsed;
	}
	gt_share(state->room, state->grow, state->active, input->count, free_space);
	for (index = 0; index < input->count; index++)
		state->base[index] += state->grow[index];
}

/*
 * Finds the size of an fr: the space to fill, less the gaps and the
 * inflexible tracks, shared by the flex factors (a sum below one counts as
 * one); tracks that the share would make smaller than their base sizes
 * become inflexible and the share is found again.
 */
static double
gt_find_fr(
	struct gt_state *state,
	int first,
	int last,
	int64_t space)
{
	const struct grid_track_spec *track;
	double leftover;
	double sum;
	double size;
	int index;
	int restart;
	int flexible;

	/* What the inflexible tracks and the gaps leave. */
	leftover = (double)space - (double)gt_gaps(state, first, last - first + 1);
	for (index = first; index <= last; index++) {
		track = &state->input->tracks[index];
		state->inflexible[index] = 0;
		if (track->collapsed)
			continue;
		if (track->maximum != GRID_TRACK_FLEX)
			leftover -= (double)state->base[index];
	}

	/* The share, found again while a flexible track would be smaller than its base size. */
	for (;;) {
		sum = 0.0;
		flexible = 0;
		for (index = first; index <= last; index++) {
			track = &state->input->tracks[index];
			if (track->collapsed || track->maximum != GRID_TRACK_FLEX || state->inflexible[index])
				continue;
			sum += (double)track->flex;
			flexible++;
		}
		if (flexible == 0)
			return 0.0;
		if (sum < 1.0)
			sum = 1.0;
		size = leftover / sum;
		if (size < 0.0)
			size = 0.0;
		restart = 0;
		for (index = first; index <= last; index++) {
			track = &state->input->tracks[index];
			if (track->collapsed || track->maximum != GRID_TRACK_FLEX || state->inflexible[index])
				continue;
			if (size * (double)track->flex < (double)state->base[index]) {
				state->inflexible[index] = 1;
				leftover -= (double)state->base[index];
				restart = 1;
			}
		}
		if (!restart)
			return size;
	}
}

/* Expands the flexible tracks to their share of the space. */
static void
gt_expand(
	struct gt_state *state)
{
	const struct grid_track_input *input;
	const struct grid_track_spec *track;
	double fraction;
	double other;
	double total;
	size_t item;
	int64_t sum;
	int64_t size;
	int index;

	/* Without flexible tracks, or under a min-content constraint, nothing grows. */
	input = state->input;
	if (!state->has_flex || input->constraint == GRID_TRACK_MIN_CONSTRAINT)
		return;

	/* The size of an fr. */
	fraction = 0.0;
	if (input->constraint == GRID_TRACK_DEFINITE) {
		sum = gt_gaps(state, 0, input->count);
		for (index = 0; index < input->count; index++)
			sum += state->base[index];
		if (input->available == sum)
			return;
		fraction = gt_find_fr(state, 0, input->count - 1, input->available);
	} else {
		/* The greatest that any flexible track or item needs. */
		for (index = 0; index < input->count; index++) {
			track = &input->tracks[index];
			if (track->collapsed || track->maximum != GRID_TRACK_FLEX)
				continue;
			other = (double)state->base[index];
			if (track->flex > 1.0f)
				other = other / (double)track->flex;
			if (other > fraction)
				fraction = other;
		}
		for (item = 0; item < input->item_count; item++) {
			if (!gt_spans_flex(state, item))
				continue;
			other = gt_find_fr(state, input->items[item].start,
			    input->items[item].start + input->items[item].span - 1,
			    gt_answer(state, item, GRID_CONTRIBUTION_MAX_CONTENT));
			if (other > fraction)
				fraction = other;
		}

		/* A size short of the container's minimum or past its maximum redoes the share for that size. */
		total = (double)gt_gaps(state, 0, input->count);
		for (index = 0; index < input->count; index++) {
			track = &input->tracks[index];
			if (track->collapsed)
				continue;
			if (track->maximum == GRID_TRACK_FLEX && fraction * (double)track->flex > (double)state->base[index])
				total += fraction * (double)track->flex;
			else
				total += (double)state->base[index];
		}
		if (input->container_minimum >= 0 && total < (double)input->container_minimum)
			fraction = gt_find_fr(state, 0, input->count - 1, input->container_minimum);
		else if (input->container_maximum >= 0 && total > (double)input->container_maximum)
			fraction = gt_find_fr(state, 0, input->count - 1, input->container_maximum);
	}

	/* Each flexible track takes its share when that is more than it has. */
	for (index = 0; index < input->count; index++) {
		track = &input->tracks[index];
		if (track->collapsed || track->maximum != GRID_TRACK_FLEX)
			continue;
		size = (int64_t)(fraction * (double)track->flex);
		if (size > state->base[index])
			state->base[index] = size;
	}
}

/* Gives the room left in the container to the tracks with an auto maximum, equally. */
static void
gt_stretch(
	struct gt_state *state)
{
	const struct grid_track_input *input;
	int64_t free_space;
	int64_t share;
	int64_t remainder;
	int count;
	int last;
	int index;

	/* Only when content distribution stretches. */
	input = state->input;
	if (!input->stretch_auto)
		return;

	/* The room left: of the definite size, or of the container's minimum when the size is the content's. */
	if (input->constraint == GRID_TRACK_DEFINITE)
		free_space = input->available;
	else if (input->container_minimum >= 0)
		free_space = input->container_minimum;
	else
		return;
	free_space -= gt_gaps(state, 0, input->count);
	for (index = 0; index < input->count; index++)
		free_space -= state->base[index];
	if (free_space <= 0)
		return;

	/* The tracks that stretch. */
	count = 0;
	last = -1;
	for (index = 0; index < input->count; index++) {
		if (input->tracks[index].collapsed || input->tracks[index].maximum != GRID_TRACK_AUTO)
			continue;
		count++;
		last = index;
	}
	if (count == 0)
		return;

	/* An equal share each; the rounding remainder goes to the last. */
	share = free_space / count;
	remainder = free_space - share * count;
	for (index = 0; index < input->count; index++) {
		if (input->tracks[index].collapsed || input->tracks[index].maximum != GRID_TRACK_AUTO)
			continue;
		state->base[index] += share;
	}
	state->base[last] += remainder;
}
