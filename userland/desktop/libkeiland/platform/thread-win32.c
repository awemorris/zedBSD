/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A program's threads on Windows (<keiland/keiland_thread.h>): threads of the
 * C runtime (_beginthreadex), slim reader-writer locks taken exclusively for
 * the mutexes, condition variables, one-time initializations and fiber-local
 * storage, kept inside the room the interface gives them, and a record of
 * each thread the library allocates.
 *
 * The contract of every function is the header's.  What differs from POSIX:
 * there are no signals (nothing is blocked), a thread's name needs Windows 10
 * version 1607 (SetThreadDescription, looked up at run time; without it the
 * name is not set and the start still succeeds), a key's destroy runs once
 * (not up to PTHREAD_DESTRUCTOR_ITERATIONS) and kl_thread_key_delete runs it
 * for the values still set (the header leaves that unspecified).
 */

#include <keiland/keiland_thread.h>

#include <windows.h>
#include <process.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The room the interface gives holds the system's objects (each is one pointer). */
_Static_assert(sizeof(SRWLOCK) <= KL_THREAD_STORAGE, "a lock fits its room");
_Static_assert(_Alignof(SRWLOCK) <= _Alignof(struct kl_mutex), "a lock is aligned in its room");
_Static_assert(sizeof(CONDITION_VARIABLE) <= KL_THREAD_STORAGE, "a condition fits its room");
_Static_assert(_Alignof(CONDITION_VARIABLE) <= _Alignof(struct kl_cond), "a condition is aligned in its room");
_Static_assert(sizeof(INIT_ONCE) <= KL_THREAD_ONCE_STORAGE, "a once fits its room");
_Static_assert(_Alignof(INIT_ONCE) <= _Alignof(struct kl_once), "a once is aligned in its room");

/*
 * A thread the program started: the system's handle and identifier, what it
 * runs, its name and priority (set by the thread itself as it starts), and
 * how many still hold the record.
 *
 * owners starts at 2, the program's hold and the thread's own, as on POSIX:
 * the thread drops its hold as it ends (InterlockedDecrement) and
 * kl_thread_detach drops the program's after closing the handle; the one
 * that drops the last frees the record.  kl_thread_join waits for the
 * handle, closes it and frees the record (the thread's hold is gone by then).
 */
struct kl_thread {
	HANDLE handle;
	DWORD id;
	void (*run)(void *argument);
	void *argument;
	char name[KL_THREAD_NAME_MAX + 1U];
	unsigned priority;
	LONG owners;
};

/*
 * The value a key holds for one thread, as fiber-local storage keeps it.
 *
 * The system's callback is given the stored pointer alone, not the key, so
 * the key's destroy travels with each value: kl_thread_key_set allocates
 * this record the first time a thread sets the key (ENOMEM when it cannot)
 * and changes its value after; the callback (thread_key_destroyed, NTAPI)
 * calls destroy on a value that is not NULL and frees the record.  The key's
 * room keeps the index FlsAlloc gave and the destroy for the records made
 * after.
 */
struct thread_key_value {
	void (*destroy)(void *value);
	void *value;
};

/*
 * What a key's room holds: the fiber-local index and the destroy given to
 * kl_thread_key_create (NULL for none).
 */
struct thread_key {
	DWORD index;
	void (*destroy)(void *value);
};

/* A key's index and destroy fit the key's room. */
_Static_assert(sizeof(struct thread_key) <= KL_THREAD_KEY_STORAGE, "a key fits its room");
_Static_assert(_Alignof(struct thread_key) <= _Alignof(struct kl_thread_key), "a key is aligned in its room");

/* What kl_once hands the system's one-time initialization: the function to run. */
struct thread_once_call {
	void (*run)(void);
};

