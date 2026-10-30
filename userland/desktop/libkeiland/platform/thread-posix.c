/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A program's threads on zedBSD, Linux and FreeBSD (<keiland/keiland_thread.h>):
 * POSIX threads, mutexes and conditions kept inside the room the interface
 * gives them, and a record of each thread the library allocates.
 */

#include <keiland/keiland_thread.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

/* Whether the system keeps a thread's name (pthread_setname_np). */
#if defined(__linux__) || defined(__FreeBSD__)
#define THREAD_NAMES	1
#else
#define THREAD_NAMES	0
#endif

/*
 * How far a thread's niceness moves from the program's for a low and for a
 * high priority, where the system lets a thread have its own (Linux).
 */
#define THREAD_NICE_LOW		10
#define THREAD_NICE_HIGH	(-5)

/* The niceness the system allows at most (the lowest preference). */
#define THREAD_NICE_MAX		19

/* Nanoseconds in a second and in a millisecond. */
#define THREAD_NS_SECOND	1000000000L
#define THREAD_NS_MILLISECOND	1000000L

/* The room the interface gives holds the system's objects. */
_Static_assert(sizeof(pthread_mutex_t) <= KL_THREAD_STORAGE, "a mutex fits its room");
_Static_assert(_Alignof(pthread_mutex_t) <= _Alignof(struct kl_mutex), "a mutex is aligned in its room");
_Static_assert(sizeof(pthread_cond_t) <= KL_THREAD_STORAGE, "a condition fits its room");
_Static_assert(_Alignof(pthread_cond_t) <= _Alignof(struct kl_cond), "a condition is aligned in its room");
_Static_assert(sizeof(pthread_once_t) <= KL_THREAD_ONCE_STORAGE, "a once fits its room");
_Static_assert(_Alignof(pthread_once_t) <= _Alignof(struct kl_once), "a once is aligned in its room");
_Static_assert(sizeof(pthread_key_t) <= KL_THREAD_KEY_STORAGE, "a key fits its room");
_Static_assert(_Alignof(pthread_key_t) <= _Alignof(struct kl_thread_key), "a key is aligned in its room");

/*
 * A thread the program started: the system's thread, what it runs, its
 * name and priority (set by the thread itself as it starts), and how many
 * still hold the record.
 *
 * owners starts at 2, the program's hold and the thread's own.  The thread
 * drops its hold as it ends and kl_thread_detach drops the program's; the
 * one that drops the last frees the record.  kl_thread_join frees it after
 * the thread ended (its hold is dropped by then).
 */
struct kl_thread {
	pthread_t thread;
	void (*run)(void *argument);
	void *argument;
	char name[KL_THREAD_NAME_MAX + 1U];
	unsigned priority;
	atomic_uint owners;
};

static void *thread_main(void *argument);
static void thread_signals(sigset_t *blocked);
static void thread_release(struct kl_thread *thread);
static void thread_set_priority(unsigned priority);
static pthread_mutex_t *thread_mutex(struct kl_mutex *mutex);
static pthread_cond_t *thread_cond(struct kl_cond *cond);

/*
 * Starts a thread with every signal blocked, under its name and priority.
 */
int
kl_thread_start(
	const struct kl_thread_options *options,
	struct kl_thread **thread)
{
	struct kl_thread *record;
	pthread_attr_t attributes;
	sigset_t all;
	sigset_t kept;
	size_t stack_size;
	int error;

	/* Nothing started yet. */
	*thread = NULL;

	/* A thread runs something. */
	if (options == NULL || options->run == NULL)
		return EINVAL;

	/* A thread's priority is one the interface knows. */
	if (options->priority > KL_THREAD_PRIORITY_HIGH)
		return EINVAL;

	/* The record the program and the thread share. */
	record = calloc(1U, sizeof(*record));
	if (record == NULL)
		return ENOMEM;

	/* What the thread runs, its name (cut to what threads keep) and its priority. */
	record->run = options->run;
	record->argument = options->argument;

	/* The name is kept only when the program gave one. */
	if (options->name != NULL)
		(void)strncpy(record->name, options->name, KL_THREAD_NAME_MAX);

	/* The priority the thread sets for itself as it starts. */
	record->priority = options->priority;

	/* Held by the program and by the thread until each gives it back. */
	atomic_init(&record->owners, 2U);

	/* Prepares the thread's attributes. */
	error = pthread_attr_init(&attributes);
	if (error != 0) {
		free(record);
		return error;
	}

