/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

static void ensure_main(void);
static struct pthread_tcb *self_tcb(void);

static intptr_t
call(uint32_t number, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d,
     uintptr_t e, uintptr_t f)
{
	return syscall_result(__syscall6(number, a, b, c, d, e, f));
}

#if defined(ZEDBSD_DYNAMIC_LIBC)
#define RTLD_CALL(name) (__rtld_exports.name)
#else
#define RTLD_CALL(name) (__rtld_##name)
#endif

static int
usync_wait_word_flags_cancelable(volatile uint32_t *address, uint32_t value,
				 const struct timespec *timeout, int pshared,
				 int cancelable, unsigned timeout_flags)
{
	intptr_t result = call(ZEDBSD_SYS_usync, (uintptr_t)address,
			       ZEDBSD_USYNC_WAIT, value, (uintptr_t)timeout, 0,
			       (pshared ? 0 : ZEDBSD_USYNC_PRIVATE) |
				   (cancelable ? ZEDBSD_USYNC_CANCELABLE : 0) |
				   timeout_flags);
	return result < 0 ? errno : 0;
}

static int
usync_wait_word_flags(volatile uint32_t *address, uint32_t value,
		      const struct timespec *timeout, int pshared)
{
	return usync_wait_word_flags_cancelable(address, value, timeout,
						pshared, 0, 0);
}

static int
usync_wait_word_absolute(volatile uint32_t *address, uint32_t value,
			 const struct timespec *absolute, int pshared,
			 int cancelable, clockid_t clock)
{
	unsigned flags = ZEDBSD_USYNC_ABSTIME;

	if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)
		return EINVAL;
	if (clock == CLOCK_REALTIME)
		flags |= ZEDBSD_USYNC_CLOCK_REALTIME;
	return usync_wait_word_flags_cancelable(address, value, absolute,
						pshared, cancelable, flags);
}

static void
usync_wake_word_flags(volatile uint32_t *address, unsigned count, int pshared)
{
	(void)call(ZEDBSD_SYS_usync, (uintptr_t)address, ZEDBSD_USYNC_WAKE, 0,
		   0, count, pshared ? 0 : ZEDBSD_USYNC_PRIVATE);
}

static int
usync_wait_word(volatile uint32_t *a, uint32_t v, const struct timespec *t)
{
	return usync_wait_word_flags(a, v, t, 0);
}
static void
usync_wake_word(volatile uint32_t *a, unsigned n)
{
	usync_wake_word_flags(a, n, 0);
}

static void
word_lock(volatile uint32_t *word)
{
	for (;;) {
		if (__atomic_exchange_n(word, 1, __ATOMIC_ACQUIRE) == 0)
			return;
		(void)usync_wait_word(word, 1, NULL);
	}
}

static void
word_unlock(volatile uint32_t *word)
{
	__atomic_store_n(word, 0, __ATOMIC_RELEASE);
	usync_wake_word(word, 1);
}

void
__libc_heap_lock(void)
{
	word_lock(&heap_lock);
}
void
__libc_heap_unlock(void)
{
	word_unlock(&heap_lock);
}
void
__libc_environment_lock(void)
{
	word_lock(&environment_lock);
}
void
__libc_environment_unlock(void)
{
	word_unlock(&environment_lock);
}
uintptr_t
__stdio_thread_token(void)
{
	struct pthread_tcb *tcb = self_tcb();
	return (uintptr_t)(tcb != NULL ? tcb : &main_tcb);
}
void
__stdio_lock_wait(volatile uint32_t *word)
{
	(void)usync_wait_word(word, 1U, NULL);
}
void
__stdio_lock_wake(volatile uint32_t *word)
{
	usync_wake_word(word, 1U);
}

static struct pthread_tcb *
self_tcb(void)
{
	return (struct pthread_tcb *)RTLD_CALL(pthread_private)();
}

int
__pthread_cancel_enabled(void)
{
	struct pthread_tcb *tcb;

	ensure_main();
	tcb = self_tcb();
	return tcb != NULL && tcb->cancel_state == PTHREAD_CANCEL_ENABLE;
}

static int
cancel_pending(void)
{
	intptr_t pending;

	if (!__pthread_cancel_enabled())
		return 0;
	pending = call(ZEDBSD_SYS_thread_cancel, 0, ZEDBSD_THREAD_CANCEL_TEST,
		       0, 0, 0, 0);
	return pending > 0;
}

/* getenv() returns a pointer whose lifetime must not be ended by another
 * thread mutating environ.  The environment implementation installs a
 * private value snapshot here and replaces it only on this thread's next
 * environment operation.  Keeping this in the TCB also gives thread exit a
 * well-defined reclamation point without exposing pthread internals. */
char *
__pthread_environment_exchange(char *replacement)
{
	struct pthread_tcb *tcb;
	char *previous;

	ensure_main();
	tcb = self_tcb();
	if (tcb == NULL)
		return NULL;
	previous = tcb->environment_value;
	tcb->environment_value = replacement;
	return previous;
}

const void *
__pthread_locale_exchange(const void *replacement, int change)
{
	struct pthread_tcb *tcb;
	const void *previous;

	ensure_main();
	tcb = self_tcb();
	if (tcb == NULL)
		return NULL;
	previous = tcb->locale_value;
	if (change)
		tcb->locale_value = replacement;
	return previous;
}

void *
__pthread_mbstate(unsigned which)
{
	struct pthread_tcb *tcb;

	ensure_main();
	tcb = self_tcb();
	if (tcb == NULL || which > 2U)
		return NULL;
	return &tcb->multibyte_state[which * 2U];
}

fenv_t *
__libc_fenv_location(void)
{
#if defined(ZEDBSD_DYNAMIC_LIBC)
	static _Thread_local fenv_t environment = {0U, FE_TONEAREST};
	return &environment;
#else
	static fenv_t bootstrap = {0U, FE_TONEAREST};
	struct pthread_tcb *tcb = self_tcb();
	if (tcb != NULL && tcb->floating_environment.rounding == 0)
		tcb->floating_environment.rounding = FE_TONEAREST;
	return tcb != NULL ? &tcb->floating_environment : &bootstrap;
#endif
}

