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
#include <limits.h>
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

/* The most milliseconds one Sleep is given (UINT32_MAX is INFINITE to the system). */
#define THREAD_SLEEP_MAX	((ULONGLONG)UINT32_MAX - 1U)

/* The UTF-16 units a thread's name takes in the system's call, with its end. */
#define THREAD_NAME_WIDE	(KL_THREAD_NAME_MAX + 2U)

/* The bits that tell a UTF-8 byte inside a character (10xxxxxx) from the first of one. */
#define THREAD_UTF8_MASK		0xC0U
#define THREAD_UTF8_CONTINUATION	0x80U

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

/* The system's call that names a thread, as GetProcAddress gives it. */
typedef HRESULT (WINAPI *thread_describe_function)(HANDLE thread, PCWSTR description);

/* What kl_once hands the system's one-time initialization: the function to run. */
struct thread_once_call {
	void (*run)(void);
};

static unsigned __stdcall thread_main(void *argument);
static void thread_release(struct kl_thread *thread);
static void thread_name_keep(char *kept, const char *name);
static void thread_set_name(const char *name);
static void thread_set_priority(unsigned priority);
static BOOL CALLBACK thread_once_run(PINIT_ONCE once, PVOID parameter, PVOID *context);
static void NTAPI thread_key_destroyed(PVOID stored);
static SRWLOCK *thread_lock(struct kl_mutex *mutex);
static CONDITION_VARIABLE *thread_condition(struct kl_cond *cond);
static INIT_ONCE *thread_once_room(struct kl_once *once);
static struct thread_key *thread_key(struct kl_thread_key *key);

/*
 * Starts a thread under its name and priority.
 *
 * The thread is made with the C runtime's _beginthreadex (not CreateThread,
 * so that the runtime is set up for it), suspended, with the stack size as a
 * reservation; it runs once its handle and identifier are in the record, so
 * that kl_thread_is_current reads them from inside the thread.  Windows has
 * no signals: nothing is blocked.
 */
int
kl_thread_start(
	const struct kl_thread_options *options,
	struct kl_thread **thread)
{
	struct kl_thread *record;
	uintptr_t created;
	unsigned identifier;
	unsigned stack_size;
	DWORD resumed;

	/* Nothing started yet. */
	*thread = NULL;

	/* A thread runs something, at a priority the interface knows. */
	if (options == NULL || options->run == NULL)
		return EINVAL;
	if (options->priority > KL_THREAD_PRIORITY_HIGH)
		return EINVAL;

	/* The record the program and the thread share. */
	record = calloc(1U, sizeof(*record));
	if (record == NULL)
		return ENOMEM;

	/* What the thread runs, its name (cut to what threads keep) and its priority. */
	record->run = options->run;
	record->argument = options->argument;
	if (options->name != NULL)
		thread_name_keep(record->name, options->name);
	record->priority = options->priority;

	/* Held by the program and by the thread until each gives it back. */
	record->owners = 2;

	/* The stack the program asked for; what the system cannot count in an unsigned is the most it counts. */
	stack_size = UINT_MAX;
	if ((unsigned long long)options->stack_size < (unsigned long long)UINT_MAX)
		stack_size = (unsigned)options->stack_size;

	/* The thread, held back until the record has its handle. */
	created = _beginthreadex(
		NULL,
		stack_size,
		thread_main,
		record,
		CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION,
		&identifier);
	if (created == 0U) {
		free(record);
		return EAGAIN;
	}

	/* The thread's handle and identifier, written before it can run. */
	record->handle = (HANDLE)created;
	record->id = identifier;

	/*
	 * Lets the thread run.  When that fails the thread never ran, so it is
	 * ended here and nothing of it is left.
	 */
	resumed = ResumeThread(record->handle);
	if (resumed == (DWORD)-1) {
		(void)TerminateThread(record->handle, 0U);
		(void)CloseHandle(record->handle);
		free(record);
		return EAGAIN;
	}

	/* Succeeded: the thread runs. */
	*thread = record;
	return 0;
}

/*
 * Waits for a thread to end and frees its record.
 */
int
kl_thread_join(
	struct kl_thread *thread)
{
	DWORD self;
	DWORD waited;

	/* A thread does not wait for itself. */
	self = GetCurrentThreadId();
	if (self == thread->id)
		return EDEADLK;

