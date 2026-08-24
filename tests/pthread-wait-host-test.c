/* pthread wait predicate regression tests. SPDX-License-Identifier: Zlib */
#include "userland/base/libc/syscall.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <zedbsd/rtld-abi.h>
#include <zedbsd/syscall.h>
#include <zedbsd/thread.h>
#include <zedbsd/usync.h>

static pthread_barrier_t barrier;
static pthread_cond_t condition;
static pthread_cond_t timed_condition;
static pthread_cond_t cancel_condition;
static pthread_mutex_t mutex;
static sem_t cancel_sem;
static sem_t retry_sem;
static sem_t timed_sem;
static sem_t expired_sem;
static void *thread_private;
static unsigned wait_calls;
static unsigned cond_wait_calls;
static unsigned timed_wait_calls, clock_calls;
static unsigned cancel_wait_calls;
static unsigned sem_cancel_wait_calls;
static unsigned sem_retry_wait_calls;
static unsigned sem_timed_wait_calls;
static unsigned sem_expired_wait_calls;
static int cancel_pending;
static int cancel_jump_active;
static jmp_buf cancel_jump;

int
clock_gettime(clockid_t clock, struct timespec *value)
{
	assert(clock == CLOCK_REALTIME && value != NULL);
	value->tv_sec = (time_t)clock_calls++;
	value->tv_nsec = 0;
	return 0;
}

void
__libc_assert_fail(const char *expression, const char *file, int line)
{
	(void)expression;
	(void)file;
	(void)line;
	abort();
}

int __rtld_thread_alloc(void *private, struct __rtld_tcb **result)
{ (void)private; (void)result; return ENOSYS; }
void __rtld_thread_free(struct __rtld_tcb *tcb) { (void)tcb; }
int __rtld_thread_attach(void *private) { thread_private = private; return 0; }

void *
__rtld_pthread_private(void)
{
	return thread_private;
}

intptr_t
syscall_result(intptr_t result)
{
	if (result < 0) {
		errno = (int)-result;
		return -1;
	}
	return result;
}