char *
__pthread_ptsname_buffer(size_t *size)
{
	struct pthread_tcb *tcb;

	ensure_main();
	tcb = self_tcb();
	if (tcb == NULL)
		return NULL;
	if (size != NULL)
		*size = sizeof(tcb->ptsname_buffer);
	return tcb->ptsname_buffer;
}

int *
__libc_errno_location(void)
{
#if defined(ZEDBSD_DYNAMIC_LIBC)
	static _Thread_local int dynamic_errno;
	return &dynamic_errno;
#else
	static int bootstrap_errno;
	struct pthread_tcb *tcb = self_tcb();
	return tcb != NULL ? &tcb->error : &bootstrap_errno;
#endif
}

static void
registry_add(struct pthread_tcb *tcb)
{
	word_lock(&registry_lock);
	tcb->next = tcb_list;
	tcb_list = tcb;
	word_unlock(&registry_lock);
}

static struct pthread_tcb *
registry_remove(pthread_t tid)
{
	struct pthread_tcb **link, *found = NULL;
	word_lock(&registry_lock);
	for (link = &tcb_list; *link != NULL; link = &(*link)->next) {
		if ((*link)->tid == tid) {
			found = *link;
			*link = found->next;
			found->next = NULL;
			break;
		}
	}
	word_unlock(&registry_lock);
	return found;
}

static struct pthread_tcb *
registry_find(pthread_t tid)
{
	struct pthread_tcb *tcb;

	word_lock(&registry_lock);
	for (tcb = tcb_list; tcb != NULL; tcb = tcb->next) {
		if (tcb->tid == tid)
			break;
	}
	word_unlock(&registry_lock);
	return tcb;
}

