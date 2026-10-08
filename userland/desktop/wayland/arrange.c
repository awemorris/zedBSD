/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rules of the arrangement of windows (WS181; arrange.h says what they
 * are): the slots of a layout, and which window goes to which slot.
 */

#include "arrange.h"

/* The masks of the slots taken, for up to KWL_ARRANGE_MAX slots. */
#define ARRANGE_MASKS		(1U << KWL_ARRANGE_MAX)

/* A cost no assignment reaches: no way to take that set of slots yet. */
#define ARRANGE_NONE		(-1)

static void arrange_split(int32_t start, int32_t length, unsigned parts, unsigned index, int32_t *at, int32_t *size);
static unsigned arrange_bits(unsigned mask);
static int64_t arrange_cost(const int32_t centre[2], const struct kwl_arrange_rect *slot);

/*
 * Names a layout for the log (KWL ARRANGE) and the menu's catalog.
 */
const char *
kwl_arrange_name(
	unsigned layout)
{
	/* Each layout's name. */
	switch (layout) {
	case KWL_ARRANGE_COLUMNS:
		return "columns";
	case KWL_ARRANGE_ROWS:
		return "rows";
	case KWL_ARRANGE_RIGHT_MAIN:
		return "right-main";
	case KWL_ARRANGE_LEFT_MAIN:
		return "left-main";
	case KWL_ARRANGE_TOP_MAIN:
		return "top-main";
	case KWL_ARRANGE_BOTTOM_MAIN:
		return "bottom-main";
	default:
		break;
	}

	/* The last layout is the grid. */
	return "grid";
}

/*
 * Tells how many windows a layout takes at most: four side by side,
 * stacked, or beside or under or over one, nine in a grid.  The windows past it stay where
 * they are (the 2026-10-07 user decision D7).
 */
unsigned
kwl_arrange_limit(
	unsigned layout)
{
	/* A grid takes up to three rows of three. */
	if (layout == KWL_ARRANGE_GRID)
		return KWL_ARRANGE_MAX;

	/* Succeeded: every other layout takes four. */
	return 4U;
}

/*
 * Makes a layout's slots for count windows in the work area (the docked
 * space; the slots keep KWL_ARRANGE_MARGIN from its sides and
 * KWL_ARRANGE_GAP between them): side by side across, stacked down, one
 * on the right (or left) half beside the others stacked on the other half,
 * one on the top (or bottom) half over the others side by side on the
 * other half, or a grid of rows whose last row's slots widen to fill it.  A slot is a
 * window's whole frame, its title bar included.  Returns the number of
 * slots made: count, at most the layout's limit.
 */
unsigned
kwl_arrange_slots(
	unsigned layout,
	unsigned count,
	const struct kwl_arrange_rect *area,
	struct kwl_arrange_rect *slots)
{
	struct kwl_arrange_rect inner;
	unsigned limit;
	unsigned index;
	unsigned columns;
	unsigned rows;
	unsigned row;
	unsigned in_row;
	unsigned main_side;
	unsigned other_side;

	/* As many slots as there are windows, up to the layout's limit. */
	limit = kwl_arrange_limit(layout);
	if (count > limit)
		count = limit;
	if (count == 0U)
		return 0U;

	/* The area inside the margin. */
	inner.x = area->x + KWL_ARRANGE_MARGIN;
	inner.y = area->y + KWL_ARRANGE_MARGIN;
	inner.width = area->width - 2 * KWL_ARRANGE_MARGIN;
	inner.height = area->height - 2 * KWL_ARRANGE_MARGIN;

