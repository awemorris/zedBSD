/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What libkeiland asks of the operating system beyond its Wayland
 * connection: a wake one thread rings and the application's dispatch waits
 * for, a lock, the threads, and the processes the application starts.  The
 * public interface (<keiland/keiland.h>) shows none of it: no descriptor, no
 * process ID and no signal leaves the library.  platform-posix.c implements
 * it for the POSIX systems (zedBSD, Linux, FreeBSD) with POSIX threads;
 * platform-win32.c implements it for Windows with the system's own threads
 * and locks, without a POSIX thread layer, so on Windows this header does not
 * include <pthread.h>.  Nothing here leaves the library.
 */

#ifndef KEIUI_PLATFORM_H
#define KEIUI_PLATFORM_H

#ifndef _WIN32
#include <pthread.h>
#endif
#include <stddef.h>

/*
 * A wake: rung from any thread, waited for among the dispatch's
 * descriptors (keiui_wake_descriptor), and cleared by the thread that waits.
 * Ringing it again before it is cleared adds nothing.  On POSIX it is a
 * pipe (ends; event unused).  On Windows (platform-win32.c) it is a
 * manual-reset event object (event, a HANDLE; ends unused, -1): there is no
 * descriptor, keiui_wake_descriptor returns -1, and the dispatch waits for
 * keiui_wake_event with MsgWaitForMultipleObjectsEx.
 */
struct keiui_wake {
	int ends[2];
	void *event;
};

/*
 * A lock between the application's thread and the library's others.  On
 * POSIX it is a mutex.  On Windows it is a slim reader-writer lock taken
 * exclusively, kept in room (the system's SRWLOCK is one pointer, and this
 * header does not include <windows.h>).
 */
struct keiui_lock {
#ifdef _WIN32
	void *room;
#else
	pthread_mutex_t mutex;
#endif
};

/*
 * A process's side of the system (process-posix.c): its ID (0 before it
 * runs), the application's ends of its standard input, output and error
 * and of its terminal (-1 when it has none or they closed), the pipe that
 * wakes its reading thread, and whether its terminal is to hang up.
 */
struct keiui_child {
	long pid;
	int input;
	int output;
	int error;
	int terminal;
	int control[2];
	int hang_up;
};

#ifdef _WIN32
/*
 * A thread's identity on Windows: the system's number of the thread (a
 * DWORD), which tells threads apart while they live, and the handle of a
 * thread started by keiui_thread_start_joined (NULL for any other), which
 * keiui_thread_join waits for and closes.
 */
struct keiui_thread {
	unsigned long number;
	void *handle;
};
#endif

/* A thread's identity, for asking whether the caller is the application's thread. */
#ifdef _WIN32
typedef struct keiui_thread keiui_thread_id;
#else
typedef pthread_t keiui_thread_id;
#endif

/* The wake (platform-posix.c). */
int keiui_wake_open(struct keiui_wake *wake);
void keiui_wake_close(struct keiui_wake *wake);
void keiui_wake_ring(struct keiui_wake *wake);
void keiui_wake_clear(struct keiui_wake *wake);
int keiui_wake_descriptor(const struct keiui_wake *wake);
/*
 * The wake's event object, for a Windows dispatch's wait (platform-win32.c
 * only; the POSIX file does not define it).  NULL when the wake is not open.
 */
void *keiui_wake_event(const struct keiui_wake *wake);

/* The lock (platform-posix.c). */
int keiui_lock_open(struct keiui_lock *lock);
void keiui_lock_close(struct keiui_lock *lock);
void keiui_lock_take(struct keiui_lock *lock);
void keiui_lock_give(struct keiui_lock *lock);

/* The threads (platform-posix.c): the caller's identity, a thread of the library's own that nobody joins, and its name. */
keiui_thread_id keiui_thread_self(void);
int keiui_thread_is(keiui_thread_id thread);
int keiui_thread_start(void *(*run)(void *), void *argument);
int keiui_thread_start_joined(void *(*run)(void *), void *argument, keiui_thread_id *thread);
void keiui_thread_join(keiui_thread_id thread);
void keiui_thread_name(const char *name);

#endif
