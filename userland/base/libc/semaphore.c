/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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
static void cancel_point(void)
{ if (__pthread_cancel_point != NULL) __pthread_cancel_point(); }

static int
sem_usync_wait(sem_t *sem, const struct timespec *timeout)
{
	intptr_t result = syscall_result(__syscall6(ZEDBSD_SYS_usync,
	    (uintptr_t)&sem->value, ZEDBSD_USYNC_WAIT, 0, (uintptr_t)timeout,
	    0, sem->pshared ? 0 : ZEDBSD_USYNC_PRIVATE));
	return result < 0 ? -1 : 0;
}

static void
sem_usync_wake(sem_t *sem)
{
	(void)__syscall6(ZEDBSD_SYS_usync, (uintptr_t)&sem->value,
	    ZEDBSD_USYNC_WAKE, 0, 0, 1,
	    sem->pshared ? 0 : ZEDBSD_USYNC_PRIVATE);
}

static void
sem_lock(sem_t *sem)
{
	while (__atomic_exchange_n(&sem->guard, 1, __ATOMIC_ACQUIRE) != 0) {
		(void)__syscall6(ZEDBSD_SYS_usync, (uintptr_t)&sem->guard,
		    ZEDBSD_USYNC_WAIT, 1, 0, 0,
		    sem->pshared ? 0 : ZEDBSD_USYNC_PRIVATE);
	}
}

static void
sem_unlock(sem_t *sem)
{
	__atomic_store_n(&sem->guard, 0, __ATOMIC_RELEASE);
	(void)__syscall6(ZEDBSD_SYS_usync, (uintptr_t)&sem->guard,
	    ZEDBSD_USYNC_WAKE, 0, 0, 1,
	    sem->pshared ? 0 : ZEDBSD_USYNC_PRIVATE);
}

int
sem_init(sem_t *sem, int pshared, unsigned value)
{
	if (sem == NULL || value > SEM_VALUE_MAX) { errno = EINVAL; return -1; }
	sem->value = value;
	sem->waiters = 0;
	sem->guard = 0;
	sem->pshared = pshared != 0;
	__atomic_store_n(&sem->magic, SEM_MAGIC, __ATOMIC_RELEASE);
	return 0;
}

int sem_destroy(sem_t *sem)
{
	if (sem == NULL || sem->magic != SEM_MAGIC || sem->waiters != 0) {
		errno = sem != NULL && sem->waiters != 0 ? EBUSY : EINVAL;
		return -1;
	}
	sem->magic = 0;
	return 0;
}

int
sem_trywait(sem_t *sem)
{
	if (sem == NULL || sem->magic != SEM_MAGIC) { errno = EINVAL; return -1; }
	sem_lock(sem);
	if (sem->value != 0) {
		sem->value--;
		sem_unlock(sem);
		return 0;
	}
	sem_unlock(sem);
	errno = EAGAIN;
	return -1;
}

static int
sem_wait_relative(sem_t *sem, const struct timespec *relative)
{
	cancel_point();
	for (;;) {
		if (sem_trywait(sem) == 0) {
			cancel_point();
			return 0;
		}
		if (errno != EAGAIN)
			return -1;
		__atomic_add_fetch(&sem->waiters, 1, __ATOMIC_RELAXED);
		if (sem_usync_wait(sem, relative) != 0) {
			__atomic_sub_fetch(&sem->waiters, 1, __ATOMIC_RELAXED);
			return -1;
		}
		__atomic_sub_fetch(&sem->waiters, 1, __ATOMIC_RELAXED);
		cancel_point();
	}
}

int sem_wait(sem_t *sem) { return sem_wait_relative(sem, NULL); }

int
sem_timedwait(sem_t *sem, const struct timespec *absolute)
{
	struct timespec now, relative;
	if (absolute == NULL || absolute->tv_nsec < 0 ||
	    absolute->tv_nsec >= 1000000000L) { errno = EINVAL; return -1; }
	if (clock_gettime(CLOCK_REALTIME, &now) != 0)
		return -1;
	if (absolute->tv_sec < now.tv_sec ||
	    (absolute->tv_sec == now.tv_sec && absolute->tv_nsec <= now.tv_nsec)) {
		errno = ETIMEDOUT;
		return -1;
	}
	relative.tv_sec = absolute->tv_sec - now.tv_sec;
	relative.tv_nsec = absolute->tv_nsec - now.tv_nsec;
	if (relative.tv_nsec < 0) { relative.tv_sec--; relative.tv_nsec += 1000000000L; }
	return sem_wait_relative(sem, &relative);
}