	/* Each layout's slots. */
	switch (layout) {
	case KWL_ARRANGE_ROWS:
		/* Stacked down, each as wide as the area. */
		for (index = 0U; index < count; index++) {
			slots[index].x = inner.x;
			slots[index].width = inner.width;
			arrange_split(inner.y, inner.height, count, index, &slots[index].y, &slots[index].height);
		}
		break;
	case KWL_ARRANGE_RIGHT_MAIN:
	case KWL_ARRANGE_LEFT_MAIN:
		/* One window: the whole area. */
		if (count == 1U) {
			slots[0] = inner;
			break;
		}

		/* The main slot on its half, the others stacked on the other half. */
		main_side = 1U;
		other_side = 0U;
		if (layout == KWL_ARRANGE_LEFT_MAIN) {
			main_side = 0U;
			other_side = 1U;
		}
		slots[0].y = inner.y;
		slots[0].height = inner.height;
		arrange_split(inner.x, inner.width, 2U, main_side, &slots[0].x, &slots[0].width);
		for (index = 1U; index < count; index++) {
			arrange_split(inner.x, inner.width, 2U, other_side, &slots[index].x, &slots[index].width);
			arrange_split(inner.y, inner.height, count - 1U, index - 1U, &slots[index].y, &slots[index].height);
		}
		break;
	case KWL_ARRANGE_TOP_MAIN:
	case KWL_ARRANGE_BOTTOM_MAIN:
		/* One window: the whole area. */
		if (count == 1U) {
			slots[0] = inner;
			break;
		}

		/* The main slot on its half, the others side by side on the other half. */
		main_side = 0U;
		other_side = 1U;
		if (layout == KWL_ARRANGE_BOTTOM_MAIN) {
			main_side = 1U;
			other_side = 0U;
		}
		slots[0].x = inner.x;
		slots[0].width = inner.width;
		arrange_split(inner.y, inner.height, 2U, main_side, &slots[0].y, &slots[0].height);
		for (index = 1U; index < count; index++) {
			arrange_split(inner.y, inner.height, 2U, other_side, &slots[index].y, &slots[index].height);
			arrange_split(inner.x, inner.width, count - 1U, index - 1U, &slots[index].x, &slots[index].width);
		}
		break;
	case KWL_ARRANGE_GRID:
		/* The fewest columns whose square holds them all, and the rows they need. */
		columns = 1U;
		while (columns * columns < count)
			columns++;
		rows = (count + columns - 1U) / columns;

		/* Row by row; the last row's slots widen to fill it. */
		for (index = 0U; index < count; index++) {
			row = index / columns;
			in_row = columns;
			if (row == rows - 1U)
				in_row = count - columns * (rows - 1U);
			arrange_split(inner.x, inner.width, in_row, index % columns, &slots[index].x, &slots[index].width);
			arrange_split(inner.y, inner.height, rows, row, &slots[index].y, &slots[index].height);
		}
		break;
	default:
		/* Side by side across, each as tall as the area. */
		for (index = 0U; index < count; index++) {
			slots[index].y = inner.y;
			slots[index].height = inner.height;
			arrange_split(inner.x, inner.width, count, index, &slots[index].x, &slots[index].width);
		}
		break;
	}

	/* Succeeded: the number of slots. */
	return count;
}

/*
 * Chooses which window goes to which slot: the assignment whose sum of the
 * squared distances from each window's centre to its slot's centre is the
 * least, so that each window goes roughly where it is (the 2026-10-07
 * UAT).  The windows are given top first; among equal sums the one found
 * first is kept, so the result is always the same.  Sets order[window] to
 * its slot.
 */
void
kwl_arrange_assign(
	const int32_t centres[][2],
	unsigned count,
	const struct kwl_arrange_rect *slots,
	unsigned *order)
{
	int64_t best[ARRANGE_MASKS];
	unsigned last[ARRANGE_MASKS];
	unsigned full;
	unsigned mask;
	unsigned slot;
	unsigned window;
	int64_t cost;

	/* No window, nothing to choose. */
	if (count == 0U)
		return;
	if (count > KWL_ARRANGE_MAX)
		count = KWL_ARRANGE_MAX;

	/* Nothing reached yet but the empty set. */
	full = (1U << count) - 1U;
	for (mask = 0U; mask <= full; mask++) {
		best[mask] = ARRANGE_NONE;
		last[mask] = 0U;
	}
	best[0] = 0;

	/* The windows in order, each into a slot not taken by those before it. */
	for (mask = 0U; mask < full; mask++) {
		if (best[mask] == ARRANGE_NONE)
			continue;
		window = arrange_bits(mask);
		for (slot = 0U; slot < count; slot++) {
			/* A slot taken already. */
			if ((mask & (1U << slot)) != 0U)
				continue;

			/* The cheaper way to this set of slots is kept. */
			cost = best[mask] + arrange_cost(centres[window], &slots[slot]);
			if (best[mask | (1U << slot)] == ARRANGE_NONE || cost < best[mask | (1U << slot)]) {
				best[mask | (1U << slot)] = cost;
				last[mask | (1U << slot)] = slot;
			}
		}
	}

	/* Back from the whole set: the last window's slot, then the one before it. */
	mask = full;
	for (window = count; window > 0U; window--) {
		slot = last[mask];
		order[window - 1U] = slot;
		mask &= ~(1U << slot);
	}
}