static void
detached_enqueue(struct pthread_tcb *tcb)
{
	word_lock(&detached_lock);
	if (!tcb->detached_queued) {
		tcb->detached_queued = 1;
		tcb->detached_next = NULL;
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

static void *
detached_reaper(void *argument)
{
	(void)argument;
	for (;;) {
		struct pthread_tcb *tcb;
		uint32_t generation;

		word_lock(&detached_lock);
		tcb = detached_head;
		if (tcb != NULL) {
			detached_head = tcb->detached_next;
			if (detached_head == NULL)
				detached_tail = NULL;
			tcb->detached_next = NULL;
		}
		generation =
		    __atomic_load_n(&detached_generation, __ATOMIC_ACQUIRE);
		word_unlock(&detached_lock);
		if (tcb == NULL) {
			(void)usync_wait_word(&detached_generation, generation,
					      NULL);
			continue;
		}
		(void)call(ZEDBSD_SYS_thread_join, tcb->tid, 0, 0, 0, 0, 0);
		(void)registry_remove(tcb->tid);
		RTLD_CALL(thread_free)(tcb->runtime_tcb);
		if (tcb->owns_stack)
			(void)munmap(tcb->stack, tcb->stack_size);
		free(tcb);
	}
	return NULL;
}

static int
ensure_detached_reaper(void)
{
	int error = 0;

	word_lock(&detached_lock);
	if (!detached_reaper_started) {
		detached_reaper_started = 1;
		error = pthread_create(&detached_reaper_tid, NULL,
				       detached_reaper, NULL);
		if (error != 0)
			detached_reaper_started = 0;
	}
	word_unlock(&detached_lock);
	return error;
}

static void
ensure_main(void)
{
	if (self_tcb() != NULL)
		return;
	memset(&main_tcb, 0, sizeof(main_tcb));
	main_tcb.tid =
	    (pthread_t)call(ZEDBSD_SYS_thread_self, 0, 0, 0, 0, 0, 0);
	if (RTLD_CALL(thread_attach)(&main_tcb) != 0)
		for (;;)
			;
	main_tcb.runtime_tcb = (struct __rtld_tcb *)(uintptr_t)__syscall6(
	    ZEDBSD_SYS_thread_self, ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
}

void
__pthread_initialize_main(void)
{
	ensure_main();
}

static void
thread_trampoline(void)
{
	struct pthread_tcb *tcb = self_tcb();
	void *result = tcb != NULL ? tcb->start(tcb->argument) : NULL;
	pthread_exit(result);
}

int
pthread_create(pthread_t *result, const pthread_attr_t *attributes,
	       void *(*start)(void *), void *argument)
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

	if (result == NULL || start == NULL || size < PTHREAD_STACK_MIN)
		return EINVAL;
	ensure_main();
	if (attributes != NULL && attributes->stackset) {
		stack = usable_stack = attributes->stackaddr;
		mapping_size = size;
		owns_stack = 0;
		if (stack == NULL)
			return EINVAL;
	} else {
		guard = (guard + 4095U) & ~(size_t)4095U;
		if (guard > SIZE_MAX - size)
			return EAGAIN;
		mapping_size = guard + size;
		stack = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (stack == MAP_FAILED)
			return EAGAIN;
		if (guard != 0 && mprotect(stack, guard, PROT_NONE) != 0) {
			(void)munmap(stack, mapping_size);
			return EAGAIN;
		}
		usable_stack = (unsigned char *)stack + guard;
		owns_stack = 1;
	}
	tcb = malloc(sizeof(*tcb));
	if (tcb == NULL) {
		if (owns_stack)
			(void)munmap(stack, mapping_size);
		return EAGAIN;
	}
	memset(tcb, 0, sizeof(*tcb));
	tcb->stack = stack;
	tcb->stack_size = mapping_size;
	tcb->owns_stack = (unsigned)owns_stack;
	tcb->start = start;
	tcb->argument = argument;
	if (RTLD_CALL(thread_alloc)(tcb, &tcb->runtime_tcb) != 0) {
		free(tcb);
		if (owns_stack)
			(void)munmap(stack, mapping_size);
		return EAGAIN;
	}
	error =
	    (int)call(ZEDBSD_SYS_thread_create, (uintptr_t)thread_trampoline,
		      (uintptr_t)usable_stack + size, 0,
		      (uintptr_t)tcb->runtime_tcb, 0, (uintptr_t)&tcb->tid);
	if (error < 0) {
		error = errno;
		RTLD_CALL(thread_free)(tcb->runtime_tcb);
		free(tcb);
		if (owns_stack)
			(void)munmap(stack, mapping_size);
		return error;
	}
	registry_add(tcb);
	*result = tcb->tid;
	if (attributes != NULL &&
	    attributes->detachstate == PTHREAD_CREATE_DETACHED) {
		error = pthread_detach(tcb->tid);
		if (error != 0)
			return error;
	}
	return 0;
}

static void
run_destructors(struct pthread_tcb *tcb)
{
	unsigned pass, key;
	if (tcb == NULL)
		return;
	for (pass = 0; pass < DESTRUCTOR_ITERATIONS; pass++) {
		int invoked = 0;
		for (key = 0; key < KEY_MAX; key++) {
			void (*destructor)(void *) = key_destructor[key];
			void *value = (void *)tcb->keys[key];
			if (value == NULL || destructor == NULL ||
			    destructor == (void (*)(void *))1)
				continue;
			tcb->keys[key] = NULL;
			destructor(value);
			invoked = 1;
		}
		if (!invoked)
			break;
	}
}

void
pthread_exit(void *value)
{
	struct pthread_tcb *tcb = self_tcb();
	while (tcb != NULL && tcb->cleanup != NULL) {
		struct __pthread_cleanup *cleanup = tcb->cleanup;
		tcb->cleanup = cleanup->previous;
		cleanup->routine(cleanup->argument);
	}
	run_destructors(tcb);
	if (tcb != NULL) {
		free(tcb->environment_value);
		tcb->environment_value = NULL;
	}
	if (tcb != NULL && tcb->detached)
		detached_enqueue(tcb);
	(void)__syscall6(ZEDBSD_SYS_thread_exit, (uintptr_t)value, 0, 0, 0, 0,
			 0);
	for (;;)
		;
}

int
pthread_join(pthread_t thread, void **value)
{
	struct pthread_tcb *tcb;
	struct pthread_tcb *self;
	intptr_t result;

	pthread_testcancel();
	tcb = registry_find(thread);
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
		if (result < 0 && errno == EINTR)
			pthread_testcancel();
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		return errno;
	tcb = registry_remove(thread);
	if (tcb != NULL && tcb != &main_tcb) {
		RTLD_CALL(thread_free)(tcb->runtime_tcb);
		if (tcb->owns_stack)
			(void)munmap(tcb->stack, tcb->stack_size);
		free(tcb);
	}
	return 0;
}

int
pthread_detach(pthread_t thread)
{
	struct pthread_tcb *tcb;
	int error;

	ensure_main();
	tcb = registry_find(thread);
	if (tcb == NULL || tcb == &main_tcb)
		return ESRCH;
	word_lock(&registry_lock);
	if (tcb->detached) {
		word_unlock(&registry_lock);
		return EINVAL;
	}
	word_unlock(&registry_lock);
	error = ensure_detached_reaper();
	if (error != 0)
		return error;
	word_lock(&registry_lock);
	if (tcb->detached) {
		word_unlock(&registry_lock);
		return EINVAL;
	}
	tcb->detached = 1;
	word_unlock(&registry_lock);
	/* The reaper may wait before the target reaches thread_exit().  Queuing
	 * here also covers the race where the target is already a zombie. */
	detached_enqueue(tcb);
	return 0;
}

int
pthread_atfork(void (*prepare)(void), void (*parent)(void), void (*child)(void))
{
	if (prepare == NULL && parent == NULL && child == NULL)
		return 0;
	word_lock(&atfork_lock);
	if (atfork_count == ATFORK_MAX) {
		word_unlock(&atfork_lock);
		return ENOMEM;
	}
	atfork_handlers[atfork_count].prepare = prepare;
	atfork_handlers[atfork_count].parent = parent;
	atfork_handlers[atfork_count].child = child;
	atfork_count++;
	word_unlock(&atfork_lock);
	return 0;
}

void
__pthread_fork_prepare(void)
{
	unsigned index;

	ensure_main();
	RTLD_CALL(fork_prepare)();
	word_lock(&atfork_lock);
	atfork_active_count = atfork_count;
	for (index = atfork_active_count; index != 0; index--)
		if (atfork_handlers[index - 1U].prepare != NULL)
			atfork_handlers[index - 1U].prepare();
}

void
__pthread_fork_parent(void)
{
	unsigned index;

	for (index = 0; index < atfork_active_count; index++)
		if (atfork_handlers[index].parent != NULL)
			atfork_handlers[index].parent();
	atfork_active_count = 0;
	word_unlock(&atfork_lock);
	RTLD_CALL(fork_parent)();
}

void
__pthread_fork_child(void)
{
	struct pthread_tcb *self = self_tcb();
	unsigned count = atfork_active_count;
	unsigned index;

	/* Only the calling thread exists in the child.  Internal locks and the
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
	if (self != NULL) {
		self->next = NULL;
		self->detached_next = NULL;
		self->detached_queued = 0;
		self->detached = 0;
		tcb_list = self;
	} else {
		tcb_list = NULL;
	}
	for (index = 0; index < count; index++)
		if (atfork_handlers[index].child != NULL)
			atfork_handlers[index].child();
	atfork_active_count = 0;
	atfork_lock = 0;
	if (__stdio_fork_child != NULL)
		__stdio_fork_child();
	RTLD_CALL(fork_child)();
}
pthread_t
pthread_self(void)
{
	ensure_main();
	return self_tcb()->tid;
}
int
pthread_equal(pthread_t left, pthread_t right)
{
	return left == right;
}

int
pthread_attr_init(pthread_attr_t *a)
{
	if (a == NULL)
		return EINVAL;
	memset(a, 0, sizeof(*a));
	a->stacksize = THREAD_STACK_SIZE;
	a->guardsize = 4096;
	a->detachstate = 0;
	return 0;
}
int
pthread_attr_destroy(pthread_attr_t *a)
{
	return a != NULL ? 0 : EINVAL;
}
int
pthread_attr_setdetachstate(pthread_attr_t *a, int state)
{
	if (a == NULL || (state != 0 && state != 1))
		return EINVAL;
	a->detachstate = state;
	return 0;
}
int
pthread_attr_setstacksize(pthread_attr_t *a, size_t size)
{
	if (a == NULL || size < PTHREAD_STACK_MIN)
		return EINVAL;
	a->stacksize = size;
	return 0;
}
int
pthread_attr_getdetachstate(const pthread_attr_t *a, int *state)
{
	if (a == NULL || state == NULL)
		return EINVAL;
	*state = a->detachstate;
	return 0;
}
int
pthread_attr_getstacksize(const pthread_attr_t *a, size_t *size)
{
	if (a == NULL || size == NULL)
		return EINVAL;
	*size = a->stacksize;
	return 0;
}
int
pthread_attr_setguardsize(pthread_attr_t *a, size_t size)
{
	if (a == NULL)
		return EINVAL;
	a->guardsize = size;
	return 0;
}
int
pthread_attr_getguardsize(const pthread_attr_t *a, size_t *size)
{
	if (a == NULL || size == NULL)
		return EINVAL;
	*size = a->guardsize;
	return 0;
}
int
pthread_attr_setstack(pthread_attr_t *a, void *stack, size_t size)
{
	if (a == NULL || stack == NULL || size < PTHREAD_STACK_MIN)
		return EINVAL;
	a->stackaddr = stack;
	a->stacksize = size;
	a->guardsize = 0;
	a->stackset = 1;
	return 0;
}
int
pthread_attr_getstack(const pthread_attr_t *a, void **stack, size_t *size)
{
	if (a == NULL || stack == NULL || size == NULL)
		return EINVAL;
	*stack = a->stackaddr;
	*size = a->stacksize;
	return 0;
}
int
pthread_attr_getschedparam(const pthread_attr_t *a, struct sched_param *p)
{
	if (a == NULL || p == NULL)
		return EINVAL;
	*p = a->schedparam;
	return 0;
}
int
pthread_attr_setschedparam(pthread_attr_t *a, const struct sched_param *p)
{
	if (a == NULL || p == NULL)
		return EINVAL;
	if (p->sched_priority != 0)
		return ENOTSUP;
	a->schedparam = *p;
	return 0;
}

int
pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
	if (m == NULL)
		return EINVAL;
	memset(m, 0, sizeof(*m));
	if (a != NULL) {
		m->type = a->type;
		m->pshared = a->pshared;
		m->robust = a->robust;
	}
	return 0;
}
int
pthread_mutex_destroy(pthread_mutex_t *m)
{
	return m == NULL
		   ? EINVAL
		   : (__atomic_load_n(&m->locked, __ATOMIC_ACQUIRE) ? EBUSY
								    : 0);
}
int
pthread_mutexattr_init(pthread_mutexattr_t *a)
{
	if (a == NULL)
		return EINVAL;
	a->type = PTHREAD_MUTEX_NORMAL;
	a->pshared = PTHREAD_PROCESS_PRIVATE;
	a->robust = PTHREAD_MUTEX_STALLED;
	return 0;
}
int
pthread_mutexattr_destroy(pthread_mutexattr_t *a)
{
	return a != NULL ? 0 : EINVAL;
}
int
pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type)
{
	if (a == NULL || type == NULL)
		return EINVAL;
	*type = (int)a->type;
	return 0;
}
int
pthread_mutexattr_settype(pthread_mutexattr_t *a, int type)
{
	if (a == NULL || type < PTHREAD_MUTEX_NORMAL ||
	    type > PTHREAD_MUTEX_ERRORCHECK)
		return EINVAL;
	a->type = (unsigned)type;
	return 0;
}
int
pthread_mutexattr_setpshared(pthread_mutexattr_t *a, int shared)
{
	if (a == NULL || (shared != PTHREAD_PROCESS_PRIVATE &&
			  shared != PTHREAD_PROCESS_SHARED))
		return EINVAL;
	a->pshared = (unsigned)shared;
	return 0;
}
int
pthread_mutexattr_getrobust(const pthread_mutexattr_t *a, int *robust)
{
	if (a == NULL || robust == NULL)
		return EINVAL;
	*robust = (int)a->robust;
	return 0;
}
int
pthread_mutexattr_setrobust(pthread_mutexattr_t *a, int robust)
{
	if (a == NULL ||
	    (robust != PTHREAD_MUTEX_STALLED && robust != PTHREAD_MUTEX_ROBUST))
		return EINVAL;
	if (robust == PTHREAD_MUTEX_ROBUST)
		return ENOTSUP;
	a->robust = (unsigned)robust;
	return 0;
}
int
pthread_mutex_consistent(pthread_mutex_t *m)
{
	return m == NULL || m->robust != PTHREAD_MUTEX_ROBUST ? EINVAL
							      : ENOTSUP;
}
int
pthread_mutex_trylock(pthread_mutex_t *m)
{
	pthread_t self;
	if (m == NULL)
		return EINVAL;
	self = pthread_self();
	if (m->owner == self) {
		if (m->type == PTHREAD_MUTEX_RECURSIVE) {
			m->count++;
			return 0;
		}
		if (m->type == PTHREAD_MUTEX_ERRORCHECK)
			return EDEADLK;
	}
	if (__atomic_exchange_n(&m->locked, 1, __ATOMIC_ACQUIRE) != 0)
		return EBUSY;
	m->owner = self;
	m->count = 1;
	return 0;
}
int
pthread_mutex_lock(pthread_mutex_t *m)
{
	int error;
	while ((error = pthread_mutex_trylock(m)) == EBUSY)
		(void)usync_wait_word_flags(&m->locked, 1, NULL, m->pshared);
	return error;
}
int
pthread_mutex_clocklock(pthread_mutex_t *m, clockid_t clock,
			const struct timespec *absolute)
{
	int error;
	if (m == NULL)
		return EINVAL;
	while ((error = pthread_mutex_trylock(m)) == EBUSY) {
		error = usync_wait_word_absolute(&m->locked, 1, absolute,
						 m->pshared, 0, clock);
		if (error != 0 && error != EAGAIN && error != EINTR)
			return error;
	}
	return error;
}

int
pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *absolute)
{
	return pthread_mutex_clocklock(m, CLOCK_REALTIME, absolute);
}
int
pthread_mutex_unlock(pthread_mutex_t *m)
{
	if (m == NULL || !m->locked || m->owner != pthread_self())
		return EPERM;
	if (m->type == PTHREAD_MUTEX_RECURSIVE && --m->count != 0)
		return 0;
	m->owner = 0;
	m->count = 0;
	__atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
	usync_wake_word_flags(&m->locked, 1, m->pshared);
	return 0;
}

int
pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
	if (c == NULL)
		return EINVAL;
	c->sequence = 0;
	c->pshared = a != NULL ? a->pshared : 0;
	c->clock = a != NULL ? a->clock : CLOCK_REALTIME;
	return 0;
}
int
pthread_cond_destroy(pthread_cond_t *c)
{
	return c != NULL ? 0 : EINVAL;
}
int
pthread_condattr_init(pthread_condattr_t *a)
{
	if (a == NULL)
		return EINVAL;
	a->clock = CLOCK_REALTIME;
	a->pshared = PTHREAD_PROCESS_PRIVATE;
	return 0;
}
int
pthread_condattr_destroy(pthread_condattr_t *a)
{
	return a != NULL ? 0 : EINVAL;
}
int
pthread_condattr_setpshared(pthread_condattr_t *a, int shared)
{
	if (a == NULL || (shared != PTHREAD_PROCESS_PRIVATE &&
			  shared != PTHREAD_PROCESS_SHARED))
		return EINVAL;
	a->pshared = (unsigned)shared;
	return 0;
}
int
pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clock)
{
	if (a == NULL || (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC))
		return EINVAL;
	a->clock = (unsigned)clock;
	return 0;
}
int
pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clock)
{
	if (a == NULL || clock == NULL)
		return EINVAL;
	*clock = (clockid_t)a->clock;
	return 0;
}
static int
cond_wait(pthread_cond_t *c, pthread_mutex_t *m,
	  const struct timespec *absolute, clockid_t clock)
{
	uint32_t sequence;
	int error;
	if (c == NULL || m == NULL)
		return EINVAL;
	pthread_testcancel();
	sequence = __atomic_load_n(&c->sequence, __ATOMIC_ACQUIRE);
	error = pthread_mutex_unlock(m);
	if (error != 0)
		return error;
	for (;;) {
		if (__atomic_load_n(&c->sequence, __ATOMIC_ACQUIRE) !=
		    sequence) {
			error = 0;
			break;
		}
		if (absolute != NULL) {
			error = usync_wait_word_absolute(
			    &c->sequence, sequence, absolute, c->pshared,
			    __pthread_cancel_enabled(), clock);
		} else {
			error = usync_wait_word_flags_cancelable(
			    &c->sequence, sequence, NULL, c->pshared,
			    __pthread_cancel_enabled(), 0);
		}
		if (error == 0 || error == EAGAIN) {
			error = 0;
			break;
		}
		if (error != EINTR)
			break;
		if (cancel_pending()) {
			/* Do not clear/act on the request until the
			 * condition-wait contract has reacquired the caller's
			 * mutex. */
			error = 0;
			break;
		}
		/* POSIX condition waits do not expose EINTR.  A timed wait
		 * always recomputes from its absolute deadline so caught
		 * signals cannot extend the timeout. */
	}
	{
		int lock_error = pthread_mutex_lock(m);
		if (lock_error == 0)
			pthread_testcancel();
		return error != 0 ? error : lock_error;
	}
}
int
pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
	return cond_wait(c, m, NULL, CLOCK_REALTIME);
}
int
pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
		       const struct timespec *absolute)
{
	if (c == NULL || m == NULL)
		return EINVAL;
	return cond_wait(c, m, absolute, (clockid_t)c->clock);
}

