/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library pthread support.
 */

#include "userland/base/libc/syscall.h"
#include <zedbsd/syscall.h>
#include <zedbsd/usync.h>
#include <zedbsd/thread.h>
#include <zedbsd/rtld-abi.h>
#include <errno.h>
#include <fenv.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <threads.h>

#define THREAD_STACK_SIZE (1024U * 1024U)
#define KEY_MAX 32U
#define DESTRUCTOR_ITERATIONS 4U
#define ATFORK_MAX 16U

struct atfork_handler {
	void (*prepare)(void);
	void (*parent)(void);
	void (*child)(void);
};

struct pthread_tcb {
	pthread_t tid;
	void *stack;
	size_t stack_size;
	void *(*start)(void *);
	void *argument;
	const void *keys[KEY_MAX];
	struct __pthread_cleanup *cleanup;
	int cancel_state;
	int cancel_type;
	int error;
	char *environment_value;
	const void *locale_value;
	uint32_t multibyte_state[6];
	fenv_t floating_environment;
	char ptsname_buffer[32];
	unsigned detached;
	unsigned detached_queued;
	unsigned owns_stack;
	struct pthread_tcb *next;
	struct pthread_tcb *detached_next;
	struct __rtld_tcb *runtime_tcb;
};

static struct pthread_tcb main_tcb;
static struct pthread_tcb *tcb_list;
static void (*key_destructor[KEY_MAX])(void *);

static volatile uint32_t key_lock;
static volatile uint32_t registry_lock;
static volatile uint32_t heap_lock;
static volatile uint32_t environment_lock;
static volatile uint32_t detached_lock;
static volatile uint32_t detached_generation;
static struct pthread_tcb *detached_head;
static struct pthread_tcb *detached_tail;
static pthread_t detached_reaper_tid;
static unsigned detached_reaper_started;
static volatile uint32_t atfork_lock;
static struct atfork_handler atfork_handlers[ATFORK_MAX];
static unsigned atfork_count;
static unsigned atfork_active_count;

extern void __stdio_fork_child(void) __attribute__((weak));

#if defined(ZEDBSD_DYNAMIC_LIBC)
#define RTLD_CALL(name) (__rtld_exports.name)
#else
#define RTLD_CALL(name) (__rtld_##name)
#endif

struct c11_start_context {
	thrd_start_t function;
	void *argument;
};

static void word_lock(volatile uint32_t *word);
static int usync_wait_word(volatile uint32_t *a, uint32_t v, const struct timespec *t);
static int usync_wait_word_flags(volatile uint32_t *address, uint32_t value, const struct timespec *timeout, int pshared);
static int usync_wait_word_flags_cancelable(volatile uint32_t *address, uint32_t value, const struct timespec *timeout, int pshared, int cancelable, unsigned timeout_flags);
static intptr_t call(uint32_t number, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f);
static void word_unlock(volatile uint32_t *word);
static void usync_wake_word(volatile uint32_t *a, unsigned n);
static void usync_wake_word_flags(volatile uint32_t *address, unsigned count, int pshared);
static struct pthread_tcb *self_tcb(void);
static void ensure_main(void);
static void registry_add(struct pthread_tcb *tcb);
static void run_destructors(struct pthread_tcb *tcb);
static void detached_enqueue(struct pthread_tcb *tcb);
static struct pthread_tcb *registry_find(pthread_t tid);
static struct pthread_tcb *registry_remove(pthread_t tid);
static int ensure_detached_reaper(void);
static int usync_wait_word_absolute(volatile uint32_t *address, uint32_t value, const struct timespec *absolute, int pshared, int cancelable, clockid_t clock);
static int cond_wait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *absolute, clockid_t clock);
static int cancel_pending(void);
static void rwlock_guard_lock(pthread_rwlock_t *lock);
static void rwlock_guard_unlock(pthread_rwlock_t *lock);
static void barrier_guard_lock(pthread_barrier_t *barrier);
static void barrier_guard_unlock(pthread_barrier_t *barrier);
static int c11_result(int error);
static void *detached_reaper(void *argument);
static void thread_trampoline(void);
static void *c11_start(void *argument);

/*
 * Implements the libc heap lock operation.
 */
void
__libc_heap_lock(
	void)
{
	word_lock(&heap_lock);
}

/*
 * Implements the libc heap unlock operation.
 */
void
__libc_heap_unlock(
	void)
{
	word_unlock(&heap_lock);
}

/*
 * Implements the libc environment lock operation.
 */
void
__libc_environment_lock(
	void)
{
	word_lock(&environment_lock);
}

/*
 * Implements the libc environment unlock operation.
 */
void
__libc_environment_unlock(
	void)
{
	word_unlock(&environment_lock);
}

/*
 * Implements the stdio thread token operation.
 */
uintptr_t
__stdio_thread_token(
	void)
{
	struct pthread_tcb *tcb;

	tcb = self_tcb();

	/* Returns the computed result. */
	return (uintptr_t)(tcb != NULL ? tcb : &main_tcb);
}

/*
 * Implements the stdio lock wait operation.
 */
void
__stdio_lock_wait(
	volatile uint32_t *word)
{
	(void)usync_wait_word(word, 1U, NULL);
}

/*
 * Implements the stdio lock wake operation.
 */
void
__stdio_lock_wake(
	volatile uint32_t *word)
{
	usync_wake_word(word, 1U);
}

/*
 * Implements the pthread cancel enabled operation.
 */
int
__pthread_cancel_enabled(
	void)
{
	struct pthread_tcb *tcb;

	ensure_main();
	tcb = self_tcb();

	/* Returns the computed result. */
	return tcb != NULL && tcb->cancel_state == PTHREAD_CANCEL_ENABLE;
}

/*
 * getenv() returns a pointer whose lifetime must not be ended by another thread mutating environ.  The environment implementation installs a private value snapshot here and replaces it only on this thread's next environment operation.  Keeping this in the TCB also gives thread exit a well-defined reclamation point without exposing pthread internals.
 */
char *
__pthread_environment_exchange(
	char *replacement)
{
	struct pthread_tcb *tcb;
	char *previous;

	ensure_main();
	tcb = self_tcb();

	/* Handles the tcb availability. */
	if (tcb == NULL)
		return NULL;
	previous = tcb->environment_value;
	tcb->environment_value = replacement;

	/* Returns the computed result. */
	return previous;
}

/*
 * Implements the pthread locale exchange operation.
 */
const void *
__pthread_locale_exchange(
	const void *replacement,
	int change)
{
	struct pthread_tcb *tcb;
	const void *previous;

	ensure_main();
	tcb = self_tcb();

	/* Handles the tcb availability. */
	if (tcb == NULL)
		return NULL;
	previous = tcb->locale_value;

	/* Handles the change condition. */
	if (change)
		tcb->locale_value = replacement;

	/* Returns the computed result. */
	return previous;
}

/*
 * Implements the pthread mbstate operation.
 */
void *
__pthread_mbstate(
	unsigned which)
{
	struct pthread_tcb *tcb;

	ensure_main();
	tcb = self_tcb();

	/* Handles the tcb availability. */
	if (tcb == NULL || which > 2U)
		return NULL;

	/* Returns the computed result. */
	return &tcb->multibyte_state[which * 2U];
}

/*
 * Implements the libc fenv location operation.
 */
fenv_t *
__libc_fenv_location(
	void)
{
#if defined(ZEDBSD_DYNAMIC_LIBC)
	static _Thread_local fenv_t environment = {0U, FE_TONEAREST};

	/* Returns the computed result. */
	return &environment;
#else
	static fenv_t bootstrap = {0U, FE_TONEAREST};
	struct pthread_tcb *tcb = self_tcb();

	/* Handles the tcb availability. */
	if (tcb != NULL && tcb->floating_environment.rounding == 0)
		tcb->floating_environment.rounding = FE_TONEAREST;

	/* Returns the computed result. */
	return tcb != NULL ? &tcb->floating_environment : &bootstrap;
#endif
}

/*
 * Implements the pthread ptsname buffer operation.
 */
char *
__pthread_ptsname_buffer(
	size_t *size)
{
	struct pthread_tcb *tcb;

	ensure_main();
	tcb = self_tcb();

	/* Handles the tcb availability. */
	if (tcb == NULL)
		return NULL;

	/* Handles the size availability. */
	if (size != NULL)
		*size = sizeof(tcb->ptsname_buffer);
	/* Returns the computed result. */
	return tcb->ptsname_buffer;
}

/*
 * Implements the libc errno location operation.
 */
int *
__libc_errno_location(
	void)
{
#if defined(ZEDBSD_DYNAMIC_LIBC)
	static _Thread_local int dynamic_errno;

	/* Returns the computed result. */
	return &dynamic_errno;
#else
	static int bootstrap_errno;
	struct pthread_tcb *tcb = self_tcb();

	/* Returns the computed result. */
	return tcb != NULL ? &tcb->error : &bootstrap_errno;
#endif
}

/*
 * Implements the pthread initialize main operation.
 */
void
__pthread_initialize_main(
	void)
{
	ensure_main();
}

/*
 * Implements the pthread create operation.
 */
