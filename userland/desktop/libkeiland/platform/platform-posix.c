/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The library's operating system on zedBSD, Linux and FreeBSD (POSIX): the
 * wake is a pipe whose read end the dispatch polls, the lock a mutex, and
 * the threads POSIX threads.
 */

#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <unistd.h>

#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif

/* Whether the system keeps a thread's name (pthread_setname_np). */
#if defined(__linux__) || defined(__FreeBSD__)
#define PLATFORM_THREAD_NAMES	1
#else
#define PLATFORM_THREAD_NAMES	0
#endif

/*
 * Opens a wake: a pipe whose ends neither block nor pass to the programs
 * the application starts.
 */
int
keiui_wake_open(
	struct keiui_wake *wake)
{
	int status;

	/* The pipe. */
	status = pipe2(wake->ends, O_CLOEXEC | O_NONBLOCK);
	if (status != 0) {
		wake->ends[0] = -1;
		wake->ends[1] = -1;
		return errno;
	}

	/* Succeeded: the wake is quiet. */
	return 0;
}

/*
 * Closes a wake's ends.
 */
void
keiui_wake_close(
	struct keiui_wake *wake)
{
	/* Each end that is open. */
	if (wake->ends[0] >= 0)
		(void)close(wake->ends[0]);
	if (wake->ends[1] >= 0)
		(void)close(wake->ends[1]);
	wake->ends[0] = -1;
	wake->ends[1] = -1;
}

/*
 * Rings a wake from any thread: one byte, or none when the pipe is full
 * (it is rung already).
 */
void
keiui_wake_ring(
	struct keiui_wake *wake)
{
	const char byte = 1;
	ssize_t written;

	/* The byte; a full pipe (EAGAIN) is rung already, and an interrupted write is tried again. */
	do {
		written = write(wake->ends[1], &byte, 1);
	} while (written < 0 && errno == EINTR);
}

/*
 * Clears a wake: reads what was rung until the pipe is empty.
 */
void
keiui_wake_clear(
	struct keiui_wake *wake)
{
	char bytes[64];
	ssize_t got;

	/* Reads until the pipe is empty (EAGAIN) or broken. */
	for (;;) {
		got = read(wake->ends[0], bytes, sizeof(bytes));
		if (got > 0)
			continue;
		if (got < 0 && errno == EINTR)
			continue;
		break;
	}
}

/*
 * Reports the descriptor the dispatch polls for a wake.
 */
int
keiui_wake_descriptor(
	const struct keiui_wake *wake)
{
	/* The read end. */
	return wake->ends[0];
}

/*
 * Opens a lock.
 */
int
keiui_lock_open(
	struct keiui_lock *lock)
{
	int error;

	/* The mutex. */
	error = pthread_mutex_init(&lock->mutex, NULL);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Closes a lock nobody holds.
 */
void
keiui_lock_close(
	struct keiui_lock *lock)
{
	/* The mutex. */
	(void)pthread_mutex_destroy(&lock->mutex);
}

/*
 * Takes a lock, waiting while another thread holds it.
 */
void
keiui_lock_take(
	struct keiui_lock *lock)
{
	/* The mutex (it fails only for a lock never opened). */
	(void)pthread_mutex_lock(&lock->mutex);
}

/*
 * Gives a lock back.
 */
void
keiui_lock_give(
	struct keiui_lock *lock)
{
	/* The mutex. */
	(void)pthread_mutex_unlock(&lock->mutex);
}

/*
 * Reports the calling thread's identity.
 */
keiui_thread_id
keiui_thread_self(void)
{
	pthread_t self;

	/* Asks the system for the calling thread. */
	self = pthread_self();

	/* The calling POSIX thread. */
	return self;
}

/*
 * Reports whether the calling thread is the one named.
 */
int
keiui_thread_is(
	keiui_thread_id thread)
{
	pthread_t self;
	int same;

	/* The caller, compared. */
	self = pthread_self();
	same = pthread_equal(self, thread);
	if (same == 0)
		return 0;

	/* The same thread. */
	return 1;
}

/*
 * Starts a thread of the library's own that ends by itself (nobody joins
 * it).  The new thread starts with every signal blocked, so that none of the
 * program's signals runs on it and a write to a closed pipe returns EPIPE
 * there.  It names itself first (keiui_thread_name).
 */
int
keiui_thread_start(
	void *(*run)(void *),
	void *argument)
{
	pthread_attr_t attributes;
	sigset_t all;
	sigset_t kept;
	pthread_t thread;
	int error;

	/* Detached, so that it leaves nothing behind when it ends. */
	error = pthread_attr_init(&attributes);
	if (error != 0)
		return error;
	error = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	if (error != 0) {
		(void)pthread_attr_destroy(&attributes);
		return error;
	}

	/* Every signal blocked while it starts (it inherits the mask), then the caller's mask again. */
	(void)sigfillset(&all);
	(void)pthread_sigmask(SIG_SETMASK, &all, &kept);
	error = pthread_create(&thread, &attributes, run, argument);
	(void)pthread_sigmask(SIG_SETMASK, &kept, NULL);
	(void)pthread_attr_destroy(&attributes);
	if (error != 0)
		return error;

	/* Succeeded: the thread runs. */
	return 0;
}

/*
 * Starts a thread of the library's own that its owner waits for
 * (keiui_thread_join), with every signal blocked as keiui_thread_start's.
 */
int
keiui_thread_start_joined(
	void *(*run)(void *),
	void *argument,
	keiui_thread_id *thread)
{
	sigset_t all;
	sigset_t kept;
	int error;

	/* Every signal blocked while it starts, then the caller's mask again. */
	(void)sigfillset(&all);
	(void)pthread_sigmask(SIG_SETMASK, &all, &kept);
	error = pthread_create(thread, NULL, run, argument);
	(void)pthread_sigmask(SIG_SETMASK, &kept, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the thread runs. */
	return 0;
}

/*
 * Waits for a thread started by keiui_thread_start_joined to end.
 */
void
keiui_thread_join(
	keiui_thread_id thread)
{
	/* The thread's end. */
	(void)pthread_join(thread, NULL);
}

/*
 * Names the calling thread for the debugger, where the system keeps names.
 */
void
keiui_thread_name(
	const char *name)
{
	/* The name, or nothing on a system without names. */
#if PLATFORM_THREAD_NAMES
	(void)pthread_setname_np(pthread_self(), name);
#else
	(void)name;
#endif
}