	/* The stack the program asked for (at least the system's least), or the system's. */
	stack_size = options->stack_size;
	if (stack_size != 0U && stack_size < (size_t)PTHREAD_STACK_MIN)
		stack_size = (size_t)PTHREAD_STACK_MIN;

	/* A stack size is set only when one was asked for. */
	if (stack_size != 0U) {
		error = pthread_attr_setstacksize(&attributes, stack_size);
		if (error != 0) {
			(void)pthread_attr_destroy(&attributes);
			free(record);
			return EINVAL;
		}
	}

	/* The program's signals blocked while it starts (it inherits the mask), then the caller's mask again. */
	thread_signals(&all);
	(void)pthread_sigmask(SIG_SETMASK, &all, &kept);
	error = pthread_create(&record->thread, &attributes, thread_main, record);
	(void)pthread_sigmask(SIG_SETMASK, &kept, NULL);
	(void)pthread_attr_destroy(&attributes);
	if (error != 0) {
		free(record);
		return error;
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
	pthread_t self;
	int same;
	int error;

	/* A thread does not wait for itself. */
	self = pthread_self();
	same = pthread_equal(self, thread->thread);
	if (same != 0)
		return EDEADLK;

	/* The thread's end; its own hold on the record is given back by then. */
	error = pthread_join(thread->thread, NULL);
	if (error != 0)
		return error;

	/* Succeeded: nothing of the thread is left. */
	free(thread);
	return 0;
}

/*
 * Lets a thread run on by itself; its record goes when it ends.
 */
int
kl_thread_detach(
	struct kl_thread *thread)
{
	/* The system forgets the thread when it ends. */
	(void)pthread_detach(thread->thread);

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
	pthread_t self;
	int same;

	/* The caller's identity against the thread's. */
	self = pthread_self();
	same = pthread_equal(self, thread->thread);
	if (same == 0)
		return 0;

	/* The same thread. */
	return 1;
}

/*
 * Sleeps for a number of milliseconds, sleeping the rest again after a
 * signal.
 */
void
kl_thread_sleep(
	uint32_t milliseconds)
{
	struct timespec wait;
	struct timespec left;
	int status;

	/* The whole wait. */
	wait.tv_sec = (time_t)(milliseconds / 1000U);
	wait.tv_nsec = (long)(milliseconds % 1000U) * THREAD_NS_MILLISECOND;

	/* Slept until the time is out; a signal leaves the rest to sleep. */
	for (;;) {
		status = nanosleep(&wait, &left);
		if (status == 0)
			break;
		if (errno != EINTR)
			break;
		wait = left;
	}
}

/*
 * Lets another thread run.
 */
void
kl_thread_yield(void)
{
	/* The processor for another thread. */
	(void)sched_yield();
}

/*
 * Counts the processors the program can run on.
 */
unsigned
kl_thread_cpu_count(void)
{
	long online;

	/* The processors online, or one when the system does not tell. */
	online = sysconf(_SC_NPROCESSORS_ONLN);
	if (online < 1L)
		return 1U;

	/* Succeeded: so many processors. */
	return (unsigned)online;
}

/*
 * Makes a mutex ready.
 */
int
kl_mutex_init(
	struct kl_mutex *mutex)
{
	pthread_mutex_t *system_mutex;
	int error;

	/* The system's mutex in the room, with its defaults (not recursive). */
	memset(mutex, 0, sizeof(*mutex));
	system_mutex = thread_mutex(mutex);
	error = pthread_mutex_init(system_mutex, NULL);
	if (error != 0)
		return error;

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
	pthread_mutex_t *system_mutex;

	/* The system's mutex. */
	system_mutex = thread_mutex(mutex);
	(void)pthread_mutex_destroy(system_mutex);
}

/*
 * Takes a mutex.
 */
void
kl_mutex_lock(
	struct kl_mutex *mutex)
{
	pthread_mutex_t *system_mutex;

	/* Held when this returns. */
	system_mutex = thread_mutex(mutex);
	(void)pthread_mutex_lock(system_mutex);
}

/*
 * Takes a mutex if it is free.
 */
int
kl_mutex_try(
	struct kl_mutex *mutex)
{
	pthread_mutex_t *system_mutex;
	int error;

	/* Held, or another thread holds it. */
	system_mutex = thread_mutex(mutex);
	error = pthread_mutex_trylock(system_mutex);
	if (error != 0)
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
	pthread_mutex_t *system_mutex;

