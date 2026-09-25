/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the kernel heap (src/kern/heap.c).
 *
 * The heap source is included, so the test can reach the block header.
 * With -DKERN_KERNEL_HEAP_TRACE it also checks that the trace validator
 * rejects a broken chain or free list without following it (the kernel form
 * of plan/ws002/tests/heap-trace-validator.c).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/kern/heap.c"

static int failures;
static int checks;
#ifdef KERN_KERNEL_HEAP_TRACE
static unsigned observed_allocations;
static unsigned observed_releases;
static unsigned walked_pointers;
#endif

static void check(int condition, const char *what, int line);
static void test_init(void);
static void test_alloc_free(void);
static void test_errors(void);
static void test_validate(void);
#ifdef KERN_KERNEL_HEAP_TRACE
static void observe(void *context, void *pointer, size_t size, enum kern_heap_event event);
static void test_trace(void);
#endif

#define CHECK(condition) check((condition) != 0, #condition, __LINE__)

/*
 * The storage every test carves its heap from.
 */
static _Alignas(16) unsigned char storage[8192];

#ifdef KERN_KERNEL_HEAP_TRACE
/*
 * Counts the pointers the heap reports before walking its chain.
 */
void
kern_heap_trace_pointer_walk(
	void *pointer,
	void *caller)
{
	(void)pointer;
	(void)caller;

	/* Counts the report. */
	walked_pointers++;
}
#endif

/*
 * Runs every heap check and reports the result.
 */