static unsigned __stdcall thread_main(void *argument);
static void thread_release(struct kl_thread *thread);
static void thread_set_name(const char *name);
static void thread_set_priority(unsigned priority);
static BOOL CALLBACK thread_once_run(PINIT_ONCE once, PVOID parameter, PVOID *context);
static void NTAPI thread_key_destroyed(PVOID stored);
static SRWLOCK *thread_lock(struct kl_mutex *mutex);
static CONDITION_VARIABLE *thread_condition(struct kl_cond *cond);
static struct thread_key *thread_key(struct kl_thread_key *key);

/*
 * Starts a thread under its name and priority.
 *
 * Win32: the record as on POSIX, then _beginthreadex (not CreateThread, so
 * that the C runtime is set up for the thread) suspended (CREATE_SUSPENDED)
 * with the stack size as a reservation (STACK_SIZE_PARAM_IS_A_RESERVATION;
 * 0 is the system's) and thread_main as the trampoline; the handle and the
 * identifier are written into the record before ResumeThread lets the
 * thread run, so that kl_thread_is_current inside it reads them.
 * _beginthreadex's failure (0, errno EAGAIN, EINVAL or EACCES) is EAGAIN;
 * the record is freed.  ResumeThread's failure closes the handle, frees the
 * record and is EAGAIN.  Nothing is blocked: Windows has no signals.
 */
int
kl_thread_start(
	const struct kl_thread_options *options,
	struct kl_thread **thread)
{
	/* Not written yet. */
	(void)options;
	(void)thread_main;
	*thread = NULL;
	return ENOSYS;
}

/*
 * Waits for a thread to end and frees its record.
 *
 * Win32: EDEADLK when the caller is the thread (GetCurrentThreadId() == id);
 * otherwise WaitForSingleObject(handle, INFINITE), CloseHandle and free.
 */
int
kl_thread_join(
	struct kl_thread *thread)
{
	/* Not written yet. */
	(void)thread;
	return ENOSYS;
}

/*
 * Lets a thread run on alone; its record goes when it ends.
 *
 * Win32: CloseHandle (the thread runs on), then the program's hold dropped
 * (thread_release).
 */
int
kl_thread_detach(
	struct kl_thread *thread)
{
	/* Not written yet. */
	(void)thread;
	return ENOSYS;
}

/*
 * Tells whether the caller is a thread (1) or not (0).
 *
 * Win32: GetCurrentThreadId() == thread->id.
 */
int
kl_thread_is_current(
	const struct kl_thread *thread)
{
	/* Not written yet. */
	(void)thread;
	return 0;
}

/*
 * Sleeps at least a number of milliseconds.
 *
 * Win32: Sleep ends on the system's tick and may return a little early, so
 * the rest is slept again until GetTickCount64 reaches the end; one Sleep
 * is at most UINT32_MAX - 1 (UINT32_MAX is INFINITE there).
 */
void
kl_thread_sleep(
	uint32_t milliseconds)
{
	/* Not written yet. */
	(void)milliseconds;
}

/*
 * Lets another thread run.
 *
 * Win32: SwitchToThread.
 */
void
kl_thread_yield(void)
{
	/* Not written yet. */
}

/*
 * Counts the processors the program's threads run on (at least 1).
 *
 * Win32: the processors of the caller's processor group
 * (GetCurrentProcessorNumberEx's Group, then GetActiveProcessorCount of it):
 * a thread runs in one group, so the count of every group would be too many.
 * 0 from the system is 1.
 */
unsigned
kl_thread_cpu_count(void)
{
	/* Not written yet. */
	return 1U;
}

/*
 * Makes a mutex.
 *
 * Win32: InitializeSRWLock in the room (an SRW lock taken exclusively does
 * not recurse, as the header's mutex).  It cannot fail: 0.
 */
int
kl_mutex_init(
	struct kl_mutex *mutex)
{
	/* Not written yet. */
	(void)mutex;
	return ENOSYS;
}

/*
 * Ends a mutex.
 *
 * Win32: an SRW lock needs no ending (nothing to do).
 */
void
kl_mutex_destroy(
	struct kl_mutex *mutex)
{
	/* Not written yet. */
	(void)mutex;
}