	/* Free for another thread. */
	system_mutex = thread_mutex(mutex);
	(void)pthread_mutex_unlock(system_mutex);
}

/*
 * Makes a condition ready, timed on the monotonic clock.
 */
int
kl_cond_init(
	struct kl_cond *cond)
{
	pthread_condattr_t attributes;
	pthread_cond_t *system_cond;
	int error;

	/* Prepares the condition's attributes. */
	error = pthread_condattr_init(&attributes);
	if (error != 0)
		return error;

	/* Its waits time on the clock that only goes forward. */
	error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	if (error != 0) {
		(void)pthread_condattr_destroy(&attributes);
		return error;
	}

	/* The system's condition in the room. */
	memset(cond, 0, sizeof(*cond));
	system_cond = thread_cond(cond);
	error = pthread_cond_init(system_cond, &attributes);
	(void)pthread_condattr_destroy(&attributes);
	if (error != 0)
		return error;

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
	pthread_cond_t *system_cond;

	/* The system's condition. */
	system_cond = thread_cond(cond);
	(void)pthread_cond_destroy(system_cond);
}

/*
 * Waits on a condition.
 */
void
kl_cond_wait(
	struct kl_cond *cond,
	struct kl_mutex *mutex)
{
	pthread_cond_t *system_cond;
	pthread_mutex_t *system_mutex;

	/* Woken, or for nothing, with the mutex held again. */
	system_cond = thread_cond(cond);
	system_mutex = thread_mutex(mutex);
	(void)pthread_cond_wait(system_cond, system_mutex);
}

/*
 * Waits on a condition at most a number of milliseconds.
 */
int
kl_cond_wait_for(
	struct kl_cond *cond,
	struct kl_mutex *mutex,
	uint32_t milliseconds)
{
	struct timespec until;
	pthread_cond_t *system_cond;
	pthread_mutex_t *system_mutex;
	int error;

	/* The end of the wait on the monotonic clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &until);
	until.tv_sec += (time_t)(milliseconds / 1000U);
	until.tv_nsec += (long)(milliseconds % 1000U) * THREAD_NS_MILLISECOND;
	if (until.tv_nsec >= THREAD_NS_SECOND) {
		until.tv_sec++;
		until.tv_nsec -= THREAD_NS_SECOND;
	}

	/* Woken, or the time is out; the mutex is held again either way. */
	system_cond = thread_cond(cond);
	system_mutex = thread_mutex(mutex);
	error = pthread_cond_timedwait(system_cond, system_mutex, &until);
	if (error == ETIMEDOUT)
		return ETIMEDOUT;

	/* Succeeded: woken (or for nothing). */
	return 0;
}

/*
 * Wakes one waiting thread.
 */
void
kl_cond_signal(
	struct kl_cond *cond)
{
	pthread_cond_t *system_cond;

	/* One waiter, if any. */
	system_cond = thread_cond(cond);
	(void)pthread_cond_signal(system_cond);
}

/*
 * Wakes every waiting thread.
 */
void
kl_cond_broadcast(
	struct kl_cond *cond)
{
	pthread_cond_t *system_cond;

	/* All the waiters. */
	system_cond = thread_cond(cond);
	(void)pthread_cond_broadcast(system_cond);
}

/*
 * Makes a call once.
 */
void
kl_once(
	struct kl_once *once,
	void (*run)(void))
{
	pthread_once_t *system_once;

	/* run, the first time; the others wait for it to return. */
	system_once = (pthread_once_t *)(void *)once->opaque.bytes;
	(void)pthread_once(system_once, run);
}

/*
 * Makes a key for a value of each thread's own.
 */
int
kl_thread_key_create(
	struct kl_thread_key *key,
	void (*destroy)(void *value))
{
	pthread_key_t *system_key;
	int error;

	/* The system's key in the room. */
	memset(key, 0, sizeof(*key));
	system_key = (pthread_key_t *)(void *)key->opaque.bytes;
	error = pthread_key_create(system_key, destroy);
	if (error != 0)
		return error;

	/* Succeeded: every thread's value is NULL. */
	return 0;
}

/*
 * Lets a key go.
 */
void
kl_thread_key_delete(
	struct kl_thread_key *key)
{
	pthread_key_t *system_key;

