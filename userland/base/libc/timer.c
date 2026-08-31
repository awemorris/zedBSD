/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * POSIX process timers are kernel objects.  SIGEV_THREAD is implemented here
 * because creating a user thread is necessarily a libc operation.  The signal
 * handler only updates preallocated atomic state and wakes a private usync
 * word; allocation and pthread_create() are confined to timer_worker().
 */

#include "userland/base/libc/syscall.h"

#include <zedbsd/syscall.h>
#include <zedbsd/usync.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LIBC_TIMER_MAX 128U
#define LIBC_TIMER_SLOT_SHIFT 8U
#define LIBC_TIMER_GENERATION_SHIFT 16U
#define LIBC_TIMER_GENERATION_MASK 0xffffU
#define LIBC_TIMER_WAKE_SIGNAL __ZEDBSD_SIGEV_THREAD_SIGNAL

enum timer_slot_state {
	TIMER_SLOT_FREE = 0,
	TIMER_SLOT_ACTIVE = 1,
	TIMER_SLOT_DELETING = 2
};

struct libc_timer_slot {
	volatile uint32_t state;
	volatile uint32_t public_id;
	volatile uint32_t pending;
	volatile uint32_t handler_refs;
	uint32_t generation;
	timer_t kernel_id;
	void (*function)(union sigval);
	union sigval value;
	pthread_attr_t attributes;
};

struct timer_callback {
	uint32_t public_id;
	void (*function)(union sigval);
	union sigval value;
	pthread_attr_t attributes;
};

static struct libc_timer_slot timer_slots[LIBC_TIMER_MAX];
static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile uint32_t timer_wake_generation;
/*
 * Keep one worker after the first SIGEV_THREAD timer.  timer_delete() may
 * race a notification already queued by the kernel; removing the handler on
 * the last delete would expose that stale implementation signal to its
 * default disposition.  The signal is outside the public RT interval, so
 * retaining this service changes no application-visible handler or mask. */
static int timer_service_ready;
static struct sigaction timer_previous_action;

extern void __signal_restorer(void);

static intptr_t timer_syscall(uint32_t number, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d);
static int timer_service_start_locked(void);
static uint32_t timer_new_id(struct libc_timer_slot *record, unsigned slot);
static int timer_is_libc_id(timer_t timer);
static struct libc_timer_slot *timer_lookup_locked(timer_t timer);
static int timer_id_slot(uint32_t id, unsigned *slot);
static void timer_pending_increment(volatile uint32_t *pending);
static void timer_signal_handler(int signo, siginfo_t *information, void *context);
static int timer_callback_still_valid(uint32_t id);
static void *timer_callback_start(void *argument);
static void timer_dispatch_pending(unsigned slot, uint32_t id, uint32_t count);
static void timer_dispatch_slot(unsigned slot);
static void *timer_worker(void *argument);

/*
 * Implements the timer sigev thread fork child operation.
 */
void
__timer_sigev_thread_fork_child(
	void)
{
	unsigned slot;

	/*
 * POSIX timers are not inherited.  Only the calling thread survives, so
	 * discard the copied service state and invalidate every public ID.
	 * The fixed slot array intentionally needs no allocation or freeing
	 * here. */
	if (timer_service_ready) {
		(void)timer_syscall(ZEDBSD_SYS_sigaction,
				    LIBC_TIMER_WAKE_SIGNAL,
				    (uintptr_t)&timer_previous_action, 0, 0);
	}

	/* Process each element required by the operation. */
	for (slot = 0; slot < LIBC_TIMER_MAX; slot++) {
		__atomic_store_n(&timer_slots[slot].state, TIMER_SLOT_FREE,
				 __ATOMIC_RELAXED);
		__atomic_store_n(&timer_slots[slot].pending, 0,
				 __ATOMIC_RELAXED);
		__atomic_store_n(&timer_slots[slot].handler_refs, 0,
				 __ATOMIC_RELAXED);
		__atomic_store_n(&timer_slots[slot].public_id, 0,
				 __ATOMIC_RELAXED);
		timer_slots[slot].kernel_id = 0;
		timer_slots[slot].generation =
		    (timer_slots[slot].generation + 1U) &
		    LIBC_TIMER_GENERATION_MASK;
	}
	__atomic_store_n(&timer_wake_generation, 0, __ATOMIC_RELAXED);
	timer_service_ready = 0;
	timer_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
}