int
main(
	void)
{
	/* Runs the groups of checks. */
	test_init();
	test_alloc_free();
	test_errors();
	test_validate();
#ifdef KERN_KERNEL_HEAP_TRACE
	test_trace();
#endif

	/* Reports the result. */
	if (failures != 0) {
		printf("kern-heap-test: FAIL (%d of %d checks)\n", failures, checks);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("kern-heap-test: PASS (%d checks)\n", checks);
	return 0;
}

/* Counts one check and reports it when it fails. */
static void
check(
	int condition,
	const char *what,
	int line)
{
	/* Counts the check. */
	checks++;

	/* Reports a failed check. */
	if (!condition) {
		failures++;
		printf("FAIL line %d: %s\n", line, what);
	}
}

/* Checks that initialization trims the range to the grid. */
static void
test_init(
	void)
{
	struct kern_heap heap;

	/* A misaligned start is moved up and the size cut to the grid. */
	kern_heap_init(&heap, storage + 3, 1000);
	CHECK((uintptr_t)heap.begin % 16 == 0);
	CHECK(heap.begin == storage + 16);
	CHECK((size_t)(heap.end - heap.begin) == ((1000 - 13) & ~(size_t)15));
	CHECK(heap.first == (struct kern_heap_block *)heap.begin);
	CHECK(heap.free_list == heap.first);
	CHECK(kern_heap_largest_free(&heap) == (size_t)(heap.end - heap.begin) - kern_heap_header_size());
	CHECK(kern_heap_validate(&heap));

	/* A range too small for one block, or none at all, gives an empty heap. */
	kern_heap_init(&heap, storage, 32);
	CHECK(heap.first == NULL);
	CHECK(kern_heap_alloc(&heap, 1) == NULL);
	CHECK(kern_heap_validate(&heap));
	kern_heap_init(&heap, NULL, 4096);
	CHECK(heap.begin == NULL);
	CHECK(kern_heap_alloc(&heap, 1) == NULL);
	CHECK(kern_heap_largest_failed(&heap) == 1);
}

/* Checks allocation, release and the merging of free neighbours. */
static void
test_alloc_free(
	void)
{
	struct kern_heap heap;
	unsigned char *a;
	unsigned char *b;
	unsigned char *c;
	unsigned char *zero;
	size_t whole;

	kern_heap_init(&heap, storage, sizeof(storage));
	whole = kern_heap_largest_free(&heap);

	/* Allocations are aligned, distinct and counted by their requested size. */
	a = kern_heap_alloc(&heap, 73);
	b = kern_heap_alloc(&heap, 100);
	c = kern_heap_alloc(&heap, 16);
	zero = kern_heap_alloc(&heap, 0);
	CHECK(a != NULL && b != NULL && c != NULL && zero != NULL);
	CHECK((uintptr_t)a % 16 == 0 && (uintptr_t)b % 16 == 0);
	CHECK((uintptr_t)c % 16 == 0 && (uintptr_t)zero % 16 == 0);
	CHECK(b >= a + 73 && c >= b + 100 && zero >= c + 16);
	CHECK(kern_heap_current(&heap) == 73 + 100 + 16);
	CHECK(kern_heap_peak(&heap) == 73 + 100 + 16);
	CHECK(kern_heap_validate(&heap));
	memset(a, 0x11, 73);
	memset(b, 0x22, 100);
	memset(c, 0x33, 16);

	/* A released block in the middle is reused by a request that fits it. */
	kern_heap_free(&heap, b);
	CHECK(kern_heap_current(&heap) == 73 + 16);
	CHECK(kern_heap_peak(&heap) == 73 + 100 + 16);
	CHECK(kern_heap_validate(&heap));
	b = kern_heap_alloc(&heap, 96);
	CHECK(b != NULL);
	CHECK(kern_heap_validate(&heap));

	/* Releasing everything merges the heap back into one block. */
	kern_heap_free(&heap, a);
	kern_heap_free(&heap, c);
	kern_heap_free(&heap, b);
	kern_heap_free(&heap, zero);
	CHECK(kern_heap_current(&heap) == 0);
	CHECK(kern_heap_validate(&heap));
	CHECK(heap.first->next_physical == NULL);
	CHECK(kern_heap_largest_free(&heap) == whole);
	CHECK(heap.errors == 0);

	/* A request larger than the heap fails and is remembered. */
	CHECK(kern_heap_alloc(&heap, sizeof(storage)) == NULL);
	CHECK(kern_heap_largest_failed(&heap) == sizeof(storage));
	CHECK(kern_heap_alloc(&heap, SIZE_MAX) == NULL);
	CHECK(kern_heap_largest_failed(&heap) == SIZE_MAX);

	/* The whole free block can be taken at once. */
	a = kern_heap_alloc(&heap, whole);
	CHECK(a != NULL);
	CHECK(kern_heap_largest_free(&heap) == 0);
	CHECK(kern_heap_alloc(&heap, 1) == NULL);
	kern_heap_free(&heap, a);
	CHECK(kern_heap_validate(&heap));
}

/* Checks that foreign and repeated releases are counted, not obeyed. */
static void
test_errors(
	void)
{
	struct kern_heap heap;
	unsigned char *a;
	unsigned char outside[32];

	kern_heap_init(&heap, storage, sizeof(storage));
	a = kern_heap_alloc(&heap, 40);
	CHECK(a != NULL);

	/* A null pointer is ignored. */
	kern_heap_free(&heap, NULL);
	CHECK(heap.errors == 0);

	/* A pointer outside the range, or inside a block, is an error. */
	kern_heap_free(&heap, outside);
	CHECK(heap.errors == 1);
	kern_heap_free(&heap, a + 16);
	CHECK(heap.errors == 2);

	/* A second release of the same block is an error. */
	kern_heap_free(&heap, a);
	CHECK(heap.errors == 2);
	kern_heap_free(&heap, a);
	CHECK(heap.errors == 3);
	CHECK(kern_heap_current(&heap) == 0);
	CHECK(kern_heap_validate(&heap));
}

/* Checks that the validator detects broken invariants. */
static void
test_validate(
	void)
{
	struct kern_heap heap;
	struct kern_heap_block *first;
	struct kern_heap_block *next;
	unsigned char *a;
	unsigned char *b;
	size_t saved;

	kern_heap_init(&heap, storage, sizeof(storage));
	a = kern_heap_alloc(&heap, 64);
	b = kern_heap_alloc(&heap, 64);
	CHECK(a != NULL && b != NULL);
	CHECK(kern_heap_validate(&heap));
	first = heap.first;
	next = first->next_physical;

	/* A wrong byte count. */
	heap.current_bytes++;
	CHECK(!kern_heap_validate(&heap));
	heap.current_bytes--;

	/* A capacity that no longer tiles the range. */
	saved = first->capacity;
	first->capacity += 16;
	CHECK(!kern_heap_validate(&heap));
	first->capacity = saved;

	/* A broken back link. */
	next->previous_physical = NULL;
	CHECK(!kern_heap_validate(&heap));
	next->previous_physical = first;

	/* A free block missing from the free list. */
	kern_heap_free(&heap, a);
	CHECK(kern_heap_validate(&heap));
	heap.free_list = heap.free_list->next_free;
	CHECK(!kern_heap_validate(&heap));
	kern_heap_init(&heap, storage, sizeof(storage));

	/* A looping free list. */
	a = kern_heap_alloc(&heap, 64);
	b = kern_heap_alloc(&heap, 64);
	kern_heap_free(&heap, a);
	CHECK(kern_heap_validate(&heap));
	heap.free_list->next_free = heap.free_list;
	CHECK(!kern_heap_validate(&heap));
	(void)b;
}

#ifdef KERN_KERNEL_HEAP_TRACE
/* Counts the allocations and releases the heap reports. */
static void
observe(
	void *context,
	void *pointer,
	size_t size,
	enum kern_heap_event event)
{
	(void)context;
	(void)pointer;
	(void)size;

	/* Counts the event by its kind. */
	if (event == KERN_HEAP_EVENT_ALLOCATED) {
		observed_allocations++;
	} else {
		observed_releases++;
	}
}

/* Checks the trace validator and the trace hooks. */
static void
test_trace(
	void)
{
	struct kern_heap heap;
	struct kern_heap_block *first;
	struct kern_heap_block *next;
	struct kern_heap_block fabricated;
	void *a;
	void *b;
	size_t capacity;

	/* Counts only what this test does; the earlier tests released pointers too. */
	observed_allocations = 0;
	observed_releases = 0;
	walked_pointers = 0;

	kern_heap_init(&heap, storage, sizeof(storage));
	kern_heap_set_observer(&heap, observe, NULL);
	CHECK(kern_heap_trace_validate(&heap));

	/* The observer and the walk hook see every allocation and release. */
	a = kern_heap_alloc(&heap, 73);
	b = kern_heap_alloc(&heap, 155);
	CHECK(a != NULL && b != NULL);
	CHECK(observed_allocations == 2);
	CHECK(kern_heap_trace_validate(&heap));

	/* A chain that loops, points outside, or overflows is refused. */
	first = heap.first;
	next = first->next_physical;
	first->next_physical = first;
	CHECK(!kern_heap_trace_validate(&heap));
	first->next_physical = (struct kern_heap_block *)(uintptr_t)1;
	CHECK(!kern_heap_trace_validate(&heap));
	first->next_physical = next;
	capacity = first->capacity;
	first->capacity = SIZE_MAX;
	CHECK(!kern_heap_trace_validate(&heap));
	first->capacity = capacity + 1;
	first->next_physical = (struct kern_heap_block *)((unsigned char *)next + 1);
	CHECK(!kern_heap_trace_validate(&heap));
	first->capacity = capacity;
	first->next_physical = next;
	CHECK(kern_heap_trace_validate(&heap));

	/* Releases are reported to both hooks. */
	kern_heap_free(&heap, a);
	kern_heap_free(&heap, b);
	CHECK(observed_releases == 2);
	CHECK(walked_pointers == 2);
	CHECK(kern_heap_trace_validate(&heap));

	/* A free list that loops, holds a block off the chain, or links back wrong is refused. */
	first = heap.first;
	CHECK(first->state == KERN_HEAP_FREE);
	first->next_free = first;
	CHECK(!kern_heap_trace_validate(&heap));
	first->next_free = NULL;
	memset(&fabricated, 0, sizeof(fabricated));
	fabricated.magic = KERN_HEAP_MAGIC;
	fabricated.state = KERN_HEAP_FREE;
	heap.free_list = &fabricated;
	CHECK(!kern_heap_trace_validate(&heap));
	heap.free_list = first;
	first->previous_free = first;
	CHECK(!kern_heap_trace_validate(&heap));
	first->previous_free = NULL;
	heap.free_list = NULL;
	CHECK(!kern_heap_trace_validate(&heap));
	heap.free_list = first;
	CHECK(kern_heap_trace_validate(&heap));

	/* A release does not follow a looping chain forever. */
	kern_heap_init(&heap, storage, sizeof(storage));
	a = kern_heap_alloc(&heap, 73);
	b = kern_heap_alloc(&heap, 73);
	CHECK(a != NULL && b != NULL);
	first = heap.first;
	next = first->next_physical;
	first->next_physical = first;
	kern_heap_free(&heap, b);
	CHECK(heap.errors == 1);
	first->next_physical = next;
	kern_heap_free(&heap, b);
	CHECK(heap.errors == 1);
	CHECK(kern_heap_trace_validate(&heap));
}
#endif
