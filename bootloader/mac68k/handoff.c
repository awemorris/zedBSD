/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The handoff plan: how the trampoline places the kernel with translation
 * off.
 *
 * The kernel runs at physical addresses where Mac OS keeps its own
 * memory, and the loader's buffers lie wherever the ROM's heap put them,
 * at logical addresses the ROM maps to physical pages of its choice.  The
 * plan lists, in physical addresses, every step the trampoline takes once
 * translation is off: first each source page that lies where the kernel
 * or its boot information goes is moved to a spare page that nothing
 * else uses, then every range is copied (or zeroed) to its place.  No
 * step then writes over a source a later step reads, nor over the area
 * that holds the trampoline and the plan.
 */

#include "bootloader/mac68k/stage2.h"

/* The offset inside a page, and the page of an address. */
#define PAGE_OFFSET_MASK	(MAC68K_PAGE_SIZE - 1U)

/*
 * Where the search for spare pages stands: it walks the RAM from the top
 * down, one page at a time, and never offers a page twice.
 */
struct spare_cursor {
	const struct mac68k_memory *memory;
	unsigned bank;
	uint32_t next;
	int done;
};

static int overlaps(uint32_t first, uint32_t first_length, uint32_t second, uint32_t second_length);
static int hits_targets(const struct mac68k_load_range *ranges, unsigned count, uint32_t base, uint32_t length);
static const char *split_ranges(struct mac68k_piece *pieces, uint32_t capacity, uint32_t *used, const struct mac68k_load_range *ranges, unsigned count, mac68k_translate_fn translate, void *cookie);
static void spare_start(struct spare_cursor *cursor, const struct mac68k_memory *memory);
static int spare_next(struct spare_cursor *cursor, uint32_t *page);
static int page_is_free(uint32_t page, const struct mac68k_piece *pieces, uint32_t copy_count, const struct mac68k_load_range *ranges, unsigned range_count, uint32_t area_base, uint32_t area_size);
static void reverse_pieces(struct mac68k_piece *pieces, uint32_t first, uint32_t last);

/*
 * Lists what the handoff writes for a kernel whose file is at a logical
 * address, with its boot information: each segment's file bytes, zeroes from
 * the end of its file bytes to the end of that page (or of the segment),
 * and the boot information right after the image.  The rest of each
 * segment is the kernel's BSS, which the kernel clears itself.
 */
unsigned
mac68k_handoff_ranges(
	struct mac68k_load_range *ranges,
	const struct mac68k_kernel_image *image,
	uint32_t kernel,
	uint32_t bootinfo,
	uint32_t bootinfo_size)
{
	const struct mac68k_segment *segment;
	struct mac68k_load_range *range;
	uint32_t file_end;
	uint32_t zero_end;
	uint32_t segment_end;
	unsigned index;
	unsigned count;

	/* Each segment's bytes and the zeroes after them. */
	count = 0;
	for (index = 0; index < image->segment_count; index++) {
		segment = &image->segments[index];
		if (segment->file_size != 0U) {
			range = &ranges[count];
			range->source = kernel + segment->file_offset;
			range->target = segment->physical;
			range->length = segment->file_size;
			range->flags = 0;
			count++;
		}

		/* The zeroes up to the end of the page, inside the segment. */
		file_end = segment->physical + segment->file_size;
		segment_end = segment->physical + segment->memory_size;
		zero_end = (file_end + MAC68K_PAGE_SIZE - 1U) & ~(MAC68K_PAGE_SIZE - 1U);
		if (zero_end > segment_end)
			zero_end = segment_end;
		if (zero_end > file_end) {
			range = &ranges[count];
			range->source = 0;
			range->target = file_end;
			range->length = zero_end - file_end;
			range->flags = MAC68K_PIECE_ZERO;
			count++;
		}
	}

	/* The boot information right after the image, on a 4-byte boundary. */
	range = &ranges[count];
	range->source = bootinfo;
	range->target = (image->end + 3U) & ~3U;
	range->length = bootinfo_size;
	range->flags = 0;
	count++;

	/* Succeeded: the number of ranges. */
	return count;
}

/*
 * Reports how many steps a plan for the ranges can need at most: every
 * copy may be cut at each source page and may need a move first.
 */
uint32_t
mac68k_handoff_pieces_max(
	const struct mac68k_load_range *ranges,
	unsigned count)
{
	uint32_t total;
	unsigned index;

	/* A zero fill is one step; a copy is a step per source page it touches. */
	total = 0;
	for (index = 0; index < count; index++) {
		if ((ranges[index].flags & MAC68K_PIECE_ZERO) != 0U) {
			total += 1U;
		} else {
			total += ranges[index].length / MAC68K_PAGE_SIZE + 2U;
		}
	}

	/* Succeeded: a move may precede every step. */
	return total * 2U;
}