/*
 * Implements the timer create operation.
 */
int
timer_create(
	clockid_t clock,
	const struct sigevent *event,
	timer_t *result)
{
	int function_result;
	struct sigevent kernel_event;
	struct libc_timer_slot *record;
	timer_t kernel_id;
	uint32_t public_id;
	unsigned slot;
	int error;

	record = NULL;

	/* Handles the result availability. */
	if (result == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the event availability. */
	if (event != NULL && event->sigev_notify == SIGEV_SIGNAL &&
	    event->sigev_signo > SIGRTMAX) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the event availability. */
	if (event == NULL || event->sigev_notify != SIGEV_THREAD) {
		/* Computes the function result. */
		function_result = (int)timer_syscall(ZEDBSD_SYS_timer_create, clock,
					  (uintptr_t)event, (uintptr_t)result,
					  0);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the sigev notify function availability. */
	if (event->sigev_notify_function == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	(void)pthread_mutex_lock(&timer_lock);
	error = timer_service_start_locked();

	/* Handles an operation failure. */
	if (error != 0)
		goto fail_locked;

	/* Process each element required by the operation. */
	for (slot = 0; slot < LIBC_TIMER_MAX; slot++) {
		/* Handles a failed atomic load n operation. */
		if (__atomic_load_n(&timer_slots[slot].state,
				    __ATOMIC_RELAXED) == TIMER_SLOT_FREE) {
			record = &timer_slots[slot];
			break;
		}
	}

	/* Handles the record availability. */
	if (record == NULL) {
		error = EAGAIN;
		goto fail_locked;
	}
	public_id = timer_new_id(record, slot);
	memset(&kernel_event, 0, sizeof(kernel_event));
	kernel_event.sigev_notify = SIGEV_SIGNAL;
	kernel_event.sigev_signo = LIBC_TIMER_WAKE_SIGNAL;
	kernel_event.sigev_value.__sival_pad = public_id;

	/* Handles a failed timer syscall operation. */
	if (timer_syscall(ZEDBSD_SYS_timer_create, clock,
			  (uintptr_t)&kernel_event, (uintptr_t)&kernel_id,
			  0) < 0) {
		error = errno;
		goto fail_locked;
	}
	record->kernel_id = kernel_id;
	record->function = event->sigev_notify_function;
	record->value = event->sigev_value;

	/* Handles the sigev notify attributes availability. */
	if (event->sigev_notify_attributes != NULL)
		record->attributes = *event->sigev_notify_attributes;
	else
		(void)pthread_attr_init(&record->attributes);
	record->attributes.detachstate = PTHREAD_CREATE_DETACHED;
	__atomic_store_n(&record->pending, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&record->handler_refs, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&record->public_id, public_id, __ATOMIC_RELAXED);
	__atomic_store_n(&record->state, TIMER_SLOT_ACTIVE, __ATOMIC_RELEASE);
	*result = (timer_t)public_id;
	(void)pthread_mutex_unlock(&timer_lock);

	/* Reports successful completion. */
	return 0;

fail_locked:
	(void)pthread_mutex_unlock(&timer_lock);
	errno = error;

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the timer delete operation.
 */
int
timer_delete(
	timer_t timer)
{
	int function_result;
	struct libc_timer_slot *record;
	int error;

	/* Handles a failed timer is libc id operation. */
	if (!timer_is_libc_id(timer)) {
		/* Computes the function result. */
		function_result = (int)timer_syscall(ZEDBSD_SYS_timer_delete,
					  (uintptr_t)timer, 0, 0, 0);

		/* Returns the computed result. */
		return function_result;
	}
	(void)pthread_mutex_lock(&timer_lock);
	record = timer_lookup_locked(timer);

	/* Handles the record availability. */
	if (record == NULL) {
		(void)pthread_mutex_unlock(&timer_lock);
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	__atomic_store_n(&record->state, TIMER_SLOT_DELETING, __ATOMIC_RELEASE);

	/* Handles a failed timer syscall operation. */
	if (timer_syscall(ZEDBSD_SYS_timer_delete, (uintptr_t)record->kernel_id,
			  0, 0, 0) < 0) {
		error = errno;
		__atomic_store_n(&record->state, TIMER_SLOT_ACTIVE,
				 __ATOMIC_RELEASE);
		(void)pthread_mutex_unlock(&timer_lock);
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	/*
 * A handler that observed the old generation must finish before this
	 * slot becomes reusable.  It never blocks, allocates, or takes
	 * timer_lock. */
	while (__atomic_load_n(&record->handler_refs, __ATOMIC_ACQUIRE) != 0)
		(void)sched_yield();
	__atomic_store_n(&record->pending, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&record->public_id, 0, __ATOMIC_RELAXED);
	record->kernel_id = 0;
	record->function = NULL;
	__atomic_store_n(&record->state, TIMER_SLOT_FREE, __ATOMIC_RELEASE);
	(void)pthread_mutex_unlock(&timer_lock);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the timer settime operation.
 */
int
timer_settime(
	timer_t timer,
	int flags,
	const struct itimerspec *value,
	struct itimerspec *old_value)
{
	int function_result;
	struct libc_timer_slot *record;
	intptr_t result;
	int saved_errno;

	/* Handles a failed timer is libc id operation. */
	if (!timer_is_libc_id(timer)) {
		/* Computes the function result. */
		function_result = (int)timer_syscall(
		    ZEDBSD_SYS_timer_settime, (uintptr_t)timer, flags,
		    (uintptr_t)value, (uintptr_t)old_value);

		/* Returns the computed result. */
		return function_result;
	}
	(void)pthread_mutex_lock(&timer_lock);
	record = timer_lookup_locked(timer);

	/* Handles the record availability. */
	if (record == NULL) {
		(void)pthread_mutex_unlock(&timer_lock);
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	result = timer_syscall(ZEDBSD_SYS_timer_settime,
			       (uintptr_t)record->kernel_id, flags,
			       (uintptr_t)value, (uintptr_t)old_value);
	saved_errno = errno;
	(void)pthread_mutex_unlock(&timer_lock);
	errno = saved_errno;

	/* Returns the computed result. */
	return (int)result;
}

/*
 * Implements the timer gettime operation.
 */
int
timer_gettime(
	timer_t timer,
	struct itimerspec *value)
{
	int function_result;
	struct libc_timer_slot *record;
	intptr_t result;
	int saved_errno;

	/* Handles a failed timer is libc id operation. */
	if (!timer_is_libc_id(timer)) {
		/* Computes the function result. */
		function_result = (int)timer_syscall(ZEDBSD_SYS_timer_gettime,
					  (uintptr_t)timer, (uintptr_t)value, 0,
					  0);

		/* Returns the computed result. */
		return function_result;
	}
	(void)pthread_mutex_lock(&timer_lock);
	record = timer_lookup_locked(timer);

	/* Handles the record availability. */
	if (record == NULL) {
		(void)pthread_mutex_unlock(&timer_lock);
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	result =
	    timer_syscall(ZEDBSD_SYS_timer_gettime,
			  (uintptr_t)record->kernel_id, (uintptr_t)value, 0, 0);
	saved_errno = errno;
	(void)pthread_mutex_unlock(&timer_lock);
	errno = saved_errno;

	/* Returns the computed result. */
	return (int)result;
}

/*
 * Implements the timer getoverrun operation.
 */
int
timer_getoverrun(
	timer_t timer)
{
	int function_result;
	struct libc_timer_slot *record;
	intptr_t result;
	int saved_errno;

	/* Handles a failed timer is libc id operation. */
	if (!timer_is_libc_id(timer)) {
		/* Computes the function result. */
		function_result = (int)timer_syscall(ZEDBSD_SYS_timer_getoverrun,
					  (uintptr_t)timer, 0, 0, 0);

		/* Returns the computed result. */
		return function_result;
	}
	(void)pthread_mutex_lock(&timer_lock);
	record = timer_lookup_locked(timer);

	/* Handles the record availability. */
	if (record == NULL) {
		(void)pthread_mutex_unlock(&timer_lock);
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	result = timer_syscall(ZEDBSD_SYS_timer_getoverrun,
			       (uintptr_t)record->kernel_id, 0, 0, 0);
	saved_errno = errno;
	(void)pthread_mutex_unlock(&timer_lock);
	errno = saved_errno;

	/* Returns the computed result. */
	return (int)result;
}

/* Supports the timer syscall operation. */
static intptr_t
timer_syscall(
	uint32_t number,
	uintptr_t a,
	uintptr_t b,
	uintptr_t c,
	uintptr_t d)
{
	intptr_t result;

	result = __syscall6(number, a, b, c, d, 0, 0);

	/* Checks the operation result. */
	if (result < 0) {
		errno = (int)-result;

		/* Reports operation failure. */
		return -1;
	}

	/* Returns the computed result. */
	return result;
}

/* Supports the timer service start locked operation. */
static int
timer_service_start_locked(
	void)
{
	struct sigaction action;
	pthread_attr_t attributes;
	pthread_t worker;
	int error;

	/* Handles the timer service ready condition. */
	if (timer_service_ready)
		return 0;

	memset(&action, 0, sizeof(action));
	action.sa_handler = (uint64_t)(uintptr_t)timer_signal_handler;
	action.sa_flags = SA_SIGINFO | SA_RESTART;
	action.sa_restorer = (uint64_t)(uintptr_t)__signal_restorer;

	/* Handles a failed timer syscall operation. */
	if (timer_syscall(ZEDBSD_SYS_sigaction, LIBC_TIMER_WAKE_SIGNAL,
			  (uintptr_t)&action, (uintptr_t)&timer_previous_action,
			  0) < 0) {
		error = errno;

		/* Returns the computed result. */
		return error;
	}
	(void)pthread_attr_init(&attributes);
	(void)pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	error = pthread_create(&worker, &attributes, timer_worker, NULL);
	(void)pthread_attr_destroy(&attributes);

	/* Handles an operation failure. */
	if (error != 0) {
		(void)timer_syscall(ZEDBSD_SYS_sigaction,
				    LIBC_TIMER_WAKE_SIGNAL,
				    (uintptr_t)&timer_previous_action, 0, 0);

		/* Returns the computed result. */
		return error;
	}
	timer_service_ready = 1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the timer new id operation. */
static uint32_t
timer_new_id(
	struct libc_timer_slot *record,
	unsigned slot)
{
	record->generation =
	    (record->generation + 1U) & LIBC_TIMER_GENERATION_MASK;

	/* Handles the record condition. */
	if (record->generation == 0)
		record->generation = 1;

	/* Returns the computed result. */
	return (record->generation << LIBC_TIMER_GENERATION_SHIFT) |
	       ((slot + 1U) << LIBC_TIMER_SLOT_SHIFT);
}

/* Supports the timer is libc id operation. */
static int
timer_is_libc_id(
	timer_t timer)
{
	uint32_t raw;

	raw = (uint32_t)timer;

	/*
 * Kernel timer handles always encode slot+1 in the low byte.  Reserving
	 * zero there gives libc a disjoint namespace even after the kernel's
	 * generation counter crosses the sign bit. */
	return raw != 0 && (raw & 0xffU) == 0;
}

/* Supports the timer lookup locked operation. */
static struct libc_timer_slot *
timer_lookup_locked(
	timer_t timer)
{
	struct libc_timer_slot *record;
	unsigned slot;
	uint32_t id;

	id = (uint32_t)timer;

	/* Handles a failed timer id slot operation. */
	if (timer_id_slot(id, &slot) != 0)
		return NULL;
	record = &timer_slots[slot];

	/* Handles a failed atomic load n operation. */
	if (__atomic_load_n(&record->state, __ATOMIC_ACQUIRE) !=
		TIMER_SLOT_ACTIVE ||
	    __atomic_load_n(&record->public_id, __ATOMIC_RELAXED) != id)

		/* Reports that no result is available. */
		return NULL;

	/* Returns the computed result. */
	return record;
}

/* Supports the timer id slot operation. */
static int
timer_id_slot(
	uint32_t id,
	unsigned *slot)
{
	unsigned encoded;

	/* Handles a failed timer is libc id operation. */
	if (!timer_is_libc_id((timer_t)id))
		return EINVAL;
	encoded = (id >> LIBC_TIMER_SLOT_SHIFT) & 0xffU;

	/* Handles the encoded condition. */
	if (encoded == 0 || encoded > LIBC_TIMER_MAX)
		return EINVAL;
	*slot = encoded - 1U;
	/* Reports successful completion. */
	return 0;
}

/* The kernel's per-process RT queue bounds outstanding notifications far below UINT32_MAX.  Atomic add is sufficient here and, unlike compare-exchange, remains an inline pre-CMPXCHG operation in the i386 user ABI. */
static void
timer_pending_increment(
	volatile uint32_t *pending)
{
	(void)__atomic_add_fetch(pending, 1U, __ATOMIC_RELEASE);
}

/* Supports the timer signal handler operation. */
static void
timer_signal_handler(
	int signo,
	siginfo_t *information,
	void *context)
{
	struct libc_timer_slot *record;
	uint32_t id;
	unsigned slot;

	(void)context;

	/* Handles the information availability. */
	if (signo != LIBC_TIMER_WAKE_SIGNAL || information == NULL ||
	    information->si_code != SI_TIMER)

		/* Returns the computed result. */
		return;
	id = (uint32_t)information->si_value.__sival_pad;

	/* Handles a failed timer id slot operation. */
	if (timer_id_slot(id, &slot) != 0)
		return;
	record = &timer_slots[slot];
	(void)__atomic_add_fetch(&record->handler_refs, 1, __ATOMIC_ACQUIRE);

	/* Handles a failed atomic load n operation. */
	if (__atomic_load_n(&record->state, __ATOMIC_ACQUIRE) !=
		TIMER_SLOT_ACTIVE ||
	    __atomic_load_n(&record->public_id, __ATOMIC_RELAXED) != id) {
		(void)__atomic_sub_fetch(&record->handler_refs, 1,
					 __ATOMIC_RELEASE);

		/* Returns the computed result. */
		return;
	}
	timer_pending_increment(&record->pending);

	/*
 * usync wake is a raw syscall and therefore does not run cancellation
	 * or errno machinery in signal context.  The generation comparison in
	 * the worker makes this safe even when the wake runs just before it
	 * sleeps. */
	(void)__atomic_add_fetch(&timer_wake_generation, 1, __ATOMIC_RELEASE);
	(void)__syscall6(ZEDBSD_SYS_usync, (uintptr_t)&timer_wake_generation,
			 ZEDBSD_USYNC_WAKE, 0, 0, UINT32_MAX,
			 ZEDBSD_USYNC_PRIVATE);
	(void)__atomic_sub_fetch(&record->handler_refs, 1, __ATOMIC_RELEASE);
}

/* Supports the timer callback still valid operation. */
static int
timer_callback_still_valid(
	uint32_t id)
{
	struct libc_timer_slot *record;
	int valid;

	(void)pthread_mutex_lock(&timer_lock);
	record = timer_lookup_locked((timer_t)id);
	valid = record != NULL;
	(void)pthread_mutex_unlock(&timer_lock);

	/* Returns the computed result. */
	return valid;
}

/* Supports the timer callback start operation. */
static void *
timer_callback_start(
	void *argument)
{
	struct timer_callback *callback;
	void (*function)(union sigval);
	union sigval value;
	uint32_t id;
	int valid;

	callback = argument;
	function = callback->function;
	value = callback->value;
	id = callback->public_id;
	valid = timer_callback_still_valid(id);

	free(callback);

	/* Handles the valid condition. */
	if (valid)
		function(value);

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the timer dispatch pending operation. */
static void
timer_dispatch_pending(
	unsigned slot,
	uint32_t id,
	uint32_t count)
{
	struct libc_timer_slot *record;
	struct timer_callback *callback;
	pthread_t thread;
	int error;

	/* Process each remaining element. */
	while (count-- != 0) {
		callback = malloc(sizeof(*callback));

		/* Handles the callback availability. */
		if (callback == NULL)
			continue;
		(void)pthread_mutex_lock(&timer_lock);
		record = &timer_slots[slot];

		/* Handles a failed atomic load n operation. */
		if (__atomic_load_n(&record->state, __ATOMIC_ACQUIRE) !=
			TIMER_SLOT_ACTIVE ||
		    __atomic_load_n(&record->public_id, __ATOMIC_RELAXED) !=
			id) {
			(void)pthread_mutex_unlock(&timer_lock);
			free(callback);
			break;
		}
		callback->public_id = id;
		callback->function = record->function;
		callback->value = record->value;
		callback->attributes = record->attributes;
		(void)pthread_mutex_unlock(&timer_lock);

		error = pthread_create(&thread, &callback->attributes,
				       timer_callback_start, callback);

		/*
 * Notification happens asynchronously, so POSIX provides no
		 * caller to which resource-exhaustion can be reported.  The
		 * kernel overrun value remains authoritative; a callback whose
		 * thread cannot be allocated is dropped instead of spinning the
		 * single delivery worker forever. */
		if (error != 0)
			free(callback);
	}
}

/* Supports the timer dispatch slot operation. */
static void
timer_dispatch_slot(
	unsigned slot)
{
	uint32_t id, count;

	/*
 * Pair the ID with the pending count while delete/reuse is excluded.  A
	 * lock-free load followed by exchange could otherwise sample an old ID
	 * and consume a newly-created timer's first notification from the same
	 * slot. */
	(void)pthread_mutex_lock(&timer_lock);
	id = __atomic_load_n(&timer_slots[slot].public_id, __ATOMIC_ACQUIRE);
	count = __atomic_exchange_n(&timer_slots[slot].pending, 0,
				    __ATOMIC_ACQ_REL);
	(void)pthread_mutex_unlock(&timer_lock);

	/*
 * Keep the ID sampled with the pending count.  If delete/recreate
	 * reuses this slot while the worker waits for timer_lock, the
	 * generation check in timer_dispatch_pending() rejects the old work
	 * instead of invoking the new timer's callback. */
	timer_dispatch_pending(slot, id, count);
}

/* Supports the timer worker operation. */
static void *
timer_worker(
	void *argument)
{
	uint32_t observed;
	unsigned slot;
	sigset_t signal_set;

	signal_set = (sigset_t)1ULL << (LIBC_TIMER_WAKE_SIGNAL - 1U);

	(void)argument;

	/*
 * At least the worker must always be able to receive the reserved
	 * signal, even when the thread that created the first timer had it
	 * blocked.  Build the private bit directly because the public sigset
	 * helpers intentionally reject every signal above SIGRTMAX. */
	(void)timer_syscall(ZEDBSD_SYS_sigprocmask, SIG_UNBLOCK,
			    (uintptr_t)&signal_set, 0, 0);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		observed = __atomic_load_n(&timer_wake_generation, __ATOMIC_ACQUIRE);

		/* Process each element required by the operation. */
		for (slot = 0; slot < LIBC_TIMER_MAX; slot++) {
			/* Handles a failed atomic load n operation. */
			if (__atomic_load_n(&timer_slots[slot].pending,
					    __ATOMIC_ACQUIRE) != 0)
				timer_dispatch_slot(slot);
		}

		/* Handles a failed atomic load n operation. */
		if (__atomic_load_n(&timer_wake_generation, __ATOMIC_ACQUIRE) ==
		    observed) {
			(void)__syscall6(ZEDBSD_SYS_usync,
					 (uintptr_t)&timer_wake_generation,
					 ZEDBSD_USYNC_WAIT, observed, 0, 0,
					 ZEDBSD_USYNC_PRIVATE);
		}
	}

	/* Reports that no result is available. */
	return NULL;
}
