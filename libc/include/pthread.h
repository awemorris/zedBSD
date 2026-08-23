/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_PTHREAD_H
#define ZEDBSD_PTHREAD_H
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <sys/types.h>
typedef tid_t pthread_t;
typedef unsigned pthread_key_t;
typedef struct __pthread_attr { size_t stacksize, guardsize; void *stackaddr; int detachstate, stackset; struct sched_param schedparam; } pthread_attr_t;
typedef struct { volatile uint32_t locked; pthread_t owner; unsigned count, type, pshared, robust; } pthread_mutex_t;
typedef struct { unsigned type, pshared, robust; } pthread_mutexattr_t;
typedef struct { volatile uint32_t sequence; unsigned pshared, clock; } pthread_cond_t;
typedef struct { unsigned clock, pshared; } pthread_condattr_t;
typedef struct { volatile uint32_t state; } pthread_once_t;
typedef struct { volatile uint32_t guard, sequence; unsigned readers, writer, pshared; } pthread_rwlock_t;
typedef struct { unsigned pshared; } pthread_rwlockattr_t;
typedef struct { volatile uint32_t guard, sequence; unsigned count, trip, pshared; } pthread_barrier_t;
typedef struct { unsigned pshared; } pthread_barrierattr_t;
typedef volatile uint32_t pthread_spinlock_t;
struct __pthread_cleanup {
	void (*routine)(void *);
	void *argument;
	struct __pthread_cleanup *previous;
};
#define PTHREAD_MUTEX_INITIALIZER {0,0,0,0,0,0}
#define PTHREAD_COND_INITIALIZER {0,0,CLOCK_REALTIME}
#define PTHREAD_ONCE_INIT {0}
#define PTHREAD_RWLOCK_INITIALIZER {0,0,0,0,0}
#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_STALLED 0
#define PTHREAD_MUTEX_ROBUST 1
#define PTHREAD_PROCESS_PRIVATE 0
#define PTHREAD_PROCESS_SHARED 1
#define PTHREAD_CANCELED ((void *)-1)
#define PTHREAD_CANCEL_ENABLE 0
#define PTHREAD_CANCEL_DISABLE 1
#define PTHREAD_CANCEL_DEFERRED 0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
#define PTHREAD_STACK_MIN 65536U
#define PTHREAD_BARRIER_SERIAL_THREAD (-1)
int pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
void pthread_exit(void *) __attribute__((noreturn));
int pthread_join(pthread_t,void **); int pthread_detach(pthread_t);
pthread_t pthread_self(void); int pthread_equal(pthread_t,pthread_t);
int pthread_attr_init(pthread_attr_t *); int pthread_attr_destroy(pthread_attr_t *);
int pthread_attr_setdetachstate(pthread_attr_t *,int); int pthread_attr_getdetachstate(const pthread_attr_t *,int *);
int pthread_attr_setstacksize(pthread_attr_t *,size_t); int pthread_attr_getstacksize(const pthread_attr_t *,size_t *);
int pthread_attr_setguardsize(pthread_attr_t *,size_t); int pthread_attr_getguardsize(const pthread_attr_t *,size_t *);
int pthread_attr_setstack(pthread_attr_t *,void *,size_t); int pthread_attr_getstack(const pthread_attr_t *,void **,size_t *);
int pthread_attr_getschedparam(const pthread_attr_t *,struct sched_param *);
int pthread_attr_setschedparam(pthread_attr_t *,const struct sched_param *);
int pthread_mutex_init(pthread_mutex_t *,const pthread_mutexattr_t *); int pthread_mutex_destroy(pthread_mutex_t *);
int pthread_mutexattr_init(pthread_mutexattr_t *); int pthread_mutexattr_destroy(pthread_mutexattr_t *);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *,int *);
int pthread_mutexattr_settype(pthread_mutexattr_t *,int); int pthread_mutexattr_setpshared(pthread_mutexattr_t *,int);
int pthread_mutexattr_getrobust(const pthread_mutexattr_t *,int *);
int pthread_mutexattr_setrobust(pthread_mutexattr_t *,int);
int pthread_mutex_consistent(pthread_mutex_t *);
int pthread_mutex_lock(pthread_mutex_t *); int pthread_mutex_trylock(pthread_mutex_t *); int pthread_mutex_timedlock(pthread_mutex_t *,const struct timespec *); int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_cond_init(pthread_cond_t *,const pthread_condattr_t *); int pthread_cond_destroy(pthread_cond_t *);
int pthread_condattr_init(pthread_condattr_t *); int pthread_condattr_destroy(pthread_condattr_t *);
int pthread_condattr_setpshared(pthread_condattr_t *,int);
int pthread_condattr_setclock(pthread_condattr_t *,clockid_t); int pthread_condattr_getclock(const pthread_condattr_t *,clockid_t *);
int pthread_cond_wait(pthread_cond_t *,pthread_mutex_t *); int pthread_cond_timedwait(pthread_cond_t *,pthread_mutex_t *,const struct timespec *);
int pthread_cond_signal(pthread_cond_t *); int pthread_cond_broadcast(pthread_cond_t *);
int pthread_rwlock_init(pthread_rwlock_t *,const pthread_rwlockattr_t *);
int pthread_rwlock_destroy(pthread_rwlock_t *); int pthread_rwlock_rdlock(pthread_rwlock_t *);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *); int pthread_rwlock_timedrdlock(pthread_rwlock_t *,const struct timespec *); int pthread_rwlock_wrlock(pthread_rwlock_t *);
int pthread_rwlock_trywrlock(pthread_rwlock_t *); int pthread_rwlock_timedwrlock(pthread_rwlock_t *,const struct timespec *); int pthread_rwlock_unlock(pthread_rwlock_t *);
int pthread_rwlockattr_init(pthread_rwlockattr_t *); int pthread_rwlockattr_destroy(pthread_rwlockattr_t *);
int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *,int);
int pthread_barrier_init(pthread_barrier_t *,const pthread_barrierattr_t *,unsigned);
int pthread_barrier_destroy(pthread_barrier_t *); int pthread_barrier_wait(pthread_barrier_t *);
int pthread_barrierattr_init(pthread_barrierattr_t *); int pthread_barrierattr_destroy(pthread_barrierattr_t *);
int pthread_barrierattr_setpshared(pthread_barrierattr_t *,int);
int pthread_spin_init(pthread_spinlock_t *,int); int pthread_spin_destroy(pthread_spinlock_t *);
int pthread_spin_lock(pthread_spinlock_t *); int pthread_spin_trylock(pthread_spinlock_t *);
int pthread_spin_unlock(pthread_spinlock_t *);
int pthread_once(pthread_once_t *,void (*)(void));
int pthread_key_create(pthread_key_t *,void (*)(void *)); int pthread_key_delete(pthread_key_t);
int pthread_setspecific(pthread_key_t,const void *); void *pthread_getspecific(pthread_key_t);
int pthread_sigmask(int,const sigset_t *,sigset_t *); int pthread_kill(pthread_t,int);
int pthread_cancel(pthread_t); int pthread_setcancelstate(int,int *);
int pthread_setcanceltype(int,int *); void pthread_testcancel(void);
int pthread_atfork(void (*)(void),void (*)(void),void (*)(void));
int pthread_getconcurrency(void);
int pthread_setconcurrency(int);
void __pthread_cleanup_push(struct __pthread_cleanup *,
	void (*)(void *),void *);
void __pthread_cleanup_pop(struct __pthread_cleanup *,int);
#define pthread_cleanup_push(routine,argument) do { \
	struct __pthread_cleanup __pthread_cleanup_record; \
	__pthread_cleanup_push(&__pthread_cleanup_record,(routine),(argument));
#define pthread_cleanup_pop(execute) \
	__pthread_cleanup_pop(&__pthread_cleanup_record,(execute)); \
} while (0)
#endif
