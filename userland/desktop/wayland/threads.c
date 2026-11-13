/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The register of the compositor's threads and the way they are made.
 *
 * A thread is made with every signal blocked, so that SIGINT, SIGTERM and
 * SIGCHLD reach the main thread alone, and its name is kept in the
 * register until it is joined, or for a detached thread (one made for a
 * single job that may hang, which nobody waits for) until it ends.  The register is only read for the log, so
 * a thread that cannot be registered (the register full) still runs.
 *
 * The timers are the main thread's deadlines in a min-heap that only the
 * main thread touches; the timer thread is told the earliest deadline under
 * a lock and sleeps until it (or without a time limit while none is armed).
 * At the deadline it writes the main thread's wake pipe once, and the main
 * thread's take fires every timer that is due and tells the thread the
 * next deadline.
 */

#include "threads.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * The names of the threads running now, one slot each (an empty name is
 * a free slot).  register_lock guards them; a thread is put in at its
 * start and taken out at its join, both on the thread that starts it, or
 * by a detached thread itself as it ends.
 */
static pthread_mutex_t register_lock = PTHREAD_MUTEX_INITIALIZER;
static char register_names[KWL_THREADS_MAX][KWL_THREAD_NAME];

static void register_add(const char *name);
static void register_remove(const char *name);
static void heap_set(struct kwl_timers *timers, unsigned index, struct kwl_timer *timer);
static void heap_up(struct kwl_timers *timers, unsigned index);
static void heap_down(struct kwl_timers *timers, unsigned index);
static void heap_remove(struct kwl_timers *timers, struct kwl_timer *timer);
static void timers_publish(struct kwl_timers *timers, unsigned always);
static void *timer_run(void *argument);
static void timer_deadline(uint64_t due_ms, struct timespec *deadline);

/*
 * Starts a thread of the compositor's own under a name, with every signal
 * blocked.  Returns 0, or the errno value of pthread_create (the thread is
 * then not started and needs no join).
 */
int
kwl_thread_start(
	struct kwl_thread *thread,
	const char *name,
	void *(*run)(void *),
	void *argument)
{
	sigset_t all;
	sigset_t previous;
	int error;

	/* The record names the thread before it runs. */
	memset(thread, 0, sizeof(*thread));
	(void)snprintf(thread->name, sizeof(thread->name), "%s", name);

	/* The new thread inherits a mask that blocks every signal; this thread's own mask is put back after. */
	(void)sigfillset(&all);
	(void)pthread_sigmask(SIG_SETMASK, &all, &previous);
	error = pthread_create(&thread->thread, NULL, run, argument);
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	if (error != 0)
		return error;

	/* started tells the join that a thread is to be waited for; the register shows it until then. */
	thread->started = 1U;
	register_add(thread->name);

	/* Succeeded: the thread runs. */
	return 0;
}

/*
 * Starts a thread lent to the backend under a name, with every signal
 * blocked, and gives its handle: the backend joins it itself and then
 * calls kwl_thread_ended with the same name.  Returns 0, or the errno
 * value of pthread_create (the thread is then not started).
 */
int
kwl_thread_start_lent(
	const char *name,
	void *(*run)(void *),
	void *argument,
	pthread_t *handle)
{
	struct kwl_thread thread;
	int error;

	/* The thread, in the register under its name. */
	error = kwl_thread_start(&thread, name, run, argument);
	if (error != 0)
		return error;

	/* Succeeded: the backend holds the handle to join. */
	*handle = thread.thread;
	return 0;
}

/*
 * Starts a detached thread of the compositor's own under a name, with
 * every signal blocked: nobody joins it, and it calls kwl_thread_ended
 * with the same name as it ends.  Returns 0, or the errno value of
 * pthread_create (the thread is then not started).
 */
int
kwl_thread_start_detached(
	const char *name,
	void *(*run)(void *),
	void *argument)
{
	pthread_attr_t attributes;
	pthread_t thread;
	sigset_t all;
	sigset_t previous;
	int error;

	/* Detached: its end frees it. */
	error = pthread_attr_init(&attributes);
	if (error != 0)
		return error;
	(void)pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);

	/* Registered before it runs, so that its own end cannot come first. */
	register_add(name);

	/* The new thread inherits a mask that blocks every signal; this thread's own mask is put back after. */
	(void)sigfillset(&all);
	(void)pthread_sigmask(SIG_SETMASK, &all, &previous);
	error = pthread_create(&thread, &attributes, run, argument);
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	(void)pthread_attr_destroy(&attributes);
	if (error != 0) {
		register_remove(name);
		return error;
	}

	/* Succeeded: the thread runs until its job ends. */
	return 0;
}

