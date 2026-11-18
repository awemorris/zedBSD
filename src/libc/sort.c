/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The array sorts of the C library: qsort, qsort_r, heapsort and mergesort.
 *
 * qsort and qsort_r are an introspective sort: a quicksort whose pivot is a
 * median of three (of three medians of three on a long range), which gives up
 * to a heapsort when the ranges stop shrinking, and which finishes short ranges
 * by insertion.  Every sort is O(n log n) in the worst case.  heapsort sorts
 * in place without memory of its own; mergesort is the stable one and borrows
 * a copy of the array.
 *
 * Elements are moved by the widest machine word that the element size and the
 * array's alignment both allow, so an array of pointers or of structures moves
 * a word at a time instead of a byte at a time.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Ranges of at most this many elements are finished by insertion. */
#define SORT_INSERTION_LIMIT 16

/* Ranges longer than this take their pivot as the median of three medians. */
#define SORT_NINTHER_LIMIT 64

/* mergesort orders runs of this many elements by insertion before merging. */
#define SORT_MERGE_RUN 8

/*
 * The unit an element is swapped and copied in.
 *
 * It is chosen once per call from the element size and the addresses of the
 * arrays involved, so every element of the call can use it.
 */
enum sort_unit {
	SORT_UNIT_WORD,
	SORT_UNIT_WORD32,
	SORT_UNIT_BYTE
};

/*
 * A machine word read and written in place of the caller's element type.
 *
 * may_alias tells the compiler that the element really is of another type,
 * so moving it through this type is not an aliasing violation.
 */
typedef unsigned long __attribute__((__may_alias__)) sort_word;

/*
 * A 32-bit word, for elements whose size or alignment is a multiple of four
 * but not of the machine word.
 */
typedef uint32_t __attribute__((__may_alias__)) sort_word32;

/*
 * What one sort call orders by and how it moves elements.
 *
 * Exactly one of the two comparison functions is set: qsort, heapsort and
 * mergesort supply the plain one, qsort_r the one that takes the caller's
 * argument.  The structure lives on the caller's stack for the one call.
 */
struct sort_order {
	int (*compare)(const void *, const void *);
	int (*compare_with_argument)(const void *, const void *, void *);
	void *argument;
	size_t size;
	enum sort_unit unit;
};

static void sort_prepare(struct sort_order *order, size_t size, uintptr_t address_bits);
static int sort_compare(const struct sort_order *order, const unsigned char *left, const unsigned char *right);
static void sort_swap(const struct sort_order *order, unsigned char *left, unsigned char *right);
static void sort_copy(const struct sort_order *order, unsigned char *target, const unsigned char *source, size_t count);
static void sort_insertion(const struct sort_order *order, unsigned char *base, size_t count);
static void sort_sift_down(const struct sort_order *order, unsigned char *base, size_t root, size_t count);
static void sort_heap(const struct sort_order *order, unsigned char *base, size_t count);
static unsigned char *sort_median(const struct sort_order *order, unsigned char *first, unsigned char *second, unsigned char *third);
static unsigned char *sort_choose_pivot(const struct sort_order *order, unsigned char *base, size_t count);
static size_t sort_partition(const struct sort_order *order, unsigned char *base, size_t count);
static size_t sort_depth_limit(size_t count);
static void sort_introspective(const struct sort_order *order, unsigned char *base, size_t count, size_t depth);
static void sort_merge(const struct sort_order *order, const unsigned char *source, size_t start, size_t middle, size_t end, unsigned char *target);

/*
 * Sorts an array in ascending order of a comparison function.
 *
 * The order of elements that compare equal is unspecified.
 */
void
qsort(
	void *base,
	size_t count,
	size_t size,
	int (*compare)(const void *, const void *))
{
	struct sort_order order;
	size_t depth;

	/* An array of empty elements or with no order has nothing to sort. */
	if (size == 0 || compare == NULL)
		return;

	/* Describes the order and the unit the elements move in. */
	sort_prepare(&order, size, (uintptr_t)base);
	order.compare = compare;

	/* Sorts the whole array, giving up to a heapsort past twice its depth. */
	depth = sort_depth_limit(count);
	sort_introspective(&order, base, count, depth);
}