	/* The thread's end; its own hold on the record is given back before the handle is signalled. */
	waited = WaitForSingleObject(thread->handle, INFINITE);
	if (waited != WAIT_OBJECT_0)
		return EINVAL;

	/* The thread's handle goes with the record. */
	(void)CloseHandle(thread->handle);
	free(thread);

	/* Succeeded: nothing of the thread is left. */
	return 0;
}

/*
 * Lets a thread run on by itself; its record goes when it ends.
 */
int
kl_thread_detach(
	struct kl_thread *thread)
{
	/* The thread runs on; only the program's handle goes. */
	(void)CloseHandle(thread->handle);

	/* The program's hold given back: the record goes now if the thread ended, or as it ends. */
	thread_release(thread);

	/* Succeeded: the thread is the system's. */
	return 0;
}

/*
 * Tells whether the caller is a thread.
 */
int
kl_thread_is_current(
	const struct kl_thread *thread)
{
	DWORD self;

	/* The caller's identifier against the thread's. */
	self = GetCurrentThreadId();
	if (self != thread->id)
		return 0;

	/* The same thread. */
	return 1;
}

/*
 * Sleeps for at least a number of milliseconds.
 *
 * Sleep ends on the system's tick and may return a little early, so the time
 * is measured with the performance counter (which counts finer than a
 * millisecond) and the rest slept again until the counter has run the whole
 * time.
 */
void
kl_thread_sleep(
	uint32_t milliseconds)
{
	LARGE_INTEGER frequency;
	LARGE_INTEGER start;
	LARGE_INTEGER now;
	ULONGLONG ticks_per_second;
	ULONGLONG wanted;
	ULONGLONG elapsed;
	ULONGLONG left;
	ULONGLONG left_milliseconds;

	/* The counter's rate and where it stands; the rate is never 0 on Windows XP and later. */
	(void)QueryPerformanceFrequency(&frequency);
	(void)QueryPerformanceCounter(&start);
	ticks_per_second = (ULONGLONG)frequency.QuadPart;
	if (ticks_per_second == 0U)
		ticks_per_second = 1000U;

	/* The counter ticks the wait takes, rounded up (seconds apart from the rest so that the product cannot overflow). */
	wanted = ((ULONGLONG)milliseconds / 1000U) * ticks_per_second;
	wanted += (((ULONGLONG)milliseconds % 1000U) * ticks_per_second + 999U) / 1000U;

	/* Slept until the counter has run the whole wait; a Sleep that returned early leaves the rest to sleep. */
	for (;;) {
		(void)QueryPerformanceCounter(&now);
		elapsed = (ULONGLONG)(now.QuadPart - start.QuadPart);
		if (elapsed >= wanted)
			break;

		/* What is left in whole milliseconds, rounded up so that the last Sleep does not end early. */
		left = wanted - elapsed;
		left_milliseconds = (left / ticks_per_second) * 1000U;
		left_milliseconds += ((left % ticks_per_second) * 1000U + ticks_per_second - 1U) / ticks_per_second;
		if (left_milliseconds > THREAD_SLEEP_MAX)
			left_milliseconds = THREAD_SLEEP_MAX;
		Sleep((DWORD)left_milliseconds);
	}
}

/*
 * Lets another thread run.
 */
void
kl_thread_yield(void)
{
	/* The processor for another thread. */
	(void)SwitchToThread();
}

/*
 * Counts the processors the program can run on.
 *
 * A thread runs in one processor group, so the count is of the caller's
 * group: that of every group would be too many.  The process's affinity mask
 * narrows it when the system gives one (it does not when the process has
 * threads in several groups).
 */
unsigned
kl_thread_cpu_count(void)
{
	PROCESSOR_NUMBER number;
	DWORD count;
	DWORD_PTR process_mask;
	DWORD_PTR system_mask;
	BOOL got;
	unsigned allowed;

	/* The group the caller runs in. */
	GetCurrentProcessorNumberEx(&number);

	/* The processors of that group, or one when the system does not tell. */
	count = GetActiveProcessorCount(number.Group);
	if (count == 0UL)
		count = 1UL;

	/* The processors the process may use, when the system tells. */
	got = GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask);
	if (got != FALSE) {
		/* Counts the processors in the mask. */
		allowed = 0U;
		while (process_mask != 0U) {
			if ((process_mask & 1U) != 0U)
				allowed++;
			process_mask >>= 1;
		}

		/* A mask with fewer processors than the group has narrows the count. */
		if (allowed != 0U && allowed < (unsigned)count)
			count = (DWORD)allowed;
	}

	/* Succeeded: so many processors. */
	return (unsigned)count;
}