/*
 * Locks a mutex.
 *
 * Win32: AcquireSRWLockExclusive.  KL_MUTEX_INIT's zero bytes are
 * SRWLOCK_INIT.
 */
void
kl_mutex_lock(
	struct kl_mutex *mutex)
{
	/* Not written yet. */
	(void)mutex;
}

/*
 * Locks a mutex if no one holds it: 0, or EBUSY.
 *
 * Win32: TryAcquireSRWLockExclusive (nonzero: taken).
 */
int
kl_mutex_try(
	struct kl_mutex *mutex)
{
	/* Not written yet. */
	(void)mutex;
	return ENOSYS;
}

/*
 * Unlocks a mutex.
 *
 * Win32: ReleaseSRWLockExclusive.
 */
void
kl_mutex_unlock(
	struct kl_mutex *mutex)
{
	/* Not written yet. */
	(void)mutex;
}

/*
 * Makes a condition.
 *
 * Win32: InitializeConditionVariable in the room.  It cannot fail: 0.
 */
int
kl_cond_init(
	struct kl_cond *cond)
{
	/* Not written yet. */
	(void)cond;
	return ENOSYS;
}

/*
 * Ends a condition.
 *
 * Win32: a condition variable needs no ending (nothing to do).
 */
void
kl_cond_destroy(
	struct kl_cond *cond)
{
	/* Not written yet. */
	(void)cond;
}

/*
 * Waits on a condition, the mutex released while waiting.
 *
 * Win32: SleepConditionVariableSRW(condition, lock, INFINITE, 0) (0: the lock
 * is held exclusively).
 */
void
kl_cond_wait(
	struct kl_cond *cond,
	struct kl_mutex *mutex)
{
	/* Not written yet. */
	(void)cond;
	(void)mutex;
}

/*
 * Waits on a condition at most a number of milliseconds: 0, or ETIMEDOUT.
 *
 * Win32: SleepConditionVariableSRW takes the relative milliseconds as they
 * are (the system's wait does not follow the wall clock); UINT32_MAX is
 * INFINITE there, so UINT32_MAX - 1 is passed.  FALSE with GetLastError()
 * ERROR_TIMEOUT is ETIMEDOUT; TRUE is 0.
 */
int
kl_cond_wait_for(
	struct kl_cond *cond,
	struct kl_mutex *mutex,
	uint32_t milliseconds)
{
	/* Not written yet. */
	(void)cond;
	(void)mutex;
	(void)milliseconds;
	return ENOSYS;
}

/*
 * Wakes one waiter of a condition.
 *
 * Win32: WakeConditionVariable.
 */
void
kl_cond_signal(
	struct kl_cond *cond)
{
	/* Not written yet. */
	(void)cond;
}

/*
 * Wakes every waiter of a condition.
 *
 * Win32: WakeAllConditionVariable.
 */
void
kl_cond_broadcast(
	struct kl_cond *cond)
{
	/* Not written yet. */
	(void)cond;
}

/*
 * Runs a function once in the program; other callers wait for it to end.
 *
 * Win32: InitOnceExecuteOnce(room, thread_once_run, &call, NULL) with a
 * struct thread_once_call on the stack holding run (a function pointer is
 * not passed through a PVOID itself).  KL_ONCE_INIT's zero bytes are
 * INIT_ONCE_STATIC_INIT.
 */
void
kl_once(
	struct kl_once *once,
	void (*run)(void))
{
	/* Not written yet. */
	(void)once;
	(void)run;
}

/*
 * Makes a key for a value of each thread.
 *
 * Win32: FlsAlloc(thread_key_destroyed) (TlsAlloc never calls a destroy);
 * FLS_OUT_OF_INDEXES is EAGAIN.  The room keeps the index and destroy
 * (struct thread_key).
 */
int
kl_thread_key_create(
	struct kl_thread_key *key,
	void (*destroy)(void *value))
{
	/* Not written yet. */
	(void)key;
	(void)destroy;
	(void)thread_key_destroyed;
	return ENOSYS;
}

