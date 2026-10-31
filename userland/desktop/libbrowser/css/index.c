/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rule index of a style sheet (ws074-p068): every selector of the
 * sheet filed under the most telling simple selector of its rightmost
 * compound (an id, else a class, else a type), so that an element is
 * matched only against the selectors that could match it.  A sheet of a
 * megabyte has tens of thousands of selectors; an element with an id, a
 * few classes and a tag reaches a few dozen of them.
 *
 * The keys are atoms of the heap, compared by address.  The index lives
 * in the sheet's arena and is built once, when the sheet is parsed.
 */

#include "css/internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The kinds of key a selector is filed under, in the order the build sorts them. */
enum index_kind {
	INDEX_KIND_ID,
	INDEX_KIND_CLASS,
	INDEX_KIND_TAG,
	INDEX_KIND_UNIVERSAL,
	INDEX_KIND_NONE
};

/*
 * One selector on its way into the index: the kind and key it is filed
 * under and the rule and selector it names.
 */
struct index_item {
	int kind;
	struct vm_string *key;
	uint32_t rule;
	uint32_t selector;
};

static int index_kind_of(const struct css_selector *selector, struct vm_string **key);
static int index_item_compare(const void *left, const void *right);
static int index_table_make(struct wb_arena *arena, struct css_index_table *table, size_t keys);
static void index_table_insert(struct css_index_table *table, struct vm_string *key, uint32_t start, uint32_t count);
static size_t index_slot(const struct css_index_table *table, const struct vm_string *key);
static size_t index_count_keys(const struct index_item *items, size_t count, int kind);

/*
 * Builds the rule index of a parsed sheet in its arena.
 */
int
css_index_build(
	struct css_sheet *sheet)
{
	struct css_rule_index *index;
	struct index_item *items;
	struct index_item item;
	struct wb_vector list;
	const struct css_rule *rule;
	size_t rule_number;
	size_t selector_number;
	size_t start;
	size_t position;
	size_t id_keys;
	size_t class_keys;
	size_t tag_keys;
	int error;

	/* Files every selector of every rule under its key. */
	index = &sheet->index;
	memset(index, 0, sizeof(*index));
	wb_vector_init(&list, sizeof(struct index_item));
	for (rule_number = 0; rule_number < sheet->rule_count; rule_number++) {
		rule = &sheet->rules[rule_number];
		for (selector_number = 0; selector_number < rule->selector_count; selector_number++) {
			item.kind = index_kind_of(&rule->selectors[selector_number], &item.key);
			item.rule = (uint32_t)rule_number;
			item.selector = (uint32_t)selector_number;

			/* A selector that can never match is left out. */
			if (item.kind == INDEX_KIND_NONE)
				continue;

			/* Keeps the item. */
			error = wb_vector_push(&list, &item);
			if (error != 0) {
				wb_vector_release(&list);
				return ENOMEM;
			}
		}
	}

	/* An empty sheet has an empty index. */
	if (list.count == 0) {
		wb_vector_release(&list);
		return 0;
	}

	/* Groups the items by kind and key, each group in rule order. */
	items = list.items;
	qsort(items, list.count, sizeof(*items), index_item_compare);

	/* The entries, in the grouped order. */
	index->entries = wb_arena_alloc(&sheet->arena, list.count * sizeof(struct css_index_entry));
	if (index->entries == NULL) {
		wb_vector_release(&list);
		return ENOMEM;
	}

	/* Copies the rule and selector of each item. */
	for (position = 0; position < list.count; position++) {
		index->entries[position].rule = items[position].rule;
		index->entries[position].selector = items[position].selector;
	}

	/* The index holds every item. */
	index->entry_count = list.count;

	/* Counts the distinct keys of each kind. */
	id_keys = index_count_keys(items, list.count, INDEX_KIND_ID);
	class_keys = index_count_keys(items, list.count, INDEX_KIND_CLASS);
	tag_keys = index_count_keys(items, list.count, INDEX_KIND_TAG);

