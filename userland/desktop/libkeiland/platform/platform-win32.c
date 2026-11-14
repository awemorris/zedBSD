/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What libkeiland asks of Windows: the wake is a manual-reset event object
 * the dispatch waits on, the lock a slim reader-writer lock taken
 * exclusively, and the threads the system's own (CreateThread), identified by
 * their numbers.  No POSIX thread layer is used.
 */

#include "userland/desktop/libkeiland/platform/platform.h"

#include <windows.h>

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

/*
 * The most bytes of a thread's name the debugger is given; a longer name is
 * cut at a character boundary.
 */
#define PLATFORM_THREAD_NAME_MAX	63U

/* The bits that tell a UTF-8 byte inside a character (10xxxxxx) from the first of one. */
#define PLATFORM_UTF8_MASK		0xC0U
#define PLATFORM_UTF8_CONTINUATION	0x80U

/* The lock's room holds the system's lock (one pointer). */
_Static_assert(sizeof(SRWLOCK) <= sizeof(((struct keiui_lock *)0)->room), "a lock fits its room");
_Static_assert(_Alignof(SRWLOCK) <= _Alignof(void *), "a lock is aligned in its room");

/* The thread's number is the system's DWORD. */
_Static_assert(sizeof(DWORD) == sizeof(((keiui_thread_id *)0)->number), "a thread's number fits");

/* The system's call that names a thread, as GetProcAddress gives it. */
typedef HRESULT (WINAPI *platform_describe_function)(HANDLE thread, PCWSTR description);

/*
 * What a new thread runs: the function and its argument, handed from the
 * starting thread to the new one on the heap.  The new thread frees it before
 * it runs the function; when the thread does not start, the starting thread
 * frees it.
 */
struct platform_start {
	void *(*run)(void *);
	void *argument;
};

static int platform_wake_error(DWORD code);
static int platform_thread_error(DWORD code);
static int platform_thread_create(void *(*run)(void *), void *argument, HANDLE *handle, DWORD *number);
static DWORD WINAPI platform_thread_main(LPVOID parameter);
static SRWLOCK *platform_lock(struct keiui_lock *lock);

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
 * Opens a lock: a slim reader-writer lock, which needs no closing and never
 * fails to open.
 */
int
keiui_lock_open(
	struct keiui_lock *lock)
{
	SRWLOCK *slim;

	/* The lock in its room, free. */
	slim = platform_lock(lock);
	InitializeSRWLock(slim);

	/* Succeeded. */
	return 0;
}

/*
 * Closes a lock nobody holds: a slim reader-writer lock holds nothing to
 * release, so the room is only cleared.
 */
void
keiui_lock_close(
	struct keiui_lock *lock)
{
	/* Nothing of the system's to release. */
	lock->room = NULL;
}

/*
 * Takes a lock, waiting while another thread holds it.  The lock is not
 * recursive: the thread that holds it must not take it again.
 */
void
keiui_lock_take(
	struct keiui_lock *lock)
{
	SRWLOCK *slim;

	/* Taken exclusively. */
	slim = platform_lock(lock);
	AcquireSRWLockExclusive(slim);
}

/*
 * Gives a lock back.
 */
void
keiui_lock_give(
	struct keiui_lock *lock)
{
	SRWLOCK *slim;

	/* Released by the thread that took it. */
	slim = platform_lock(lock);
	ReleaseSRWLockExclusive(slim);
}

/*
 * Reports the calling thread's identity: its number, without a handle.
 */
keiui_thread_id
keiui_thread_self(void)
{
	keiui_thread_id self;

	/* The system's number of the calling thread. */
	self.number = GetCurrentThreadId();
	self.handle = NULL;

	/* The calling thread. */
	return self;
}

/*
 * Reports whether the calling thread is the one named, by the threads'
 * numbers.
 */
int
keiui_thread_is(
	keiui_thread_id thread)
{
	DWORD self;

	/* The caller, compared. */
	self = GetCurrentThreadId();
	if (self != thread.number)
		return 0;

	/* The same thread. */
	return 1;
}

/*
 * Starts a thread of the library's own that ends by itself (nobody joins
 * it): its handle is closed at once, and the thread runs on.  Windows has no
 * signals to block.  It names itself first (keiui_thread_name).  Without a
 * function to run nothing starts.
 */