/*
 * Writes the plan's steps for the ranges into pieces.  The area (the
 * trampoline and the plan, at its physical address) and the spare pages
 * must lie outside every target.  Reports NULL, or why no safe plan was
 * found.
 */
const char *
mac68k_handoff_plan(
	struct mac68k_plan *plan,
	struct mac68k_piece *pieces,
	uint32_t capacity,
	const struct mac68k_load_range *ranges,
	unsigned range_count,
	mac68k_translate_fn translate,
	void *cookie,
	const struct mac68k_memory *memory,
	uint32_t area_base,
	uint32_t area_size)
{
	struct spare_cursor cursor;
	struct mac68k_piece *piece;
	struct mac68k_piece *move;
	const char *problem;
	uint32_t copy_count;
	uint32_t count;
	uint32_t index;
	uint32_t page;
	unsigned range;
	int found;
	int is_free;
	int held;
	int hit;

	/* Refuses a target that is not RAM, or that covers the area. */
	for (range = 0; range < range_count; range++) {
		held = mac68k_memory_holds(memory, ranges[range].target, ranges[range].length);
		if (!held)
			return "the kernel does not fit in one bank of RAM";
		hit = overlaps(ranges[range].target, ranges[range].length, area_base, area_size);
		if (hit)
			return "the handoff area lies where the kernel goes";
	}

	/* Cuts the ranges into steps whose sources each lie in one page. */
	copy_count = 0;
	problem = split_ranges(pieces, capacity, &copy_count, ranges, range_count, translate, cookie);
	if (problem != NULL)
		return problem;

	/* Moves every source that lies where a range goes to a spare page first. */
	count = copy_count;
	spare_start(&cursor, memory);
	for (index = 0; index < copy_count; index++) {
		piece = &pieces[index];
		if ((piece->flags & MAC68K_PIECE_ZERO) != 0U)
			continue;
		hit = hits_targets(ranges, range_count, piece->source, piece->length);
		if (!hit)
			continue;

		/* Finds a page that no target, source, move or the area uses. */
		found = 0;
		is_free = 0;
		while (!is_free) {
			found = spare_next(&cursor, &page);
			if (!found)
				break;
			is_free = page_is_free(page, pieces, copy_count, ranges, range_count, area_base, area_size);
		}
		if (!found)
			return "no spare page is left for the handoff";

		/* Refuses a plan longer than its buffer. */
		if (count >= capacity)
			return "the handoff plan does not fit its buffer";

		/* The move to the spare page, keeping the offset in the page. */
		move = &pieces[count];
		move->source = piece->source;
		move->target = page + (piece->source & PAGE_OFFSET_MASK);
		move->length = piece->length;
		move->flags = 0;
		count++;

		/* The copy now reads the moved bytes. */
		piece->source = move->target;
	}

	/* Puts the moves before the copies: rotates the two runs of steps. */
	if (count > copy_count) {
		reverse_pieces(pieces, 0, copy_count);
		reverse_pieces(pieces, copy_count, count);
		reverse_pieces(pieces, 0, count);
	}

	/* Succeeded: the steps, moves first. */
	plan->count = count;
	return NULL;
}

/* Tells whether two ranges share a byte, without overflowing at the top of memory. */
static int
overlaps(
	uint32_t first,
	uint32_t first_length,
	uint32_t second,
	uint32_t second_length)
{
	/* Empty ranges share nothing. */
	if (first_length == 0U || second_length == 0U)
		return 0;

	/* The second starts inside the first, or the first inside the second. */
	if (second >= first && second - first < first_length)
		return 1;
	if (first >= second && first - second < second_length)
		return 1;

	/* Apart: neither starts inside the other. */
	return 0;
}

/* Tells whether a physical range shares a byte with any range's target. */
static int
hits_targets(
	const struct mac68k_load_range *ranges,
	unsigned count,
	uint32_t base,
	uint32_t length)
{
	unsigned index;
	int hit;

	/* Looks at every target. */
	for (index = 0; index < count; index++) {
		hit = overlaps(base, length, ranges[index].target, ranges[index].length);
		if (hit)
			return 1;
	}

	/* No target is hit. */
	return 0;
}

/*
 * Cuts the ranges into steps: a zero fill stays whole, a copy is cut
 * where its source crosses a page, and each source is translated.
 */