	/* The three tables, sized for their keys. */
	error = index_table_make(&sheet->arena, &index->ids, id_keys);
	if (error == 0)
		error = index_table_make(&sheet->arena, &index->classes, class_keys);
	if (error == 0)
		error = index_table_make(&sheet->arena, &index->tags, tag_keys);
	if (error != 0) {
		wb_vector_release(&list);
		return error;
	}

	/* Files each group of entries under its key; the universal ones are one run. */
	start = 0;
	while (start < list.count) {
		position = start;
		while (position < list.count &&
		    items[position].kind == items[start].kind &&
		    items[position].key == items[start].key)
			position++;

		/* The group's kind picks the table it goes into. */
		switch (items[start].kind) {
		case INDEX_KIND_ID:
			index_table_insert(&index->ids, items[start].key, (uint32_t)start, (uint32_t)(position - start));
			break;
		case INDEX_KIND_CLASS:
			index_table_insert(&index->classes, items[start].key, (uint32_t)start, (uint32_t)(position - start));
			break;
		case INDEX_KIND_TAG:
			index_table_insert(&index->tags, items[start].key, (uint32_t)start, (uint32_t)(position - start));
			break;
		default:
			index->universal_start = (uint32_t)start;
			index->universal_count = (uint32_t)(position - start);
			break;
		}

		/* On to the next group. */
		start = position;
	}

	/* The items are no longer needed. */
	wb_vector_release(&list);

	/* Succeeded: the sheet's selectors are indexed. */
	return 0;
}

/*
 * Finds the run of entries filed under a key in one of the index's
 * tables; NULL when no selector is filed under it.
 */
const struct css_index_bucket *
css_index_find(
	const struct css_index_table *table,
	const struct vm_string *key)
{
	const struct css_index_bucket *bucket;
	size_t slot;

	/* A table without slots holds nothing, and neither does a missing key. */
	if (table->capacity == 0)
		return NULL;
	if (key == NULL)
		return NULL;

	/* The slot the key probes to: the key's own, or the empty one where probing stopped. */
	slot = index_slot(table, key);
	bucket = &table->slots[slot];
	if (bucket->key == NULL)
		return NULL;

	/* Succeeded: the key's run of entries. */
	return bucket;
}

/*
 * Picks the kind and key a selector is filed under from its rightmost
 * compound: its id, else its first class, else its type; a compound of
 * other simple selectors is universal, and one with a pseudo-element
 * never matches an element.
 */
static int
index_kind_of(
	const struct css_selector *selector,
	struct vm_string **key)
{
	const struct css_compound *compound;
	struct vm_string *class_name;
	struct vm_string *type_name;
	size_t position;

	/* A selector without compounds matches nothing. */
	*key = NULL;
	if (selector->count == 0)
		return INDEX_KIND_NONE;

	/* A pseudo-element that makes no box never matches (::before and ::after are filed as their compound says). */
	compound = &selector->compounds[selector->count - 1U];
	if (compound->pseudo_element == CSS_PSEUDO_ELEMENT_OTHER)
		return INDEX_KIND_NONE;

	/* The subject of ::part() and ::slotted() is not the element the other simple selectors name. */
	for (position = 0; position < compound->count; position++) {
		if (compound->simples[position].kind == CSS_SIMPLE_PART || compound->simples[position].kind == CSS_SIMPLE_SLOTTED)
			return INDEX_KIND_UNIVERSAL;
	}

	/* Looks at every simple selector of the rightmost compound. */
	class_name = NULL;
	type_name = NULL;
	for (position = 0; position < compound->count; position++) {
		switch (compound->simples[position].kind) {
		case CSS_SIMPLE_ID:
			/* An id is the most telling key of all. */
			*key = compound->simples[position].name;
			return INDEX_KIND_ID;
		case CSS_SIMPLE_CLASS:
			if (class_name == NULL)
				class_name = compound->simples[position].name;
			break;
		case CSS_SIMPLE_TYPE:
			type_name = compound->simples[position].name;
			break;
		case CSS_SIMPLE_NEVER:
			/* A pseudo-element's compound never matches an element. */
			return INDEX_KIND_NONE;
		case CSS_SIMPLE_PART:
		case CSS_SIMPLE_SLOTTED:
			/* The subject is not the element the other simple selectors name. */
			return INDEX_KIND_UNIVERSAL;
		default:
			break;
		}
	}