/*
 * Sorts an array by a comparison function that takes the caller's argument.
 *
 * The comparison receives the two elements and then the argument.  The order
 * of elements that compare equal is unspecified.
 */
void
qsort_r(
	void *base,
	size_t count,
	size_t size,
	int (*compare)(const void *, const void *, void *),
	void *argument)
{
	struct sort_order order;
	size_t depth;

	/* An array of empty elements or with no order has nothing to sort. */
	if (size == 0 || compare == NULL)
		return;

	/* Describes the order, the argument and the unit the elements move in. */
	sort_prepare(&order, size, (uintptr_t)base);
	order.compare_with_argument = compare;
	order.argument = argument;

	/* Sorts the whole array, giving up to a heapsort past twice its depth. */
	depth = sort_depth_limit(count);
	sort_introspective(&order, base, count, depth);
}

/*
 * Sorts an array in place by a heapsort.
 *
 * It needs no memory and is O(n log n) in every case; the order of elements
 * that compare equal is unspecified.  An element size of zero is refused
 * with EINVAL.
 */
int
heapsort(
	void *base,
	size_t count,
	size_t size,
	int (*compare)(const void *, const void *))
{
	struct sort_order order;

	/* Refuses elements of no size. */
	if (size == 0) {
		errno = EINVAL;
		return -1;
	}

	/* An array with no order has nothing to sort. */
	if (compare == NULL)
		return 0;

	/* Describes the order and the unit the elements move in. */
	sort_prepare(&order, size, (uintptr_t)base);
	order.compare = compare;

	/* Sorts the whole array as one heap. */
	sort_heap(&order, base, count);

	/* Succeeded: the array is in ascending order. */
	return 0;
}

/*
 * Sorts an array stably by a merge sort.
 *
 * Elements that compare equal keep their order.  The merge borrows a copy of
 * the array; when it cannot be allocated, the array is left as it was and
 * ENOMEM is reported.  An element size of zero is refused with EINVAL.
 */
int
mergesort(
	void *base,
	size_t count,
	size_t size,
	int (*compare)(const void *, const void *))
{
	struct sort_order order;
	unsigned char *elements;
	unsigned char *scratch;
	unsigned char *source;
	unsigned char *target;
	unsigned char *exchanged;
	size_t start;
	size_t length;
	size_t width;
	size_t middle;
	size_t end;

	/* Refuses elements of no size. */
	if (size == 0) {
		errno = EINVAL;
		return -1;
	}

	/* A short array or one with no order is already sorted. */
	if (count < 2 || compare == NULL)
		return 0;

	/* Refuses an array whose copy could not even be measured. */
	if (count > SIZE_MAX / size) {
		errno = ENOMEM;
		return -1;
	}

	/* Borrows the copy that every merge pass writes into. */
	scratch = malloc(count * size);
	if (scratch == NULL) {
		errno = ENOMEM;
		return -1;
	}

	/* Describes the order and a unit that suits both the array and its copy. */
	elements = base;
	sort_prepare(&order, size, (uintptr_t)elements | (uintptr_t)scratch);
	order.compare = compare;

	/* Orders each short run by insertion, which keeps equal elements in order. */
	for (start = 0;
	     start < count;
	     start += length) {
		/* The last run may be shorter than the others. */
		length = count - start;
		if (length > SORT_MERGE_RUN)
			length = SORT_MERGE_RUN;

		/* Sorts the run in place. */
		sort_insertion(&order, elements + start * size, length);
	}

	/*
	 * Merges pairs of neighbouring runs, doubling the run each pass and
	 * alternating between the array and its copy.  The widths cannot
	 * overflow, because the copy of the whole array was allocated.
	 */
	source = elements;
	target = scratch;
	for (width = SORT_MERGE_RUN;
	     width < count;
	     width *= 2) {
		/* Merges every pair of runs of this pass into the other buffer. */
		for (start = 0;
		     start < count;
		     start += 2 * width) {
			/* Ends the left run a width on, or at the array's end. */
			middle = count;
			if (count - start > width)
				middle = start + width;

			/* Ends the right run a width after the left one, or at the array's end. */
			end = count;
			if (count - middle > width)
				end = middle + width;

			/* Merges the two runs into the other buffer. */
			sort_merge(&order, source, start, middle, end, target);
		}

		/* The merged buffer is the source of the next pass. */
		exchanged = source;
		source = target;
		target = exchanged;
	}

	/* Brings the result back when the last pass left it in the copy. */
	if (source != elements)
		sort_copy(&order, elements, source, count);

	/* Returns the borrowed copy. */
	free(scratch);

	/* Succeeded: the array is in ascending order, equal elements in their old order. */
	return 0;
}