int
pthread_cond_clockwait(pthread_cond_t *c, pthread_mutex_t *m, clockid_t clock,
		       const struct timespec *absolute)
{
	if (c == NULL || m == NULL)
		return EINVAL;
	return cond_wait(c, m, absolute, clock);
}
int
pthread_cond_signal(pthread_cond_t *c)
{
	if (c == NULL)
		return EINVAL;
	__atomic_add_fetch(&c->sequence, 1, __ATOMIC_RELEASE);
	usync_wake_word_flags(&c->sequence, 1, c->pshared);
	return 0;
}
int
pthread_cond_broadcast(pthread_cond_t *c)
{
	if (c == NULL)
		return EINVAL;
	__atomic_add_fetch(&c->sequence, 1, __ATOMIC_RELEASE);
	usync_wake_word_flags(&c->sequence, UINT32_MAX, c->pshared);
	return 0;
}

static void
rwlock_guard_lock(pthread_rwlock_t *lock)
{
	while (__atomic_exchange_n(&lock->guard, 1, __ATOMIC_ACQUIRE) != 0)
		(void)usync_wait_word_flags(&lock->guard, 1, NULL,
					    lock->pshared);
}

static void
rwlock_guard_unlock(pthread_rwlock_t *lock)
{
	__atomic_store_n(&lock->guard, 0, __ATOMIC_RELEASE);
	usync_wake_word_flags(&lock->guard, 1, lock->pshared);
}