	/* The system's key. */
	system_key = (pthread_key_t *)(void *)key->opaque.bytes;
	(void)pthread_key_delete(*system_key);
}

/*
 * Sets the calling thread's value of a key.
 */
int
kl_thread_key_set(
	struct kl_thread_key *key,
	void *value)
{
	pthread_key_t *system_key;
	int error;

	/* The caller's value. */
	system_key = (pthread_key_t *)(void *)key->opaque.bytes;
	error = pthread_setspecific(*system_key, value);
	if (error != 0)
		return ENOMEM;

	/* Succeeded: the value is the thread's. */
	return 0;
}

/*
 * Reads the calling thread's value of a key.
 */
void *
kl_thread_key_get(
	struct kl_thread_key *key)
{
	pthread_key_t *system_key;
	void *value;

	/* The caller's value, NULL until it is set. */
	system_key = (pthread_key_t *)(void *)key->opaque.bytes;
	value = pthread_getspecific(*system_key);

	/* The value. */
	return value;
}

/* Runs a started thread: names it, sets its priority, runs it, and gives back its hold. */
static void *
thread_main(
	void *argument)
{
	struct kl_thread *thread;
#if THREAD_NAMES
	pthread_t self;
#endif

	/* The thread's own record. */
	thread = argument;

	/* The name the system shows, where it keeps one. */
#if THREAD_NAMES
	if (thread->name[0] != '\0') {
		self = pthread_self();
		(void)pthread_setname_np(self, thread->name);
	}
#endif

	/* The priority asked for, as far as the system gives it. */
	if (thread->priority != KL_THREAD_PRIORITY_NORMAL)
		thread_set_priority(thread->priority);

	/* What the program runs on it. */
	thread->run(thread->argument);

	/* The thread's hold given back: a detached thread's record goes here. */
	thread_release(thread);

	/* Ended. */
	return NULL;
}

/*
 * Fills the signals a started thread blocks: every signal but the faults
 * the thread's own instructions raise, which a debugger or a sanitizer is
 * to see on the thread that faulted (a blocked fault is not caught, it
 * kills the program).
 */
static void
thread_signals(
	sigset_t *blocked)
{
	/* Everything, then the faults let through. */
	(void)sigfillset(blocked);
	(void)sigdelset(blocked, SIGSEGV);
	(void)sigdelset(blocked, SIGBUS);
	(void)sigdelset(blocked, SIGFPE);
	(void)sigdelset(blocked, SIGILL);
	(void)sigdelset(blocked, SIGTRAP);
}

/* Gives back one hold on a thread's record, freeing it with the last. */
static void
thread_release(
	struct kl_thread *thread)
{
	unsigned before;

	/*
	 * The holds count the program (until join or detach) and the thread
	 * (until it ends): the one that takes the count to 0 frees the record.
	 */
	before = atomic_fetch_sub_explicit(&thread->owners, 1U, memory_order_acq_rel);
	if (before == 1U)
		free(thread);
}

/* Moves the calling thread's preference as far as the system allows (Linux: its own niceness). */
static void
thread_set_priority(
	unsigned priority)
{
#if defined(__linux__)
	pid_t self;
	int nice;

	/* The thread's own niceness, starting from the program's. */
	self = (pid_t)syscall(SYS_gettid);
	errno = 0;
	nice = getpriority(PRIO_PROCESS, (id_t)self);
	if (nice == -1 && errno != 0)
		return;

	/* Less preferred, or more (which needs the right to; without it the thread stays as it is). */
	if (priority == KL_THREAD_PRIORITY_LOW) {
		nice += THREAD_NICE_LOW;
	} else {
		nice += THREAD_NICE_HIGH;
	}

	/* The niceness stays within what the system allows. */
	if (nice > THREAD_NICE_MAX)
		nice = THREAD_NICE_MAX;

	/* The calling thread's niceness set. */
	(void)setpriority(PRIO_PROCESS, (id_t)self, nice);
#else
	/* zedBSD and FreeBSD give the threads of a time-shared program no priority of their own. */
	(void)priority;
#endif
}

/* The system's mutex in a mutex's room. */
static pthread_mutex_t *
thread_mutex(
	struct kl_mutex *mutex)
{
	/* The room's first bytes. */
	return (pthread_mutex_t *)(void *)mutex->opaque.bytes;
}

/* The system's condition in a condition's room. */
static pthread_cond_t *
thread_cond(
	struct kl_cond *cond)
{
	/* The room's first bytes. */
	return (pthread_cond_t *)(void *)cond->opaque.bytes;
}