/*
 * Takes a detached thread out of the register, on that thread as it ends.
 */
void
kwl_thread_ended(
	const char *name)
{
	/* Its slot. */
	register_remove(name);
}

/*
 * Waits for a thread that was started to end and takes it out of the
 * register.  A thread never started needs nothing.
 */
void
kwl_thread_join(
	struct kwl_thread *thread)
{
	/* Nothing was started. */
	if (!thread->started)
		return;

	/* The thread has ended once this returns. */
	(void)pthread_join(thread->thread, NULL);
	thread->started = 0U;
	register_remove(thread->name);
}

/*
 * Counts the threads in the register.
 */
unsigned
kwl_threads_count(void)
{
	unsigned count;
	unsigned slot;

	/* Every slot that holds a name. */
	pthread_mutex_lock(&register_lock);

	count = 0U;
	for (slot = 0U; slot < KWL_THREADS_MAX; slot++) {
		if (register_names[slot][0] != '\0')
			count++;
	}

	pthread_mutex_unlock(&register_lock);

	/* Reports the threads running. */
	return count;
}

/*
 * Prints the threads in the register on one line (KWL THREADS), in the
 * order of their slots.
 */
void
kwl_threads_report(void)
{
	char line[KWL_THREADS_MAX * KWL_THREAD_NAME];
	size_t used;
	unsigned count;
	unsigned slot;
	int written;

	/* The names, with a comma between two. */
	pthread_mutex_lock(&register_lock);

	line[0] = '\0';
	used = 0U;
	count = 0U;
	for (slot = 0U; slot < KWL_THREADS_MAX; slot++) {
		if (register_names[slot][0] == '\0')
			continue;

		/* A comma before every name but the first. */
		written = snprintf(line + used, sizeof(line) - used, "%s%s", count == 0U ? "" : ",", register_names[slot]);
		if (written < 0 || (size_t)written >= sizeof(line) - used)
			break;
		used += (size_t)written;
		count++;
	}

	pthread_mutex_unlock(&register_lock);

	/* The line the tests read. */
	printf("KWL THREADS count=%u names=%s\n", count, line);
}

/*
 * Counts the processors online, for the sizes of the pools: at least 1,
 * also when the system does not say.
 */
unsigned
kwl_cpu_count(void)
{
	long online;

	/* The processors the system runs now. */
	online = sysconf(_SC_NPROCESSORS_ONLN);
	if (online < 1L)
		return 1U;

	/* A count too large for the pools is as many as they could use anyway. */
	if (online > 1024L)
		return 1024U;

	/* Succeeded: the processors online. */
	return (unsigned)online;
}

/*
 * Writes one byte to the main thread's wake pipe, whose end does not
 * block: a full pipe already wakes the main thread, so the byte is not
 * needed then.  A descriptor below 0 (no pipe) is passed by.
 */
void
kwl_thread_wake(
	int descriptor)
{
	unsigned char byte;
	ssize_t written;

	/* No pipe to wake. */
	if (descriptor < 0)
		return;

	/* The byte, again only when a signal cut the write short. */
	byte = 1U;
	do {
		written = write(descriptor, &byte, 1U);
	} while (written < 0 && errno == EINTR);
}

/*
 * Readies a feature's timer: not armed, with its name (for the log) and
 * what it does when it is due.
 */
void
kwl_timer_init(
	struct kwl_timer *timer,
	const char *name,
	kwl_timer_fire_fn *fire)
{
	/* Not in the heap, never fired. */
	memset(timer, 0, sizeof(*timer));
	timer->name = name;
	timer->fire = fire;
}

/*
 * Tells whether a timer is armed (waits in the heap for its time).
 */
int
kwl_timer_armed(
	const struct kwl_timer *timer)
{
	/* A place in the heap means armed. */
	if (timer->place != 0U)
		return 1;

	/* Not armed. */
	return 0;
}

/*
 * Readies the timers with none armed: the lock and the condition the timer
 * thread waits on, on the monotonic clock.  Returns 0, or the errno value
 * of making them.
 */
int
kwl_timers_init(
	struct kwl_timers *timers)
{
	pthread_condattr_t attributes;
	int error;

	/* Nothing armed, nothing told to a thread, no thread. */
	memset(timers, 0, sizeof(*timers));
	timers->published = UINT64_MAX;
	timers->next_ms = UINT64_MAX;
	timers->wake = -1;