int
pthread_rwlock_init(pthread_rwlock_t *lock, const pthread_rwlockattr_t *attr)
{
	if (lock == NULL)
		return EINVAL;
	memset(lock, 0, sizeof(*lock));
	lock->pshared = attr != NULL ? attr->pshared : PTHREAD_PROCESS_PRIVATE;
	return 0;
}

int
pthread_rwlock_destroy(pthread_rwlock_t *lock)
{
	if (lock == NULL)
		return EINVAL;
	return lock->guard != 0 || lock->readers != 0 || lock->writer != 0
		   ? EBUSY
		   : 0;
}

int
pthread_rwlock_tryrdlock(pthread_rwlock_t *lock)
{
	int error = 0;
	if (lock == NULL)
		return EINVAL;
	rwlock_guard_lock(lock);
	if (lock->writer != 0)
		error = EBUSY;
	else if (lock->readers == UINT32_MAX)
		error = EAGAIN;
	else
		lock->readers++;
	rwlock_guard_unlock(lock);
	return error;
}

int
pthread_rwlock_rdlock(pthread_rwlock_t *lock)
{
	uint32_t sequence;
	int error;
	if (lock == NULL)
		return EINVAL;
	for (;;) {
		error = pthread_rwlock_tryrdlock(lock);
		if (error != EBUSY)
			return error;
		sequence = __atomic_load_n(&lock->sequence, __ATOMIC_ACQUIRE);
		error = usync_wait_word_flags(&lock->sequence, sequence, NULL,
					      lock->pshared);
		if (error != 0 && error != EAGAIN && error != EINTR)
			return error;
	}
}