int
sem_post(sem_t *sem)
{
	if (sem == NULL || sem->magic != SEM_MAGIC) { errno = EINVAL; return -1; }
	sem_lock(sem);
	if (sem->value >= SEM_VALUE_MAX) {
		sem_unlock(sem); errno = EOVERFLOW; return -1;
	}
	sem->value++;
	sem_unlock(sem);
	if (__atomic_load_n(&sem->waiters, __ATOMIC_RELAXED) != 0)
		sem_usync_wake(sem);
	return 0;
}

int sem_getvalue(sem_t *sem, int *result)
{
	if (sem == NULL || result == NULL || sem->magic != SEM_MAGIC) {
		errno = EINVAL; return -1;
	}
	*result = (int)__atomic_load_n(&sem->value, __ATOMIC_ACQUIRE);
	return 0;
}

static int
named_sem_name(const char *name, char output[PATH_MAX])
{
	size_t length;
	if (name == NULL || name[0] != '/' || name[1] == '\0' ||
	    strchr(name + 1, '/') != NULL) { errno = EINVAL; return -1; }
	length = strlen(name + 1);
	if (length > PATH_MAX - sizeof("/sem.")) { errno = ENAMETOOLONG; return -1; }
	memcpy(output, "/sem.", sizeof("/sem.") - 1U);
	memcpy(output + sizeof("/sem.") - 1U, name + 1, length + 1U);
	return 0;
}

sem_t *
sem_open(const char *name, int flags, ...)
{
	char object_name[PATH_MAX];
	mode_t mode = 0;
	unsigned value = 0;
	sem_t *sem;
	int descriptor, created = 0;
	va_list args;
	if (named_sem_name(name, object_name) != 0)
		return SEM_FAILED;
	if ((flags & O_CREAT) != 0) {
		va_start(args, flags);
		mode = va_arg(args, mode_t);
		value = va_arg(args, unsigned);
		va_end(args);
		if (value > SEM_VALUE_MAX) { errno = EINVAL; return SEM_FAILED; }
	}
	if ((flags & O_CREAT) != 0) {
		descriptor = shm_open(object_name, O_RDWR | O_CREAT | O_EXCL, mode);
		if (descriptor >= 0)
			created = 1;
		else if (errno == EEXIST && (flags & O_EXCL) == 0)
			descriptor = shm_open(object_name, O_RDWR, mode);
	} else {
		descriptor = shm_open(object_name, O_RDWR, mode);
	}
	if (descriptor < 0)
		return SEM_FAILED;
	if (created && ftruncate(descriptor, (off_t)sizeof(*sem)) != 0) {
		(void)close(descriptor); (void)shm_unlink(object_name); return SEM_FAILED;
	}
	sem = mmap(NULL, sizeof(*sem), PROT_READ | PROT_WRITE, MAP_SHARED,
	    descriptor, 0);
	(void)close(descriptor);
	if (sem == MAP_FAILED) {
		if (created) (void)shm_unlink(object_name);
		return SEM_FAILED;
	}
	if (created) {
		(void)sem_init(sem, 1, value);
	} else if (__atomic_load_n(&sem->magic, __ATOMIC_ACQUIRE) != SEM_MAGIC) {
		(void)munmap(sem, sizeof(*sem)); errno = EINVAL; return SEM_FAILED;
	}
	return sem;
}

int sem_close(sem_t *sem)
{
	if (sem == NULL || sem == SEM_FAILED) { errno = EINVAL; return -1; }
	(void)msync(sem, sizeof(*sem), MS_SYNC);
	return munmap(sem, sizeof(*sem));
}
int sem_unlink(const char *name)
{ char object_name[PATH_MAX]; return named_sem_name(name, object_name) == 0 ? shm_unlink(object_name) : -1; }