intptr_t
__syscall6(uint32_t number, uintptr_t address, uintptr_t operation,
	uintptr_t expected, uintptr_t timeout, uintptr_t count, uintptr_t flags)
{
	(void)count;
	if (number == ZEDBSD_SYS_thread_self)
		return 7;
	if (number == ZEDBSD_SYS_thread_cancel) {
		int result = cancel_pending;

		assert(operation == ZEDBSD_THREAD_CANCEL_TEST ||
		    operation == ZEDBSD_THREAD_CANCEL_CLEAR);
		if (operation == ZEDBSD_THREAD_CANCEL_CLEAR)
			cancel_pending = 0;
		return result;
	}
	if (number == ZEDBSD_SYS_thread_exit) {
		assert(address == (uintptr_t)PTHREAD_CANCELED && cancel_jump_active);
		longjmp(cancel_jump, 1);
	}
	assert(number == ZEDBSD_SYS_usync);
	if (operation == ZEDBSD_USYNC_WAKE)
		return 0;
	assert(operation == ZEDBSD_USYNC_WAIT);
	if (address == (uintptr_t)&barrier.sequence) {
		assert(flags == ZEDBSD_USYNC_PRIVATE);
		wait_calls++;
		if (wait_calls == 1U)
			return -EINTR;
		if (wait_calls == 2U)
			return 0; /* Wake of a colliding usync key. */
		assert(wait_calls == 3U);
		barrier.count = 0;
		__atomic_add_fetch(&barrier.sequence, 1U, __ATOMIC_RELEASE);
		return 0;
	}
	if (address == (uintptr_t)&condition.sequence) {
		assert(flags == (ZEDBSD_USYNC_PRIVATE |
		    ZEDBSD_USYNC_CANCELABLE));
		cond_wait_calls++;
		if (cond_wait_calls == 1U)
			return -EINTR;
		assert(cond_wait_calls == 2U);
		__atomic_add_fetch(&condition.sequence, 1U, __ATOMIC_RELEASE);
		return 0;
	}
	if (address == (uintptr_t)&cancel_condition.sequence) {
		assert(flags == (ZEDBSD_USYNC_PRIVATE |
		    ZEDBSD_USYNC_CANCELABLE));
		cancel_wait_calls++;
		cancel_pending = 1;
		return -EINTR;
	}
	if (address == (uintptr_t)&cancel_sem.value) {
		assert(expected == 0 && timeout == 0 && flags ==
		    (ZEDBSD_USYNC_PRIVATE | ZEDBSD_USYNC_CANCELABLE));
		sem_cancel_wait_calls++;
		cancel_pending = 1;
		return -EINTR;
	}
	if (address == (uintptr_t)&retry_sem.value) {
		assert(expected == 0 && timeout == 0 && flags ==
		    (ZEDBSD_USYNC_PRIVATE | ZEDBSD_USYNC_CANCELABLE));
		sem_retry_wait_calls++;
		retry_sem.value = 1;
		return -EAGAIN;
	}
	if (address == (uintptr_t)&timed_sem.value) {
		assert(expected == 0 && timeout != 0 && flags ==
		    (ZEDBSD_USYNC_PRIVATE | ZEDBSD_USYNC_CANCELABLE |
		    ZEDBSD_USYNC_ABSTIME | ZEDBSD_USYNC_CLOCK_REALTIME));
		sem_timed_wait_calls++;
		assert(((const struct timespec *)timeout)->tv_sec == 6);
		if (sem_timed_wait_calls == 1U)
			return 0; /* Spurious/colliding bucket wake. */
		assert(sem_timed_wait_calls == 2U);
		timed_sem.value = 1;
		return -EAGAIN;
	}
	if (address == (uintptr_t)&expired_sem.value) {
		assert(expected == 0 && timeout != 0 && flags ==
		    (ZEDBSD_USYNC_PRIVATE | ZEDBSD_USYNC_CANCELABLE |
		    ZEDBSD_USYNC_ABSTIME | ZEDBSD_USYNC_CLOCK_REALTIME));
		sem_expired_wait_calls++;
		assert(sem_expired_wait_calls == 1U &&
		    ((const struct timespec *)timeout)->tv_sec == 5);
		return -ETIMEDOUT;
	}
	assert(address == (uintptr_t)&timed_condition.sequence);
	assert(flags == (ZEDBSD_USYNC_PRIVATE | ZEDBSD_USYNC_CANCELABLE |
	    ZEDBSD_USYNC_ABSTIME | ZEDBSD_USYNC_CLOCK_REALTIME));
	timed_wait_calls++;
	assert(timeout != 0);
	assert(((const struct timespec *)timeout)->tv_sec == 2);
	if (timed_wait_calls == 1U)
		return -EINTR;
	assert(timed_wait_calls == 2U);
	__atomic_add_fetch(&timed_condition.sequence, 1U, __ATOMIC_RELEASE);
	return 0;
}