int
pthread_rwlock_clockrdlock(pthread_rwlock_t *lock, clockid_t clock,
			   const struct timespec *absolute)
{
	uint32_t sequence;
	int error;
	if (lock == NULL)
		return EINVAL;
	for (;;) {
		error = pthread_rwlock_tryrdlock(lock);
		if (error != EBUSY)
			return error;
		sequence = __atomic_load_n(&lock->sequence, __ATOMIC_ACQUIRE);
		error =
		    usync_wait_word_absolute(&lock->sequence, sequence,
					     absolute, lock->pshared, 0, clock);
		if (error != 0 && error != EAGAIN && error != EINTR)
			return error;
	}
}

int
pthread_rwlock_timedrdlock(pthread_rwlock_t *lock,
			   const struct timespec *absolute)
{
	return pthread_rwlock_clockrdlock(lock, CLOCK_REALTIME, absolute);
}

int
pthread_rwlock_trywrlock(pthread_rwlock_t *lock)
{
	int error = 0;
	if (lock == NULL)
		return EINVAL;
	rwlock_guard_lock(lock);
	if (lock->writer != 0 || lock->readers != 0)
		error = EBUSY;
	else
		lock->writer = 1;
	rwlock_guard_unlock(lock);
	return error;
}

int
pthread_rwlock_wrlock(pthread_rwlock_t *lock)
{
	uint32_t sequence;
	int error;
	if (lock == NULL)
		return EINVAL;
	for (;;) {
		error = pthread_rwlock_trywrlock(lock);
		if (error != EBUSY)
			return error;
		sequence = __atomic_load_n(&lock->sequence, __ATOMIC_ACQUIRE);
		error = usync_wait_word_flags(&lock->sequence, sequence, NULL,
					      lock->pshared);
		if (error != 0 && error != EAGAIN && error != EINTR)
			return error;
	}
}

int
pthread_rwlock_clockwrlock(pthread_rwlock_t *lock, clockid_t clock,
			   const struct timespec *absolute)
{
	uint32_t sequence;
	int error;
	if (lock == NULL)
		return EINVAL;
	for (;;) {
		error = pthread_rwlock_trywrlock(lock);
		if (error != EBUSY)
			return error;
		sequence = __atomic_load_n(&lock->sequence, __ATOMIC_ACQUIRE);
		error =
		    usync_wait_word_absolute(&lock->sequence, sequence,
					     absolute, lock->pshared, 0, clock);
		if (error != 0 && error != EAGAIN && error != EINTR)
			return error;
	}
}

int
pthread_rwlock_timedwrlock(pthread_rwlock_t *lock,
			   const struct timespec *absolute)
{
	return pthread_rwlock_clockwrlock(lock, CLOCK_REALTIME, absolute);
}

int
pthread_rwlock_unlock(pthread_rwlock_t *lock)
{
	int error = 0;
	if (lock == NULL)
		return EINVAL;
	rwlock_guard_lock(lock);
	if (lock->writer != 0)
		lock->writer = 0;
	else if (lock->readers != 0)
		lock->readers--;
	else
		error = EPERM;
	if (error == 0)
		(void)__atomic_add_fetch(&lock->sequence, 1, __ATOMIC_RELEASE);
	rwlock_guard_unlock(lock);
	if (error == 0)
		usync_wake_word_flags(&lock->sequence, UINT32_MAX,
				      lock->pshared);
	return error;
}

int
pthread_rwlockattr_init(pthread_rwlockattr_t *attr)
{
	if (attr == NULL)
		return EINVAL;
	attr->pshared = PTHREAD_PROCESS_PRIVATE;
	return 0;
}
int
pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr)
{
	return attr != NULL ? 0 : EINVAL;
}
int
pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attr, int shared)
{
	if (attr == NULL || (shared != PTHREAD_PROCESS_PRIVATE &&
			     shared != PTHREAD_PROCESS_SHARED))
		return EINVAL;
	attr->pshared = (unsigned)shared;
	return 0;
}

static void
barrier_guard_lock(pthread_barrier_t *barrier)
{
	while (__atomic_exchange_n(&barrier->guard, 1, __ATOMIC_ACQUIRE) != 0)
		(void)usync_wait_word_flags(&barrier->guard, 1, NULL,
					    barrier->pshared);
}

static void
barrier_guard_unlock(pthread_barrier_t *barrier)
{
	__atomic_store_n(&barrier->guard, 0, __ATOMIC_RELEASE);
	usync_wake_word_flags(&barrier->guard, 1, barrier->pshared);
}

int
pthread_barrier_init(pthread_barrier_t *barrier,
		     const pthread_barrierattr_t *attr, unsigned count)
{
	if (barrier == NULL || count == 0)
		return EINVAL;
	memset(barrier, 0, sizeof(*barrier));
	barrier->trip = count;
	barrier->pshared =
	    attr != NULL ? attr->pshared : PTHREAD_PROCESS_PRIVATE;
	return 0;
}

int
pthread_barrier_destroy(pthread_barrier_t *barrier)
{
	if (barrier == NULL)
		return EINVAL;
	return barrier->guard != 0 || barrier->count != 0 ? EBUSY : 0;
}

int
pthread_barrier_wait(pthread_barrier_t *barrier)
{
	uint32_t generation;
	int error;
	if (barrier == NULL || barrier->trip == 0)
		return EINVAL;
	barrier_guard_lock(barrier);
	generation = barrier->sequence;
	barrier->count++;
	if (barrier->count == barrier->trip) {
		barrier->count = 0;
		(void)__atomic_add_fetch(&barrier->sequence, 1,
					 __ATOMIC_RELEASE);
		barrier_guard_unlock(barrier);
		usync_wake_word_flags(&barrier->sequence, UINT32_MAX,
				      barrier->pshared);
		return PTHREAD_BARRIER_SERIAL_THREAD;
	}
	barrier_guard_unlock(barrier);
	for (;;) {
		if (__atomic_load_n(&barrier->sequence, __ATOMIC_ACQUIRE) !=
		    generation)
			return 0;
		error = usync_wait_word_flags(&barrier->sequence, generation,
					      NULL, barrier->pshared);
		/* usync buckets deliberately wake colliding addresses.  A zero
		 * return is therefore only a hint; the generation is the
		 * barrier predicate. */
		if (error == 0 || error == EAGAIN || error == EINTR)
			continue;
		barrier_guard_lock(barrier);
		if (barrier->sequence == generation && barrier->count != 0)
			barrier->count--;
		barrier_guard_unlock(barrier);
		return error;
	}
}