	/* The lock of what the thread shares. */
	error = pthread_mutex_init(&timers->lock, NULL);
	if (error != 0)
		return error;

	/* The thread's condition, whose timed waits count on the clock the deadlines are given in. */
	error = pthread_condattr_init(&attributes);
	if (error != 0) {
		pthread_mutex_destroy(&timers->lock);
		return error;
	}

	/* The monotonic clock, as kwl_milliseconds reads. */
	error = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	if (error == 0)
		error = pthread_cond_init(&timers->changed, &attributes);
	(void)pthread_condattr_destroy(&attributes);
	if (error != 0) {
		pthread_mutex_destroy(&timers->lock);
		return error;
	}

	/* ready tells stop and destroy that the lock and the condition exist. */
	timers->ready = 1U;

	/* Succeeded: timers may be armed; they fire once the thread runs. */
	return 0;
}

/*
 * Starts the timer thread, which writes wake (the main thread's wake pipe)
 * when the earliest armed timer is due.  Returns 0, or the errno value of
 * starting it (the timers then never fire on their own).
 */
int
kwl_timers_start(
	struct kwl_timers *timers,
	int wake)
{
	int error;

	/* Only readied timers, once. */
	if (!timers->ready)
		return EINVAL;
	if (timers->running)
		return 0;

	/* The pipe the thread writes, and nothing asked to stop. */
	pthread_mutex_lock(&timers->lock);

	timers->wake = wake;
	timers->stop = 0U;

	pthread_mutex_unlock(&timers->lock);

	/* The thread, under its name in the register. */
	error = kwl_thread_start(&timers->thread, "timer", timer_run, timers);
	if (error != 0)
		return error;

	/* running tells the event loop that the thread wakes it, and stop that a thread is to be joined. */
	timers->running = 1U;

	/* The thread is told the earliest of what was armed before it started. */
	timers_publish(timers, 1U);

	/* Succeeded: the thread waits for the earliest deadline. */
	return 0;
}

/*
 * Stops the timer thread and waits for it to end, and logs how often it
 * woke the main thread (KWL TIMER).  The timers stay armed, and do not fire
 * on their own any more.
 */
void
kwl_timers_stop(
	struct kwl_timers *timers)
{
	unsigned wakes;

	/* No thread runs. */
	if (!timers->running)
		return;

	/* The thread is told to end, from whatever wait it is in. */
	pthread_mutex_lock(&timers->lock);

	timers->stop = 1U;
	pthread_cond_signal(&timers->changed);

	pthread_mutex_unlock(&timers->lock);

	/* It has ended once this returns. */
	kwl_thread_join(&timers->thread);
	timers->running = 0U;

	/* Its count of wakes, read after it ended. */
	wakes = timers->wakes;
	printf("KWL TIMER stopped wakes=%u armed=%u\n", wakes, timers->count);
}

/*
 * Lets go of the lock and the condition of stopped timers.
 */
void
kwl_timers_destroy(
	struct kwl_timers *timers)
{
	/* Only readied ones, after their thread. */
	if (!timers->ready)
		return;
	kwl_timers_stop(timers);

	/* Their lock and condition. */
	pthread_cond_destroy(&timers->changed);
	pthread_mutex_destroy(&timers->lock);
	timers->ready = 0U;
}

/*
 * Arms a timer for due_ms (kwl_milliseconds' clock; a time past fires at
 * the next take), or moves it there when it is armed already.  The main
 * thread only.  Returns 0, or ENOSPC when the heap is full (the timer is
 * then not armed).
 */
int
kwl_timer_arm(
	struct kwl_timers *timers,
	struct kwl_timer *timer,
	uint64_t due_ms)
{
	uint64_t was;
	unsigned index;

	/* Armed already: it moves to its new time. */
	if (timer->place != 0U) {
		index = timer->place - 1U;
		was = timer->due_ms;
		timer->due_ms = due_ms;

		/* Earlier: towards the top; later (or the same): towards the bottom. */
		if (due_ms < was) {
			heap_up(timers, index);
		} else {
			heap_down(timers, index);
		}

		/* The thread hears a new earliest deadline. */
		timers_publish(timers, 0U);
		return 0;
	}

	/* No room: every feature's timer fits, so a full heap is a fault the caller reports. */
	if (timers->count >= KWL_TIMERS_MAX)
		return ENOSPC;

	/* The new one at the end of the heap, then up to its place. */
	timer->due_ms = due_ms;
	index = timers->count;
	timers->count++;
	heap_set(timers, index, timer);
	heap_up(timers, index);

	/* The thread hears a new earliest deadline. */
	timers_publish(timers, 0U);

	/* Succeeded: the timer fires at the first take at or after due_ms. */
	return 0;
}