int
main(void)
{
	assert(pthread_barrier_init(&barrier, NULL, 2) == 0);
	assert(pthread_barrier_wait(&barrier) == 0);
	assert(wait_calls == 3U);
	assert(pthread_barrier_destroy(&barrier) == 0);

	assert(pthread_mutex_init(&mutex, NULL) == 0);
	assert(pthread_cond_init(&condition, NULL) == 0);
	assert(pthread_mutex_lock(&mutex) == 0);
	assert(pthread_cond_wait(&condition, &mutex) == 0);
	assert(cond_wait_calls == 2U);
	assert(pthread_mutex_unlock(&mutex) == 0);
	assert(pthread_cond_destroy(&condition) == 0);

	{
		const struct timespec deadline = { 2, 0 };

		assert(pthread_cond_init(&timed_condition, NULL) == 0);
		assert(pthread_mutex_lock(&mutex) == 0);
		assert(pthread_cond_timedwait(&timed_condition, &mutex,
		    &deadline) == 0);
		assert(timed_wait_calls == 2U && clock_calls == 0U);
		assert(pthread_mutex_unlock(&mutex) == 0);
		assert(pthread_cond_destroy(&timed_condition) == 0);
	}

	/* Inject cancellation from inside the kernel wait.  The implementation
	 * must leave the request pending until it has reacquired mutex, then act
	 * on it rather than leaking EINTR or returning normally. */
#if !defined(__SANITIZE_ADDRESS__)
	assert(pthread_cond_init(&cancel_condition, NULL) == 0);
	assert(pthread_mutex_lock(&mutex) == 0);
	cancel_jump_active = 1;
	if (setjmp(cancel_jump) == 0) {
		(void)pthread_cond_wait(&cancel_condition, &mutex);
		assert(!"cancelled condition wait returned");
	}
	cancel_jump_active = 0;
	assert(cancel_wait_calls == 1U && cancel_pending == 0);
	assert(pthread_mutex_unlock(&mutex) == 0);
	assert(pthread_cond_destroy(&cancel_condition) == 0);
#endif
	assert(pthread_mutex_destroy(&mutex) == 0);

	/* A semaphore cancellation rolls waiter accounting back before exit. */
#if !defined(__SANITIZE_ADDRESS__)
	assert(sem_init(&cancel_sem, 0, 0) == 0);
	cancel_jump_active = 1;
	if (setjmp(cancel_jump) == 0) {
		(void)sem_wait(&cancel_sem);
		assert(!"cancelled semaphore wait returned");
	}
	cancel_jump_active = 0;
	assert(sem_cancel_wait_calls == 1U && cancel_sem.waiters == 0 &&
	    cancel_pending == 0);
	assert(sem_destroy(&cancel_sem) == 0);
#endif

	/* A compare mismatch is a predicate retry, not a user-visible EAGAIN. */
	assert(sem_init(&retry_sem, 0, 0) == 0);
	assert(sem_wait(&retry_sem) == 0);
	assert(sem_retry_wait_calls == 1U && retry_sem.waiters == 0 &&
	    retry_sem.value == 0);
	assert(sem_destroy(&retry_sem) == 0);

	/* The original absolute deadline is retained across spurious wakes and
	 * compare collisions; the kernel evaluates it against the selected clock. */
	{
		const struct timespec deadline = { 6, 0 };

		assert(sem_init(&timed_sem, 0, 0) == 0);
		assert(sem_timedwait(&timed_sem, &deadline) == 0);
		assert(sem_timed_wait_calls == 2U && clock_calls == 0U &&
		    timed_sem.waiters == 0 && timed_sem.value == 0);
		assert(sem_destroy(&timed_sem) == 0);
	}

	/* Kernel expiration of an absolute deadline is returned unchanged. */
	{
		const struct timespec deadline = { 5, 0 };

		assert(sem_init(&expired_sem, 0, 0) == 0);
		assert(sem_timedwait(&expired_sem, &deadline) == -1 &&
		    errno == ETIMEDOUT);
		assert(sem_expired_wait_calls == 1U && clock_calls == 0U &&
		    expired_sem.waiters == 0);
		assert(sem_destroy(&expired_sem) == 0);
	}

	/* POSIX permits an immediately available token to ignore an otherwise
	 * invalid/expired timeout because no timed wait is performed. */
	{
		sem_t immediate;
		const struct timespec invalid = { 0, 1000000000L };

		assert(sem_init(&immediate, 0, 1) == 0);
		assert(sem_timedwait(&immediate, &invalid) == 0);
		assert(clock_calls == 0U);
		assert(sem_destroy(&immediate) == 0);
	}
	return 0;
}