/* Clears an order and picks the widest unit the size and the addresses allow. */
static void
sort_prepare(
	struct sort_order *order,
	size_t size,
	uintptr_t address_bits)
{
	/* Starts from an order with no comparison and no argument. */
	order->compare = NULL;
	order->compare_with_argument = NULL;
	order->argument = NULL;
	order->size = size;

	/*
	 * Every element starts at a multiple of the size from the array, so an
	 * aligned array of whole words stays aligned at every element.
	 */
	if ((address_bits | size) % sizeof(sort_word) == 0) {
		/* Whole machine words at word-aligned addresses. */
		order->unit = SORT_UNIT_WORD;
	} else if ((address_bits | size) % sizeof(sort_word32) == 0) {
		/* Whole 32-bit words at 32-bit aligned addresses. */
		order->unit = SORT_UNIT_WORD32;
	} else {
		/* Any other element moves a byte at a time. */
		order->unit = SORT_UNIT_BYTE;
	}
}

/* Reports how the left element orders against the right one. */
static int
sort_compare(
	const struct sort_order *order,
	const unsigned char *left,
	const unsigned char *right)
{
	int ordering;

	/* qsort_r's comparison also takes the caller's argument. */
	if (order->compare_with_argument != NULL) {
		ordering = order->compare_with_argument(left, right, order->argument);

		/* Succeeded: reports the caller's ordering. */
		return ordering;
	}

	/* Asks the plain comparison. */
	ordering = order->compare(left, right);

	/* Succeeded: reports the caller's ordering. */
	return ordering;
}

/* Exchanges the contents of two elements of the array. */
static void
sort_swap(
	const struct sort_order *order,
	unsigned char *left,
	unsigned char *right)
{
	sort_word *left_words;
	sort_word *right_words;
	sort_word held_word;
	sort_word32 *left_words32;
	sort_word32 *right_words32;
	sort_word32 held_word32;
	unsigned char held_byte;
	size_t units;
	size_t index;

	/* Picks the loop of the unit the elements were found to allow. */
	switch (order->unit) {
	case SORT_UNIT_WORD:
		/* Exchanges the elements a machine word at a time. */
		left_words = (sort_word *)(void *)left;
		right_words = (sort_word *)(void *)right;
		units = order->size / sizeof(sort_word);
		for (index = 0; index < units; index++) {
			held_word = left_words[index];
			left_words[index] = right_words[index];
			right_words[index] = held_word;
		}

		break;
	case SORT_UNIT_WORD32:
		/* Exchanges the elements four bytes at a time. */
		left_words32 = (sort_word32 *)(void *)left;
		right_words32 = (sort_word32 *)(void *)right;
		units = order->size / sizeof(sort_word32);
		for (index = 0; index < units; index++) {
			held_word32 = left_words32[index];
			left_words32[index] = right_words32[index];
			right_words32[index] = held_word32;
		}

		break;
	case SORT_UNIT_BYTE:
	default:
		/* Exchanges the elements a byte at a time. */
		for (index = 0; index < order->size; index++) {
			held_byte = left[index];
			left[index] = right[index];
			right[index] = held_byte;
		}

		break;
	}
}

