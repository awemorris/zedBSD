/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library semaphore support.
 */

#include "userland/base/libc/syscall.h"
#include <zedbsd/syscall.h>
#include <zedbsd/usync.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define SEM_MAGIC 0x5a53454dU
extern void __pthread_cancel_point(void) __attribute__((weak));
extern int __pthread_cancel_enabled(void) __attribute__((weak));

static void sem_lock(sem_t *sem);
static void sem_unlock(sem_t *sem);
static int sem_wait_common(sem_t *sem, clockid_t clock, const struct timespec *absolute);
static void cancel_point(void);
static int sem_usync_wait(sem_t *sem, const struct timespec *timeout, clockid_t clock);
static uintptr_t cancelable_flag(void);
static void sem_usync_wake(sem_t *sem);
static int named_sem_name(const char *name, char output[PATH_MAX]);

/*
 * Implements the sem init operation.
 */
int
sem_init(
	sem_t *sem,
	int pshared,
	unsigned value)
{
	/* Handles the sem availability. */
	if (sem == NULL || value > SEM_VALUE_MAX) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	sem->value = value;
	sem->waiters = 0;
	sem->guard = 0;
	sem->pshared = pshared != 0;
	__atomic_store_n(&sem->magic, SEM_MAGIC, __ATOMIC_RELEASE);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sem destroy operation.
 */
int
sem_destroy(
	sem_t *sem)
{
	/* Handles the sem availability. */
	if (sem == NULL || sem->magic != SEM_MAGIC || sem->waiters != 0) {
		errno = sem != NULL && sem->waiters != 0 ? EBUSY : EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	sem->magic = 0;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sem trywait operation.
 */
int
sem_trywait(
	sem_t *sem)
{
	/* Handles the sem availability. */
	if (sem == NULL || sem->magic != SEM_MAGIC) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	sem_lock(sem);

	/* Handles the sem condition. */
	if (sem->value != 0) {
		sem->value--;
		sem_unlock(sem);

		/* Reports successful completion. */
		return 0;
	}
	sem_unlock(sem);
	errno = EAGAIN;

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the sem wait operation.
 */
int
sem_wait(
	sem_t *sem)
{
	int function_result;

	/* Obtains the sem wait common result. */
	function_result = sem_wait_common(sem, CLOCK_REALTIME, NULL);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sem timedwait operation.
 */
int
sem_timedwait(
	sem_t *sem,
	const struct timespec *absolute)
{
	int function_result;

	/* Handles the absolute availability. */
	if (absolute == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the sem wait common result. */
	function_result = sem_wait_common(sem, CLOCK_REALTIME, absolute);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sem clockwait operation.
 */
int
sem_clockwait(
	sem_t *sem,
	clockid_t clock,
	const struct timespec *absolute)
{
	int function_result;

	/* Handles the absolute availability. */
	if (absolute == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the sem wait common result. */
	function_result = sem_wait_common(sem, clock, absolute);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sem post operation.
 */
int
sem_post(
	sem_t *sem)
{
	/* Handles the sem availability. */
	if (sem == NULL || sem->magic != SEM_MAGIC) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	sem_lock(sem);

	/* Handles the sem condition. */
	if (sem->value >= SEM_VALUE_MAX) {
		sem_unlock(sem);
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	sem->value++;
	sem_unlock(sem);

	/* Handles a failed atomic load n operation. */
	if (__atomic_load_n(&sem->waiters, __ATOMIC_RELAXED) != 0)
		sem_usync_wake(sem);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sem getvalue operation.
 */
int
sem_getvalue(
	sem_t *sem,
	int *result)
{
	/* Handles the sem availability. */
	if (sem == NULL || result == NULL || sem->magic != SEM_MAGIC) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	*result = (int)__atomic_load_n(&sem->value, __ATOMIC_ACQUIRE);
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sem open operation.
 */
sem_t *
sem_open(
	const char *name,
	int flags,
	...)
{
	char object_name[PATH_MAX];
	mode_t mode;
	unsigned value;
	sem_t *sem;
	int descriptor, created;

	va_list args;

	mode = 0;
	value = 0;
	created = 0;

	/* Handles a failed named sem name operation. */
	if (named_sem_name(name, object_name) != 0)
		return SEM_FAILED;

	/* Checks the active flags. */
	if ((flags & O_CREAT) != 0) {
		va_start(args, flags);
		mode = va_arg(args, mode_t);
		value = va_arg(args, unsigned);
		va_end(args);

		/* Validates the current value. */
		if (value > SEM_VALUE_MAX) {
			errno = EINVAL;

			/* Returns the computed result. */
			return SEM_FAILED;
		}
	}

	/* Checks the active flags. */
	if ((flags & O_CREAT) != 0) {
		descriptor =
		    shm_open(object_name, O_RDWR | O_CREAT | O_EXCL, mode);

		/* Checks the file descriptor. */
		if (descriptor >= 0)
			created = 1;
		else if (errno == EEXIST && (flags & O_EXCL) == 0)
			descriptor = shm_open(object_name, O_RDWR, mode);
	} else {
		descriptor = shm_open(object_name, O_RDWR, mode);
	}

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return SEM_FAILED;

	/* Handles a failed ftruncate operation. */
	if (created && ftruncate(descriptor, (off_t)sizeof(*sem)) != 0) {
		(void)close(descriptor);
		(void)shm_unlink(object_name);

		/* Returns the computed result. */
		return SEM_FAILED;
	}
	sem = mmap(NULL, sizeof(*sem), PROT_READ | PROT_WRITE, MAP_SHARED,
		   descriptor, 0);
	(void)close(descriptor);

	/* Handles an operation failure. */
	if (sem == MAP_FAILED) {
		/* Handles the created condition. */
		if (created)
			(void)shm_unlink(object_name);

		/* Returns the computed result. */
		return SEM_FAILED;
	}

	/* Handles the created condition. */
	if (created) {
		(void)sem_init(sem, 1, value);
	} else if (__atomic_load_n(&sem->magic, __ATOMIC_ACQUIRE) !=
		   SEM_MAGIC) {
		(void)munmap(sem, sizeof(*sem));
		errno = EINVAL;

		/* Returns the computed result. */
		return SEM_FAILED;
	}

	/* Returns the computed result. */
	return sem;
}

/*
 * Implements the sem close operation.
 */
int
sem_close(
	sem_t *sem)
{
	int function_result;

	/* Handles an operation failure. */
	if (sem == NULL || sem == SEM_FAILED) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	(void)msync(sem, sizeof(*sem), MS_SYNC);

	/* Obtains the munmap result. */
	function_result = munmap(sem, sizeof(*sem));

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sem unlink operation.
 */
int
sem_unlink(
	const char *name)
{
	int function_result;
	char object_name[PATH_MAX];

	/* Computes the function result. */
	function_result = named_sem_name(name, object_name) == 0 ? shm_unlink(object_name)
						      : -1;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the sem lock operation. */
static void
sem_lock(
	sem_t *sem)
{
	/* Continue while the operation condition remains true. */
	while (__atomic_exchange_n(&sem->guard, 1, __ATOMIC_ACQUIRE) != 0) {
		(void)__syscall6(ZEDBSD_SYS_usync, (uintptr_t)&sem->guard,
				 ZEDBSD_USYNC_WAIT, 1, 0, 0,
				 sem->pshared ? 0 : ZEDBSD_USYNC_PRIVATE);
	}
}

/* Supports the sem unlock operation. */
static void
sem_unlock(
	sem_t *sem)
{
	__atomic_store_n(&sem->guard, 0, __ATOMIC_RELEASE);
	(void)__syscall6(ZEDBSD_SYS_usync, (uintptr_t)&sem->guard,
			 ZEDBSD_USYNC_WAKE, 0, 0, 1,
			 sem->pshared ? 0 : ZEDBSD_USYNC_PRIVATE);
}

/* Supports the sem wait common operation. */
static int
sem_wait_common(
	sem_t *sem,
	clockid_t clock,
	const struct timespec *absolute)
{
	int saved_errno;

	cancel_point();

	/* Handles the absolute availability. */
	if (absolute != NULL &&
	    (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed sem trywait operation. */
		if (sem_trywait(sem) == 0) {
			cancel_point();

			/* Reports successful completion. */
			return 0;
		}

		/* Handles the reported system error. */
		if (errno != EAGAIN)
			return -1;
		__atomic_add_fetch(&sem->waiters, 1, __ATOMIC_RELAXED);

		/* Handles a failed sem usync wait operation. */
		if (sem_usync_wait(sem, absolute, clock) != 0) {
			saved_errno = errno;

			__atomic_sub_fetch(&sem->waiters, 1, __ATOMIC_RELAXED);

			/* Handles the saved errno condition. */
			if (saved_errno == EAGAIN) {
				/*
 * The value changed between sem_trywait and the
				 * kernel comparison.  Retry the predicate and
				 * also honor a cancel request which bypassed
				 * wait registration. */
				cancel_point();
				continue;
			}

			/*
 * A cancellation request must be acted on only after
			 * waiter accounting is rolled back.  An ordinary signal
			 * remains EINTR. */
			if (saved_errno == EINTR)
				cancel_point();
			errno = saved_errno;

			/* Reports operation failure. */
			return -1;
		}
		__atomic_sub_fetch(&sem->waiters, 1, __ATOMIC_RELAXED);
		cancel_point();
	}
}

/* Supports the cancel point operation. */
static void
cancel_point(
	void)
{
	/* Handles the pthread cancel point availability. */
	if (__pthread_cancel_point != NULL)
		__pthread_cancel_point();
}

/* Supports the sem usync wait operation. */
static int
sem_usync_wait(
	sem_t *sem,
	const struct timespec *timeout,
	clockid_t clock)
{
	intptr_t result;
	uintptr_t timeout_flags;

	timeout_flags = 0;

	/* Handles the timeout availability. */
	if (timeout != NULL) {
		timeout_flags = ZEDBSD_USYNC_ABSTIME;

		/* Handles the clock condition. */
		if (clock == CLOCK_REALTIME)
			timeout_flags |= ZEDBSD_USYNC_CLOCK_REALTIME;
	}
	result = syscall_result(
	    __syscall6(ZEDBSD_SYS_usync, (uintptr_t)&sem->value,
		       ZEDBSD_USYNC_WAIT, 0, (uintptr_t)timeout, 0,
		       (sem->pshared ? 0 : ZEDBSD_USYNC_PRIVATE) |
			   cancelable_flag() | timeout_flags));

	/* Returns the computed result. */
	return result < 0 ? -1 : 0;
}

/* Supports the cancelable flag operation. */
static uintptr_t
cancelable_flag(
	void)
{
	uintptr_t function_result;

	/* Computes the function result. */
	function_result = __pthread_cancel_enabled != NULL && __pthread_cancel_enabled()
		   ? ZEDBSD_USYNC_CANCELABLE
		   : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the sem usync wake operation. */
static void
sem_usync_wake(
	sem_t *sem)
{
	(void)__syscall6(ZEDBSD_SYS_usync, (uintptr_t)&sem->value,
			 ZEDBSD_USYNC_WAKE, 0, 0, 1,
			 sem->pshared ? 0 : ZEDBSD_USYNC_PRIVATE);
}

/* Supports the named sem name operation. */
static int
named_sem_name(
	const char *name,
	char output[PATH_MAX])
{
	size_t length;

	/* Handles a failed strchr operation. */
	if (name == NULL || name[0] != '/' || name[1] == '\0' ||
	    strchr(name + 1, '/') != NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	length = strlen(name + 1);

	/* Checks the current data length. */
	if (length > PATH_MAX - sizeof("/sem.")) {
		errno = ENAMETOOLONG;

		/* Reports operation failure. */
		return -1;
	}
	memcpy(output, "/sem.", sizeof("/sem.") - 1U);
	memcpy(output + sizeof("/sem.") - 1U, name + 1, length + 1U);

	/* Reports successful completion. */
	return 0;
}
