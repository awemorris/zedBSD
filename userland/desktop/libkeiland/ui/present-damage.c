/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The stale parts of a swapchain's image (ws210-p013 E2, see
 * present-damage.h).
 */

#include "present-damage.h"

#include <string.h>

/* The most rectangles while one is added: those kept and the new one. */
#define STALE_WORK	(KEIUI_STALE_RECTS + 1U)

static uint64_t stale_area(const struct kl_rect *rect);
static void stale_union(const struct kl_rect *a, const struct kl_rect *b, struct kl_rect *box);
static int stale_overlap(const struct kl_rect *a, const struct kl_rect *b);
static int stale_near(const struct kl_rect *a, const struct kl_rect *b);
static void stale_merge(struct kl_rect *rects, unsigned *count, unsigned first, unsigned second);
static void stale_settle(struct kl_rect *rects, unsigned *count);
static void stale_squeeze(struct kl_rect *rects, unsigned *count);

/*
 * Marks an image's contents as not known (a new image, or a frame that
 * failed after it was written): the next frame writes all of it.
 */
void
keiui_stale_unknown(
	struct keiui_stale *stale)
{
	/* The whole frame, and no rectangle. */
	stale->whole = 1;
	stale->count = 0U;
}

/*
 * Marks an image as holding the last frame (a frame was written into it).
 */
void
keiui_stale_clear(
	struct keiui_stale *stale)
{
	/* Nothing stale. */
	stale->whole = 0;
	stale->count = 0U;
}

/*
 * Adds a part of the frame (width by height pixels) that changed to an
 * image's stale parts: clipped to the frame, its edges rounded out to
 * KEIUI_STALE_ALIGN columns, merged with a rectangle it overlaps or lies
 * near (their box at most half again their areas), and, beyond
 * KEIUI_STALE_RECTS rectangles, the two whose box grows least merged.
 */
void
keiui_stale_add(
	struct keiui_stale *stale,
	uint32_t width,
	uint32_t height,
	const struct kl_rect *part)
{
	struct kl_rect rects[STALE_WORK];
	struct kl_rect rect;
	unsigned count;
	int right;
	int bottom;

	/* An image written whole next takes the part already. */
	if (stale->whole)
		return;

	/* The part's edges inside the frame. */
	rect = *part;
	right = rect.x + rect.width;
	bottom = rect.y + rect.height;
	if (rect.x < 0)
		rect.x = 0;
	if (rect.y < 0)
		rect.y = 0;
	if (right > (int)width)
		right = (int)width;
	if (bottom > (int)height)
		bottom = (int)height;

	/* Nothing of it in the frame. */
	if (right <= rect.x || bottom <= rect.y)
		return;

	/* The left edge rounded down and the right edge up to the columns, within the frame. */
	rect.x -= rect.x % KEIUI_STALE_ALIGN;
	right += (KEIUI_STALE_ALIGN - right % KEIUI_STALE_ALIGN) % KEIUI_STALE_ALIGN;
	if (right > (int)width)
		right = (int)width;
	rect.width = right - rect.x;
	rect.height = bottom - rect.y;

	/* The rectangles kept and the new one, merged until they are few and apart. */
	memcpy(rects, stale->rects, stale->count * sizeof(rects[0]));
	count = stale->count;
	rects[count] = rect;
	count++;
	stale_settle(rects, &count);
	stale_squeeze(rects, &count);

	/* Kept. */
	memcpy(stale->rects, rects, count * sizeof(rects[0]));
	stale->count = count;
}

/*
 * Lists the regions a frame writes into an image (frame of width by
 * height pixels): the whole frame when its contents are not known or the
 * stale parts cover more than KEIUI_STALE_WHOLE_PERCENT of it (one region
 * of the whole frame takes the GPU's own copy on the Raspberry Pi 4), else
 * the stale parts.  Returns how many (0: nothing to write).
 */
unsigned
keiui_stale_regions(
	const struct keiui_stale *stale,
	uint32_t width,
	uint32_t height,
	struct kl_rect *regions)
{
	uint64_t frame;
	uint64_t sum;
	unsigned index;

	/* The stale parts' area. */
	sum = 0U;
	for (index = 0U; index < stale->count; index++)
		sum += stale_area(&stale->rects[index]);

	/* The whole frame, for an image not known or a large stale area. */
	frame = (uint64_t)width * (uint64_t)height;
	if (stale->whole || sum * 100U > frame * KEIUI_STALE_WHOLE_PERCENT) {
		regions[0].x = 0;
		regions[0].y = 0;
		regions[0].width = (int)width;
		regions[0].height = (int)height;
		return 1U;
	}