int
pthread_barrierattr_init(pthread_barrierattr_t *attr)
{
	if (attr == NULL)
		return EINVAL;
	attr->pshared = PTHREAD_PROCESS_PRIVATE;
	return 0;
}
int
pthread_barrierattr_destroy(pthread_barrierattr_t *attr)
{
	return attr != NULL ? 0 : EINVAL;
}
int
pthread_barrierattr_setpshared(pthread_barrierattr_t *attr, int shared)
{
	if (attr == NULL || (shared != PTHREAD_PROCESS_PRIVATE &&
			     shared != PTHREAD_PROCESS_SHARED))
		return EINVAL;
	attr->pshared = (unsigned)shared;
	return 0;
}

int
pthread_spin_init(pthread_spinlock_t *lock, int shared)
{
	if (lock == NULL || (shared != PTHREAD_PROCESS_PRIVATE &&
			     shared != PTHREAD_PROCESS_SHARED))
		return EINVAL;
	*lock = 0;
	return 0;
}
int
pthread_spin_destroy(pthread_spinlock_t *lock)
{
	return lock == NULL ? EINVAL : (*lock != 0 ? EBUSY : 0);
}
int
pthread_spin_trylock(pthread_spinlock_t *lock)
{
	if (lock == NULL)
		return EINVAL;
	return __atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE) == 0 ? 0 : EBUSY;
}
int
pthread_spin_lock(pthread_spinlock_t *lock)
{
	int error;
	if (lock == NULL)
		return EINVAL;
	while ((error = pthread_spin_trylock(lock)) == EBUSY)
		(void)usync_wait_word(lock, 1, NULL);
	return error;
}
int
pthread_spin_unlock(pthread_spinlock_t *lock)
{
	if (lock == NULL || *lock == 0)
		return EPERM;
	__atomic_store_n(lock, 0, __ATOMIC_RELEASE);
	usync_wake_word(lock, 1);
	return 0;
}

int
pthread_once(pthread_once_t *once, void (*function)(void))
{
	uint32_t previous;
	if (once == NULL || function == NULL)
		return EINVAL;
	previous = __atomic_exchange_n(&once->state, 1, __ATOMIC_ACQ_REL);
	if (previous == 0) {
		function();
		__atomic_store_n(&once->state, 2, __ATOMIC_RELEASE);
		usync_wake_word(&once->state, UINT32_MAX);
	} else {
		if (previous == 2) {
			__atomic_store_n(&once->state, 2, __ATOMIC_RELEASE);
			usync_wake_word(&once->state, UINT32_MAX);
		}
		while (__atomic_load_n(&once->state, __ATOMIC_ACQUIRE) != 2)
			(void)usync_wait_word(&once->state, 1, NULL);
	}
	return 0;
}

int
pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
	unsigned i;
	if (key == NULL)
		return EINVAL;
	word_lock(&key_lock);
	for (i = 0; i < KEY_MAX; i++) {
		if (key_destructor[i] == NULL) {
			key_destructor[i] = destructor != NULL
						? destructor
						: (void (*)(void *))1;
			*key = i;
			word_unlock(&key_lock);
			return 0;
		}
	}
	word_unlock(&key_lock);
	return EAGAIN;
}
int
pthread_key_delete(pthread_key_t key)
{
	if (key >= KEY_MAX)
		return EINVAL;
	word_lock(&key_lock);
	key_destructor[key] = NULL;
	word_unlock(&key_lock);
	return 0;
}
int
pthread_setspecific(pthread_key_t key, const void *value)
{
	struct pthread_tcb *tcb;
	ensure_main();
	tcb = self_tcb();
	if (key >= KEY_MAX || key_destructor[key] == NULL || tcb == NULL)
		return EINVAL;
	tcb->keys[key] = value;
	return 0;
}
void *
pthread_getspecific(pthread_key_t key)
{
	struct pthread_tcb *tcb;
	ensure_main();
	tcb = self_tcb();
	return key < KEY_MAX && tcb != NULL ? (void *)tcb->keys[key] : NULL;
}
int
pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
	return sigprocmask(how, set, old) == 0 ? 0 : errno;
}
int
pthread_kill(pthread_t thread, int signo)
{
	intptr_t result;
	if (signo < 0 || signo > SIGRTMAX)
		return EINVAL;
	result = call(ZEDBSD_SYS_thread_kill, thread, signo, 0, 0, 0, 0);
	return result < 0 ? errno : 0;
}

int
pthread_cancel(pthread_t thread)
{
	intptr_t result = call(ZEDBSD_SYS_thread_cancel, thread,
			       ZEDBSD_THREAD_CANCEL_REQUEST, 0, 0, 0, 0);
	if (result < 0)
		return errno;
	if (thread == pthread_self())
		pthread_testcancel();
	return 0;
}

int
pthread_setcancelstate(int state, int *old_state)
{
	struct pthread_tcb *tcb;
	if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE)
		return EINVAL;
	ensure_main();
	tcb = self_tcb();
	if (old_state != NULL)
		*old_state = tcb->cancel_state;
	tcb->cancel_state = state;
	if (state == PTHREAD_CANCEL_ENABLE &&
	    tcb->cancel_type == PTHREAD_CANCEL_ASYNCHRONOUS)
		pthread_testcancel();
	return 0;
}

int
pthread_setcanceltype(int type, int *old_type)
{
	struct pthread_tcb *tcb;
	if (type != PTHREAD_CANCEL_DEFERRED &&
	    type != PTHREAD_CANCEL_ASYNCHRONOUS)
		return EINVAL;
	ensure_main();
	tcb = self_tcb();
	if (old_type != NULL)
		*old_type = tcb->cancel_type;
	tcb->cancel_type = type;
	if (type == PTHREAD_CANCEL_ASYNCHRONOUS &&
	    tcb->cancel_state == PTHREAD_CANCEL_ENABLE)
		pthread_testcancel();
	return 0;
}