	/* A class, then a type. */
	if (class_name != NULL) {
		*key = class_name;
		return INDEX_KIND_CLASS;
	}

	/* The type, when there is one. */
	if (type_name != NULL) {
		*key = type_name;
		return INDEX_KIND_TAG;
	}

	/* Any element may match. */
	return INDEX_KIND_UNIVERSAL;
}

/* Orders two items by kind, key, rule and selector. */
static int
index_item_compare(
	const void *left,
	const void *right)
{
	const struct index_item *a;
	const struct index_item *b;
	uintptr_t a_key;
	uintptr_t b_key;

	/* Compares field by field: a smaller kind, key address, rule or selector comes first. */
	a = left;
	b = right;
	a_key = (uintptr_t)a->key;
	b_key = (uintptr_t)b->key;
	if (a->kind < b->kind)
		return -1;
	if (a->kind > b->kind)
		return 1;
	if (a_key < b_key)
		return -1;
	if (a_key > b_key)
		return 1;
	if (a->rule < b->rule)
		return -1;
	if (a->rule > b->rule)
		return 1;
	if (a->selector < b->selector)
		return -1;
	if (a->selector > b->selector)
		return 1;

	/* The same selector. */
	return 0;
}

/* Allocates a table with room for keys at most half full. */
static int
index_table_make(
	struct wb_arena *arena,
	struct css_index_table *table,
	size_t keys)
{
	size_t capacity;

	/* No key needs no slots. */
	table->slots = NULL;
	table->capacity = 0;
	if (keys == 0)
		return 0;

	/* The smallest power of two at least twice the keys. */
	capacity = 8;
	while (capacity < keys * 2U)
		capacity *= 2U;

	/* The empty slots. */
	table->slots = wb_arena_zalloc(arena, capacity * sizeof(struct css_index_bucket));
	if (table->slots == NULL)
		return ENOMEM;

	/* Succeeded: the table is empty. */
	table->capacity = capacity;
	return 0;
}

/* Files a run of entries under a key (the table has room: it was sized for its keys). */
static void
index_table_insert(
	struct css_index_table *table,
	struct vm_string *key,
	uint32_t start,
	uint32_t count)
{
	struct css_index_bucket *bucket;
	size_t slot;

	/* The key's empty slot. */
	slot = index_slot(table, key);
	bucket = &table->slots[slot];

	/* The run. */
	bucket->key = key;
	bucket->start = start;
	bucket->count = count;
}

/* Probes a table for a key: its slot, or the empty slot where the key would go. */
static size_t
index_slot(
	const struct css_index_table *table,
	const struct vm_string *key)
{
	uint64_t hash;
	size_t mask;
	size_t slot;

	/* The atom's address, mixed, picks the first slot; probing goes on linearly. */
	hash = (uint64_t)(uintptr_t)key;
	hash = (hash >> 4) * 0x9e3779b97f4a7c15ULL;
	mask = table->capacity - 1U;
	slot = (size_t)(hash >> 32) & mask;
	while (table->slots[slot].key != NULL && table->slots[slot].key != key)
		slot = (slot + 1U) & mask;

	/* Reports the slot. */
	return slot;
}

/* Counts the distinct keys of one kind among the sorted items. */
static size_t
index_count_keys(
	const struct index_item *items,
	size_t count,
	int kind)
{
	size_t keys;
	size_t position;

	/* Each item of the kind whose key differs from the one before starts a key. */
	keys = 0;
	for (position = 0; position < count; position++) {
		if (items[position].kind != kind)
			continue;
		if (position > 0 && items[position - 1U].kind == kind && items[position - 1U].key == items[position].key)
			continue;
		keys++;
	}

	/* Reports the number of keys. */
	return keys;
}
