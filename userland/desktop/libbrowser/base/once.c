/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The calls made once in the process and the mutexes of static storage
 * (base/thread.h), with atomic operations instead of the system's once.
 *
 * The first thread to pass a once makes its call; a thread that passes it
 * meanwhile spins until the call returned.  The calls the engine makes
 * once are short (a text built, a mutex made), so the spin is brief.
 */

#include "base/thread.h"

#include <errno.h>
#include <stddef.h>

/* The states of a once: not made, being made, made. */
#define ONCE_NOT_MADE	0
#define ONCE_MAKING	1
#define ONCE_MADE	2

static void once_make_static_mutex(void *argument);

/*
 * Makes a call the first time a once is passed, and never again; a thread
 * that passes it while the call runs waits until it returned.
 */
void
wb_once_run(
	struct wb_once *once,
	void (*run)(void *argument),
	void *argument)
{
	int expected;
	int state;

	/* A call made before. */
	state = __atomic_load_n(&once->state, __ATOMIC_ACQUIRE);
	if (state == ONCE_MADE)
		return;

	/* The first thread here makes the call, then says it is made. */
	expected = ONCE_NOT_MADE;
	if (__atomic_compare_exchange_n(&once->state, &expected, ONCE_MAKING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		run(argument);
		__atomic_store_n(&once->state, ONCE_MADE, __ATOMIC_RELEASE);
		return;
	}

	/* Another thread makes it: the wait until it is made. */
	for (;;) {
		state = __atomic_load_n(&once->state, __ATOMIC_ACQUIRE);
		if (state == ONCE_MADE)
			break;
	}
}

/*
 * Takes a mutex of static storage, made the first time.  Returns 0, or
 * what making it reported (ENOTSUP before the program gave its threads,
 * ENOMEM); the mutex is then not held.
 */
int
wb_static_mutex_lock(
	struct wb_static_mutex *mutex)
{
	int ready;

	/* Before the program gave its threads the mutex cannot be made (nor is it spent on a failure). */
	ready = wb_threads_ready();
	if (!ready)
		return ENOTSUP;

	/* The mutex, made once. */
	wb_once_run(&mutex->once, once_make_static_mutex, mutex);
	if (mutex->error != 0)
		return mutex->error;

	/* Taken. */
	wb_mutex_lock(&mutex->mutex);
	return 0;
}

/*
 * Gives back a mutex of static storage the caller took.
 */
void
wb_static_mutex_unlock(
	struct wb_static_mutex *mutex)
{
	/* Given back. */
	wb_mutex_unlock(&mutex->mutex);
}

/* Makes the mutex of a static mutex record (its once's call). */
static void
once_make_static_mutex(
	void *argument)
{
	struct wb_static_mutex *mutex;

	/* The mutex, and what making it reported. */
	mutex = argument;
	mutex->error = wb_mutex_init(&mutex->mutex);
}