/*
 * Makes a mutex ready.
 *
 * A slim reader-writer lock taken exclusively does not recurse, as the
 * interface's mutex.  It cannot fail.
 */
int
kl_mutex_init(
	struct kl_mutex *mutex)
{
	/* The system's lock in the room, free. */
	memset(mutex, 0, sizeof(*mutex));
	InitializeSRWLock(thread_lock(mutex));

	/* Succeeded: the mutex is free. */
	return 0;
}

/*
 * Lets a mutex go.
 */
void
kl_mutex_destroy(
	struct kl_mutex *mutex)
{
	/* A slim reader-writer lock needs no ending. */
	(void)mutex;
}

/*
 * Takes a mutex.
 *
 * KL_MUTEX_INIT's zero bytes are a free lock.
 */
void
kl_mutex_lock(
	struct kl_mutex *mutex)
{
	/* Held when this returns. */
	AcquireSRWLockExclusive(thread_lock(mutex));
}

/*
 * Takes a mutex if it is free.
 */
int
kl_mutex_try(
	struct kl_mutex *mutex)
{
	BOOLEAN taken;

	/* Held, or another thread holds it. */
	taken = TryAcquireSRWLockExclusive(thread_lock(mutex));
	if (taken == FALSE)
		return EBUSY;

	/* Succeeded: the caller holds it. */
	return 0;
}

/*
 * Gives back a mutex.
 */
void
kl_mutex_unlock(
	struct kl_mutex *mutex)
{
	/* Free for another thread. */
	ReleaseSRWLockExclusive(thread_lock(mutex));
}

/*
 * Makes a condition ready.  It cannot fail.
 */
int
kl_cond_init(
	struct kl_cond *cond)
{
	/* The system's condition in the room, with nobody waiting. */
	memset(cond, 0, sizeof(*cond));
	InitializeConditionVariable(thread_condition(cond));

	/* Succeeded: nobody waits on it. */
	return 0;
}

/*
 * Lets a condition go.
 */
void
kl_cond_destroy(
	struct kl_cond *cond)
{
	/* A condition variable needs no ending. */
	(void)cond;
}

/*
 * Waits on a condition.
 */
void
kl_cond_wait(
	struct kl_cond *cond,
	struct kl_mutex *mutex)
{
	/* Woken, or for nothing, with the lock held again (taken exclusively: no flags). */
	(void)SleepConditionVariableSRW(thread_condition(cond), thread_lock(mutex), INFINITE, 0UL);
}

/*
 * Waits on a condition at most a number of milliseconds.
 *
 * The system counts the wait from now, so a change of the date does not move
 * it.  A wait that ends for any reason but the timeout is a wake.
 */
int
kl_cond_wait_for(
	struct kl_cond *cond,
	struct kl_mutex *mutex,
	uint32_t milliseconds)
{
	DWORD wait;
	BOOL woken;
	DWORD error;

	/* The wait in milliseconds; UINT32_MAX is INFINITE to the system, so one less. */
	wait = (DWORD)milliseconds;
	if (wait == INFINITE)
		wait = INFINITE - 1UL;

	/* Waits with the lock held again afterwards. */
	woken = SleepConditionVariableSRW(thread_condition(cond), thread_lock(mutex), wait, 0UL);
	if (woken != FALSE)
		return 0;

	/* Tells the timeout from the other ends of a wait. */
	error = GetLastError();
	if (error == ERROR_TIMEOUT)
		return ETIMEDOUT;

	/* Succeeded: the wait ended for another reason, which the caller takes as a wake that may be for nothing. */
	return 0;
}

/*
 * Wakes one waiter of a condition.
 */
void
kl_cond_signal(
	struct kl_cond *cond)
{
	/* One waiting thread runs again. */
	WakeConditionVariable(thread_condition(cond));
}

/*
 * Wakes every waiter of a condition.
 */
void
kl_cond_broadcast(
	struct kl_cond *cond)
{
	/* Every waiting thread runs again. */
	WakeAllConditionVariable(thread_condition(cond));
}

