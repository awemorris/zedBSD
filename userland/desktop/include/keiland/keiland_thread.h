/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The threads of a program, part of <keiland/keiland.h>.
 */

#ifndef KEILAND_THREAD_H
#define KEILAND_THREAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KL_VERSION 103: a thin layer over the system's threads (POSIX threads, or
 * the Win32 threads), the same on every system the desktop runs on: threads
 * started and joined, mutexes, conditions, a call made once, and a value of
 * each thread's own.  Atomic variables are C11's <stdatomic.h>, used as
 * they are.
 *
 * Every call may be made from any thread.  The window's calls (kl_app_*,
 * kl_window_*) stay on the application's thread: a thread hands its results
 * to it with kl_app_post or kl_window_post.
 *
 * A thread starts with the program's signals blocked where the system has
 * signals, so that they reach its first thread (a write to a closed pipe or
 * socket then returns EPIPE on it); the faults of its own instructions
 * (SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP) are not blocked.
 */
struct kl_thread;

/* The longest name a thread keeps, in bytes without its NUL; a longer one is cut. */
#define KL_THREAD_NAME_MAX	15U

/*
 * How much a thread is to be preferred (kl_thread_options' priority): as
 * any thread, less (work the user does not wait for, such as a scan), or
 * more (work that keeps time, such as sound).  It is a hint: a system that
 * cannot give it, or will not to this program, runs the thread as any.
 */
#define KL_THREAD_PRIORITY_NORMAL	0U
#define KL_THREAD_PRIORITY_LOW		1U
#define KL_THREAD_PRIORITY_HIGH	2U

/*
 * What a thread is started with: its name (for debuggers and the system's
 * lists; NULL for none), what it runs (it ends when run returns) and run's
 * argument, its priority (KL_THREAD_PRIORITY_*), and the size of its stack
 * in bytes (0: the system's; less than the system's least is the least).
 */
struct kl_thread_options {
	const char *name;
	void (*run)(void *argument);
	void *argument;
	unsigned priority;
	size_t stack_size;
};

/*
 * Starts a thread.  Returns 0 with *thread set, EINVAL (no run, or an
 * unknown priority), EAGAIN (the system's threads are used up) or ENOMEM.
 * A name or a priority the system cannot give is left out; the thread
 * still starts.  The thread is given back with kl_thread_join or
 * kl_thread_detach, exactly once.  On failure *thread is NULL.
 */
int kl_thread_start(const struct kl_thread_options *options, struct kl_thread **thread);

/*
 * Waits for a thread to end and gives it back.  Returns 0, or EDEADLK when
 * the thread is the caller (then nothing is given back).
 */
int kl_thread_join(struct kl_thread *thread);

/*
 * Gives a thread back without waiting: it runs on, and what the library
 * keeps of it goes when it ends.  Returns 0.
 */
int kl_thread_detach(struct kl_thread *thread);

/* Tells whether the caller is the thread (1) or not (0). */
int kl_thread_is_current(const struct kl_thread *thread);

/* Sleeps for at least so many milliseconds. */
void kl_thread_sleep(uint32_t milliseconds);

/* Lets another thread run. */
void kl_thread_yield(void);

/* The number of processors the program can run on (1 when the system does not tell). */
unsigned kl_thread_cpu_count(void);

/*
 * The room a mutex, a condition, a call made once and a thread's value
 * take, whatever the system: they live where the program puts them (in its
 * own records, or statically), and the library keeps the system's own
 * object inside.  The sizes are part of the interface.
 */
#define KL_THREAD_STORAGE	64U
#define KL_THREAD_ONCE_STORAGE	32U
#define KL_THREAD_KEY_STORAGE	16U

/* A mutex (not recursive: a thread that holds it does not lock it again). */
struct kl_mutex {
	union {
		unsigned char bytes[KL_THREAD_STORAGE];
		void *pointer;
		long long number;
		double real;
	} opaque;
};