/* Copies a run of elements between the array and mergesort's copy of it. */
static void
sort_copy(
	const struct sort_order *order,
	unsigned char *target,
	const unsigned char *source,
	size_t count)
{
	sort_word *target_words;
	const sort_word *source_words;
	sort_word32 *target_words32;
	const sort_word32 *source_words32;
	size_t units;
	size_t index;

	/* Picks the loop of the unit both buffers were found to allow. */
	switch (order->unit) {
	case SORT_UNIT_WORD:
		/* Copies the run a machine word at a time. */
		target_words = (sort_word *)(void *)target;
		source_words = (const sort_word *)(const void *)source;
		units = count * (order->size / sizeof(sort_word));
		for (index = 0; index < units; index++)
			target_words[index] = source_words[index];

		break;
	case SORT_UNIT_WORD32:
		/* Copies the run four bytes at a time. */
		target_words32 = (sort_word32 *)(void *)target;
		source_words32 = (const sort_word32 *)(const void *)source;
		units = count * (order->size / sizeof(sort_word32));
		for (index = 0; index < units; index++)
			target_words32[index] = source_words32[index];

		break;
	case SORT_UNIT_BYTE:
	default:
		/* Copies the run a byte at a time. */
		units = count * order->size;
		for (index = 0; index < units; index++)
			target[index] = source[index];

		break;
	}
}

/*
 * Sorts a short range by insertion.
 *
 * An element moves down only past elements that compare greater, so equal
 * elements keep their order, which mergesort's runs rely on.
 */
static void
sort_insertion(
	const struct sort_order *order,
	unsigned char *base,
	size_t count)
{
	unsigned char *current;
	unsigned char *previous;
	size_t index;
	int ordering;

	/* Inserts each element into the sorted prefix before it. */
	for (index = 1; index < count; index++) {
		/* Moves the element down past every greater element before it. */
		current = base + index * order->size;
		while (current != base) {
			/* Stops behind an element that is not greater. */
			previous = current - order->size;
			ordering = sort_compare(order, previous, current);
			if (ordering <= 0)
				break;

			/* Exchanges the pair and follows the element down. */
			sort_swap(order, previous, current);
			current = previous;
		}
	}
}

/* Moves a heap's root element down until neither child is greater. */
static void
sort_sift_down(
	const struct sort_order *order,
	unsigned char *base,
	size_t root,
	size_t count)
{
	unsigned char *parent;
	unsigned char *child;
	unsigned char *sibling;
	size_t child_index;
	int ordering;

	/* Descends while the element still has a child in the heap. */
	while (root < count / 2) {
		/* Starts from the left child, which exists below this bound. */
		child_index = 2 * root + 1;
		child = base + child_index * order->size;

		/* Prefers the right child when there is one and it is greater. */
		if (child_index + 1 < count) {
			sibling = child + order->size;
			ordering = sort_compare(order, sibling, child);
			if (ordering > 0) {
				child_index++;
				child = sibling;
			}
		}

		/* Stops where the element is not less than its greater child. */
		parent = base + root * order->size;
		ordering = sort_compare(order, parent, child);
		if (ordering >= 0)
			return;

		/* Exchanges the element with the child and follows it down. */
		sort_swap(order, parent, child);
		root = child_index;
	}
}

/* Sorts a range in place by building a heap and draining it from the end. */
static void
sort_heap(
	const struct sort_order *order,
	unsigned char *base,
	size_t count)
{
	unsigned char *last;
	size_t root;
	size_t end;

	/* Builds a heap whose greatest element is at the front. */
	for (root = count / 2; root != 0; root--)
		sort_sift_down(order, base, root - 1, count);

	/* Moves the greatest remaining element behind the shrinking heap. */
	for (end = count; end > 1; end--) {
		last = base + (end - 1) * order->size;
		sort_swap(order, base, last);
		sort_sift_down(order, base, 0, end - 1);
	}
}

