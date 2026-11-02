/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The browser's threads for the engine (shell_threads): the table of
 * <browser/browser.h> over libkeiland's threads, which are the POSIX
 * threads on zedBSD and Linux and the Win32 threads on Windows, so that
 * one table serves every system the browser runs on.  The engine names
 * its threads and decides how many it has; this file only makes them.
 *
 * A mutex and a condition are libkeiland's records in a block of their own
 * (the engine keeps a pointer); a thread is libkeiland's handle.
 */

#include "shell/internal.h"

#include <browser/browser.h>

#include <errno.h>
#include <stdlib.h>

static int threads_start(void *context, const char *name, size_t stack_size, void (*run)(void *argument), void *argument, void **thread);
static void threads_join(void *context, void *thread);
static int threads_is_current(void *context, const void *thread);
static int threads_mutex_create(void *context, void **mutex);
static void threads_mutex_destroy(void *context, void *mutex);
static void threads_mutex_lock(void *context, void *mutex);
static void threads_mutex_unlock(void *context, void *mutex);
static int threads_cond_create(void *context, void **cond);
static void threads_cond_destroy(void *context, void *cond);
static void threads_cond_wait(void *context, void *cond, void *mutex);
static int threads_cond_wait_for(void *context, void *cond, void *mutex, uint32_t milliseconds);
static void threads_cond_signal(void *context, void *cond);
static void threads_cond_broadcast(void *context, void *cond);

/* The table, constant for the life of the program. */
static const struct browser_threads threads_table = {
	BROWSER_THREADS_VERSION,
	NULL,
	threads_start,
	threads_join,
	threads_is_current,
	threads_mutex_create,
	threads_mutex_destroy,
	threads_mutex_lock,
	threads_mutex_unlock,
	threads_cond_create,
	threads_cond_destroy,
	threads_cond_wait,
	threads_cond_wait_for,
	threads_cond_signal,
	threads_cond_broadcast
};

/*
 * Reports the browser's threads for the engine.
 */
const struct browser_threads *
shell_threads(void)
{
	/* The constant table. */
	return &threads_table;
}

/* Starts a thread with libkeiland (the table's start). */
static int
threads_start(
	void *context,
	const char *name,
	size_t stack_size,
	void (*run)(void *argument),
	void *argument,
	void **thread)
{
	struct kl_thread_options options;
	struct kl_thread *started;
	int error;

	UNUSED_PARAMETER(context);

	/* The engine's name and work, at the usual priority. */
	options.name = name;
	options.run = run;
	options.argument = argument;
	options.priority = KL_THREAD_PRIORITY_NORMAL;
	options.stack_size = stack_size;

	/* The thread. */
	error = kl_thread_start(&options, &started);
	if (error != 0)
		return error;

	/* Succeeded: handed over. */
	*thread = started;
	return 0;
}

/* Waits for a thread to end (the table's join). */
static void
threads_join(
	void *context,
	void *thread)
{
	UNUSED_PARAMETER(context);

	/* libkeiland's join (the engine never joins a thread from itself). */
	(void)kl_thread_join(thread);
}

/* Tells whether the caller is the thread (the table's is_current). */
static int
threads_is_current(
	void *context,
	const void *thread)
{
	int current;

	UNUSED_PARAMETER(context);

	/* libkeiland's answer. */
	current = kl_thread_is_current(thread);
	return current;
}

/* Makes a mutex (the table's mutex_create). */
static int
threads_mutex_create(
	void *context,
	void **mutex)
{
	struct kl_mutex *made;
	int error;

	UNUSED_PARAMETER(context);

	/* The block, then the mutex in it. */
	made = malloc(sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	error = kl_mutex_init(made);
	if (error != 0) {
		free(made);
		return ENOMEM;
	}

	/* Succeeded: handed over. */
	*mutex = made;
	return 0;
}

/* Lets a mutex go (the table's mutex_destroy). */
static void
threads_mutex_destroy(
	void *context,
	void *mutex)
{
	UNUSED_PARAMETER(context);

	/* The mutex, then its block. */
	kl_mutex_destroy(mutex);
	free(mutex);
}

/* Takes a mutex (the table's mutex_lock). */
static void
threads_mutex_lock(
	void *context,
	void *mutex)
{
	UNUSED_PARAMETER(context);

	/* libkeiland's lock. */
	kl_mutex_lock(mutex);
}

/* Gives a mutex back (the table's mutex_unlock). */
static void
threads_mutex_unlock(
	void *context,
	void *mutex)
{
	UNUSED_PARAMETER(context);

	/* libkeiland's unlock. */
	kl_mutex_unlock(mutex);
}

/* Makes a condition, timed on a clock that only goes forward (the table's cond_create). */
static int
threads_cond_create(
	void *context,
	void **cond)
{
	struct kl_cond *made;
	int error;

	UNUSED_PARAMETER(context);

	/* The block, then the condition in it. */
	made = malloc(sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	error = kl_cond_init(made);
	if (error != 0) {
		free(made);
		return ENOMEM;
	}

	/* Succeeded: handed over. */
	*cond = made;
	return 0;
}

/* Lets a condition go (the table's cond_destroy). */
static void
threads_cond_destroy(
	void *context,
	void *cond)
{
	UNUSED_PARAMETER(context);

	/* The condition, then its block. */
	kl_cond_destroy(cond);
	free(cond);
}

/* Waits on a condition (the table's cond_wait). */
static void
threads_cond_wait(
	void *context,
	void *cond,
	void *mutex)
{
	UNUSED_PARAMETER(context);

	/* libkeiland's wait. */
	kl_cond_wait(cond, mutex);
}

/* Waits on a condition at most so many milliseconds (the table's cond_wait_for). */
static int
threads_cond_wait_for(
	void *context,
	void *cond,
	void *mutex,
	uint32_t milliseconds)
{
	int status;

	UNUSED_PARAMETER(context);

	/* libkeiland's wait, which says ETIMEDOUT when the time ran out. */
	status = kl_cond_wait_for(cond, mutex, milliseconds);
	return status;
}

/* Wakes one waiter (the table's cond_signal). */
static void
threads_cond_signal(
	void *context,
	void *cond)
{
	UNUSED_PARAMETER(context);

	/* libkeiland's signal. */
	kl_cond_signal(cond);
}

/* Wakes every waiter (the table's cond_broadcast). */
static void
threads_cond_broadcast(
	void *context,
	void *cond)
{
	UNUSED_PARAMETER(context);

	/* libkeiland's broadcast. */
	kl_cond_broadcast(cond);
}