/* A condition threads wait on, under a mutex. */
struct kl_cond {
	union {
		unsigned char bytes[KL_THREAD_STORAGE];
		void *pointer;
		long long number;
		double real;
	} opaque;
};

/* A call made once in the program's life (kl_once). */
struct kl_once {
	union {
		unsigned char bytes[KL_THREAD_ONCE_STORAGE];
		void *pointer;
		long long number;
	} opaque;
};

/* A value each thread keeps for itself (kl_thread_key_*). */
struct kl_thread_key {
	union {
		unsigned char bytes[KL_THREAD_KEY_STORAGE];
		void *pointer;
		long long number;
	} opaque;
};

/*
 * A mutex of static storage that needs neither kl_mutex_init nor
 * kl_mutex_destroy: static struct kl_mutex lock = KL_MUTEX_INIT;
 */
#define KL_MUTEX_INIT	{ { { 0 } } }

/* A call not made yet: static struct kl_once once = KL_ONCE_INIT; */
#define KL_ONCE_INIT	{ { { 0 } } }

/* Makes a mutex ready.  Returns 0, ENOMEM or EAGAIN. */
int kl_mutex_init(struct kl_mutex *mutex);

/* Lets a mutex nobody holds go. */
void kl_mutex_destroy(struct kl_mutex *mutex);

/* Takes a mutex, waiting while another thread holds it. */
void kl_mutex_lock(struct kl_mutex *mutex);

/* Takes a mutex if nobody holds it.  Returns 0, or EBUSY. */
int kl_mutex_try(struct kl_mutex *mutex);

/* Gives back a mutex the caller holds. */
void kl_mutex_unlock(struct kl_mutex *mutex);

/*
 * Makes a condition ready.  Its waits time on a clock that only goes
 * forward (a change of the date does not move them).  Returns 0, ENOMEM or
 * EAGAIN.
 */
int kl_cond_init(struct kl_cond *cond);

/* Lets a condition nobody waits on go. */
void kl_cond_destroy(struct kl_cond *cond);

/*
 * Waits on a condition, giving back the mutex the caller holds while it
 * waits and taking it again before it returns.  A wait may end without a
 * signal: the caller checks what it waits for again.
 */
void kl_cond_wait(struct kl_cond *cond, struct kl_mutex *mutex);

/*
 * Waits on a condition as kl_cond_wait, at most so many milliseconds
 * (UINT32_MAX too is a limit, about 49 days).  Returns 0 (woken, or for
 * nothing), or ETIMEDOUT.
 */
int kl_cond_wait_for(struct kl_cond *cond, struct kl_mutex *mutex, uint32_t milliseconds);

/* Wakes one thread waiting on a condition. */
void kl_cond_signal(struct kl_cond *cond);

/* Wakes every thread waiting on a condition. */
void kl_cond_broadcast(struct kl_cond *cond);

/*
 * Calls run the first time a once is passed (KL_ONCE_INIT), and never
 * again: another thread that passes it meanwhile waits until run returned.
 */
void kl_once(struct kl_once *once, void (*run)(void));

/*
 * Makes a key for a value of each thread's own; destroy (NULL for none) is
 * called with a thread's value when the thread ends while its value is not
 * NULL (again, on some systems, while destroy leaves values set).  Returns
 * 0, EAGAIN (too many keys) or ENOMEM.
 */
int kl_thread_key_create(struct kl_thread_key *key, void (*destroy)(void *value));

/*
 * Lets a key go.  Whether destroy is called for the values threads still
 * hold depends on the system: the program lets go of them first.
 */
void kl_thread_key_delete(struct kl_thread_key *key);

/* Sets the calling thread's value.  Returns 0, or ENOMEM. */
int kl_thread_key_set(struct kl_thread_key *key, void *value);

/* The calling thread's value (NULL until it is set). */
void *kl_thread_key_get(struct kl_thread_key *key);

#ifdef __cplusplus
}
#endif

#endif
