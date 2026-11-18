/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The table of the program's threads (browser_threads_set) and the
 * engine's calls over it (base/thread.h).
 *
 * The table is copied once, before the engine makes anything that needs a
 * thread, and only read afterwards: thread_state says whether the copy is
 * there, and its release store publishes the copy to every thread that
 * reads the state with an acquire load.
 */

#include "base/thread.h"

#include <browser/browser.h>

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* The states of the table: none given, being copied, ready. */
#define THREAD_TABLE_NONE	0
#define THREAD_TABLE_COPYING	1
#define THREAD_TABLE_READY	2

/*
 * The program's table, copied by browser_threads_set and never changed
 * afterwards; read only once thread_state is THREAD_TABLE_READY.
 */
static struct browser_threads thread_table;

/*
 * Whether the table was given: THREAD_TABLE_NONE until browser_threads_set,
 * THREAD_TABLE_COPYING while it copies, then THREAD_TABLE_READY for the
 * rest of the process.  Read and written only with atomic operations.
 */
static int thread_state;

static int thread_table_complete(const struct browser_threads *threads);

/*
 * Gives the engine the program's threads (<browser/browser.h>).
 */
int
browser_threads_set(
	const struct browser_threads *threads)
{
	int expected;
	int claimed;
	int complete;

	/* A table is needed. */
	if (threads == NULL)
		return EINVAL;

	/* A table of this version, with every call. */
	if (threads->version != BROWSER_THREADS_VERSION)
		return ENOTSUP;
	complete = thread_table_complete(threads);
	if (!complete)
		return ENOTSUP;

	/* Claims the right to keep this table: the first table given is the one kept. */
	expected = THREAD_TABLE_NONE;
	claimed = __atomic_compare_exchange_n(&thread_state, &expected, THREAD_TABLE_COPYING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	if (!claimed)
		return EBUSY;

	/* The copy, then published to every thread. */
	thread_table = *threads;
	__atomic_store_n(&thread_state, THREAD_TABLE_READY, __ATOMIC_RELEASE);

	/* Succeeded: the engine has the program's threads. */
	return 0;
}

/*
 * Tells whether the program gave its threads (1) or not (0).
 */
int
wb_threads_ready(void)
{
	int state;

	/* The state, as published by browser_threads_set. */
	state = __atomic_load_n(&thread_state, __ATOMIC_ACQUIRE);
	if (state != THREAD_TABLE_READY)
		return 0;

	/* The table is there. */
	return 1;
}

/*
 * Starts a thread of the engine's through the program's table.
 *
 * Returns 0 with the thread, ENOTSUP when the program gave no threads, or
 * what the program's start reported (EAGAIN, ENOMEM).
 */
int
wb_thread_start(
	const char *name,
	size_t stack_size,
	void (*run)(void *argument),
	void *argument,
	struct wb_thread *thread)
{
	void *handle;
	int ready;
	int error;

	/* Nothing started yet. */
	thread->handle = NULL;

	/* The program's threads are needed. */
	ready = wb_threads_ready();
	if (!ready)
		return ENOTSUP;

	/* The thread. */
	handle = NULL;
	error = thread_table.start(thread_table.context, name, stack_size, run, argument, &handle);
	if (error != 0)
		return error;

	/* Succeeded: the thread runs. */
	thread->handle = handle;
	return 0;
}

/*
 * Waits for a thread of the engine's to end and gives it back; one never
 * started (or joined already) is passed over.
 */
void
wb_thread_join(
	struct wb_thread *thread)
{
	/* A thread that is not running. */
	if (thread->handle == NULL)
		return;

	/* The wait, then the handle goes. */
	thread_table.join(thread_table.context, thread->handle);
	thread->handle = NULL;
}

/*
 * Tells whether the calling thread is a thread of the engine's (1) or not
 * (0, also for one not running).
 */
int
wb_thread_is_current(
	const struct wb_thread *thread)
{
	int current;

	/* A thread that is not running is not the caller. */
	if (thread->handle == NULL)
		return 0;

	/* The program's answer. */
	current = thread_table.is_current(thread_table.context, thread->handle);
	if (current == 0)
		return 0;

	/* The caller is the thread. */
	return 1;
}

/*
 * Makes a mutex.  Returns 0, ENOTSUP (no threads given) or ENOMEM; on
 * failure the mutex holds NULL.
 */
int
wb_mutex_init(
	struct wb_mutex *mutex)
{
	void *handle;
	int ready;
	int error;

	/* Nothing made yet. */
	mutex->handle = NULL;

	/* The program's threads are needed. */
	ready = wb_threads_ready();
	if (!ready)
		return ENOTSUP;

	/* The program's mutex. */
	handle = NULL;
	error = thread_table.mutex_create(thread_table.context, &handle);
	if (error != 0)
		return error;

	/* Succeeded: the mutex is made. */
	mutex->handle = handle;
	return 0;
}

/*
 * Lets a mutex nobody holds go; one never made is passed over.
 */
void
wb_mutex_destroy(
	struct wb_mutex *mutex)
{
	/* A mutex that was not made. */
	if (mutex->handle == NULL)
		return;

	/* The program's mutex goes. */
	thread_table.mutex_destroy(thread_table.context, mutex->handle);
	mutex->handle = NULL;
}

/*
 * Takes a mutex, waiting while another thread holds it.
 */
void
wb_mutex_lock(
	struct wb_mutex *mutex)
{
	/* The program's lock. */
	thread_table.mutex_lock(thread_table.context, mutex->handle);
}

/*
 * Gives back a mutex the caller holds.
 */
void
wb_mutex_unlock(
	struct wb_mutex *mutex)
{
	/* The program's unlock. */
	thread_table.mutex_unlock(thread_table.context, mutex->handle);
}

/*
 * Makes a condition.  Returns 0, ENOTSUP (no threads given) or ENOMEM; on
 * failure the condition holds NULL.
 */
int
wb_cond_init(
	struct wb_cond *cond)
{
	void *handle;
	int ready;
	int error;

	/* Nothing made yet. */
	cond->handle = NULL;

	/* The program's threads are needed. */
	ready = wb_threads_ready();
	if (!ready)
		return ENOTSUP;

	/* The program's condition. */
	handle = NULL;
	error = thread_table.cond_create(thread_table.context, &handle);
	if (error != 0)
		return error;

	/* Succeeded: the condition is made. */
	cond->handle = handle;
	return 0;
}

/*
 * Lets a condition nobody waits on go; one never made is passed over.
 */
void
wb_cond_destroy(
	struct wb_cond *cond)
{
	/* A condition that was not made. */
	if (cond->handle == NULL)
		return;

	/* The program's condition goes. */
	thread_table.cond_destroy(thread_table.context, cond->handle);
	cond->handle = NULL;
}

/*
 * Waits on a condition under the mutex the caller holds (a wait may end
 * without a signal).
 */
void
wb_cond_wait(
	struct wb_cond *cond,
	struct wb_mutex *mutex)
{
	/* The program's wait. */
	thread_table.cond_wait(thread_table.context, cond->handle, mutex->handle);
}

/*
 * Waits on a condition as wb_cond_wait, at most so many milliseconds on a
 * clock that only goes forward.  Returns 0 (woken, or for nothing) or
 * ETIMEDOUT.
 */
int
wb_cond_wait_for(
	struct wb_cond *cond,
	struct wb_mutex *mutex,
	uint32_t milliseconds)
{
	int status;

	/* The program's wait. */
	status = thread_table.cond_wait_for(thread_table.context, cond->handle, mutex->handle, milliseconds);
	if (status == ETIMEDOUT)
		return ETIMEDOUT;

	/* Woken before the limit. */
	return 0;
}

/*
 * Wakes one thread waiting on a condition.
 */
void
wb_cond_signal(
	struct wb_cond *cond)
{
	/* The program's signal. */
	thread_table.cond_signal(thread_table.context, cond->handle);
}

/*
 * Wakes every thread waiting on a condition.
 */
void
wb_cond_broadcast(
	struct wb_cond *cond)
{
	/* The program's broadcast. */
	thread_table.cond_broadcast(thread_table.context, cond->handle);
}

/* Tells whether a table has every call (1) or lacks one (0). */
static int
thread_table_complete(
	const struct browser_threads *threads)
{
	/* The threads. */
	if (threads->start == NULL || threads->join == NULL || threads->is_current == NULL)
		return 0;

	/* The mutexes. */
	if (threads->mutex_create == NULL || threads->mutex_destroy == NULL)
		return 0;
	if (threads->mutex_lock == NULL || threads->mutex_unlock == NULL)
		return 0;

	/* The conditions. */
	if (threads->cond_create == NULL || threads->cond_destroy == NULL)
		return 0;
	if (threads->cond_wait == NULL || threads->cond_wait_for == NULL)
		return 0;
	if (threads->cond_signal == NULL || threads->cond_broadcast == NULL)
		return 0;

	/* Every call is there. */
	return 1;
}
