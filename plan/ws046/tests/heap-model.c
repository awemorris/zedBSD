/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A host model of the libc allocator (src/libc/heap.c) for ws046-p007.
 *
 *	heap-model bench COUNT		times COUNT live allocations and their frees
 *	heap-model trace COUNT SEED	runs COUNT random operations and prints a hash
 *
 * The heap grows 64 KiB at a time from one large arena, as the user heap
 * grows with sbrk.  The trace hashes every returned offset, every validation
 * and the error count, so two builds of the allocator that must behave the
 * same print the same hash.
 *
 * Build it against a heap.c (the renames keep the host's malloc):
 *
 *	cc -std=c11 -D_DEFAULT_SOURCE -O2 -I. -Dmalloc=zed_test_malloc \
 *	    -Dcalloc=zed_test_calloc -Drealloc=zed_test_realloc \
 *	    -Dfree=zed_test_free -c src/libc/heap.c -o heap.o
 *	cc -std=c11 -D_DEFAULT_SOURCE -O2 -I. \
 *	    plan/ws046/tests/heap-model.c heap.o -o heap-model
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "src/libc/heap.h"

/* How much address space the arena reserves. */
#define MODEL_ARENA_SIZE (1024UL * 1024UL * 1024UL)

/* How much the heap grows at a time (the user heap's sbrk step). */
#define MODEL_GROW_STEP 65536UL

/* How many live pointers the trace keeps. */
#define MODEL_SLOTS 4096U

/* The arena, its size so far, and the random state; one thread uses them. */
static unsigned char *model_arena;
static size_t model_arena_used;
static uint64_t model_random_state;
static uint64_t model_hash;

static size_t model_grow(void *context, void *end, size_t minimum);
static uint64_t model_random(void);
static void model_mix(uint64_t value);
static uint64_t model_offset(const void *pointer);
static long long model_now_ns(void);
static int model_bench(struct heap_allocator *heap, size_t count);
static int model_trace(struct heap_allocator *heap, size_t count);

/*
 * Runs the benchmark or the trace named by the first argument.
 *
 * It reports 0 when the mode ran and the heap stayed valid.
 */
int
main(
	int argc,
	char **argv)
{
	struct heap_allocator heap;
	size_t count;
	int bench;
	int ok;

	/* Refuses a command line that names no mode. */
	if (argc < 3) {
		fprintf(stderr, "usage: heap-model bench COUNT | trace COUNT SEED\n");
		return 2;
	}

	/* Reserves the arena. */
	model_arena = mmap(NULL, MODEL_ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (model_arena == MAP_FAILED) {
		perror("mmap");
		return 2;
	}


	/* Starts the heap on the arena's first step. */
	model_arena_used = MODEL_GROW_STEP;
	heap_allocator_init(&heap, model_arena, MODEL_GROW_STEP);
	heap_allocator_set_grow(&heap, model_grow, NULL);

	/* Runs the mode the command line asks for. */
	count = strtoul(argv[2], NULL, 10);
	bench = strcmp(argv[1], "bench") == 0;
	if (bench) {
		ok = model_bench(&heap, count);
	} else {
		model_random_state = strtoull(argv[3], NULL, 10) * 2654435761ULL + 1U;
		ok = model_trace(&heap, count);
	}

	/* Reports whether the mode kept the heap valid. */
	if (!ok)
		return 1;
	return 0;
}

/* Grows the heap by whole steps at its end, as sbrk does for the user heap. */
static size_t
model_grow(
	void *context,
	void *end,
	size_t minimum)
{
	size_t amount;

	/* The model has one arena and needs no context. */
	(void)context;

	/* Refuses a growth that does not continue the arena. */
	if ((unsigned char *)end != model_arena + model_arena_used)
		return 0;

	/* Rounds the growth up to whole steps within the arena. */
	amount = (minimum + MODEL_GROW_STEP - 1U) & ~(MODEL_GROW_STEP - 1U);
	if (amount > MODEL_ARENA_SIZE - model_arena_used)
		return 0;

	/* Hands the steps to the heap. */
	model_arena_used += amount;

	/* Reports how much the heap may add. */
	return amount;
}

/* Returns the next pseudo-random number (xorshift64). */
static uint64_t
model_random(
	void)
{
	/* Steps the generator. */
	model_random_state ^= model_random_state << 13;
	model_random_state ^= model_random_state >> 7;
	model_random_state ^= model_random_state << 17;

	/* Reports the new state as the number. */
	return model_random_state;
}

/* Mixes a value into the trace hash (FNV-1a over its eight bytes). */
static void
model_mix(
	uint64_t value)
{
	unsigned index;

	/* Folds each byte of the value into the hash. */
	for (index = 0; index < 8U; index++) {
		model_hash ^= (value >> (index * 8U)) & 0xffU;
		model_hash *= 1099511628211ULL;
	}
}

/* Returns a pointer's offset in the arena, or all ones for NULL. */
static uint64_t
model_offset(
	const void *pointer)
{
	/* NULL has no offset. */
	if (pointer == NULL)
		return UINT64_MAX;

	/* Reports the distance from the arena's start. */
	return (uint64_t)((const unsigned char *)pointer - model_arena);
}

/* Returns the monotonic clock in nanoseconds. */
static long long
model_now_ns(
	void)
{
	struct timespec time;

	/* Reads the clock. */
	clock_gettime(CLOCK_MONOTONIC, &time);

	/* Combines the seconds and nanoseconds. */
	return (long long)time.tv_sec * 1000000000LL + time.tv_nsec;
}

/*
 * Times COUNT small allocations that stay live, then frees every other one
 * and the rest, the pattern of a compiler building and dropping its trees.
 */
static int
model_bench(
	struct heap_allocator *heap,
	size_t count)
{
	void **pointers;
	long long start;
	long long allocated;
	size_t index;

	/* Allocates the table of live pointers. */
	pointers = calloc(count, sizeof(*pointers));
	if (pointers == NULL)
		return 0;

	/* Allocates every object, sizes from 16 to 271 bytes. */
	start = model_now_ns();
	for (index = 0; index < count; index++)
		pointers[index] = heap_allocator_alloc(heap, 16U + (index * 37U) % 256U);

	/* Notes when the allocations ended. */
	allocated = model_now_ns();

	/* Frees the odd objects first, then the even ones. */
	for (index = 1; index < count; index += 2)
		heap_allocator_free(heap, pointers[index]);
	for (index = 0; index < count; index += 2)
		heap_allocator_free(heap, pointers[index]);

	/* Prints both times and releases the table. */
	printf("alloc %zu: %.3f s, free: %.3f s, errors %zu\n", count, (allocated - start) / 1e9, (model_now_ns() - allocated) / 1e9, heap_allocator_error_count(heap));
	free(pointers);

	/* Reports whether the heap is whole again. */
	return heap_allocator_validate(heap) && heap_allocator_current(heap) == 0;
}

/*
 * Runs COUNT random operations over a table of live pointers: allocations of
 * every kind, reallocations, frees, and frees the allocator must refuse (a
 * double free, a pointer inside a block, a pointer outside the heap).
 */
static int
model_trace(
	struct heap_allocator *heap,
	size_t count)
{
	static void *slots[MODEL_SLOTS];
	static unsigned char outside[64];
	uint64_t choice;
	size_t index;
	size_t size;
	size_t alignment;
	unsigned slot;
	void *pointer;
	int valid;

	/* Applies one random operation at a time. */
	model_hash = 14695981039346656037ULL;
	for (index = 0; index < count; index++) {
		choice = model_random();
		slot = (unsigned)(choice % MODEL_SLOTS);
		size = (size_t)((choice >> 16) % 2048U);
		if ((choice >> 40) % 64U == 0)
			size = (size_t)((choice >> 16) % 200000U);

		/* Applies the operation the number names. */
		switch ((choice >> 32) % 10U) {
		case 0:
		case 1:
		case 2:
			/* Allocates into the slot, freeing what was there. */
			heap_allocator_free(heap, slots[slot]);
			slots[slot] = heap_allocator_alloc(heap, size);
			model_mix(model_offset(slots[slot]));
			break;
		case 3:
			/* Allocates zeroed memory into the slot. */
			heap_allocator_free(heap, slots[slot]);
			slots[slot] = heap_allocator_calloc(heap, 1U, size);
			model_mix(model_offset(slots[slot]));
			break;
		case 4:
			/* Allocates aligned memory into the slot. */
			heap_allocator_free(heap, slots[slot]);
			alignment = (size_t)1U << (4U + (choice >> 48) % 9U);
			slots[slot] = heap_allocator_aligned_alloc(heap, alignment, size);
			model_mix(model_offset(slots[slot]));
			break;
		case 5:
		case 6:
			/* Resizes the slot's memory. */
			pointer = heap_allocator_realloc(heap, slots[slot], size);
			if (pointer != NULL || size == 0)
				slots[slot] = pointer;

			/* Records where the memory is now. */
			model_mix(model_offset(pointer));
			break;
		case 7:
			/* Frees the slot. */
			heap_allocator_free(heap, slots[slot]);
			slots[slot] = NULL;
			break;
		case 8:
			/* Frees the slot twice; the allocator counts the second. */
			heap_allocator_free(heap, slots[slot]);
			heap_allocator_free(heap, slots[slot]);
			slots[slot] = NULL;
			break;
		default:
			/* Frees pointers that were never returned. */
			if (slots[slot] != NULL)
				heap_allocator_free(heap, (unsigned char *)slots[slot] + 16);

			/* Frees memory outside the heap. */
			heap_allocator_free(heap, outside);
			break;
		}

		/* Checks the whole heap now and then, and the error count always. */
		model_mix(heap_allocator_error_count(heap));
		valid = 1;
		if (index % 997U == 0)
			valid = heap_allocator_validate(heap);
		if (!valid) {
			printf("invalid heap after %zu operations\n", index);
			return 0;
		}
	}

	/* Frees what is left and checks the heap once more. */
	for (slot = 0; slot < MODEL_SLOTS; slot++)
		heap_allocator_free(heap, slots[slot]);

	/* Hashes the final accounting and prints the trace. */
	model_mix(heap_allocator_current(heap));
	model_mix(heap_allocator_error_count(heap));
	printf("trace %zu: hash %016llx errors %zu grown %zu\n", count, (unsigned long long)model_hash, heap_allocator_error_count(heap), model_arena_used);

	/* Reports whether the heap is whole again. */
	return heap_allocator_validate(heap) && heap_allocator_current(heap) == 0;
}
