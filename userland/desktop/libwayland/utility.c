/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements caller-owned Wayland utility containers and fixed coordinates.
 */

#include "internal.h"

/*
 * Initializes an empty growable byte array.
 */
void
wl_array_init(
	struct wl_array *array)
{
	/* Makes the caller's array independent of any allocation. */
	array->size = 0;
	array->alloc = 0;
	array->data = NULL;

	/* Succeeded: the array is ready for append operations. */
	return;
}

/*
 * Releases storage owned by a Wayland array.
 */
void
wl_array_release(
	struct wl_array *array)
{
	/* Drops the allocation while leaving the descriptor reusable. */
	free(array->data);
	wl_array_init(array);

	/* Succeeded: the array owns no allocation. */
	return;
}

/*
 * Appends uninitialized bytes to a Wayland array.
 */
void *
wl_array_add(
	struct wl_array *array,
	size_t size)
{
	void *allocation;
	void *appended;
	size_t capacity;
	size_t required;

	/* Rejects arithmetic overflow before allocating or advancing the array. */
	if (size > SIZE_MAX - array->size) {
		errno = ENOMEM;
		return NULL;
	}

	/* Reserves geometric growth without imposing a fixed object limit. */
	required = array->size + size;
	if (required > array->alloc) {
		capacity = array->alloc;

		/* Starts small arrays with room for ordinary protocol state lists. */
		if (capacity == 0)
			capacity = 32;

		/* Grows until the full append fits, checking the final doubling. */
		while (capacity < required) {
			/* Avoids wrapping the capacity near the address-space limit. */
			if (capacity > SIZE_MAX / 2) {
				capacity = required;
				break;
			}

			capacity *= 2;
		}

		/* Preserves the original array if allocation fails. */
		allocation = realloc(array->data, capacity);
		if (allocation == NULL)
			return NULL;

		array->data = allocation;
		array->alloc = capacity;
	}

	/* Publishes the new logical extent only after storage is available. */
	appended = array->data;
	if (array->data != NULL)
		appended = (unsigned char *)array->data + array->size;

	array->size = required;

	/* Succeeded: reports the first byte of the appended range. */
	return appended;
}

/*
 * Replaces an array with an independent copy of another array.
 */
int
wl_array_copy(
	struct wl_array *array,
	struct wl_array *source)
{
	struct wl_array replacement;
	void *data;

	/* Treats copying an array to itself as already satisfied. */
	if (array == source)
		return 0;

	/* Builds the replacement before discarding the existing contents. */
	wl_array_init(&replacement);
	if (source->size != 0) {
		/* Obtains independent storage for the entire source range. */
		data = wl_array_add(&replacement, source->size);
		if (data == NULL)
			return -1;

		/* Copies only the source's initialized byte extent. */
		memcpy(data, source->data, source->size);
	}

	/* Transfers the completed replacement to the caller. */
	wl_array_release(array);
	*array = replacement;

	/* Succeeded: changes to either array no longer affect the other. */
	return 0;
}

/*
 * Initializes an empty caller-owned list anchor.
 */
void
wl_list_init(
	struct wl_list *list)
{
	/* A self-linked anchor represents an empty list. */
	list->prev = list;
	list->next = list;

	/* Succeeded: the anchor can accept elements. */
	return;
}

/*
 * Inserts an unlinked element immediately after a list link.
 */
void
wl_list_insert(
	struct wl_list *list,
	struct wl_list *element)
{
	/* Preserves both neighboring links while splicing the new element. */
	element->prev = list;
	element->next = list->next;
	list->next->prev = element;
	list->next = element;

	/* Succeeded: the caller's element participates in the list. */
	return;
}

/*
 * Unlinks an element without releasing caller-owned storage.
 */
void
wl_list_remove(
	struct wl_list *element)
{
	/* Joins the former neighbors and invalidates the removed link. */
	element->prev->next = element->next;
	element->next->prev = element->prev;
	element->prev = NULL;
	element->next = NULL;

	/* Succeeded: the element no longer belongs to the list. */
	return;
}

/*
 * Counts elements following a circular list anchor.
 */
int
wl_list_length(
	const struct wl_list *list)
{
	const struct wl_list *element;
	int count;

	/* Traverses each caller-owned element exactly once. */
	count = 0;
	for (element = list->next; element != list; element = element->next)
		count++;

	/* Succeeded: reports the number of elements excluding the anchor. */
	return count;
}

/*
 * Reports whether a circular list has no elements.
 */
int
wl_list_empty(
	const struct wl_list *list)
{
	/* An anchor pointing to itself contains no caller elements. */
	if (list->next == list)
		return 1;

	/* Succeeded: at least one caller element is present. */
	return 0;
}

/*
 * Inserts all elements of one list after another list link.
 */
void
wl_list_insert_list(
	struct wl_list *list,
	struct wl_list *other)
{
	/* Leaves the destination unchanged for an empty source. */
	if (other->next == other)
		return;

	/* Transfers the source chain without retaining its anchor. */
	other->next->prev = list;
	other->prev->next = list->next;
	list->next->prev = other->prev;
	list->next = other->next;

	/* Succeeded: the source anchor requires reinitialization before reuse. */
	return;
}

/*
 * Converts a fixed coordinate to an integer with truncation toward zero.
 */
int
wl_fixed_to_int(
	wl_fixed_t fixed)
{
	/* Succeeded: removes the eight fractional coordinate bits. */
	return fixed / 256;
}

/*
 * Converts an in-range integer to a fixed coordinate.
 */
wl_fixed_t
wl_fixed_from_int(
	int integer)
{
	/* Succeeded: the caller's representable integer gains eight fraction bits. */
	return integer * 256;
}

/*
 * Converts a fixed coordinate to an exact double precision value.
 */
double
wl_fixed_to_double(
	wl_fixed_t fixed)
{
	/* Succeeded: every 24.8 coordinate is exactly representable in double. */
	return (double)fixed / 256.0;
}

/*
 * Converts an in-range floating coordinate to its nearest fixed value.
 */
wl_fixed_t
wl_fixed_from_double(
	double real)
{
	double scaled;
	int32_t rounded;
	double remainder;

	/* Separates the integer and fraction without changing the rounding mode. */
	scaled = real * 256.0;
	rounded = (int32_t)scaled;
	remainder = scaled - (double)rounded;

	/* Rounds positive half-way values to an even coordinate unit. */
	if (remainder > 0.5 || (remainder == 0.5 && (rounded & 1) != 0))
		rounded++;

	/* Rounds negative half-way values symmetrically toward the nearest even. */
	if (remainder < -0.5 || (remainder == -0.5 && (rounded & 1) != 0))
		rounded--;

	/* Succeeded: reports the nearest representable fixed coordinate. */
	return rounded;
}