	/* Otherwise the stale parts. */
	memcpy(regions, stale->rects, stale->count * sizeof(regions[0]));
	return stale->count;
}

/* Reports a rectangle's area in pixels. */
static uint64_t
stale_area(
	const struct kl_rect *rect)
{
	/* Width by height. */
	return (uint64_t)rect->width * (uint64_t)rect->height;
}

/* Finds the box of two rectangles. */
static void
stale_union(
	const struct kl_rect *a,
	const struct kl_rect *b,
	struct kl_rect *box)
{
	int right;
	int bottom;

	/* The far edges. */
	right = a->x + a->width;
	if (b->x + b->width > right)
		right = b->x + b->width;
	bottom = a->y + a->height;
	if (b->y + b->height > bottom)
		bottom = b->y + b->height;

	/* The near edges, and the box between. */
	box->x = a->x;
	if (b->x < box->x)
		box->x = b->x;
	box->y = a->y;
	if (b->y < box->y)
		box->y = b->y;
	box->width = right - box->x;
	box->height = bottom - box->y;
}

/* Tells whether two rectangles share a pixel (1) or not (0). */
static int
stale_overlap(
	const struct kl_rect *a,
	const struct kl_rect *b)
{
	/* Apart across. */
	if (a->x + a->width <= b->x || b->x + b->width <= a->x)
		return 0;

	/* Apart up and down. */
	if (a->y + a->height <= b->y || b->y + b->height <= a->y)
		return 0;

	/* They overlap. */
	return 1;
}

/* Tells whether two rectangles overlap or lie near: their box at most half again their areas (1), or not (0). */
static int
stale_near(
	const struct kl_rect *a,
	const struct kl_rect *b)
{
	struct kl_rect box;
	int overlap;

	/* Overlapping ones are merged. */
	overlap = stale_overlap(a, b);
	if (overlap)
		return 1;

	/* So are those whose box adds little. */
	stale_union(a, b, &box);
	if (2U * stale_area(&box) <= 3U * (stale_area(a) + stale_area(b)))
		return 1;

	/* Apart. */
	return 0;
}

/* Merges the second of two rectangles of a list into the first, the last taking the second's place. */
static void
stale_merge(
	struct kl_rect *rects,
	unsigned *count,
	unsigned first,
	unsigned second)
{
	struct kl_rect box;

	/* The box in the first's place. */
	stale_union(&rects[first], &rects[second], &box);
	rects[first] = box;

	/* The last in the second's place. */
	rects[second] = rects[*count - 1U];
	(*count)--;
}

/* Merges the rectangles of a list that overlap or lie near, until none do. */
static void
stale_settle(
	struct kl_rect *rects,
	unsigned *count)
{
	unsigned first;
	unsigned second;
	int merged;
	int near;

	/* Each pass merges one pair, until a pass finds none. */
	merged = 1;
	while (merged) {
		merged = 0;
		for (first = 0U; first < *count && !merged; first++) {
			/* The first against each after it. */
			for (second = first + 1U; second < *count; second++) {
				near = stale_near(&rects[first], &rects[second]);
				if (near) {
					stale_merge(rects, count, first, second);
					merged = 1;
					break;
				}
			}
		}
	}
}

/* Merges, while the list holds more than KEIUI_STALE_RECTS rectangles, the two whose box grows least, then any that then overlap. */
static void
stale_squeeze(
	struct kl_rect *rects,
	unsigned *count)
{
	struct kl_rect box;
	uint64_t growth;
	uint64_t least;
	unsigned first;
	unsigned second;
	unsigned best_first;
	unsigned best_second;

	/* Until few enough. */
	while (*count > KEIUI_STALE_RECTS) {
		/* The pair whose box grows least over their areas. */
		least = UINT64_MAX;
		best_first = 0U;
		best_second = 1U;
		for (first = 0U; first < *count; first++) {
			for (second = first + 1U; second < *count; second++) {
				stale_union(&rects[first], &rects[second], &box);
				growth = stale_area(&box) - stale_area(&rects[first]) - stale_area(&rects[second]);
				if (growth < least) {
					least = growth;
					best_first = first;
					best_second = second;
				}
			}
		}

		/* Merged, and the box apart from the others again. */
		stale_merge(rects, count, best_first, best_second);
		stale_settle(rects, count);
	}
}