void
pthread_testcancel(void)
{
	struct pthread_tcb *tcb;
	intptr_t pending;
	ensure_main();
	tcb = self_tcb();
	if (tcb == NULL || tcb->cancel_state == PTHREAD_CANCEL_DISABLE)
		return;
	pending = call(ZEDBSD_SYS_thread_cancel, 0, ZEDBSD_THREAD_CANCEL_CLEAR,
		       0, 0, 0, 0);
	if (pending > 0)
		pthread_exit(PTHREAD_CANCELED);
}

void
__pthread_cancel_point(void)
{
	pthread_testcancel();
}

void
__pthread_cleanup_push(struct __pthread_cleanup *cleanup,
		       void (*routine)(void *), void *argument)
{
	struct pthread_tcb *tcb;
	if (cleanup == NULL || routine == NULL)
		return;
	ensure_main();
	tcb = self_tcb();
	cleanup->routine = routine;
	cleanup->argument = argument;
	cleanup->previous = tcb->cleanup;
	tcb->cleanup = cleanup;
}

void
__pthread_cleanup_pop(struct __pthread_cleanup *cleanup, int execute)
{
	struct pthread_tcb *tcb;
	if (cleanup == NULL)
		return;
	ensure_main();
	tcb = self_tcb();
	if (tcb->cleanup == cleanup)
		tcb->cleanup = cleanup->previous;
	if (execute)
		cleanup->routine(cleanup->argument);
}

static int
c11_result(int error)
{
	if (error == 0)
		return thrd_success;
	if (error == ENOMEM || error == EAGAIN)
		return thrd_nomem;
	if (error == ETIMEDOUT)
		return thrd_timedout;
	if (error == EBUSY)
		return thrd_busy;
	return thrd_error;
}

void
call_once(once_flag *flag, void (*function)(void))
{
	(void)pthread_once(flag, function);
}

int
cnd_broadcast(cnd_t *condition)
{
	return c11_result(pthread_cond_broadcast(condition));
}

void
cnd_destroy(cnd_t *condition)
{
	(void)pthread_cond_destroy(condition);
}

int
cnd_init(cnd_t *condition)
{
	return c11_result(pthread_cond_init(condition, NULL));
}

int
cnd_signal(cnd_t *condition)
{
	return c11_result(pthread_cond_signal(condition));
}

int
cnd_timedwait(cnd_t *condition, mtx_t *mutex, const struct timespec *absolute)
{
	return c11_result(pthread_cond_timedwait(condition, mutex, absolute));
}

int
cnd_wait(cnd_t *condition, mtx_t *mutex)
{
	return c11_result(pthread_cond_wait(condition, mutex));
}

void
mtx_destroy(mtx_t *mutex)
{
	(void)pthread_mutex_destroy(mutex);
}

int
mtx_init(mtx_t *mutex, int type)
{
	pthread_mutexattr_t attributes;
	int error;

	if ((type & ~(mtx_recursive | mtx_timed)) != 0)
		return thrd_error;
	error = pthread_mutexattr_init(&attributes);
	if (error == 0 && (type & mtx_recursive) != 0)
		error = pthread_mutexattr_settype(&attributes,
						  PTHREAD_MUTEX_RECURSIVE);
	if (error == 0)
		error = pthread_mutex_init(mutex, &attributes);
	(void)pthread_mutexattr_destroy(&attributes);
	return c11_result(error);
}

int
mtx_lock(mtx_t *mutex)
{
	return c11_result(pthread_mutex_lock(mutex));
}

int
mtx_timedlock(mtx_t *mutex, const struct timespec *absolute)
{
	return c11_result(pthread_mutex_timedlock(mutex, absolute));
}

int
mtx_trylock(mtx_t *mutex)
{
	return c11_result(pthread_mutex_trylock(mutex));
}

int
mtx_unlock(mtx_t *mutex)
{
	return c11_result(pthread_mutex_unlock(mutex));
}

struct c11_start_context {
	thrd_start_t function;
	void *argument;
};

static void *
c11_start(void *argument)
{
	struct c11_start_context *context = argument;
	thrd_start_t function = context->function;
	void *function_argument = context->argument;
	int result;

	free(context);
	result = function(function_argument);
	return (void *)(intptr_t)result;
}

int
thrd_create(thrd_t *thread, thrd_start_t function, void *argument)
{
	struct c11_start_context *context;
	int error;

	if (thread == NULL || function == NULL)
		return thrd_error;
	context = malloc(sizeof(*context));
	if (context == NULL)
		return thrd_nomem;
	context->function = function;
	context->argument = argument;
	error = pthread_create(thread, NULL, c11_start, context);
	if (error != 0)
		free(context);
	return c11_result(error);
}

thrd_t
thrd_current(void)
{
	return pthread_self();
}

int
thrd_detach(thrd_t thread)
{
	return c11_result(pthread_detach(thread));
}

int
thrd_equal(thrd_t left, thrd_t right)
{
	return pthread_equal(left, right);
}

_Noreturn void
thrd_exit(int result)
{
	pthread_exit((void *)(intptr_t)result);
}

int
thrd_join(thrd_t thread, int *result)
{
	void *value;
	int error;

	error = pthread_join(thread, &value);
	if (error == 0 && result != NULL)
		*result = (int)(intptr_t)value;
	return c11_result(error);
}

int
thrd_sleep(const struct timespec *duration, struct timespec *remaining)
{
	if (nanosleep(duration, remaining) == 0)
		return 0;
	return errno == EINTR ? -1 : -2;
}

void
thrd_yield(void)
{
	(void)sched_yield();
}

int
tss_create(tss_t *key, tss_dtor_t destructor)
{
	return c11_result(pthread_key_create(key, destructor));
}

void
tss_delete(tss_t key)
{
	(void)pthread_key_delete(key);
}

void *
tss_get(tss_t key)
{
	return pthread_getspecific(key);
}

int
tss_set(tss_t key, void *value)
{
	return c11_result(pthread_setspecific(key, value));
}
