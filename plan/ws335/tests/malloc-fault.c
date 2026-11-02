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

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *pointer, size_t size);

/* The allocations so far, and the one to fail (0: none; -1: not read yet). */
static long fault_count;
static long fault_at = -1;

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

	if (fault_at < 0) {
		text = getenv("WS335_FAIL_ALLOC");
		fault_at = text != NULL ? atol(text) : 0;
		if (getenv("WS335_COUNT_ALLOC") != NULL)
			atexit(fault_report);
	}
	fault_count++;
	if (fault_at > 0 && fault_count == fault_at) {
		fprintf(stderr, "malloc-fault: allocation %ld fails\n", fault_count);
		return 1;
	}
	return 0;
}

void *
__wrap_malloc(
	size_t size)
{
	return fault_now() ? NULL : __real_malloc(size);
}

void *
__wrap_calloc(
	size_t count,
	size_t size)
{
	return fault_now() ? NULL : __real_calloc(count, size);
}

void *
__wrap_realloc(
	void *pointer,
	size_t size)
{
	return fault_now() ? NULL : __real_realloc(pointer, size);
}