/*
 * Disarms a timer (one not armed needs nothing).  The main thread only.
 */
void
kwl_timer_disarm(
	struct kwl_timers *timers,
	struct kwl_timer *timer)
{
	/* Not armed. */
	if (timer->place == 0U)
		return;

	/* Out of the heap; the thread hears a new earliest deadline. */
	heap_remove(timers, timer);
	timers_publish(timers, 0U);
}

/*
 * Fires the timers due at now_ms, earliest first, each with context, and
 * tells the timer thread the next deadline.  A timer fires once per take,
 * also when its handler arms it again for a time past (the next take fires
 * it).  The main thread only, after the wake.  Returns how many fired.
 */
unsigned
kwl_timers_take(
	struct kwl_timers *timers,
	uint64_t now_ms,
	void *context)
{
	struct kwl_timer *timer;
	unsigned fired;

	/* The thread's firing is taken: a later one writes the pipe again. */
	pthread_mutex_lock(&timers->lock);

	timers->fired = 0U;

	pthread_mutex_unlock(&timers->lock);

	/* This take's number, which a timer it fired remembers. */
	timers->take++;

	/* The earliest, while it is due and has not fired in this take. */
	fired = 0U;
	while (timers->count != 0U) {
		timer = timers->heap[0];
		if (timer->due_ms > now_ms)
			break;
		if (timer->taken == timers->take)
			break;

		/* Out of the heap before its handler, which may arm it again. */
		heap_remove(timers, timer);
		timer->taken = timers->take;
		fired++;
		timer->fire(context, timer);
	}

	/* The thread waits for the next deadline (also when it did not change: it waits for none since it fired). */
	timers_publish(timers, 1U);

	/* Reports the timers fired. */
	return fired;
}

/*
 * Gives the earliest armed deadline, or UINT64_MAX when none is armed.
 */
uint64_t
kwl_timers_next(
	const struct kwl_timers *timers)
{
	/* None armed. */
	if (timers->count == 0U)
		return UINT64_MAX;

	/* The heap's first. */
	return timers->heap[0]->due_ms;
}

/*
 * Reads the monotonic clock in milliseconds, the clock of the deadlines
 * (UINT64_MAX when it cannot be read).
 */
