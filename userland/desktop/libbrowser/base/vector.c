/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The growable array of equally sized items.
 */

#include "base/base.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The smallest number of items an array grows to. */
#define VECTOR_MIN_CAPACITY	8U

/*
 * Prepares an empty array of items of item_size bytes.
 */
void
wb_vector_init(
	struct wb_vector *vector,
	size_t item_size)
{
	/* An empty array owns no memory until the first push. */
	vector->items = NULL;
	vector->count = 0;
	vector->capacity = 0;
	vector->item_size = item_size;
}

/*
 * Appends a copy of one item.
 */
int
wb_vector_push(
	struct wb_vector *vector,
	const void *item)
{
	void *items;
	size_t capacity;

	/* Grows the storage when it is full, doubling it. */
	if (vector->count == vector->capacity) {
		capacity = vector->capacity * 2U;
		if (capacity < VECTOR_MIN_CAPACITY)
			capacity = VECTOR_MIN_CAPACITY;
		if (capacity > ((size_t)-1) / vector->item_size)
			return ENOMEM;

		/* Moves the items to the larger storage. */
		items = realloc(vector->items, capacity * vector->item_size);
		if (items == NULL)
			return ENOMEM;

		/* Publishes the larger storage. */
		vector->items = items;
		vector->capacity = capacity;
	}

	/* Copies the item into the first free place. */
	memcpy((unsigned char *)vector->items + vector->count * vector->item_size, item, vector->item_size);
	vector->count++;

	/* Succeeded: the item is the last one. */
	return 0;
}

/*
 * Finds the item at index, which must be below the count.
 */
void *
wb_vector_at(
	const struct wb_vector *vector,
	size_t index)
{
	assert(index < vector->count);

	/* Reports where the item lives until the array next grows. */
	return (unsigned char *)vector->items + index * vector->item_size;
}

/*
 * Drops the last item, which must exist.
 */
void
wb_vector_pop(
	struct wb_vector *vector)
{
	assert(vector->count > 0);

	/* Forgets the last item. */
	vector->count--;
}

/*
 * Drops every item and keeps the storage.
 */
void
wb_vector_clear(
	struct wb_vector *vector)
{
	/* Forgets the items. */
	vector->count = 0;
}

/*
 * Frees the array's storage.
 */
void
wb_vector_release(
	struct wb_vector *vector)
{
	/* Frees the items and leaves an empty array of the same item size. */
	free(vector->items);
	wb_vector_init(vector, vector->item_size);
}