/* Reports which of three elements is the median. */
static unsigned char *
sort_median(
	const struct sort_order *order,
	unsigned char *first,
	unsigned char *second,
	unsigned char *third)
{
	int ordering;

	/* Orders the first two, then places the third against them. */
	ordering = sort_compare(order, first, second);
	if (ordering < 0) {
		/* The second is the median when the third lies above it. */
		ordering = sort_compare(order, second, third);
		if (ordering < 0)
			return second;

		/* The third is the median when it lies between the two. */
		ordering = sort_compare(order, first, third);
		if (ordering < 0)
			return third;

		/* The third is at most the first, which is the median. */
		return first;
	}

	/* The second is the median when the third lies below it. */
	ordering = sort_compare(order, second, third);
	if (ordering > 0)
		return second;

	/* The third is the median when it lies between the two. */
	ordering = sort_compare(order, first, third);
	if (ordering > 0)
		return third;

	/* The first is at most the third, so it is the median. */
	return first;
}

/*
 * Picks the pivot of a range: the median of its ends and middle, or on a long
 * range the median of three such medians, which also splits sawtooth and
 * organ-pipe inputs well.
 */
static unsigned char *
sort_choose_pivot(
	const struct sort_order *order,
	unsigned char *base,
	size_t count)
{
	unsigned char *first;
	unsigned char *middle;
	unsigned char *last;
	unsigned char *pivot;
	size_t step;

	/* Locates the ends and the middle of the range. */
	first = base;
	middle = base + (count / 2) * order->size;
	last = base + (count - 1) * order->size;

	/* Takes the median of three medians of neighbouring samples on a long range. */
	if (count > SORT_NINTHER_LIMIT) {
		step = (count / 8) * order->size;
		first = sort_median(order, first, first + step, first + 2 * step);
		middle = sort_median(order, middle - step, middle, middle + step);
		last = sort_median(order, last - 2 * step, last - step, last);
	}

	/* Takes the median of the three samples. */
	pivot = sort_median(order, first, middle, last);

	/* Reports the chosen pivot element. */
	return pivot;
}

/*
 * Partitions a range around a pivot and reports where the pivot ends.
 *
 * Elements before the reported index compare at most the pivot and elements
 * after it at least the pivot.  Both scans stop at elements equal to the
 * pivot, so a range of equal elements still splits in the middle.  Every scan
 * is bounded by the range, so a comparison that is not a consistent order
 * cannot move an index outside it.
 */
static size_t
sort_partition(
	const struct sort_order *order,
	unsigned char *base,
	size_t count)
{
	unsigned char *chosen;
	unsigned char *left;
	unsigned char *right;
	size_t left_index;
	size_t right_index;
	int ordering;

	/* Chooses the pivot of the range. */
	chosen = sort_choose_pivot(order, base, count);

	/* Moves the pivot to the front, where it stays while the range is scanned. */
	if (chosen != base)
		sort_swap(order, base, chosen);

	/* Exchanges misplaced pairs until the two scans meet. */
	left_index = 0;
	right_index = count;
	for (;;) {
		/* Advances the left scan past elements less than the pivot. */
		for (;;) {
			left_index++;
			if (left_index >= count)
				break;

			/* Stops at an element that is not less than the pivot. */
			left = base + left_index * order->size;
			ordering = sort_compare(order, left, base);
			if (ordering >= 0)
				break;
		}

		/* Moves the right scan back past elements greater than the pivot. */
		for (;;) {
			right_index--;
			if (right_index == 0)
				break;

			/* Stops at an element that is not greater than the pivot. */
			right = base + right_index * order->size;
			ordering = sort_compare(order, right, base);
			if (ordering <= 0)
				break;
		}

		/* Stops when the scans have met or crossed. */
		if (left_index >= right_index)
			break;

		/* Exchanges the pair, each of which belongs on the other side. */
		left = base + left_index * order->size;
		right = base + right_index * order->size;
		sort_swap(order, left, right);
	}

	/* Puts the pivot between the two parts. */
	if (right_index != 0) {
		right = base + right_index * order->size;
		sort_swap(order, base, right);
	}

	/* Reports the pivot's final index. */
	return right_index;
}