int
keiui_thread_start(
	void *(*run)(void *),
	void *argument)
{
	HANDLE handle;
	DWORD number;
	int error;

	/* A thread needs something to run. */
	if (run == NULL)
		return EINVAL;

	/* The thread. */
	error = platform_thread_create(run, argument, &handle, &number);
	if (error != 0)
		return error;

	/* Nobody waits for it, so its handle is not kept. */
	(void)CloseHandle(handle);

	/* Succeeded: the thread runs. */
	return 0;
}

/*
 * Starts a thread of the library's own that its owner waits for
 * (keiui_thread_join), which keeps its handle until then.  Without a
 * function to run, or without a place to keep the identity, nothing starts.
 */
int
keiui_thread_start_joined(
	void *(*run)(void *),
	void *argument,
	keiui_thread_id *thread)
{
	HANDLE handle;
	DWORD number;
	int error;

	/* A thread needs something to run and an identity to be joined by. */
	if (run == NULL)
		return EINVAL;
	if (thread == NULL)
		return EINVAL;

	/* The thread. */
	error = platform_thread_create(run, argument, &handle, &number);
	if (error != 0)
		return error;

	/* Its identity, with the handle the join waits for. */
	thread->number = number;
	thread->handle = handle;

	/* Succeeded: the thread runs. */
	return 0;
}

/*
 * Waits for a thread started by keiui_thread_start_joined to end, then closes
 * its handle.  An identity without a handle names no thread to wait for.
 */
void
keiui_thread_join(
	keiui_thread_id thread)
{
	HANDLE handle;

	/* Only a joined thread has a handle. */
	handle = (HANDLE)thread.handle;
	if (handle == NULL)
		return;

	/* The thread's end, then its handle. */
	(void)WaitForSingleObject(handle, INFINITE);
	(void)CloseHandle(handle);
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
 * Starts a thread that runs the function with its argument: the pair goes to
 * the new thread on the heap (platform_thread_main frees it).  On success the
 * thread's handle and number are reported; on failure nothing runs and the
 * error number is returned: ENOMEM for no memory, EAGAIN when the system
 * cannot make another thread, EIO otherwise.
 */
static int
platform_thread_create(
	void *(*run)(void *),
	void *argument,
	HANDLE *handle,
	DWORD *number)
{
	struct platform_start *start;
	HANDLE created;
	DWORD code;

	/* What the new thread runs. */
	start = malloc(sizeof(*start));
	if (start == NULL)
		return ENOMEM;

	/* Hands the thread's function and its argument to the new thread. */
	start->run = run;
	start->argument = argument;

	/* The thread, with the default stack and security; it runs at once. */
	created = CreateThread(NULL, 0, platform_thread_main, start, 0, number);
	if (created == NULL) {
		code = GetLastError();
		free(start);
		return platform_thread_error(code);
	}

	/* Succeeded: the new thread owns the pair now. */
	*handle = created;
	return 0;
}

/*
 * A thread's first function: takes the function and argument off the heap,
 * frees the pair and runs the function.  What the function returns is not
 * kept (keiui_thread_join reports nothing).
 */
static DWORD WINAPI
platform_thread_main(
	LPVOID parameter)
{
	struct platform_start *start;
	void *(*run)(void *);
	void *argument;
	void *result;

	/* The pair, freed before the function runs. */
	start = (struct platform_start *)parameter;
	run = start->run;
	argument = start->argument;
	free(start);

	/* The function. */
	result = run(argument);
	(void)result;

	/* The thread ends. */
	return 0;
}

/*
 * The system's lock in a lock's room.
 */
static SRWLOCK *
platform_lock(
	struct keiui_lock *lock)
{
	/* The room is aligned and large enough for the lock (checked above). */
	return (SRWLOCK *)(void *)&lock->room;
}

/*
 * Turns a failure of the system's thread call into the error number a start
 * reports: no memory (ENOMEM), no room for another thread (EAGAIN, as POSIX
 * reports it), or another failure (EIO).
 */
static int
platform_thread_error(
	DWORD code)
{
	/* The codes that mean the system ran out of memory. */
	if (code == ERROR_NOT_ENOUGH_MEMORY)
		return ENOMEM;
	if (code == ERROR_OUTOFMEMORY)
		return ENOMEM;
	if (code == ERROR_COMMITMENT_LIMIT)
		return ENOMEM;

	/* The codes that mean the system cannot make another thread now. */
	if (code == ERROR_NO_SYSTEM_RESOURCES)
		return EAGAIN;
	if (code == ERROR_MAX_THRDS_REACHED)
		return EAGAIN;

	/* Any other failure. */
	return EIO;
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