/*
 * Places a window of limited size in the body its slot gives it
 * (ws177-p035): its size is the body's, made no smaller than its smallest
 * size and no larger than its largest (limits: the smallest width and
 * height, the largest width and height, 0 for none), in the body's
 * middle.  A window kept smaller than the body (one of a fixed size) sits
 * in the middle of it with room around it.  Returns 1 when the window
 * fits in the body, 0 when its smallest size is larger (placed then
 * reaches past the body: the window does not go in that slot).
 */
int
kwl_arrange_fit(
	const struct kwl_arrange_rect *body,
	const int32_t limits[4],
	struct kwl_arrange_rect *placed)
{
	int32_t width;
	int32_t height;
	int fits;

	/* The body's size, no larger than the window's largest size. */
	width = body->width;
	height = body->height;
	if (limits[2] > 0 && width > limits[2])
		width = limits[2];
	if (limits[3] > 0 && height > limits[3])
		height = limits[3];

	/* No smaller than its smallest size, which may not fit. */
	fits = 1;
	if (width < limits[0]) {
		width = limits[0];
		fits = 0;
	}

	/* Likewise its smallest height. */
	if (height < limits[1]) {
		height = limits[1];
		fits = 0;
	}

	/* In the body's middle. */
	placed->x = body->x + (body->width - width) / 2;
	placed->y = body->y + (body->height - height) / 2;
	placed->width = width;
	placed->height = height;

	/* Too large for the body. */
	if (!fits)
		return 0;

	/* Succeeded: the window fits. */
	return 1;
}

/*
 * Gives part index of parts equal parts of a length from start, with
 * KWL_ARRANGE_GAP between them; the last part takes what the division
 * leaves over.
 */
static void
arrange_split(
	int32_t start,
	int32_t length,
	unsigned parts,
	unsigned index,
	int32_t *at,
	int32_t *size)
{
	int32_t each;

	/* The length of a part, the gaps taken out. */
	each = (length - KWL_ARRANGE_GAP * (int32_t)(parts - 1U)) / (int32_t)parts;

	/* Where the part starts. */
	*at = start + (int32_t)index * (each + KWL_ARRANGE_GAP);

	/* Its length: the last one reaches the end. */
	*size = each;
	if (index == parts - 1U)
		*size = start + length - *at;
}

/* Counts the slots a mask takes: the number of windows placed so far. */
static unsigned
arrange_bits(
	unsigned mask)
{
	unsigned count;

	/* Each bit set. */
	count = 0U;
	while (mask != 0U) {
		count += mask & 1U;
		mask >>= 1;
	}

	/* Succeeded: the count. */
	return count;
}

/* Gives the squared distance from a window's centre to a slot's centre. */
static int64_t
arrange_cost(
	const int32_t centre[2],
	const struct kwl_arrange_rect *slot)
{
	int64_t dx;
	int64_t dy;

	/* From the window's centre to the slot's. */
	dx = (int64_t)centre[0] - ((int64_t)slot->x + slot->width / 2);
	dy = (int64_t)centre[1] - ((int64_t)slot->y + slot->height / 2);

	/* Succeeded: the squared distance. */
	return dx * dx + dy * dy;
}
