/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * malloc-fault: makes one allocation of the libraries fail, for the
 * allocation-failure test of run-faults.sh.  The program is linked with
 * -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc, so the libraries' calls
 * come here (the C library's own allocations do not).
 *
 *   WS335_FAIL_ALLOC=N    the N-th allocation (counted from 1) returns NULL
 *   WS335_COUNT_ALLOC=1   the count is printed on the standard error at exit
 *                         ("malloc-fault: <n> allocations")
 */

#include <stdio.h>
#include <stdlib.h>

/* The functions the linker's wrapping of the allocators calls into and out of. */
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *pointer, size_t size);

/* Prints the count at exit. */
static void fault_report(void);

/* Counts one allocation; returns 1 when it is the one to fail. */
static int fault_now(void);

/*
 * The number of allocations seen so far.
 *
 * It starts at zero, only ever grows, and lives for the whole run.
 */
static long fault_count;

/*
 * The number of the allocation to fail.
 *
 * 0 means none fails and -1 means the environment is not read yet; the first
 * allocation reads it, and it does not change afterwards.
 */
static long fault_at = -1;

/*
 * Wraps malloc, failing it when it is the chosen allocation.
 *
 * Returns the memory, or NULL for the allocation chosen to fail.
 */
void *
__wrap_malloc(
	size_t size)
{
	void *memory;
	int fail;

	/* Counts the allocation and learns whether it is the one to fail. */
	fail = fault_now();

	/* Fails the chosen allocation. */
	if (fail)
		return NULL;

	/* Passes every other allocation to the real allocator. */
	memory = __real_malloc(size);

	/* Reports what the real allocator returned. */
	return memory;
}

/*
 * Wraps calloc, failing it when it is the chosen allocation.
 *
 * Returns the memory, or NULL for the allocation chosen to fail.
 */
void *
__wrap_calloc(
	size_t count,
	size_t size)
{
	void *memory;
	int fail;

	/* Counts the allocation and learns whether it is the one to fail. */
	fail = fault_now();

	/* Fails the chosen allocation. */
	if (fail)
		return NULL;

	/* Passes every other allocation to the real allocator. */
	memory = __real_calloc(count, size);

	/* Reports what the real allocator returned. */
	return memory;
}

/*
 * Wraps realloc, failing it when it is the chosen allocation.
 *
 * Returns the memory, or NULL for the allocation chosen to fail.
 */
void *
__wrap_realloc(
	void *pointer,
	size_t size)
{
	void *memory;
	int fail;

	/* Counts the allocation and learns whether it is the one to fail. */
	fail = fault_now();

	/* Fails the chosen allocation. */
	if (fail)
		return NULL;

	/* Passes every other allocation to the real allocator. */
	memory = __real_realloc(pointer, size);

	/* Reports what the real allocator returned. */
	return memory;
}

/* Prints the count at exit. */
static void
fault_report(void)
{
	fprintf(stderr, "malloc-fault: %ld allocations\n", fault_count);
}

/* Counts one allocation; returns 1 when it is the one to fail. */
static int
fault_now(void)
{
	const char *text;
	const char *count_text;

	/* Reads the environment once, on the first allocation. */
	if (fault_at < 0) {
		text = getenv("WS335_FAIL_ALLOC");
		fault_at = 0;
		if (text != NULL)
			fault_at = atol(text);

		/* Asks for the count to be printed at exit. */
		count_text = getenv("WS335_COUNT_ALLOC");
		if (count_text != NULL)
			atexit(fault_report);
	}

	/* Counts this allocation. */
	fault_count++;

	/* Fails the chosen allocation and says so. */
	if (fault_at > 0 && fault_count == fault_at) {
		fprintf(stderr, "malloc-fault: allocation %ld fails\n", fault_count);
		return 1;
	}

	/* This allocation goes through. */
	return 0;
}
