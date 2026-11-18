/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Unicode's case mappings (the full mappings of SpecialCasing first, then
 * the simple ones of UnicodeData) and the Cased and Case_Ignorable
 * properties, looked up in the generated tables by binary search.
 */

#include "base/unicode.h"

static const struct wb_case_simple *unicode_find_simple(const struct wb_case_simple *table, size_t count, uint32_t code_point);
static const struct wb_case_full *unicode_find_full(const struct wb_case_full *table, size_t count, uint32_t code_point);
static int unicode_in_ranges(const struct wb_code_range *table, size_t count, uint32_t code_point);

/*
 * Maps a code point to lower or upper case (the full mapping, context-free);
 * stores the code points and reports how many (the code point itself when
 * it has no mapping).
 */
unsigned
wb_case_map(
	uint32_t code_point,
	int upper,
	uint32_t mapped[WB_CASE_MAX])
{
	const struct wb_case_full *full;
	const struct wb_case_simple *simple;
	unsigned index;

	/* A full mapping of SpecialCasing. */
	if (upper) {
		full = unicode_find_full(wb_case_upper_full, wb_case_upper_full_count, code_point);
	} else {
		full = unicode_find_full(wb_case_lower_full, wb_case_lower_full_count, code_point);
	}

	/* Found: its code points. */
	if (full != NULL) {
		for (index = 0; index < full->count; index++)
			mapped[index] = full->points[index];
		return full->count;
	}

	/* A simple mapping. */
	if (upper) {
		simple = unicode_find_simple(wb_case_upper, wb_case_upper_count, code_point);
	} else {
		simple = unicode_find_simple(wb_case_lower, wb_case_lower_count, code_point);
	}

	/* Found: its one code point. */
	if (simple != NULL) {
		mapped[0] = simple->mapping;
		return 1;
	}

	/* No mapping: the code point itself. */
	mapped[0] = code_point;
	return 1;
}

/*
 * Tells whether a code point is cased (a letter with case).
 */
int
wb_case_is_cased(
	uint32_t code_point)
{
	int found;

	/* In the ranges. */
	found = unicode_in_ranges(wb_case_cased, wb_case_cased_count, code_point);
	if (found)
		return 1;

	/* Not cased. */
	return 0;
}

/*
 * Tells whether a code point is case-ignorable (skipped by the final
 * sigma rule's look around).
 */
int
wb_case_is_ignorable(
	uint32_t code_point)
{
	int found;

	/* In the ranges. */
	found = unicode_in_ranges(wb_case_ignorable, wb_case_ignorable_count, code_point);
	if (found)
		return 1;

	/* Not ignorable. */
	return 0;
}

/* Finds a code point's simple mapping in a sorted table. */
static const struct wb_case_simple *
unicode_find_simple(
	const struct wb_case_simple *table,
	size_t count,
	uint32_t code_point)
{
	size_t low;
	size_t high;
	size_t middle;

	/* Halving the range that could hold it. */
	low = 0;
	high = count;
	while (low < high) {
		middle = low + (high - low) / 2U;
		if (table[middle].code_point == code_point)
			return &table[middle];
		if (table[middle].code_point < code_point) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}

	/* Not in the table. */
	return NULL;
}

/* Finds a code point's full mapping in a sorted table. */
static const struct wb_case_full *
unicode_find_full(
	const struct wb_case_full *table,
	size_t count,
	uint32_t code_point)
{
	size_t low;
	size_t high;
	size_t middle;

	/* Halving the range that could hold it. */
	low = 0;
	high = count;
	while (low < high) {
		middle = low + (high - low) / 2U;
		if (table[middle].code_point == code_point)
			return &table[middle];
		if (table[middle].code_point < code_point) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}

	/* Not in the table. */
	return NULL;
}

/* Tells whether a code point is in one of a sorted table's ranges. */
static int
unicode_in_ranges(
	const struct wb_code_range *table,
	size_t count,
	uint32_t code_point)
{
	size_t low;
	size_t high;
	size_t middle;

	/* Halving the range of ranges that could hold it. */
	low = 0;
	high = count;
	while (low < high) {
		middle = low + (high - low) / 2U;
		if (code_point < table[middle].first) {
			high = middle;
		} else if (code_point > table[middle].last) {
			low = middle + 1U;
		} else {
			return 1;
		}
	}

	/* In none. */
	return 0;
}