int
pthread_create(
	pthread_t *result,
	const pthread_attr_t *attributes,
	void *(*start)(void *),
	void *argument)
{
	struct pthread_tcb *tcb;
	void *stack;
	void *usable_stack;
	size_t size = attributes != NULL && attributes->stacksize != 0
			  ? attributes->stacksize
			  : THREAD_STACK_SIZE;
	size_t guard = attributes != NULL ? attributes->guardsize : 4096U;
	size_t mapping_size;
	int owns_stack;
	int error;

	/* Handles the result availability. */
	if (result == NULL || start == NULL || size < PTHREAD_STACK_MIN)
		return EINVAL;
	ensure_main();

	/* Handles the attributes availability. */
	if (attributes != NULL && attributes->stackset) {
		stack = usable_stack = attributes->stackaddr;
		mapping_size = size;
		owns_stack = 0;

		/* Handles the stack availability. */
		if (stack == NULL)
			return EINVAL;
	} else {
		guard = (guard + 4095U) & ~(size_t)4095U;

		/* Handles the guard condition. */
		if (guard > SIZE_MAX - size)
			return EAGAIN;
		mapping_size = guard + size;
		stack = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		/* Handles an operation failure. */
		if (stack == MAP_FAILED)
			return EAGAIN;

		/* Handles a failed mprotect operation. */
		if (guard != 0 && mprotect(stack, guard, PROT_NONE) != 0) {
			(void)munmap(stack, mapping_size);

			/* Returns the computed result. */
			return EAGAIN;
		}
		usable_stack = (unsigned char *)stack + guard;
		owns_stack = 1;
	}
	tcb = malloc(sizeof(*tcb));

	/* Handles the tcb availability. */
	if (tcb == NULL) {
		/* Handles the owns stack condition. */
		if (owns_stack)
			(void)munmap(stack, mapping_size);

		/* Returns the computed result. */
		return EAGAIN;
	}
	memset(tcb, 0, sizeof(*tcb));
	tcb->stack = stack;
	tcb->stack_size = mapping_size;
	tcb->owns_stack = (unsigned)owns_stack;
	tcb->start = start;
	tcb->argument = argument;

	/* Handles a failed RTLD CALL operation. */
	if (RTLD_CALL(thread_alloc)(tcb, &tcb->runtime_tcb) != 0) {
		free(tcb);

		/* Handles the owns stack condition. */
		if (owns_stack)
			(void)munmap(stack, mapping_size);

		/* Returns the computed result. */
		return EAGAIN;
	}
	error =
	    (int)call(ZEDBSD_SYS_thread_create, (uintptr_t)thread_trampoline,
		      (uintptr_t)usable_stack + size, 0,
		      (uintptr_t)tcb->runtime_tcb, 0, (uintptr_t)&tcb->tid);

	/* Handles an operation failure. */
	if (error < 0) {
		error = errno;
		RTLD_CALL(thread_free)(tcb->runtime_tcb);
		free(tcb);

		/* Handles the owns stack condition. */
		if (owns_stack)
			(void)munmap(stack, mapping_size);

		/* Returns the computed result. */
		return error;
	}
	registry_add(tcb);
	*result = tcb->tid;
	/* Handles the attributes availability. */
	if (attributes != NULL &&
	    attributes->detachstate == PTHREAD_CREATE_DETACHED) {
		error = pthread_detach(tcb->tid);

		/* Handles an operation failure. */
		if (error != 0)
			return error;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread exit operation.
 */
void
pthread_exit(
	void *value)
{
	struct __pthread_cleanup *cleanup;
	struct pthread_tcb *tcb;

	/* Continue while the operation condition remains true. */
	tcb = self_tcb();
	while (tcb != NULL && tcb->cleanup != NULL) {
		cleanup = tcb->cleanup;
		tcb->cleanup = cleanup->previous;
		cleanup->routine(cleanup->argument);
	}
	run_destructors(tcb);

	/* Handles the tcb availability. */
	if (tcb != NULL) {
		free(tcb->environment_value);
		tcb->environment_value = NULL;
	}

	/* Handles the tcb availability. */
	if (tcb != NULL && tcb->detached)
		detached_enqueue(tcb);
	(void)__syscall6(ZEDBSD_SYS_thread_exit, (uintptr_t)value, 0, 0, 0, 0,
			 0);

	/* Continue until the operation reaches a terminal state. */
	for (;;)
		;
}

/*
 * Implements the pthread join operation.
 */
int
pthread_join(
	pthread_t thread,
	void **value)
{
	struct pthread_tcb *tcb;
	struct pthread_tcb *self;
	intptr_t result;

	pthread_testcancel();
	tcb = registry_find(thread);

	/* Handles the tcb availability. */
	if (tcb != NULL && tcb->detached)
		return EINVAL;
	self = self_tcb();
	do {
		result = call(ZEDBSD_SYS_thread_join, thread, (uintptr_t)value,
			      self != NULL && self->cancel_state ==
						  PTHREAD_CANCEL_ENABLE
				  ? ZEDBSD_THREAD_JOIN_CANCELABLE
				  : 0,
			      0, 0, 0);

		/* Handles the reported system error. */
		if (result < 0 && errno == EINTR)
			pthread_testcancel();
	} while (result < 0 && errno == EINTR);

	/* Checks the operation result. */
	if (result < 0)
		return errno;
	tcb = registry_remove(thread);

	/* Handles the tcb availability. */
	if (tcb != NULL && tcb != &main_tcb) {
		RTLD_CALL(thread_free)(tcb->runtime_tcb);

		/* Handles the tcb condition. */
		if (tcb->owns_stack)
			(void)munmap(tcb->stack, tcb->stack_size);
		free(tcb);
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread detach operation.
 */
int
pthread_detach(
	pthread_t thread)
{
	struct pthread_tcb *tcb;
	int error;

	ensure_main();
	tcb = registry_find(thread);

	/* Handles the tcb availability. */
	if (tcb == NULL || tcb == &main_tcb)
		return ESRCH;
	word_lock(&registry_lock);

	/* Handles the tcb condition. */
	if (tcb->detached) {
		word_unlock(&registry_lock);

		/* Returns the computed result. */
		return EINVAL;
	}
	word_unlock(&registry_lock);
	error = ensure_detached_reaper();

	/* Handles an operation failure. */
	if (error != 0)
		return error;
	word_lock(&registry_lock);

	/* Handles the tcb condition. */
	if (tcb->detached) {
		word_unlock(&registry_lock);

		/* Returns the computed result. */
		return EINVAL;
	}
	tcb->detached = 1;
	word_unlock(&registry_lock);

	/*
 * The reaper may wait before the target reaches thread_exit().  Queuing
	 * here also covers the race where the target is already a zombie. */
	detached_enqueue(tcb);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread atfork operation.
 */
int
pthread_atfork(
	void (*prepare)(void),
	void (*parent)(void),
	void (*child)(void))
{
	/* Handles the prepare availability. */
	if (prepare == NULL && parent == NULL && child == NULL)
		return 0;
	word_lock(&atfork_lock);

	/* Handles the atfork count condition. */
	if (atfork_count == ATFORK_MAX) {
		word_unlock(&atfork_lock);

		/* Returns the computed result. */
		return ENOMEM;
	}
	atfork_handlers[atfork_count].prepare = prepare;
	atfork_handlers[atfork_count].parent = parent;
	atfork_handlers[atfork_count].child = child;
	atfork_count++;
	word_unlock(&atfork_lock);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread fork prepare operation.
 */
void
__pthread_fork_prepare(
	void)
{
	unsigned index;

	ensure_main();
	RTLD_CALL(fork_prepare)();
	word_lock(&atfork_lock);

	/* Process each remaining element. */
	atfork_active_count = atfork_count;
	for (index = atfork_active_count; index != 0; index--) {
		/* Handles the prepare availability. */
		if (atfork_handlers[index - 1U].prepare != NULL)
			atfork_handlers[index - 1U].prepare();
	}
}

/*
 * Implements the pthread fork parent operation.
 */
void
__pthread_fork_parent(
	void)
{
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < atfork_active_count; index++) {
		/* Handles the parent availability. */
		if (atfork_handlers[index].parent != NULL)
			atfork_handlers[index].parent();
	}
	atfork_active_count = 0;
	word_unlock(&atfork_lock);
	RTLD_CALL(fork_parent)();
}

/*
 * Implements the pthread fork child operation.
 */
void
__pthread_fork_child(
	void)
{
	struct pthread_tcb *self;
	unsigned count;
	unsigned index;

	self = self_tcb();
	count = atfork_active_count;

	/*
 * Only the calling thread exists in the child.  Internal locks and the
	 * detached-thread reaper belonged to threads that were not copied. */
	registry_lock = 0;
	heap_lock = 0;
	environment_lock = 0;
	key_lock = 0;
	detached_lock = 0;
	detached_generation = 0;
	detached_head = detached_tail = NULL;
	detached_reaper_started = 0;
	detached_reaper_tid = 0;

	/* Handles the self availability. */
	if (self != NULL) {
		self->next = NULL;
		self->detached_next = NULL;
		self->detached_queued = 0;
		self->detached = 0;
		tcb_list = self;
	} else {
		tcb_list = NULL;
	}

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		/* Handles the child availability. */
		if (atfork_handlers[index].child != NULL)
			atfork_handlers[index].child();
	}
	atfork_active_count = 0;
	atfork_lock = 0;

	/* Handles the stdio fork child availability. */
	if (__stdio_fork_child != NULL)
		__stdio_fork_child();
	RTLD_CALL(fork_child)();
}

/*
 * Implements the pthread self operation.
 */
pthread_t
pthread_self(
	void)
{
	pthread_t function_result;

	ensure_main();

	/* Computes the function result. */
	function_result = self_tcb()->tid;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the pthread equal operation.
 */
int
pthread_equal(
	pthread_t left,
	pthread_t right)
{
	/* Returns the computed result. */
	return left == right;
}

/*
 * Implements the pthread attr init operation.
 */
int
pthread_attr_init(
	pthread_attr_t *a)
{
	/* Handles the a availability. */
	if (a == NULL)
		return EINVAL;
	memset(a, 0, sizeof(*a));
	a->stacksize = THREAD_STACK_SIZE;
	a->guardsize = 4096;
	a->detachstate = 0;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread attr destroy operation.
 */
int
pthread_attr_destroy(
	pthread_attr_t *a)
{
	/* Returns the computed result. */
	return a != NULL ? 0 : EINVAL;
}

/*
 * Implements the pthread attr setdetachstate operation.
 */
int
pthread_attr_setdetachstate(
	pthread_attr_t *a,
	int state)
{
	/* Handles the a availability. */
	if (a == NULL || (state != 0 && state != 1))
		return EINVAL;
	a->detachstate = state;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread attr setstacksize operation.
 */
int
pthread_attr_setstacksize(
	pthread_attr_t *a,
	size_t size)
{
	/* Handles the a availability. */
	if (a == NULL || size < PTHREAD_STACK_MIN)
		return EINVAL;
	a->stacksize = size;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread attr getdetachstate operation.
 */
int
pthread_attr_getdetachstate(
	const pthread_attr_t *a,
	int *state)
{
	/* Handles the a availability. */
	if (a == NULL || state == NULL)
		return EINVAL;
	*state = a->detachstate;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread attr getstacksize operation.
 */
int
pthread_attr_getstacksize(
	const pthread_attr_t *a,
	size_t *size)
{
	/* Handles the a availability. */
	if (a == NULL || size == NULL)
		return EINVAL;
	*size = a->stacksize;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread attr setguardsize operation.
 */
int
pthread_attr_setguardsize(
	pthread_attr_t *a,
	size_t size)
{
	/* Handles the a availability. */
	if (a == NULL)
		return EINVAL;
	a->guardsize = size;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread attr getguardsize operation.
 */
int
pthread_attr_getguardsize(
	const pthread_attr_t *a,
	size_t *size)
{
	/* Handles the a availability. */
	if (a == NULL || size == NULL)
		return EINVAL;
	*size = a->guardsize;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread attr setstack operation.
 */
int
pthread_attr_setstack(
	pthread_attr_t *a,
	void *stack,
	size_t size)
{
	/* Handles the a availability. */
	if (a == NULL || stack == NULL || size < PTHREAD_STACK_MIN)
		return EINVAL;
	a->stackaddr = stack;
	a->stacksize = size;
	a->guardsize = 0;
	a->stackset = 1;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread attr getstack operation.
 */
int
pthread_attr_getstack(
	const pthread_attr_t *a,
	void **stack,
	size_t *size)
{
	/* Handles the a availability. */
	if (a == NULL || stack == NULL || size == NULL)
		return EINVAL;
	*stack = a->stackaddr;
	*size = a->stacksize;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread attr getschedparam operation.
 */
int
pthread_attr_getschedparam(
	const pthread_attr_t *a,
	struct sched_param *p)
{
	/* Handles the a availability. */
	if (a == NULL || p == NULL)
		return EINVAL;
	*p = a->schedparam;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread attr setschedparam operation.
 */
int
pthread_attr_setschedparam(
	pthread_attr_t *a,
	const struct sched_param *p)
{
	/* Handles the a availability. */
	if (a == NULL || p == NULL)
		return EINVAL;

	/* Checks the current pointer. */
	if (p->sched_priority != 0)
		return ENOTSUP;
	a->schedparam = *p;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread mutex init operation.
 */
int
pthread_mutex_init(
	pthread_mutex_t *m,
	const pthread_mutexattr_t *a)
{
	/* Handles the m availability. */
	if (m == NULL)
		return EINVAL;
	memset(m, 0, sizeof(*m));

	/* Handles the a availability. */
	if (a != NULL) {
		m->type = a->type;
		m->pshared = a->pshared;
		m->robust = a->robust;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread mutex destroy operation.
 */
int
pthread_mutex_destroy(
	pthread_mutex_t *m)
{
	int function_result;

	/* Computes the function result. */
	function_result = m == NULL
		   ? EINVAL
		   : (__atomic_load_n(&m->locked, __ATOMIC_ACQUIRE) ? EBUSY
								    : 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the pthread mutexattr init operation.
 */
int
pthread_mutexattr_init(
	pthread_mutexattr_t *a)
{
	/* Handles the a availability. */
	if (a == NULL)
		return EINVAL;
	a->type = PTHREAD_MUTEX_NORMAL;
	a->pshared = PTHREAD_PROCESS_PRIVATE;
	a->robust = PTHREAD_MUTEX_STALLED;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread mutexattr destroy operation.
 */
int
pthread_mutexattr_destroy(
	pthread_mutexattr_t *a)
{
	/* Returns the computed result. */
	return a != NULL ? 0 : EINVAL;
}

/*
 * Implements the pthread mutexattr gettype operation.
 */
int
pthread_mutexattr_gettype(
	const pthread_mutexattr_t *a,
	int *type)
{
	/* Handles the a availability. */
	if (a == NULL || type == NULL)
		return EINVAL;
	*type = (int)a->type;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread mutexattr settype operation.
 */
int
pthread_mutexattr_settype(
	pthread_mutexattr_t *a,
	int type)
{
	/* Handles an operation failure. */
	if (a == NULL || type < PTHREAD_MUTEX_NORMAL ||
	    type > PTHREAD_MUTEX_ERRORCHECK)

		/* Returns the computed result. */
		return EINVAL;
	a->type = (unsigned)type;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread mutexattr setpshared operation.
 */
int
pthread_mutexattr_setpshared(
	pthread_mutexattr_t *a,
	int shared)
{
	/* Handles the a availability. */
	if (a == NULL || (shared != PTHREAD_PROCESS_PRIVATE &&
			  shared != PTHREAD_PROCESS_SHARED))

		/* Returns the computed result. */
		return EINVAL;
	a->pshared = (unsigned)shared;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread mutexattr getrobust operation.
 */
int
pthread_mutexattr_getrobust(
	const pthread_mutexattr_t *a,
	int *robust)
{
	/* Handles the a availability. */
	if (a == NULL || robust == NULL)
		return EINVAL;
	*robust = (int)a->robust;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread mutexattr setrobust operation.
 */
int
pthread_mutexattr_setrobust(
	pthread_mutexattr_t *a,
	int robust)
{
	/* Handles the a availability. */
	if (a == NULL ||
	    (robust != PTHREAD_MUTEX_STALLED && robust != PTHREAD_MUTEX_ROBUST))

		/* Returns the computed result. */
		return EINVAL;

	/* Handles the robust condition. */
	if (robust == PTHREAD_MUTEX_ROBUST)
		return ENOTSUP;
	a->robust = (unsigned)robust;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread mutex consistent operation.
 */
int
pthread_mutex_consistent(
	pthread_mutex_t *m)
{
	/* Returns the computed result. */
	return m == NULL || m->robust != PTHREAD_MUTEX_ROBUST ? EINVAL
							      : ENOTSUP;
}

/*
 * Implements the pthread mutex trylock operation.
 */
int
pthread_mutex_trylock(
	pthread_mutex_t *m)
{
	pthread_t self;

	/* Handles the m availability. */
	if (m == NULL)
		return EINVAL;
	self = pthread_self();

	/* Handles the m condition. */
	if (m->owner == self) {
		/* Handles the m condition. */
		if (m->type == PTHREAD_MUTEX_RECURSIVE) {
			m->count++;

			/* Reports successful completion. */
			return 0;
		}

		/* Handles an operation failure. */
		if (m->type == PTHREAD_MUTEX_ERRORCHECK)
			return EDEADLK;
	}

	/* Handles a failed atomic exchange n operation. */
	if (__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE) != 0)
		return EBUSY;
	m->owner = self;
	m->count = 1;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread mutex lock operation.
 */
int
pthread_mutex_lock(
	pthread_mutex_t *m)
{
	int error;

	/* Continue while the operation condition remains true. */
	while ((error = pthread_mutex_trylock(m)) == EBUSY)
		(void)usync_wait_word_flags(&m->locked, 1, NULL, m->pshared);

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the pthread mutex clocklock operation.
 */
int
pthread_mutex_clocklock(
	pthread_mutex_t *m,
	clockid_t clock,
	const struct timespec *absolute)
{
	int error;

	/* Handles the m availability. */
	if (m == NULL)
		return EINVAL;

	/* Continue while the operation condition remains true. */
	while ((error = pthread_mutex_trylock(m)) == EBUSY) {
		error = usync_wait_word_absolute(&m->locked, 1, absolute,
						 m->pshared, 0, clock);

		/* Handles an operation failure. */
		if (error != 0 && error != EAGAIN && error != EINTR)
			return error;
	}

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the pthread mutex timedlock operation.
 */
int
pthread_mutex_timedlock(
	pthread_mutex_t *m,
	const struct timespec *absolute)
{
	int function_result;

	/* Obtains the pthread mutex clocklock result. */
	function_result = pthread_mutex_clocklock(m, CLOCK_REALTIME, absolute);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the pthread mutex unlock operation.
 */
int
pthread_mutex_unlock(
	pthread_mutex_t *m)
{
	/* Handles a failed pthread self operation. */
	if (m == NULL || !m->locked || m->owner != pthread_self())
		return EPERM;

	/* Handles the m condition. */
	if (m->type == PTHREAD_MUTEX_RECURSIVE && --m->count != 0)
		return 0;
	m->owner = 0;
	m->count = 0;
	__atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
	usync_wake_word_flags(&m->locked, 1, m->pshared);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread cond init operation.
 */
int
pthread_cond_init(
	pthread_cond_t *c,
	const pthread_condattr_t *a)
{
	/* Handles the c availability. */
	if (c == NULL)
		return EINVAL;
	c->sequence = 0;
	c->pshared = a != NULL ? a->pshared : 0;
	c->clock = a != NULL ? a->clock : CLOCK_REALTIME;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread cond destroy operation.
 */
int
pthread_cond_destroy(
	pthread_cond_t *c)
{
	/* Returns the computed result. */
	return c != NULL ? 0 : EINVAL;
}

/*
 * Implements the pthread condattr init operation.
 */
int
pthread_condattr_init(
	pthread_condattr_t *a)
{
	/* Handles the a availability. */
	if (a == NULL)
		return EINVAL;
	a->clock = CLOCK_REALTIME;
	a->pshared = PTHREAD_PROCESS_PRIVATE;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread condattr destroy operation.
 */
int
pthread_condattr_destroy(
	pthread_condattr_t *a)
{
	/* Returns the computed result. */
	return a != NULL ? 0 : EINVAL;
}

/*
 * Implements the pthread condattr setpshared operation.
 */
int
pthread_condattr_setpshared(
	pthread_condattr_t *a,
	int shared)
{
	/* Handles the a availability. */
	if (a == NULL || (shared != PTHREAD_PROCESS_PRIVATE &&
			  shared != PTHREAD_PROCESS_SHARED))

		/* Returns the computed result. */
		return EINVAL;
	a->pshared = (unsigned)shared;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread condattr setclock operation.
 */
int
pthread_condattr_setclock(
	pthread_condattr_t *a,
	clockid_t clock)
{
	/* Handles the a availability. */
	if (a == NULL || (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC))
		return EINVAL;
	a->clock = (unsigned)clock;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread condattr getclock operation.
 */
int
pthread_condattr_getclock(
	const pthread_condattr_t *a,
	clockid_t *clock)
{
	/* Handles the a availability. */
	if (a == NULL || clock == NULL)
		return EINVAL;
	*clock = (clockid_t)a->clock;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread cond wait operation.
 */
int
pthread_cond_wait(
	pthread_cond_t *c,
	pthread_mutex_t *m)
{
	int function_result;

	/* Obtains the cond wait result. */
	function_result = cond_wait(c, m, NULL, CLOCK_REALTIME);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the pthread cond timedwait operation.
 */
int
pthread_cond_timedwait(
	pthread_cond_t *c,
	pthread_mutex_t *m,
	const struct timespec *absolute)
{
	int function_result;

	/* Handles the c availability. */
	if (c == NULL || m == NULL)
		return EINVAL;

	/* Obtains the cond wait result. */
	function_result = cond_wait(c, m, absolute, (clockid_t)c->clock);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the pthread cond clockwait operation.
 */
int
pthread_cond_clockwait(
	pthread_cond_t *c,
	pthread_mutex_t *m,
	clockid_t clock,
	const struct timespec *absolute)
{
	int function_result;

	/* Handles the c availability. */
	if (c == NULL || m == NULL)
		return EINVAL;

	/* Obtains the cond wait result. */
	function_result = cond_wait(c, m, absolute, clock);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the pthread cond signal operation.
 */
int
pthread_cond_signal(
	pthread_cond_t *c)
{
	/* Handles the c availability. */
	if (c == NULL)
		return EINVAL;
	__atomic_add_fetch(&c->sequence, 1, __ATOMIC_RELEASE);
	usync_wake_word_flags(&c->sequence, 1, c->pshared);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread cond broadcast operation.
 */
int
pthread_cond_broadcast(
	pthread_cond_t *c)
{
	/* Handles the c availability. */
	if (c == NULL)
		return EINVAL;
	__atomic_add_fetch(&c->sequence, 1, __ATOMIC_RELEASE);
	usync_wake_word_flags(&c->sequence, UINT32_MAX, c->pshared);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread rwlock init operation.
 */
int
pthread_rwlock_init(
	pthread_rwlock_t *lock,
	const pthread_rwlockattr_t *attr)
{
	/* Handles the lock availability. */
	if (lock == NULL)
		return EINVAL;
	memset(lock, 0, sizeof(*lock));
	lock->pshared = attr != NULL ? attr->pshared : PTHREAD_PROCESS_PRIVATE;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread rwlock destroy operation.
 */
int
pthread_rwlock_destroy(
	pthread_rwlock_t *lock)
{
	/* Handles the lock availability. */
	if (lock == NULL)
		return EINVAL;

	/* Returns the computed result. */
	return lock->guard != 0 || lock->readers != 0 || lock->writer != 0
		   ? EBUSY
		   : 0;
}

/*
 * Implements the pthread rwlock tryrdlock operation.
 */
int
pthread_rwlock_tryrdlock(
	pthread_rwlock_t *lock)
{
	int error;

	error = 0;

	/* Handles the lock availability. */
	if (lock == NULL)
		return EINVAL;
	rwlock_guard_lock(lock);

	/* Handles the lock condition. */
	if (lock->writer != 0)
		error = EBUSY;
	else if (lock->readers == UINT32_MAX)
		error = EAGAIN;
	else
		lock->readers++;
	rwlock_guard_unlock(lock);

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the pthread rwlock rdlock operation.
 */
int
pthread_rwlock_rdlock(
	pthread_rwlock_t *lock)
{
	uint32_t sequence;
	int error;

	/* Handles the lock availability. */
	if (lock == NULL)
		return EINVAL;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		error = pthread_rwlock_tryrdlock(lock);

		/* Handles an operation failure. */
		if (error != EBUSY)
			return error;
		sequence = __atomic_load_n(&lock->sequence, __ATOMIC_ACQUIRE);
		error = usync_wait_word_flags(&lock->sequence, sequence, NULL,
					      lock->pshared);

		/* Handles an operation failure. */
		if (error != 0 && error != EAGAIN && error != EINTR)
			return error;
	}
}

/*
 * Implements the pthread rwlock clockrdlock operation.
 */
int
pthread_rwlock_clockrdlock(
	pthread_rwlock_t *lock,
	clockid_t clock,
	const struct timespec *absolute)
{
	uint32_t sequence;
	int error;

	/* Handles the lock availability. */
	if (lock == NULL)
		return EINVAL;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		error = pthread_rwlock_tryrdlock(lock);

		/* Handles an operation failure. */
		if (error != EBUSY)
			return error;
		sequence = __atomic_load_n(&lock->sequence, __ATOMIC_ACQUIRE);
		error =
		    usync_wait_word_absolute(&lock->sequence, sequence,
					     absolute, lock->pshared, 0, clock);

		/* Handles an operation failure. */
		if (error != 0 && error != EAGAIN && error != EINTR)
			return error;
	}
}

/*
 * Implements the pthread rwlock timedrdlock operation.
 */
int
pthread_rwlock_timedrdlock(
	pthread_rwlock_t *lock,
	const struct timespec *absolute)
{
	int function_result;

	/* Obtains the pthread rwlock clockrdlock result. */
	function_result = pthread_rwlock_clockrdlock(lock, CLOCK_REALTIME, absolute);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the pthread rwlock trywrlock operation.
 */
int
pthread_rwlock_trywrlock(
	pthread_rwlock_t *lock)
{
	int error;

	error = 0;

	/* Handles the lock availability. */
	if (lock == NULL)
		return EINVAL;
	rwlock_guard_lock(lock);

	/* Handles the lock condition. */
	if (lock->writer != 0 || lock->readers != 0)
		error = EBUSY;
	else
		lock->writer = 1;
	rwlock_guard_unlock(lock);

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the pthread rwlock wrlock operation.
 */
int
pthread_rwlock_wrlock(
	pthread_rwlock_t *lock)
{
	uint32_t sequence;
	int error;

	/* Handles the lock availability. */
	if (lock == NULL)
		return EINVAL;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		error = pthread_rwlock_trywrlock(lock);

		/* Handles an operation failure. */
		if (error != EBUSY)
			return error;
		sequence = __atomic_load_n(&lock->sequence, __ATOMIC_ACQUIRE);
		error = usync_wait_word_flags(&lock->sequence, sequence, NULL,
					      lock->pshared);

		/* Handles an operation failure. */
		if (error != 0 && error != EAGAIN && error != EINTR)
			return error;
	}
}

/*
 * Implements the pthread rwlock clockwrlock operation.
 */
int
pthread_rwlock_clockwrlock(
	pthread_rwlock_t *lock,
	clockid_t clock,
	const struct timespec *absolute)
{
	uint32_t sequence;
	int error;

	/* Handles the lock availability. */
	if (lock == NULL)
		return EINVAL;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		error = pthread_rwlock_trywrlock(lock);

		/* Handles an operation failure. */
		if (error != EBUSY)
			return error;
		sequence = __atomic_load_n(&lock->sequence, __ATOMIC_ACQUIRE);
		error =
		    usync_wait_word_absolute(&lock->sequence, sequence,
					     absolute, lock->pshared, 0, clock);

		/* Handles an operation failure. */
		if (error != 0 && error != EAGAIN && error != EINTR)
			return error;
	}
}

/*
 * Implements the pthread rwlock timedwrlock operation.
 */
int
pthread_rwlock_timedwrlock(
	pthread_rwlock_t *lock,
	const struct timespec *absolute)
{
	int function_result;

	/* Obtains the pthread rwlock clockwrlock result. */
	function_result = pthread_rwlock_clockwrlock(lock, CLOCK_REALTIME, absolute);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the pthread rwlock unlock operation.
 */
int
pthread_rwlock_unlock(
	pthread_rwlock_t *lock)
{
	int error;

	error = 0;

	/* Handles the lock availability. */
	if (lock == NULL)
		return EINVAL;
	rwlock_guard_lock(lock);

	/* Handles the lock condition. */
	if (lock->writer != 0)
		lock->writer = 0;
	else if (lock->readers != 0)
		lock->readers--;
	else
		error = EPERM;

	/* Handles an operation failure. */
	if (error == 0)
		(void)__atomic_add_fetch(&lock->sequence, 1, __ATOMIC_RELEASE);
	rwlock_guard_unlock(lock);

	/* Handles an operation failure. */
	if (error == 0) {
		usync_wake_word_flags(&lock->sequence, UINT32_MAX,
				      lock->pshared);
	}

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the pthread rwlockattr init operation.
 */
int
pthread_rwlockattr_init(
	pthread_rwlockattr_t *attr)
{
	/* Handles the attr availability. */
	if (attr == NULL)
		return EINVAL;
	attr->pshared = PTHREAD_PROCESS_PRIVATE;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread rwlockattr destroy operation.
 */
int
pthread_rwlockattr_destroy(
	pthread_rwlockattr_t *attr)
{
	/* Returns the computed result. */
	return attr != NULL ? 0 : EINVAL;
}

/*
 * Implements the pthread rwlockattr setpshared operation.
 */
int
pthread_rwlockattr_setpshared(
	pthread_rwlockattr_t *attr,
	int shared)
{
	/* Handles the attr availability. */
	if (attr == NULL || (shared != PTHREAD_PROCESS_PRIVATE &&
			     shared != PTHREAD_PROCESS_SHARED))

		/* Returns the computed result. */
		return EINVAL;
	attr->pshared = (unsigned)shared;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread barrier init operation.
 */
int
pthread_barrier_init(
	pthread_barrier_t *barrier,
	const pthread_barrierattr_t *attr,
	unsigned count)
{
	/* Handles the barrier availability. */
	if (barrier == NULL || count == 0)
		return EINVAL;
	memset(barrier, 0, sizeof(*barrier));
	barrier->trip = count;
	barrier->pshared =
	    attr != NULL ? attr->pshared : PTHREAD_PROCESS_PRIVATE;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread barrier destroy operation.
 */
int
pthread_barrier_destroy(
	pthread_barrier_t *barrier)
{
	/* Handles the barrier availability. */
	if (barrier == NULL)
		return EINVAL;

	/* Returns the computed result. */
	return barrier->guard != 0 || barrier->count != 0 ? EBUSY : 0;
}

/*
 * Implements the pthread barrier wait operation.
 */
int
pthread_barrier_wait(
	pthread_barrier_t *barrier)
{
	uint32_t generation;
	int error;

	/* Handles the barrier availability. */
	if (barrier == NULL || barrier->trip == 0)
		return EINVAL;
	barrier_guard_lock(barrier);
	generation = barrier->sequence;
	barrier->count++;

	/* Handles the barrier condition. */
	if (barrier->count == barrier->trip) {
		barrier->count = 0;
		(void)__atomic_add_fetch(&barrier->sequence, 1,
					 __ATOMIC_RELEASE);
		barrier_guard_unlock(barrier);
		usync_wake_word_flags(&barrier->sequence, UINT32_MAX,
				      barrier->pshared);

		/* Returns the computed result. */
		return PTHREAD_BARRIER_SERIAL_THREAD;
	}
	barrier_guard_unlock(barrier);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed atomic load n operation. */
		if (__atomic_load_n(&barrier->sequence, __ATOMIC_ACQUIRE) !=
		    generation)

			/* Reports successful completion. */
			return 0;
		error = usync_wait_word_flags(&barrier->sequence, generation,
					      NULL, barrier->pshared);

		/*
 * usync buckets deliberately wake colliding addresses.  A zero
		 * return is therefore only a hint; the generation is the
		 * barrier predicate. */
		if (error == 0 || error == EAGAIN || error == EINTR)
			continue;
		barrier_guard_lock(barrier);

		/* Handles the barrier condition. */
		if (barrier->sequence == generation && barrier->count != 0)
			barrier->count--;
		barrier_guard_unlock(barrier);

		/* Returns the computed result. */
		return error;
	}
}

/*
 * Implements the pthread barrierattr init operation.
 */
int
pthread_barrierattr_init(
	pthread_barrierattr_t *attr)
{
	/* Handles the attr availability. */
	if (attr == NULL)
		return EINVAL;
	attr->pshared = PTHREAD_PROCESS_PRIVATE;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread barrierattr destroy operation.
 */
int
pthread_barrierattr_destroy(
	pthread_barrierattr_t *attr)
{
	/* Returns the computed result. */
	return attr != NULL ? 0 : EINVAL;
}

/*
 * Implements the pthread barrierattr setpshared operation.
 */
int
pthread_barrierattr_setpshared(
	pthread_barrierattr_t *attr,
	int shared)
{
	/* Handles the attr availability. */
	if (attr == NULL || (shared != PTHREAD_PROCESS_PRIVATE &&
			     shared != PTHREAD_PROCESS_SHARED))

		/* Returns the computed result. */
		return EINVAL;
	attr->pshared = (unsigned)shared;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread spin init operation.
 */
int
pthread_spin_init(
	pthread_spinlock_t *lock,
	int shared)
{
	/* Handles the lock availability. */
	if (lock == NULL || (shared != PTHREAD_PROCESS_PRIVATE &&
			     shared != PTHREAD_PROCESS_SHARED))

		/* Returns the computed result. */
		return EINVAL;
	*lock = 0;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread spin destroy operation.
 */
int
pthread_spin_destroy(
	pthread_spinlock_t *lock)
{
	/* Returns the computed result. */
	return lock == NULL ? EINVAL : (*lock != 0 ? EBUSY : 0);
}

/*
 * Implements the pthread spin trylock operation.
 */
int
pthread_spin_trylock(
	pthread_spinlock_t *lock)
{
	int function_result;

	/* Handles the lock availability. */
	if (lock == NULL)
		return EINVAL;

	/* Computes the function result. */
	function_result = __atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) == 0 ? 0 : EBUSY;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the pthread spin lock operation.
 */
int
pthread_spin_lock(
	pthread_spinlock_t *lock)
{
	int error;

	/* Handles the lock availability. */
	if (lock == NULL)
		return EINVAL;

	/* Continue while the operation condition remains true. */
	while ((error = pthread_spin_trylock(lock)) == EBUSY)
		(void)usync_wait_word(lock, 1, NULL);

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the pthread spin unlock operation.
 */
int
pthread_spin_unlock(
	pthread_spinlock_t *lock)
{
	/* Handles the lock availability. */
	if (lock == NULL || *lock == 0)
		return EPERM;
	__atomic_store_n(lock, 0, __ATOMIC_RELEASE);
	usync_wake_word(lock, 1);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread once operation.
 */
int
pthread_once(
	pthread_once_t *once,
	void (*function)(void))
{
	uint32_t previous;

	/* Handles the once availability. */
	if (once == NULL || function == NULL)
		return EINVAL;
	previous = __atomic_exchange_n(&once->state, 1, __ATOMIC_ACQ_REL);

	/* Handles the previous condition. */
	if (previous == 0) {
		function();
		__atomic_store_n(&once->state, 2, __ATOMIC_RELEASE);
		usync_wake_word(&once->state, UINT32_MAX);
	} else {
		/* Handles the previous condition. */
		if (previous == 2) {
			__atomic_store_n(&once->state, 2, __ATOMIC_RELEASE);
			usync_wake_word(&once->state, UINT32_MAX);
		}
		while (__atomic_load_n(&once->state, __ATOMIC_ACQUIRE) != 2)
			(void)usync_wait_word(&once->state, 1, NULL);
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread key create operation.
 */
int
pthread_key_create(
	pthread_key_t *key,
	void (*destructor)(void *))
{
	unsigned i;

	/* Handles the key availability. */
	if (key == NULL)
		return EINVAL;
	word_lock(&key_lock);

	/* Process each element required by the operation. */
	for (i = 0; i < KEY_MAX; i++) {
		/* Handles the key destructor condition. */
		if (key_destructor[i] == NULL) {
			key_destructor[i] = destructor != NULL
						? destructor
						: (void (*)(void *))1;
			*key = i;
			word_unlock(&key_lock);

			/* Reports successful completion. */
			return 0;
		}
	}
	word_unlock(&key_lock);

	/* Returns the computed result. */
	return EAGAIN;
}

/*
 * Implements the pthread key delete operation.
 */
int
pthread_key_delete(
	pthread_key_t key)
{
	/* Handles the selected key. */
	if (key >= KEY_MAX)
		return EINVAL;
	word_lock(&key_lock);
	key_destructor[key] = NULL;
	word_unlock(&key_lock);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread setspecific operation.
 */
int
pthread_setspecific(
	pthread_key_t key,
	const void *value)
{
	struct pthread_tcb *tcb;

	ensure_main();
	tcb = self_tcb();

	/* Handles the tcb availability. */
	if (key >= KEY_MAX || key_destructor[key] == NULL || tcb == NULL)
		return EINVAL;
	tcb->keys[key] = value;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread getspecific operation.
 */
void *
pthread_getspecific(
	pthread_key_t key)
{
	struct pthread_tcb *tcb;

	ensure_main();
	tcb = self_tcb();

	/* Returns the computed result. */
	return key < KEY_MAX && tcb != NULL ? (void *)tcb->keys[key] : NULL;
}

/*
 * Implements the pthread sigmask operation.
 */
int
pthread_sigmask(
	int how,
	const sigset_t *set,
	sigset_t *old)
{
	int function_result;

	/* Computes the function result. */
	function_result = sigprocmask(how, set, old) == 0 ? 0 : errno;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the pthread kill operation.
 */
int
pthread_kill(
	pthread_t thread,
	int signo)
{
	intptr_t result;

	/* Handles the signo condition. */
	if (signo < 0 || signo > SIGRTMAX)
		return EINVAL;
	result = call(ZEDBSD_SYS_thread_kill, thread, signo, 0, 0, 0, 0);

	/* Returns the computed result. */
	return result < 0 ? errno : 0;
}

/*
 * Implements the pthread cancel operation.
 */
int
pthread_cancel(
	pthread_t thread)
{
	intptr_t result;

	result = call(ZEDBSD_SYS_thread_cancel, thread,
			       ZEDBSD_THREAD_CANCEL_REQUEST, 0, 0, 0, 0);

	/* Checks the operation result. */
	if (result < 0)
		return errno;

	/* Handles a failed pthread self operation. */
	if (thread == pthread_self())
		pthread_testcancel();

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread setcancelstate operation.
 */
int
pthread_setcancelstate(
	int state,
	int *old_state)
{
	struct pthread_tcb *tcb;

	/* Handles the state condition. */
	if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE)
		return EINVAL;
	ensure_main();
	tcb = self_tcb();

	/* Handles the old state availability. */
	if (old_state != NULL)
		*old_state = tcb->cancel_state;
	tcb->cancel_state = state;

	/* Handles the state condition. */
	if (state == PTHREAD_CANCEL_ENABLE &&
	    tcb->cancel_type == PTHREAD_CANCEL_ASYNCHRONOUS)
		pthread_testcancel();

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread setcanceltype operation.
 */
int
pthread_setcanceltype(
	int type,
	int *old_type)
{
	struct pthread_tcb *tcb;

	/* Handles the type condition. */
	if (type != PTHREAD_CANCEL_DEFERRED &&
	    type != PTHREAD_CANCEL_ASYNCHRONOUS)

		/* Returns the computed result. */
		return EINVAL;
	ensure_main();
	tcb = self_tcb();

	/* Handles the old type availability. */
	if (old_type != NULL)
		*old_type = tcb->cancel_type;
	tcb->cancel_type = type;

	/* Handles the type condition. */
	if (type == PTHREAD_CANCEL_ASYNCHRONOUS &&
	    tcb->cancel_state == PTHREAD_CANCEL_ENABLE)
		pthread_testcancel();

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the pthread testcancel operation.
 */
void
pthread_testcancel(
	void)
{
	struct pthread_tcb *tcb;
	intptr_t pending;

	ensure_main();
	tcb = self_tcb();

	/* Handles the tcb availability. */
	if (tcb == NULL || tcb->cancel_state == PTHREAD_CANCEL_DISABLE)
		return;
	pending = call(ZEDBSD_SYS_thread_cancel, 0, ZEDBSD_THREAD_CANCEL_CLEAR,
		       0, 0, 0, 0);

	/* Handles the pending condition. */
	if (pending > 0)
		pthread_exit(PTHREAD_CANCELED);
}

/*
 * Implements the pthread cancel point operation.
 */
void
__pthread_cancel_point(
	void)
{
	pthread_testcancel();
}

/*
 * Implements the pthread cleanup push operation.
 */
void
__pthread_cleanup_push(
	struct __pthread_cleanup *cleanup,
	void (*routine)(void *),
	void *argument)
{
	struct pthread_tcb *tcb;

	/* Handles the cleanup availability. */
	if (cleanup == NULL || routine == NULL)
		return;
	ensure_main();
	tcb = self_tcb();
	cleanup->routine = routine;
	cleanup->argument = argument;
	cleanup->previous = tcb->cleanup;
	tcb->cleanup = cleanup;
}

/*
 * Implements the pthread cleanup pop operation.
 */
void
__pthread_cleanup_pop(
	struct __pthread_cleanup *cleanup,
	int execute)
{
	struct pthread_tcb *tcb;

	/* Handles the cleanup availability. */
	if (cleanup == NULL)
		return;
	ensure_main();
	tcb = self_tcb();

	/* Handles the tcb condition. */
	if (tcb->cleanup == cleanup)
		tcb->cleanup = cleanup->previous;

	/* Handles the execute condition. */
	if (execute)
		cleanup->routine(cleanup->argument);
}

/*
 * Implements the call once operation.
 */
void
call_once(
	once_flag *flag,
	void (*function)(void))
{
	(void)pthread_once(flag, function);
}

/*
 * Implements the cnd broadcast operation.
 */
int
cnd_broadcast(
	cnd_t *condition)
{
	int function_result;

	/* Obtains the c11 result result. */
	function_result = c11_result(pthread_cond_broadcast(condition));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the cnd destroy operation.
 */
void
cnd_destroy(
	cnd_t *condition)
{
	(void)pthread_cond_destroy(condition);
}

/*
 * Implements the cnd init operation.
 */
int
cnd_init(
	cnd_t *condition)
{
	int function_result;

	/* Obtains the c11 result result. */
	function_result = c11_result(pthread_cond_init(condition, NULL));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the cnd signal operation.
 */
int
cnd_signal(
	cnd_t *condition)
{
	int function_result;

	/* Obtains the c11 result result. */
	function_result = c11_result(pthread_cond_signal(condition));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the cnd timedwait operation.
 */
int
cnd_timedwait(
	cnd_t *condition,
	mtx_t *mutex,
	const struct timespec *absolute)
{
	int function_result;

	/* Obtains the c11 result result. */
	function_result = c11_result(pthread_cond_timedwait(condition, mutex, absolute));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the cnd wait operation.
 */
int
cnd_wait(
	cnd_t *condition,
	mtx_t *mutex)
{
	int function_result;

	/* Obtains the c11 result result. */
	function_result = c11_result(pthread_cond_wait(condition, mutex));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mtx destroy operation.
 */
void
mtx_destroy(
	mtx_t *mutex)
{
	(void)pthread_mutex_destroy(mutex);
}

/*
 * Implements the mtx init operation.
 */
int
mtx_init(
	mtx_t *mutex,
	int type)
{
	int function_result;
	pthread_mutexattr_t attributes;
	int error;

	/* Handles the type condition. */
	if ((type & ~(mtx_recursive | mtx_timed)) != 0)
		return thrd_error;
	error = pthread_mutexattr_init(&attributes);

	/* Handles an operation failure. */
	if (error == 0 && (type & mtx_recursive) != 0) {
		error = pthread_mutexattr_settype(&attributes,
						  PTHREAD_MUTEX_RECURSIVE);
	}

	/* Handles an operation failure. */
	if (error == 0)
		error = pthread_mutex_init(mutex, &attributes);
	(void)pthread_mutexattr_destroy(&attributes);

	/* Obtains the c11 result result. */
	function_result = c11_result(error);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mtx lock operation.
 */
int
mtx_lock(
	mtx_t *mutex)
{
	int function_result;

	/* Obtains the c11 result result. */
	function_result = c11_result(pthread_mutex_lock(mutex));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mtx timedlock operation.
 */
int
mtx_timedlock(
	mtx_t *mutex,
	const struct timespec *absolute)
{
	int function_result;

	/* Obtains the c11 result result. */
	function_result = c11_result(pthread_mutex_timedlock(mutex, absolute));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mtx trylock operation.
 */
int
mtx_trylock(
	mtx_t *mutex)
{
	int function_result;

	/* Obtains the c11 result result. */
	function_result = c11_result(pthread_mutex_trylock(mutex));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mtx unlock operation.
 */
int
mtx_unlock(
	mtx_t *mutex)
{
	int function_result;

	/* Obtains the c11 result result. */
	function_result = c11_result(pthread_mutex_unlock(mutex));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the thrd create operation.
 */
int
thrd_create(
	thrd_t *thread,
	thrd_start_t function,
	void *argument)
{
	int function_result;
	struct c11_start_context *context;
	int error;

	/* Handles the thread availability. */
	if (thread == NULL || function == NULL)
		return thrd_error;
	context = malloc(sizeof(*context));

	/* Handles the context availability. */
	if (context == NULL)
		return thrd_nomem;
	context->function = function;
	context->argument = argument;
	error = pthread_create(thread, NULL, c11_start, context);

	/* Handles an operation failure. */
	if (error != 0)
		free(context);

	/* Obtains the c11 result result. */
	function_result = c11_result(error);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the thrd current operation.
 */
thrd_t
thrd_current(
	void)
{
	thrd_t function_result;

	/* Obtains the pthread self result. */
	function_result = pthread_self();

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the thrd detach operation.
 */
int
thrd_detach(
	thrd_t thread)
{
	int function_result;

	/* Obtains the c11 result result. */
	function_result = c11_result(pthread_detach(thread));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the thrd equal operation.
 */
int
thrd_equal(
	thrd_t left,
	thrd_t right)
{
	int function_result;

	/* Obtains the pthread equal result. */
	function_result = pthread_equal(left, right);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the thrd exit operation.
 */
_Noreturn void
thrd_exit(
	int result)
{
	pthread_exit((void *)(intptr_t)result);
}

/*
 * Implements the thrd join operation.
 */
int
thrd_join(
	thrd_t thread,
	int *result)
{
	int function_result;
	void *value;
	int error;

	error = pthread_join(thread, &value);

	/* Handles an operation failure. */
	if (error == 0 && result != NULL)
		*result = (int)(intptr_t)value;
	/* Obtains the c11 result result. */
	function_result = c11_result(error);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the thrd sleep operation.
 */
int
thrd_sleep(
	const struct timespec *duration,
	struct timespec *remaining)
{
	/* Handles a failed nanosleep operation. */
	if (nanosleep(duration, remaining) == 0)
		return 0;

	/* Returns the computed result. */
	return errno == EINTR ? -1 : -2;
}

/*
 * Implements the thrd yield operation.
 */
void
thrd_yield(
	void)
{
	(void)sched_yield();
}

/*
 * Implements the tss create operation.
 */
int
tss_create(
	tss_t *key,
	tss_dtor_t destructor)
{
	int function_result;

	/* Obtains the c11 result result. */
	function_result = c11_result(pthread_key_create(key, destructor));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the tss delete operation.
 */
void
tss_delete(
	tss_t key)
{
	(void)pthread_key_delete(key);
}

/*
 * Implements the tss get operation.
 */
void *
tss_get(
	tss_t key)
{
	void *function_result;

	/* Obtains the pthread getspecific result. */
	function_result = pthread_getspecific(key);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the tss set operation.
 */
int
tss_set(
	tss_t key,
	void *value)
{
	int function_result;

	/* Obtains the c11 result result. */
	function_result = c11_result(pthread_setspecific(key, value));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the word lock operation. */
static void
word_lock(
	volatile uint32_t *word)
{
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed atomic exchange n operation. */
		if (__atomic_exchange_n(word, 1, __ATOMIC_ACQUIRE) == 0)
			return;
		(void)usync_wait_word(word, 1, NULL);
	}
}

/* Supports the usync wait word operation. */
static int
usync_wait_word(
	volatile uint32_t *a,
	uint32_t v,
	const struct timespec *t)
{
	int function_result;

	/* Obtains the usync wait word flags result. */
	function_result = usync_wait_word_flags(a, v, t, 0);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the usync wait word flags operation. */
static int
usync_wait_word_flags(
	volatile uint32_t *address,
	uint32_t value,
	const struct timespec *timeout,
	int pshared)
{
	int function_result;

	/* Obtains the usync wait word flags cancelable result. */
	function_result = usync_wait_word_flags_cancelable(address, value, timeout,
						pshared, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the usync wait word flags cancelable operation. */
static int
usync_wait_word_flags_cancelable(
	volatile uint32_t *address,
	uint32_t value,
	const struct timespec *timeout,
	int pshared,
	int cancelable,
	unsigned timeout_flags)
{
	intptr_t result;

	result = call(ZEDBSD_SYS_usync, (uintptr_t)address,
			       ZEDBSD_USYNC_WAIT, value, (uintptr_t)timeout, 0,
			       (pshared ? 0 : ZEDBSD_USYNC_PRIVATE) |
				   (cancelable ? ZEDBSD_USYNC_CANCELABLE : 0) |
				   timeout_flags);

	/* Returns the computed result. */
	return result < 0 ? errno : 0;
}

/* Supports the call operation. */
static intptr_t
call(
	uint32_t number,
	uintptr_t a,
	uintptr_t b,
	uintptr_t c,
	uintptr_t d,
	uintptr_t e,
	uintptr_t f)
{
	intptr_t function_result;

	/* Obtains the syscall result result. */
	function_result = syscall_result(__syscall6(number, a, b, c, d, e, f));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the word unlock operation. */
static void
word_unlock(
	volatile uint32_t *word)
{
	__atomic_store_n(word, 0, __ATOMIC_RELEASE);
	usync_wake_word(word, 1);
}

/* Supports the usync wake word operation. */
static void
usync_wake_word(
	volatile uint32_t *a,
	unsigned n)
{
	usync_wake_word_flags(a, n, 0);
}

/* Supports the usync wake word flags operation. */
static void
usync_wake_word_flags(
	volatile uint32_t *address,
	unsigned count,
	int pshared)
{
	(void)call(ZEDBSD_SYS_usync, (uintptr_t)address, ZEDBSD_USYNC_WAKE, 0,
		   0, count, pshared ? 0 : ZEDBSD_USYNC_PRIVATE);
}

/* Supports the self tcb operation. */
static struct pthread_tcb *
self_tcb(
	void)
{
	struct pthread_tcb *function_result;

	/* Computes the function result. */
	function_result = (struct pthread_tcb *)RTLD_CALL(pthread_private)();

	/* Returns the computed result. */
	return function_result;
}

/* Supports the ensure main operation. */
static void
ensure_main(
	void)
{
	/* Handles a failed self tcb operation. */
	if (self_tcb() != NULL)
		return;
	memset(&main_tcb, 0, sizeof(main_tcb));
	main_tcb.tid =
	    (pthread_t)call(ZEDBSD_SYS_thread_self, 0, 0, 0, 0, 0, 0);

	/* Handles a failed RTLD CALL operation. */
	if (RTLD_CALL(thread_attach)(&main_tcb) != 0) {
		/* Continue until the operation reaches a terminal state. */
		for (;;)
			;
	}
	main_tcb.runtime_tcb = (struct __rtld_tcb *)(uintptr_t)__syscall6(
	    ZEDBSD_SYS_thread_self, ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
}

/* Supports the registry add operation. */
static void
registry_add(
	struct pthread_tcb *tcb)
{
	word_lock(&registry_lock);
	tcb->next = tcb_list;
	tcb_list = tcb;
	word_unlock(&registry_lock);
}

/* Supports the run destructors operation. */
static void
run_destructors(
	struct pthread_tcb *tcb)
{
	void (*destructor)(void *);
	void *value;
	int invoked;
	unsigned pass, key;

	/* Handles the tcb availability. */
	if (tcb == NULL)
		return;

	/* Process each element required by the operation. */
	for (pass = 0; pass < DESTRUCTOR_ITERATIONS; pass++) {
		/* Process each element required by the operation. */
		invoked = 0;
		for (key = 0; key < KEY_MAX; key++) {
			destructor = key_destructor[key];
			value = (void *)tcb->keys[key];

			/* Handles a failed void operation. */
			if (value == NULL || destructor == NULL ||
			    destructor == (void (*)(void *))1)
				continue;
			tcb->keys[key] = NULL;
			destructor(value);
			invoked = 1;
		}

		/* Handles the invoked condition. */
		if (!invoked)
			break;
	}
}

/* Supports the detached enqueue operation. */
static void
detached_enqueue(
	struct pthread_tcb *tcb)
{
	word_lock(&detached_lock);

	/* Handles the tcb condition. */
	if (!tcb->detached_queued) {
		tcb->detached_queued = 1;
		tcb->detached_next = NULL;

		/* Handles the detached tail availability. */
		if (detached_tail != NULL)
			detached_tail->detached_next = tcb;
		else
			detached_head = tcb;
		detached_tail = tcb;
		__atomic_add_fetch(&detached_generation, 1, __ATOMIC_RELEASE);
	}
	word_unlock(&detached_lock);
	usync_wake_word(&detached_generation, 1);
}

/* Supports the registry find operation. */
static struct pthread_tcb *
registry_find(
	pthread_t tid)
{
	struct pthread_tcb *tcb;

	word_lock(&registry_lock);

	/* Process each linked entry. */
	for (tcb = tcb_list; tcb != NULL; tcb = tcb->next) {
		/* Handles the tcb condition. */
		if (tcb->tid == tid)
			break;
	}
	word_unlock(&registry_lock);

	/* Returns the computed result. */
	return tcb;
}

/* Supports the registry remove operation. */
static struct pthread_tcb *
registry_remove(
	pthread_t tid)
{
	struct pthread_tcb **link, *found;

	found = NULL;
	word_lock(&registry_lock);

	/* Process each linked entry. */
	for (link = &tcb_list; *link != NULL; link = &(*link)->next) {
		/* Handles the link condition. */
		if ((*link)->tid == tid) {
			found = *link;
			*link = found->next;
			found->next = NULL;
			break;
		}
	}
	word_unlock(&registry_lock);

	/* Returns the computed result. */
	return found;
}

/* Supports the ensure detached reaper operation. */
static int
ensure_detached_reaper(
	void)
{
	int error;

	error = 0;

	word_lock(&detached_lock);

	/* Handles the detached reaper started condition. */
	if (!detached_reaper_started) {
		detached_reaper_started = 1;
		error = pthread_create(&detached_reaper_tid, NULL,
				       detached_reaper, NULL);

		/* Handles an operation failure. */
		if (error != 0)
			detached_reaper_started = 0;
	}
	word_unlock(&detached_lock);

	/* Returns the computed result. */
	return error;
}

/* Supports the usync wait word absolute operation. */
static int
usync_wait_word_absolute(
	volatile uint32_t *address,
	uint32_t value,
	const struct timespec *absolute,
	int pshared,
	int cancelable,
	clockid_t clock)
{
	int function_result;
	unsigned flags;

	flags = ZEDBSD_USYNC_ABSTIME;

	/* Handles the clock condition. */
	if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)
		return EINVAL;

	/* Handles the clock condition. */
	if (clock == CLOCK_REALTIME)
		flags |= ZEDBSD_USYNC_CLOCK_REALTIME;

	/* Obtains the usync wait word flags cancelable result. */
	function_result = usync_wait_word_flags_cancelable(address, value, absolute,
						pshared, cancelable, flags);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the cond wait operation. */
static int
cond_wait(
	pthread_cond_t *c,
	pthread_mutex_t *m,
	const struct timespec *absolute,
	clockid_t clock)
{
	int lock_error;
	uint32_t sequence;
	int error;

	/* Handles the c availability. */
	if (c == NULL || m == NULL)
		return EINVAL;
	pthread_testcancel();
	sequence = __atomic_load_n(&c->sequence, __ATOMIC_ACQUIRE);
	error = pthread_mutex_unlock(m);

	/* Handles an operation failure. */
	if (error != 0)
		return error;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed atomic load n operation. */
		if (__atomic_load_n(&c->sequence, __ATOMIC_ACQUIRE) !=
		    sequence) {
			error = 0;
			break;
		}

		/* Handles the absolute availability. */
		if (absolute != NULL) {
			error = usync_wait_word_absolute(
			    &c->sequence, sequence, absolute, c->pshared,
			    __pthread_cancel_enabled(), clock);
		} else {
			error = usync_wait_word_flags_cancelable(
			    &c->sequence, sequence, NULL, c->pshared,
			    __pthread_cancel_enabled(), 0);
		}

		/* Handles an operation failure. */
		if (error == 0 || error == EAGAIN) {
			error = 0;
			break;
		}

		/* Handles an operation failure. */
		if (error != EINTR)
			break;

		/* Handles the cancel pending condition. */
		if (cancel_pending()) {
			/*
 * Do not clear/act on the request until the
			 * condition-wait contract has reacquired the caller's
			 * mutex. */
			error = 0;
			break;
		}

		/*
 * POSIX condition waits do not expose EINTR.  A timed wait
		 * always recomputes from its absolute deadline so caught
		 * signals cannot extend the timeout. */
	}

	lock_error = pthread_mutex_lock(m);

	/* Handles an operation failure. */
	if (lock_error == 0)
		pthread_testcancel();

	/* Returns the computed result. */
	return error != 0 ? error : lock_error;
}

/* Supports the cancel pending operation. */
static int
cancel_pending(
	void)
{
	intptr_t pending;

	/* Handles a failed pthread cancel enabled operation. */
	if (!__pthread_cancel_enabled())
		return 0;
	pending = call(ZEDBSD_SYS_thread_cancel, 0, ZEDBSD_THREAD_CANCEL_TEST,
		       0, 0, 0, 0);

	/* Returns the computed result. */
	return pending > 0;
}

/* Supports the rwlock guard lock operation. */
static void
rwlock_guard_lock(
	pthread_rwlock_t *lock)
{
	/* Continue while the operation condition remains true. */
	while (__atomic_exchange_n(&lock->guard, 1, __ATOMIC_ACQUIRE) != 0) {
		(void)usync_wait_word_flags(&lock->guard, 1, NULL,
					    lock->pshared);
	}
}

/* Supports the rwlock guard unlock operation. */
static void
rwlock_guard_unlock(
	pthread_rwlock_t *lock)
{
	__atomic_store_n(&lock->guard, 0, __ATOMIC_RELEASE);
	usync_wake_word_flags(&lock->guard, 1, lock->pshared);
}

/* Supports the barrier guard lock operation. */
static void
barrier_guard_lock(
	pthread_barrier_t *barrier)
{
	/* Continue while the operation condition remains true. */
	while (__atomic_exchange_n(&barrier->guard, 1, __ATOMIC_ACQUIRE) != 0) {
		(void)usync_wait_word_flags(&barrier->guard, 1, NULL,
					    barrier->pshared);
	}
}

/* Supports the barrier guard unlock operation. */
static void
barrier_guard_unlock(
	pthread_barrier_t *barrier)
{
	__atomic_store_n(&barrier->guard, 0, __ATOMIC_RELEASE);
	usync_wake_word_flags(&barrier->guard, 1, barrier->pshared);
}

/* Supports the c11 result operation. */
static int
c11_result(
	int error)
{
	/* Handles an operation failure. */
	if (error == 0)
		return thrd_success;

	/* Handles an operation failure. */
	if (error == ENOMEM || error == EAGAIN)
		return thrd_nomem;

	/* Handles an operation failure. */
	if (error == ETIMEDOUT)
		return thrd_timedout;

	/* Handles an operation failure. */
	if (error == EBUSY)
		return thrd_busy;

	/* Returns the computed result. */
	return thrd_error;
}

/* Supports the detached reaper operation. */
static void *
detached_reaper(
	void *argument)
{
	struct pthread_tcb *tcb;
	uint32_t generation;

	(void)argument;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		word_lock(&detached_lock);
		tcb = detached_head;

		/* Handles the tcb availability. */
		if (tcb != NULL) {
			detached_head = tcb->detached_next;

			/* Handles the detached head availability. */
			if (detached_head == NULL)
				detached_tail = NULL;
			tcb->detached_next = NULL;
		}
		generation =
		    __atomic_load_n(&detached_generation, __ATOMIC_ACQUIRE);
		word_unlock(&detached_lock);

		/* Handles the tcb availability. */
		if (tcb == NULL) {
			(void)usync_wait_word(&detached_generation, generation,
					      NULL);
			continue;
		}
		(void)call(ZEDBSD_SYS_thread_join, tcb->tid, 0, 0, 0, 0, 0);
		(void)registry_remove(tcb->tid);
		RTLD_CALL(thread_free)(tcb->runtime_tcb);

		/* Handles the tcb condition. */
		if (tcb->owns_stack)
			(void)munmap(tcb->stack, tcb->stack_size);
		free(tcb);
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the thread trampoline operation. */
static void
thread_trampoline(
	void)
{
	struct pthread_tcb *tcb;
	void *result;

	tcb = self_tcb();
	result = tcb != NULL ? tcb->start(tcb->argument) : NULL;
	pthread_exit(result);
}

/* Supports the c11 start operation. */
static void *
c11_start(
	void *argument)
{
	struct c11_start_context *context;
	thrd_start_t function;
	void *function_argument;
	int result;

	context = argument;
	function = context->function;
	function_argument = context->argument;

	free(context);
	result = function(function_argument);

	/* Returns the computed result. */
	return (void *)(intptr_t)result;
}