static const char *
split_ranges(
	struct mac68k_piece *pieces,
	uint32_t capacity,
	uint32_t *used,
	const struct mac68k_load_range *ranges,
	unsigned count,
	mac68k_translate_fn translate,
	void *cookie)
{
	const struct mac68k_load_range *range;
	struct mac68k_piece *piece;
	uint32_t physical;
	uint32_t source;
	uint32_t target;
	uint32_t left;
	uint32_t step;
	unsigned index;
	int error;

	/* Takes every range in turn. */
	for (index = 0; index < count; index++) {
		range = &ranges[index];

		/* A zero fill is one step. */
		if ((range->flags & MAC68K_PIECE_ZERO) != 0U) {
			if (*used >= capacity)
				return "the handoff plan does not fit its buffer";
			piece = &pieces[*used];
			piece->source = 0;
			piece->target = range->target;
			piece->length = range->length;
			piece->flags = MAC68K_PIECE_ZERO;
			*used += 1U;
			continue;
		}

		/* A copy is cut at the source's page boundaries. */
		source = range->source;
		target = range->target;
		left = range->length;
		while (left > 0U) {
			/* The bytes up to the end of the source's page. */
			step = MAC68K_PAGE_SIZE - (source & PAGE_OFFSET_MASK);
			if (step > left)
				step = left;

			/* Finds where the source's page is in RAM. */
			error = translate(cookie, source, &physical);
			if (error != 0)
				return "a loader buffer is not mapped to RAM";

			/* Records the step. */
			if (*used >= capacity)
				return "the handoff plan does not fit its buffer";
			piece = &pieces[*used];
			piece->source = physical;
			piece->target = target;
			piece->length = step;
			piece->flags = 0;
			*used += 1U;

			/* Moves on past the step. */
			source += step;
			target += step;
			left -= step;
		}
	}

	/* Succeeded: every range is cut into steps. */
	return NULL;
}

/* Starts the search for spare pages at the top page of the highest bank. */
static void
spare_start(
	struct spare_cursor *cursor,
	const struct mac68k_memory *memory)
{
	/* No bank means no spare page. */
	cursor->memory = memory;
	cursor->done = 0;
	if (memory->count == 0U) {
		cursor->done = 1;
		return;
	}

	/* The highest bank's last whole page. */
	cursor->bank = memory->count - 1U;
	cursor->next = (memory->banks[cursor->bank].base + memory->banks[cursor->bank].size - MAC68K_PAGE_SIZE) & ~PAGE_OFFSET_MASK;
}

/* Offers the next page down, moving to the bank below at a bank's start. */
static int
spare_next(
	struct spare_cursor *cursor,
	uint32_t *page)
{
	const struct mac68k_bank *bank;

	/* The search is over once every bank is walked. */
	if (cursor->done)
		return 0;

	/* Offers the page, then steps down. */
	*page = cursor->next;
	bank = &cursor->memory->banks[cursor->bank];
	if (cursor->next >= bank->base + MAC68K_PAGE_SIZE) {
		cursor->next -= MAC68K_PAGE_SIZE;
		return 1;
	}

	/* Steps to the bank below, or ends the search. */
	if (cursor->bank == 0U) {
		cursor->done = 1;
		return 1;
	}
	cursor->bank--;
	bank = &cursor->memory->banks[cursor->bank];
	cursor->next = (bank->base + bank->size - MAC68K_PAGE_SIZE) & ~PAGE_OFFSET_MASK;

	/* Succeeded: the page offered. */
	return 1;
}

/*
 * Tells whether a page is free for a move: outside every target and the
 * area, and not the page of any copy's source (which is also where every
 * move made so far went).
 */
static int
page_is_free(
	uint32_t page,
	const struct mac68k_piece *pieces,
	uint32_t copy_count,
	const struct mac68k_load_range *ranges,
	unsigned range_count,
	uint32_t area_base,
	uint32_t area_size)
{
	uint32_t index;
	int hit;

	/* A page a range is placed on is not spare. */
	hit = hits_targets(ranges, range_count, page, MAC68K_PAGE_SIZE);
	if (hit)
		return 0;

	/* The trampoline's and the plan's area is not spare. */
	hit = overlaps(page, MAC68K_PAGE_SIZE, area_base, area_size);
	if (hit)
		return 0;

	/* A page a copy still reads from is not spare. */
	for (index = 0; index < copy_count; index++) {
		if ((pieces[index].flags & MAC68K_PIECE_ZERO) != 0U)
			continue;
		if ((pieces[index].source & ~PAGE_OFFSET_MASK) == page)
			return 0;
	}

	/* Succeeded: nothing uses the page. */
	return 1;
}

/* Reverses the steps from first up to (not including) last. */
static void
reverse_pieces(
	struct mac68k_piece *pieces,
	uint32_t first,
	uint32_t last)
{
	struct mac68k_piece swap;

	/* Swaps the ends inwards. */
	while (first + 1U < last) {
		swap = pieces[first];
		pieces[first] = pieces[last - 1U];
		pieces[last - 1U] = swap;
		first++;
		last--;
	}
}
