/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What libkeiland asks of Windows: the wake is a manual-reset event object
 * the dispatch waits on, the lock a mutex and the threads POSIX threads of
 * the C library's pthread layer.
 */

#include "userland/desktop/libkeiland/platform/platform.h"

#include <windows.h>

#include <errno.h>
#include <pthread.h>
#include <stddef.h>

/*
 * The most bytes of a thread's name the debugger is given; a longer name is
 * cut at a character boundary.
 */
#define PLATFORM_THREAD_NAME_MAX	63U

/* The bits that tell a UTF-8 byte inside a character (10xxxxxx) from the first of one. */
#define PLATFORM_UTF8_MASK		0xC0U
#define PLATFORM_UTF8_CONTINUATION	0x80U

/* The system's call that names a thread, as GetProcAddress gives it. */
typedef HRESULT (WINAPI *platform_describe_function)(HANDLE thread, PCWSTR description);

static int platform_wake_error(DWORD code);

/*
 * Opens a wake: a manual-reset event object that starts quiet, has no name
 * and is not passed to the programs the application starts.
 */
int
keiui_wake_open(
	struct keiui_wake *wake)
{
	HANDLE event;
	DWORD code;

	/* The wake has no descriptors; only the event is its own. */
	wake->ends[0] = -1;
	wake->ends[1] = -1;
	wake->event = NULL;

	/* Manual reset, so that ringing again adds nothing; not signaled; no name; no security attributes, so not inheritable. */
	event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (event == NULL) {
		code = GetLastError();
		return platform_wake_error(code);
	}

	/* Succeeded: the wake is quiet. */
	wake->event = event;
	return 0;
}

/*
 * Closes a wake's event.
 */
void
keiui_wake_close(
	struct keiui_wake *wake)
{
	HANDLE event;

	/* The event, when it is open; closing a closed wake does nothing. */
	event = (HANDLE)wake->event;
	if (event != NULL)
		(void)CloseHandle(event);
	wake->event = NULL;
	wake->ends[0] = -1;
	wake->ends[1] = -1;
}

/*
 * Rings a wake from any thread: the event becomes signaled, and stays so
 * however often it is rung before it is cleared.
 */
void
keiui_wake_ring(
	struct keiui_wake *wake)
{
	HANDLE event;

	/* A wake that is not open cannot be rung. */
	event = (HANDLE)wake->event;
	if (event == NULL)
		return;

	/* Signals the event; every thread waiting for it is released. */
	(void)SetEvent(event);
}

/*
 * Clears a wake: the event becomes quiet again.
 */
void
keiui_wake_clear(
	struct keiui_wake *wake)
{
	HANDLE event;

	/* A wake that is not open has nothing to clear. */
	event = (HANDLE)wake->event;
	if (event == NULL)
		return;

	/* Makes the event not signaled; a manual-reset event stays signaled until this. */
	(void)ResetEvent(event);
}

/*
 * Reports the descriptor the dispatch polls for a wake: none, because on
 * Windows the dispatch waits for the event (keiui_wake_event).
 */
int
keiui_wake_descriptor(
	const struct keiui_wake *wake)
{
	(void)wake;

	/* There is no descriptor to poll. */
	return -1;
}

/*
 * Reports the event object of a wake, or NULL when the wake is not open.
 */
void *
keiui_wake_event(
	const struct keiui_wake *wake)
{
	/* The event the dispatch waits for. */
	return wake->event;
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

	/* Asks the thread layer for the calling thread. */
	self = pthread_self();

	/* The calling thread. */
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
 * it).  Windows has no signals to block.  It names itself first
 * (keiui_thread_name).  Without a function to run nothing starts.
 */
int
keiui_thread_start(
	void *(*run)(void *),
	void *argument)
{
	pthread_attr_t attributes;
	pthread_t thread;
	int error;

	/* A thread needs something to run. */
	if (run == NULL)
		return EINVAL;

	/* Detached, so that it leaves nothing behind when it ends. */
	error = pthread_attr_init(&attributes);
	if (error != 0)
		return error;
	error = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	if (error != 0) {
		(void)pthread_attr_destroy(&attributes);
		return error;
	}

	/* The thread. */
	error = pthread_create(&thread, &attributes, run, argument);
	(void)pthread_attr_destroy(&attributes);
	if (error != 0)
		return error;

	/* Succeeded: the thread runs. */
	return 0;
}

/*
 * Starts a thread of the library's own that its owner waits for
 * (keiui_thread_join).  Without a function to run, or without a place to
 * keep the identity, nothing starts.
 */
int
keiui_thread_start_joined(
	void *(*run)(void *),
	void *argument,
	keiui_thread_id *thread)
{
	int error;

	/* A thread needs something to run and an identity to be joined by. */
	if (run == NULL)
		return EINVAL;
	if (thread == NULL)
		return EINVAL;

	/* The thread. */
	error = pthread_create(thread, NULL, run, argument);
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
 * Names the calling thread for the debugger: the UTF-8 name made UTF-16 for
 * SetThreadDescription, which is looked up in kernel32.dll at run time
 * (Windows 10 version 1607 and later).  A name longer than the limit is cut
 * at a character boundary; without a name, without the call, or on any
 * failure the thread keeps the name it has.
 */
void
keiui_thread_name(
	const char *name)
{
	HMODULE kernel;
	FARPROC found;
	platform_describe_function describe;
	WCHAR wide[PLATFORM_THREAD_NAME_MAX + 1U];
	size_t length;
	int count;
	HANDLE self;
	HRESULT status;

	/* A thread without a name asked for nothing. */
	if (name == NULL)
		return;

	/* The system's library that may have the call. */
	kernel = GetModuleHandleW(L"kernel32.dll");
	if (kernel == NULL)
		return;

	/* The call itself; an older system has none. */
	found = GetProcAddress(kernel, "SetThreadDescription");
	if (found == NULL)
		return;
	describe = (platform_describe_function)(void *)found;

	/* The name's length, looking at one byte past the limit to learn whether it is longer. */
	length = 0U;
	while (length <= PLATFORM_THREAD_NAME_MAX && name[length] != '\0')
		length++;

	/* A longer name is cut, and the cut moves back out of a character it would split. */
	if (length > PLATFORM_THREAD_NAME_MAX) {
		length = PLATFORM_THREAD_NAME_MAX;
		while (length > 0U &&
		       ((unsigned char)name[length] & PLATFORM_UTF8_MASK) == PLATFORM_UTF8_CONTINUATION)
			length--;
	}

	/* The name in UTF-16, which the call takes; a name that is not valid UTF-8 is left out. */
	wide[0] = L'\0';
	if (length > 0U) {
		count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, (int)length, wide, (int)PLATFORM_THREAD_NAME_MAX);
		if (count <= 0)
			return;
		wide[count] = L'\0';
	}

	/* The calling thread's name; a failure leaves it as it was. */
	self = GetCurrentThread();
	status = describe(self, wide);
	(void)status;
}

/*
 * Turns a failure of the system's event call into the error number the
 * wake's open reports: the system ran out of memory (ENOMEM), or another
 * failure (EIO).
 */
static int
platform_wake_error(
	DWORD code)
{
	/* The codes that mean the system ran out of memory or of handles' room. */
	if (code == ERROR_NOT_ENOUGH_MEMORY)
		return ENOMEM;
	if (code == ERROR_OUTOFMEMORY)
		return ENOMEM;
	if (code == ERROR_NO_SYSTEM_RESOURCES)
		return ENOMEM;
	if (code == ERROR_COMMITMENT_LIMIT)
		return ENOMEM;

	/* Any other failure. */
	return EIO;
}
