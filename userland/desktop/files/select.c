/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The selection of a tab's items (spec §13).
 *
 * Each entry carries its own selected mark, so the selection lives and
 * dies with the listing; the tab keeps the anchor a Shift+click or a
 * Shift+arrow extends from and the cursor the keyboard moves.
 */

#include "files.h"

#include <string.h>

/*
 * Clears the selection.
 */
void
fm_select_none(
	struct fm_tab *tab)
{
	size_t index;

	/* Every entry unselected. */
	for (index = 0; index < tab->listing.count; index++)
		tab->listing.entries[index].selected = 0;
}

/*
 * Selects one item alone; it becomes the cursor and the anchor.
 */
void
fm_select_only(
	struct fm_tab *tab,
	int index)
{
	/* Nothing else stays selected. */
	fm_select_none(tab);
	if (index < 0 || (size_t)index >= tab->listing.count)
		return;

	/* The item, where the keyboard and Shift start from. */
	tab->listing.entries[index].selected = 1;
	tab->cursor = index;
	tab->anchor = index;
}

/*
 * Adds an item to the selection or takes it out (Ctrl+click); it becomes
 * the cursor and the anchor.
 */
void
fm_select_toggle(
	struct fm_tab *tab,
	int index)
{
	/* An item that is not there. */
	if (index < 0 || (size_t)index >= tab->listing.count)
		return;

	/* The item's mark turns over. */
	tab->listing.entries[index].selected = !tab->listing.entries[index].selected;
	tab->cursor = index;
	tab->anchor = index;
}

/*
 * Selects the items from one to another (either order) and nothing else;
 * the second becomes the cursor, the anchor stays.
 */
void
fm_select_range(
	struct fm_tab *tab,
	int from,
	int to)
{
	int first;
	int last;
	int index;

	/* A range from nowhere starts at its end. */
	if (from < 0)
		from = to;
	if (to < 0 || (size_t)to >= tab->listing.count)
		return;
	if ((size_t)from >= tab->listing.count)
		from = to;

	/* The two ends in order. */
	first = from;
	last = to;
	if (first > last) {
		first = to;
		last = from;
	}

	/* The items between them, and only those. */
	fm_select_none(tab);
	for (index = first; index <= last; index++)
		tab->listing.entries[index].selected = 1;
	tab->cursor = to;
}

/*
 * Selects every item.
 */
void
fm_select_all(
	struct fm_tab *tab)
{
	size_t index;

	/* Every entry selected. */
	for (index = 0; index < tab->listing.count; index++)
		tab->listing.entries[index].selected = 1;
}

/*
 * Counts the selected items and adds up their sizes (folders count as
 * nothing), and returns the count.
 */
size_t
fm_select_count(
	struct fm_tab *tab,
	uint64_t *bytes)
{
	size_t index;
	size_t count;

	/* Each selected entry. */
	count = 0;
	*bytes = 0;
	for (index = 0; index < tab->listing.count; index++) {
		if (tab->listing.entries[index].selected == 0)
			continue;

		/* It counts, and a file's size adds up. */
		count++;
		if (tab->listing.entries[index].folder == 0)
			*bytes += tab->listing.entries[index].size;
	}

	/* Reports how many are selected. */
	return count;
}

/*
 * Returns the index of the first selected item, or -1.
 */
int
fm_select_first(
	struct fm_tab *tab)
{
	size_t index;

	/* The first entry marked. */
	for (index = 0; index < tab->listing.count; index++) {
		if (tab->listing.entries[index].selected != 0)
			return (int)index;
	}

	/* None is selected. */
	return -1;
}

/*
 * Returns the index of the item of a name, or -1.
 */
int
fm_select_find(
	struct fm_tab *tab,
	const char *name)
{
	size_t index;
	int match;

	/* The first entry of that name. */
	for (index = 0; index < tab->listing.count; index++) {
		match = strcmp(tab->listing.entries[index].name, name);
		if (match == 0)
			return (int)index;
	}

	/* No item has it. */
	return -1;
}