/* Reports twice the floor of the binary logarithm of a range's length. */
static size_t
sort_depth_limit(
	size_t count)
{
	size_t depth;

	/* Adds two levels for every halving the length allows. */
	depth = 0;
	while (count > 1) {
		count /= 2;
		depth += 2;
	}

	/* Reports the number of partitions a range may take before a heapsort. */
	return depth;
}

/*
 * Sorts a range by quicksort, a heapsort past the depth limit, and insertion
 * on short ranges.
 *
 * The shorter part of each partition is sorted by a recursive call and the
 * longer one by the loop, so the recursion is no deeper than the logarithm
 * of the range.
 */
static void
sort_introspective(
	const struct sort_order *order,
	unsigned char *base,
	size_t count,
	size_t depth)
{
	unsigned char *right_base;
	size_t pivot_index;
	size_t left_count;
	size_t right_count;

	/* Partitions the range until what is left is short. */
	while (count > SORT_INSERTION_LIMIT) {
		/* Gives up on partitioning a range that keeps splitting badly. */
		if (depth == 0) {
			sort_heap(order, base, count);
			return;
		}

		/* Spends one level of the depth on this partition. */
		depth--;

		/* Splits the range around a pivot that is then in its final place. */
		pivot_index = sort_partition(order, base, count);
		left_count = pivot_index;
		right_base = base + (pivot_index + 1) * order->size;
		right_count = count - pivot_index - 1;

		/* Recurses into the shorter part and keeps the longer one. */
		if (left_count < right_count) {
			sort_introspective(order, base, left_count, depth);
			base = right_base;
			count = right_count;
		} else {
			sort_introspective(order, right_base, right_count, depth);
			count = left_count;
		}
	}

	/* Finishes the short range by insertion. */
	sort_insertion(order, base, count);
}

/*
 * Merges two neighbouring sorted runs of one buffer into the same place of the
 * other buffer.
 *
 * The left run is taken first on a tie, so equal elements keep their order.
 */
static void
sort_merge(
	const struct sort_order *order,
	const unsigned char *source,
	size_t start,
	size_t middle,
	size_t end,
	unsigned char *target)
{
	const unsigned char *left;
	const unsigned char *right;
	size_t left_index;
	size_t right_index;
	size_t output_index;
	int ordering;

	/* Copies a lone left run at the end of the array as it is. */
	if (middle == end) {
		sort_copy(order, target + start * order->size, source + start * order->size, end - start);
		return;
	}

	/* Copies two runs that are already in order as one block. */
	left = source + (middle - 1) * order->size;
	right = source + middle * order->size;
	ordering = sort_compare(order, left, right);
	if (ordering <= 0) {
		sort_copy(order, target + start * order->size, source + start * order->size, end - start);
		return;
	}

	/* Takes the lesser front element of the two runs until one runs out. */
	left_index = start;
	right_index = middle;
	output_index = start;
	while (left_index < middle && right_index < end) {
		/* Compares the two front elements. */
		left = source + left_index * order->size;
		right = source + right_index * order->size;
		ordering = sort_compare(order, right, left);

		/* Takes the right element only when it is strictly less. */
		if (ordering < 0) {
			sort_copy(order, target + output_index * order->size, right, 1);
			right_index++;
		} else {
			sort_copy(order, target + output_index * order->size, left, 1);
			left_index++;
		}

		/* Advances past the element just written. */
		output_index++;
	}

	/* Appends whatever is left of the left run. */
	sort_copy(order, target + output_index * order->size, source + left_index * order->size, middle - left_index);
	output_index += middle - left_index;

	/* Appends whatever is left of the right run. */
	sort_copy(order, target + output_index * order->size, source + right_index * order->size, end - right_index);
}
