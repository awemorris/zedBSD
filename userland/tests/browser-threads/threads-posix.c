/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The table of threads over the POSIX threads (threads-posix.h).
 *
 * A thread is a record of its own (the system's thread, what it runs and
 * its argument), freed when it is joined.  A mutex and a condition are the
 * system's objects in a block of their own.  The conditions wait on the
 * monotonic clock where the system lets a condition use it (not on
 * Windows' winpthreads, where they wait on the wall clock).  A name is
 * given to the system's thread on Linux, which keeps 15 bytes of it.
 */

/* pthread_setname_np is declared on Linux only when the C library is asked for its GNU facilities. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "userland/tests/browser-threads/threads-posix.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The most bytes of a name the system keeps (Linux's limit, without the NUL). */
#define POSIX_NAME_MAX		15U

/* How many nanoseconds make one second, and one millisecond. */
#define POSIX_NS_PER_SECOND	1000000000L
#define POSIX_NS_PER_MS		1000000L

/*
 * One thread the table started: the system's thread, what it runs and the
 * argument, and its name.  It lives from start until join frees it.
 */
struct posix_thread {
	pthread_t thread;
	void (*run)(void *argument);
	void *argument;
	char name[POSIX_NAME_MAX + 1U];
};

static int posix_start(void *context, const char *name, size_t stack_size, void (*run)(void *argument), void *argument, void **thread);
static void posix_join(void *context, void *thread);
static int posix_is_current(void *context, const void *thread);
static int posix_mutex_create(void *context, void **mutex);
static void posix_mutex_destroy(void *context, void *mutex);
static void posix_mutex_lock(void *context, void *mutex);
static void posix_mutex_unlock(void *context, void *mutex);
static int posix_cond_create(void *context, void **cond);
static void posix_cond_destroy(void *context, void *cond);
static void posix_cond_wait(void *context, void *cond, void *mutex);
static int posix_cond_wait_for(void *context, void *cond, void *mutex, uint32_t milliseconds);
static void posix_cond_signal(void *context, void *cond);
static void posix_cond_broadcast(void *context, void *cond);

/* The table, constant for the life of the program. */
static const struct browser_threads posix_table = {
	BROWSER_THREADS_VERSION,
	NULL,
	posix_start,
	posix_join,
	posix_is_current,
	posix_mutex_create,
	posix_mutex_destroy,
	posix_mutex_lock,
	posix_mutex_unlock,
	posix_cond_create,
	posix_cond_destroy,
	posix_cond_wait,
	posix_cond_wait_for,
	posix_cond_signal,
	posix_cond_broadcast
};

/* How many threads the table started and joined, counted with atomic adds (posix_counts). */
static unsigned long posix_started;
static unsigned long posix_joined;

static void *posix_thread_main(void *argument);
static clockid_t posix_clock(void);

/*
 * Reports the table over the POSIX threads.
 */
const struct browser_threads *
browser_threads_posix(void)
{
	/* The constant table. */
	return &posix_table;
}

/*
 * Counts the threads the table started and joined so far.
 */
void
browser_threads_posix_counts(
	unsigned long *started,
	unsigned long *joined)
{
	/* The two counters. */
	*started = __atomic_load_n(&posix_started, __ATOMIC_ACQUIRE);
	*joined = __atomic_load_n(&posix_joined, __ATOMIC_ACQUIRE);
}

/* Starts a thread (the table's start). */
static int
posix_start(
	void *context,
	const char *name,
	size_t stack_size,
	void (*run)(void *argument),
	void *argument,
	void **thread)
{
	struct posix_thread *made;
	pthread_attr_t attributes;
	size_t length;
	int error;

	(void)context;

	/* The record, with the name cut to what the system keeps. */
	made = calloc(1U, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	made->run = run;
	made->argument = argument;
	length = 0;
	while (name != NULL && length < POSIX_NAME_MAX && name[length] != '\0') {
		made->name[length] = name[length];
		length++;
	}
	made->name[length] = '\0';

	/* The attributes: the stack asked for (at least the system's least). */
	error = pthread_attr_init(&attributes);
	if (error != 0) {
		free(made);
		return ENOMEM;
	}
	if (stack_size != 0) {
		if (stack_size < (size_t)PTHREAD_STACK_MIN)
			stack_size = (size_t)PTHREAD_STACK_MIN;
		(void)pthread_attr_setstacksize(&attributes, stack_size);
	}

	/* The thread. */
	error = pthread_create(&made->thread, &attributes, posix_thread_main, made);
	(void)pthread_attr_destroy(&attributes);
	if (error != 0) {
		free(made);
		return EAGAIN;
	}

	/* Succeeded: counted, and handed over. */
	__atomic_add_fetch(&posix_started, 1UL, __ATOMIC_ACQ_REL);
	*thread = made;
	return 0;
}

/* Waits for a thread and frees its record (the table's join). */
static void
posix_join(
	void *context,
	void *thread)
{
	struct posix_thread *started;

	(void)context;

	/* The wait, then the record goes. */
	started = thread;
	(void)pthread_join(started->thread, NULL);
	free(started);
	__atomic_add_fetch(&posix_joined, 1UL, __ATOMIC_ACQ_REL);
}

/* Tells whether the caller is the thread (the table's is_current). */
static int
posix_is_current(
	void *context,
	const void *thread)
{
	const struct posix_thread *started;
	int same;

	(void)context;

	/* The system's comparison. */
	started = thread;
	same = pthread_equal(started->thread, pthread_self());
	if (same == 0)
		return 0;

	/* The caller is the thread. */
	return 1;
}

/* Makes a mutex (the table's mutex_create). */
static int
posix_mutex_create(
	void *context,
	void **mutex)
{
	pthread_mutex_t *made;
	int error;

	(void)context;

	/* The block, then the mutex in it. */
	made = malloc(sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	error = pthread_mutex_init(made, NULL);
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
posix_mutex_destroy(
	void *context,
	void *mutex)
{
	(void)context;

	/* The mutex, then its block. */
	(void)pthread_mutex_destroy(mutex);
	free(mutex);
}

/* Takes a mutex (the table's mutex_lock). */
static void
posix_mutex_lock(
	void *context,
	void *mutex)
{
	(void)context;

	/* The system's lock. */
	(void)pthread_mutex_lock(mutex);
}

/* Gives a mutex back (the table's mutex_unlock). */
static void
posix_mutex_unlock(
	void *context,
	void *mutex)
{
	(void)context;

	/* The system's unlock. */
	(void)pthread_mutex_unlock(mutex);
}

/* Makes a condition on the clock posix_clock names (the table's cond_create). */
static int
posix_cond_create(
	void *context,
	void **cond)
{
	pthread_condattr_t attributes;
	pthread_cond_t *made;
	int error;

	(void)context;

	/* The block. */
	made = malloc(sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* The condition, timed on the clock that only goes forward where it can be. */
	error = pthread_condattr_init(&attributes);
	if (error != 0) {
		free(made);
		return ENOMEM;
	}
#if !defined(_WIN32)
	(void)pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
#endif
	error = pthread_cond_init(made, &attributes);
	(void)pthread_condattr_destroy(&attributes);
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
posix_cond_destroy(
	void *context,
	void *cond)
{
	(void)context;

	/* The condition, then its block. */
	(void)pthread_cond_destroy(cond);
	free(cond);
}

/* Waits on a condition (the table's cond_wait). */
static void
posix_cond_wait(
	void *context,
	void *cond,
	void *mutex)
{
	(void)context;

	/* The system's wait. */
	(void)pthread_cond_wait(cond, mutex);
}

/* Waits on a condition at most so many milliseconds (the table's cond_wait_for). */
static int
posix_cond_wait_for(
	void *context,
	void *cond,
	void *mutex,
	uint32_t milliseconds)
{
	struct timespec deadline;
	int status;

	(void)context;

	/* The deadline on the condition's clock. */
	(void)clock_gettime(posix_clock(), &deadline);
	deadline.tv_sec += (time_t)(milliseconds / 1000U);
	deadline.tv_nsec += (long)(milliseconds % 1000U) * POSIX_NS_PER_MS;
	if (deadline.tv_nsec >= POSIX_NS_PER_SECOND) {
		deadline.tv_sec++;
		deadline.tv_nsec -= POSIX_NS_PER_SECOND;
	}

	/* The wait. */
	status = pthread_cond_timedwait(cond, mutex, &deadline);
	if (status == ETIMEDOUT)
		return ETIMEDOUT;

	/* Woken (or for nothing). */
	return 0;
}

/* Wakes one waiter (the table's cond_signal). */
static void
posix_cond_signal(
	void *context,
	void *cond)
{
	(void)context;

	/* The system's signal. */
	(void)pthread_cond_signal(cond);
}

/* Wakes every waiter (the table's cond_broadcast). */
static void
posix_cond_broadcast(
	void *context,
	void *cond)
{
	(void)context;

	/* The system's broadcast. */
	(void)pthread_cond_broadcast(cond);
}

/* The system's thread: names itself, then runs what the engine gave. */
static void *
posix_thread_main(
	void *argument)
{
	struct posix_thread *started;

	/* The name, where the system keeps one. */
	started = argument;
#if defined(__linux__)
	(void)pthread_setname_np(pthread_self(), started->name);
#endif

	/* The engine's work. */
	started->run(started->argument);
	return NULL;
}

/* The clock the conditions wait on: the monotonic one, except where a condition cannot use it. */
static clockid_t
posix_clock(void)
{
#if defined(_WIN32)
	/* winpthreads' conditions wait on the wall clock. */
	return CLOCK_REALTIME;
#else
	/* The clock that only goes forward. */
	return CLOCK_MONOTONIC;
#endif
}
