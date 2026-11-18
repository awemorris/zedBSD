/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The engine's threads, mutexes and conditions, made through the table of
 * the program's threads (browser_threads_set of <browser/browser.h>): the
 * engine never makes a thread of the system's itself.  thread.c keeps the
 * table and wraps its calls; once.c makes the calls made once in the
 * process with atomic operations, which need no table.
 *
 * A mutex or a condition is a handle of the program's, kept in a small
 * record of the engine's; a record that was never made (or failed to be)
 * holds NULL, and destroying it does nothing.
 */

#ifndef KEILAND_BROWSER_THREAD_H
#define KEILAND_BROWSER_THREAD_H

#include <stddef.h>
#include <stdint.h>

/* A thread the engine started: the program's handle, NULL once it was joined (or before it started). */
struct wb_thread {
	void *handle;
};

/* A mutex of the engine's (not recursive): the program's handle, NULL before it is made. */
struct wb_mutex {
	void *handle;
};

/* A condition of the engine's, waited on under a wb_mutex: the program's handle, NULL before it is made. */
struct wb_cond {
	void *handle;
};

/*
 * A call made once in the process (wb_once_run): 0 before the call, 1 while
 * the first caller makes it, 2 once it was made.  Read and written only with
 * atomic operations.
 */
struct wb_once {
	int state;
};

/* A wb_once whose call was not made yet. */
#define WB_ONCE_INIT	{ 0 }

/*
 * A mutex of static storage, made the first time it is taken: the once
 * that makes it, the mutex, and what making it reported (0 when it exists).
 */
struct wb_static_mutex {
	struct wb_once once;
	struct wb_mutex mutex;
	int error;
};

/* A wb_static_mutex not made yet. */
#define WB_STATIC_MUTEX_INIT	{ WB_ONCE_INIT, { NULL }, 0 }

int wb_threads_ready(void);
int wb_thread_start(const char *name, size_t stack_size, void (*run)(void *argument), void *argument, struct wb_thread *thread);
void wb_thread_join(struct wb_thread *thread);
int wb_thread_is_current(const struct wb_thread *thread);
int wb_mutex_init(struct wb_mutex *mutex);
void wb_mutex_destroy(struct wb_mutex *mutex);
void wb_mutex_lock(struct wb_mutex *mutex);
void wb_mutex_unlock(struct wb_mutex *mutex);
int wb_cond_init(struct wb_cond *cond);
void wb_cond_destroy(struct wb_cond *cond);
void wb_cond_wait(struct wb_cond *cond, struct wb_mutex *mutex);
int wb_cond_wait_for(struct wb_cond *cond, struct wb_mutex *mutex, uint32_t milliseconds);
void wb_cond_signal(struct wb_cond *cond);
void wb_cond_broadcast(struct wb_cond *cond);
int wb_static_mutex_lock(struct wb_static_mutex *mutex);
void wb_static_mutex_unlock(struct wb_static_mutex *mutex);
void wb_once_run(struct wb_once *once, void (*run)(void *argument), void *argument);

#endif