uint64_t
kwl_timers_now(
	void)
{
	struct timespec now;
	int error;

	/* The clock kwl_milliseconds reads. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return UINT64_MAX;

	/* Succeeded: whole milliseconds since the clock's start. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Puts a thread's name in a free slot of the register (a full register leaves it out). */
static void
register_add(
	const char *name)
{
	unsigned slot;

	/* The first free slot. */
	pthread_mutex_lock(&register_lock);

	for (slot = 0U; slot < KWL_THREADS_MAX; slot++) {
		if (register_names[slot][0] == '\0') {
			(void)snprintf(register_names[slot], sizeof(register_names[slot]), "%s", name);
			break;
		}
	}

	pthread_mutex_unlock(&register_lock);
}

/* Takes one slot with a thread's name out of the register. */
static void
register_remove(
	const char *name)
{
	unsigned slot;
	int same;

	/* The first slot of that name (threads of a pool have names of their own). */
	pthread_mutex_lock(&register_lock);

	for (slot = 0U; slot < KWL_THREADS_MAX; slot++) {
		same = strcmp(register_names[slot], name);
		if (same == 0) {
			register_names[slot][0] = '\0';
			break;
		}
	}

	pthread_mutex_unlock(&register_lock);
}

/* Puts a timer at an index of the heap and tells it its place. */
static void
heap_set(
	struct kwl_timers *timers,
	unsigned index,
	struct kwl_timer *timer)
{
	/* The slot and the timer's own note of it. */
	timers->heap[index] = timer;
	timer->place = index + 1U;
}

/* Moves the timer at an index towards the heap's top while it is earlier than its parent. */
static void
heap_up(
	struct kwl_timers *timers,
	unsigned index)
{
	struct kwl_timer *timer;
	struct kwl_timer *parent;
	unsigned above;

	/* Each step swaps it with a later parent. */
	timer = timers->heap[index];
	while (index != 0U) {
		above = (index - 1U) / 2U;
		parent = timers->heap[above];
		if (parent->due_ms <= timer->due_ms)
			break;

		/* The parent comes down, the timer goes up. */
		heap_set(timers, index, parent);
		index = above;
	}

	/* Its place. */
	heap_set(timers, index, timer);
}

/* Moves the timer at an index towards the heap's bottom while a child is earlier. */
static void
heap_down(
	struct kwl_timers *timers,
	unsigned index)
{
	struct kwl_timer *timer;
	unsigned child;

	/* Each step swaps it with its earlier child. */
	timer = timers->heap[index];
	for (;;) {
		child = index * 2U + 1U;
		if (child >= timers->count)
			break;

		/* The earlier of the two children. */
		if (child + 1U < timers->count && timers->heap[child + 1U]->due_ms < timers->heap[child]->due_ms)
			child++;

		/* The timer is no later than both: its place. */
		if (timer->due_ms <= timers->heap[child]->due_ms)
			break;

		/* The child goes up, the timer down. */
		heap_set(timers, index, timers->heap[child]);
		index = child;
	}

	/* Its place. */
	heap_set(timers, index, timer);
}

/* Takes an armed timer out of the heap; the last one fills its place. */
static void
heap_remove(
	struct kwl_timers *timers,
	struct kwl_timer *timer)
{
	struct kwl_timer *last;
	unsigned index;

	/* Its slot, and the heap one shorter. */
	index = timer->place - 1U;
	timer->place = 0U;
	timers->count--;

	/* The last one was it: nothing to fill. */
	if (index == timers->count)
		return;

	/* The last one fills the slot and finds its place from there. */
	last = timers->heap[timers->count];
	heap_set(timers, index, last);
	heap_up(timers, index);
	heap_down(timers, last->place - 1U);
}

/*
 * Tells the timer thread the earliest deadline when it is not the one told
 * last, or with always also when it is (after a firing, the thread waits
 * for none until it is told).
 */
static void
timers_publish(
	struct kwl_timers *timers,
	unsigned always)
{
	uint64_t next;

	/* The earliest, and nothing to do when the thread has it already. */
	next = kwl_timers_next(timers);
	if (!always && next == timers->published)
		return;
	timers->published = next;

	/* No thread to tell yet: start tells it. */
	if (!timers->running)
		return;

	/* The thread waits for the new deadline from now. */
	pthread_mutex_lock(&timers->lock);

	timers->next_ms = next;
	pthread_cond_signal(&timers->changed);

	pthread_mutex_unlock(&timers->lock);
}

/*
 * The timer thread: sleeps until the deadline it was told (without a time
 * limit while there is none), then writes the main thread's wake pipe once
 * and waits for the next deadline from the main thread's take.
 */
static void *
timer_run(
	void *argument)
{
	struct kwl_timers *timers;
	struct timespec deadline;
	uint64_t now;
	uint64_t next;
	unsigned first;
	int wake;

	/* The timers whose earliest deadline this thread keeps. */
	timers = argument;

	/* Each deadline in turn until the stop. */
	pthread_mutex_lock(&timers->lock);

	while (!timers->stop) {
		/* None: the next arm or take tells one. */
		next = timers->next_ms;
		if (next == UINT64_MAX) {
			pthread_cond_wait(&timers->changed, &timers->lock);
			continue;
		}

		/* Not yet: until then, or until a new deadline or the stop comes. */
		now = kwl_timers_now();
		if (now < next) {
			timer_deadline(next, &deadline);
			(void)pthread_cond_timedwait(&timers->changed, &timers->lock, &deadline);
			continue;
		}

		/*
		 * Due: the deadline is used up (the take tells the next), and the
		 * pipe gets a byte only for the first firing before the take.
		 */
		timers->next_ms = UINT64_MAX;
		first = 0U;
		if (!timers->fired)
			first = 1U;
		timers->fired = 1U;
		wake = timers->wake;
		if (first)
			timers->wakes++;

		/* The byte goes with the lock let go. */
		if (first) {
			pthread_mutex_unlock(&timers->lock);
			kwl_thread_wake(wake);
			pthread_mutex_lock(&timers->lock);
		}
	}

	pthread_mutex_unlock(&timers->lock);

	/* The thread ends. */
	return NULL;
}

/* Turns a deadline in milliseconds of the monotonic clock into the time a timed wait takes. */
static void
timer_deadline(
	uint64_t due_ms,
	struct timespec *deadline)
{
	/* Whole seconds, and the rest in nanoseconds. */
	deadline->tv_sec = (time_t)(due_ms / 1000U);
	deadline->tv_nsec = (long)((due_ms % 1000U) * 1000000U);
}