/*
 * Runs a function once in the program; other callers wait for it to end.
 *
 * The system's one-time initialization takes a pointer for the callback, but
 * a function pointer does not travel in one, so the function rides in a
 * record on the caller's stack.  KL_ONCE_INIT's zero bytes are a call not
 * made yet.
 */
void
kl_once(
	struct kl_once *once,
	void (*run)(void))
{
	struct thread_once_call call;

	/* The function the system's callback runs. */
	call.run = run;

	/* Run by the first caller; the others wait for it to end. */
	(void)InitOnceExecuteOnce(thread_once_room(once), thread_once_run, &call, NULL);
}

/*
 * Makes a key for a value of each thread.
 *
 * Fiber-local storage is used because thread-local storage never calls a
 * destroy.  Fails with EAGAIN when the system has no index left.
 */
int
kl_thread_key_create(
	struct kl_thread_key *key,
	void (*destroy)(void *value))
{
	struct thread_key *room;
	DWORD index;

	/* The index every thread's value is kept under. */
	index = FlsAlloc(thread_key_destroyed);
	if (index == FLS_OUT_OF_INDEXES)
		return EAGAIN;

	/* The index and the destroy kept in the key's room. */
	memset(key, 0, sizeof(*key));
	room = thread_key(key);
	room->index = index;
	room->destroy = destroy;

	/* Succeeded: the key is ready. */
	return 0;
}

/*
 * Ends a key.
 *
 * FlsFree runs the key's destroy for the values threads still hold (the
 * interface leaves that unspecified).
 */
void
kl_thread_key_delete(
	struct kl_thread_key *key)
{
	struct thread_key *room;

	/* The index goes, and with it the values under it. */
	room = thread_key(key);
	(void)FlsFree(room->index);
}

/*
 * Sets the caller's value of a key.
 *
 * The value rides in a small record, made the first time a thread sets the
 * key, so that the system's callback at the thread's end finds the destroy.
 */
int
kl_thread_key_set(
	struct kl_thread_key *key,
	void *value)
{
	struct thread_key *room;
	struct thread_key_value *record;
	BOOL stored;

	/* The record this thread keeps under the key, if it has one. */
	room = thread_key(key);
	record = FlsGetValue(room->index);

	/* The first time: a record carrying the key's destroy. */
	if (record == NULL) {
		record = malloc(sizeof(*record));
		if (record == NULL)
			return ENOMEM;
		record->destroy = room->destroy;
		record->value = NULL;

		/* Left with the system, which gives it back at the thread's end. */
		stored = FlsSetValue(room->index, record);
		if (stored == FALSE) {
			free(record);
			return ENOMEM;
		}
	}

	/* The caller's value (NULL too: the record stays, and destroy is not called for it). */
	record->value = value;

	/* Succeeded: the value is set. */
	return 0;
}

/*
 * Gives the caller's value of a key.
 */
void *
kl_thread_key_get(
	struct kl_thread_key *key)
{
	struct thread_key *room;
	struct thread_key_value *record;

	/* The record this thread keeps under the key, if it has one. */
	room = thread_key(key);
	record = FlsGetValue(room->index);
	if (record == NULL)
		return NULL;

	/* The thread's value. */
	return record->value;
}

/*
 * The trampoline of each thread: sets its name and priority, runs it, and
 * drops its hold on the record.  Returns 0 (the exit code nobody reads).
 */
static unsigned __stdcall
thread_main(
	void *argument)
{
	struct kl_thread *thread;

	/* The record the program started the thread with. */
	thread = argument;

	/* The name, if there is one, and the priority. */
	if (thread->name[0] != '\0')
		thread_set_name(thread->name);
	thread_set_priority(thread->priority);

	/* What the program asked the thread to run. */
	thread->run(thread->argument);

	/* The thread's hold given back: after this the record may be gone. */
	thread_release(thread);

	/* Succeeded: the thread ends. */
	return 0U;
}

/* Drops one hold on a thread's record; the last to drop frees it. */
static void
thread_release(
	struct kl_thread *thread)
{
	LONG remaining;

	/* One hold less; the one that makes it none frees the record. */
	remaining = InterlockedDecrement(&thread->owners);
	if (remaining != 0)
		return;

	/* Nobody holds the record any more. */
	free(thread);
}