/*
 * Ends a key.
 *
 * Win32: FlsFree, which calls thread_key_destroyed for every thread's record
 * still set (their destroys run then; the header leaves that unspecified).
 */
void
kl_thread_key_delete(
	struct kl_thread_key *key)
{
	/* Not written yet. */
	(void)key;
}

/*
 * Sets the caller's value of a key: 0, or ENOMEM.
 *
 * Win32: the caller's struct thread_key_value (FlsGetValue), allocated and
 * stored (FlsSetValue) the first time (ENOMEM when malloc fails, and
 * FlsSetValue's failure frees it and is ENOMEM); its destroy is the key's
 * and its value the one given (NULL too: the record stays, destroy is not
 * called for NULL).
 */
int
kl_thread_key_set(
	struct kl_thread_key *key,
	void *value)
{
	/* Not written yet. */
	(void)key;
	(void)value;
	return ENOSYS;
}

/*
 * Gives the caller's value of a key (NULL when it set none).
 *
 * Win32: FlsGetValue's record's value, or NULL when there is no record.
 */
void *
kl_thread_key_get(
	struct kl_thread_key *key)
{
	/* Not written yet. */
	(void)key;
	return NULL;
}

/*
 * The trampoline of each thread: sets its name and priority, runs it, and
 * drops its hold on the record.  Returns 0 (the exit code nobody reads).
 */
static unsigned __stdcall
thread_main(
	void *argument)
{
	/* Not written yet. */
	(void)argument;
	(void)thread_release;
	(void)thread_set_name;
	(void)thread_set_priority;
	return 0U;
}

/* Drops one hold on a thread's record (InterlockedDecrement); the last frees it. */
static void
thread_release(
	struct kl_thread *thread)
{
	/* Not written yet. */
	(void)thread;
}

/*
 * Names the calling thread: the UTF-8 name made UTF-16
 * (MultiByteToWideChar(CP_UTF8)) for SetThreadDescription, looked up in
 * kernel32.dll with GetProcAddress (Windows 10 version 1607 and later);
 * without it, or on any failure, the thread keeps no name.
 */
static void
thread_set_name(
	const char *name)
{
	/* Not written yet. */
	(void)name;
}

/*
 * Sets the calling thread's priority, a hint: SetThreadPriority with
 * THREAD_PRIORITY_BELOW_NORMAL for KL_THREAD_PRIORITY_LOW and
 * THREAD_PRIORITY_ABOVE_NORMAL for KL_THREAD_PRIORITY_HIGH; a failure is
 * ignored.
 */
static void
thread_set_priority(
	unsigned priority)
{
	/* Not written yet. */
	(void)priority;
}

/* The one-time initialization's callback: runs the struct thread_once_call's function; TRUE. */
static BOOL CALLBACK
thread_once_run(
	PINIT_ONCE once,
	PVOID parameter,
	PVOID *context)
{
	/* Not written yet. */
	(void)once;
	(void)parameter;
	(void)context;
	return FALSE;
}

/* Fiber-local storage's callback: the record's destroy on its value (when not NULL), and the record freed. */
static void NTAPI
thread_key_destroyed(
	PVOID stored)
{
	/* Not written yet. */
	(void)stored;
	(void)thread_once_run;
	(void)thread_lock;
	(void)thread_condition;
	(void)thread_key;
}

/* The SRW lock in a mutex's room. */
static SRWLOCK *
thread_lock(
	struct kl_mutex *mutex)
{
	/* The room's first bytes. */
	return (SRWLOCK *)(void *)mutex->opaque.bytes;
}

/* The condition variable in a condition's room. */
static CONDITION_VARIABLE *
thread_condition(
	struct kl_cond *cond)
{
	/* The room's first bytes. */
	return (CONDITION_VARIABLE *)(void *)cond->opaque.bytes;
}

/* The index and destroy in a key's room. */
static struct thread_key *
thread_key(
	struct kl_thread_key *key)
{
	/* The room's first bytes. */
	return (struct thread_key *)(void *)key->opaque.bytes;
}