/*
 * Copies a name to what threads keep: at most KL_THREAD_NAME_MAX bytes, cut
 * between two UTF-8 characters (a name cut inside one would not be valid).
 */
static void
thread_name_keep(
	char *kept,
	const char *name)
{
	size_t length;
	unsigned char byte;

	/* The name in bytes, cut to what threads keep. */
	length = strlen(name);
	if (length > KL_THREAD_NAME_MAX) {
		length = KL_THREAD_NAME_MAX;

		/* A cut in the middle of a character takes that character out. */
		byte = (unsigned char)name[length];
		if ((byte & THREAD_UTF8_MASK) == THREAD_UTF8_CONTINUATION) {
			/* Back over the character's continuation bytes. */
			while (length > 0U) {
				byte = (unsigned char)name[length - 1U];
				if ((byte & THREAD_UTF8_MASK) != THREAD_UTF8_CONTINUATION)
					break;
				length--;
			}

			/* And its leading byte. */
			if (length > 0U)
				length--;
		}
	}

	/* The name, ended. */
	memcpy(kept, name, length);
	kept[length] = '\0';
}

/*
 * Names the calling thread: the UTF-8 name made UTF-16 for
 * SetThreadDescription, which is looked up in kernel32.dll at run time
 * (Windows 10 version 1607 and later); without it, or on any failure, the
 * thread keeps no name.
 */
static void
thread_set_name(
	const char *name)
{
	HMODULE kernel;
	FARPROC found;
	thread_describe_function describe;
	WCHAR wide[THREAD_NAME_WIDE];
	int count;
	HRESULT status;

	/* The system's library that may have the call. */
	kernel = GetModuleHandleW(L"kernel32.dll");
	if (kernel == NULL)
		return;

	/* The call itself; an older system has none. */
	found = GetProcAddress(kernel, "SetThreadDescription");
	if (found == NULL)
		return;
	describe = (thread_describe_function)(void *)found;

	/* The name in UTF-16, which the call takes; a name that is not valid UTF-8 is left out. */
	count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, (int)THREAD_NAME_WIDE);
	if (count == 0)
		return;

	/* The calling thread's name; a failure leaves it without one. */
	status = describe(GetCurrentThread(), wide);
	(void)status;
}

/*
 * Sets the calling thread's priority, a hint: below normal for
 * KL_THREAD_PRIORITY_LOW and above normal for KL_THREAD_PRIORITY_HIGH; a
 * failure is ignored.
 */
static void
thread_set_priority(
	unsigned priority)
{
	/* Normal is what a thread starts with. */
	if (priority == KL_THREAD_PRIORITY_LOW) {
		(void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	} else if (priority == KL_THREAD_PRIORITY_HIGH) {
		(void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	}
}

/* The one-time initialization's callback: runs the struct thread_once_call's function; TRUE. */
static BOOL CALLBACK
thread_once_run(
	PINIT_ONCE once,
	PVOID parameter,
	PVOID *context)
{
	struct thread_once_call *call;

	(void)once;
	(void)context;

	/* The function kl_once was given. */
	call = parameter;
	call->run();

	/* Succeeded: the call is made, and the system records that. */
	return TRUE;
}

/*
 * Fiber-local storage's callback, at the end of a thread (or when the key is
 * let go): the record's destroy on its value (when not NULL), and the record
 * freed.  The value is taken out of the record first, as POSIX does, so that
 * the destroy finds the key without a value.
 */
static void NTAPI
thread_key_destroyed(
	PVOID stored)
{
	struct thread_key_value *record;
	void *value;

	/* A thread that set the key keeps a record; others have none. */
	record = stored;
	if (record == NULL)
		return;

	/* The value, out of the record before the destroy sees the key. */
	value = record->value;
	record->value = NULL;

	/* The key's destroy on the value it holds. */
	if (value != NULL && record->destroy != NULL)
		record->destroy(value);

	/* The record's end. */
	free(record);
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

/* The one-time initialization in a once's room. */
static INIT_ONCE *
thread_once_room(
	struct kl_once *once)
{
	/* The room's first bytes. */
	return (INIT_ONCE *)(void *)once->opaque.bytes;
}

/* The index and destroy in a key's room. */
static struct thread_key *
thread_key(
	struct kl_thread_key *key)
{
	/* The room's first bytes. */
	return (struct thread_key *)(void *)key->opaque.bytes;
}
