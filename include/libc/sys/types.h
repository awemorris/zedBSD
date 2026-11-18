/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SYS_TYPES_H
#define LIBC_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

/*
 * Which user ABI these declarations describe.  A build inside this repository
 * says so on the command line, because the kernel also compiles these headers
 * and may be describing a user ABI narrower than its own.  A program compiled
 * on the machine has no such build to speak for it, and for a program the
 * answer is never in doubt: the user ABI it is part of is the one it is being
 * compiled for, which the compiler already states.
 */
#ifndef KERN_USER_ABI_LP64
#ifdef __LP64__
#define KERN_USER_ABI_LP64 1
#endif
#endif

#include <uapi/types.h>

/*
 * The type a memory address had before void * existed.  It is kept because
 * interfaces written then still name it.
 */
typedef char *caddr_t;

/*
 * A processor time in the units of CLOCKS_PER_SEC or of sysconf(_SC_CLK_TCK),
 * as clock() and times() report it.  <time.h> and <sys/times.h> take it from
 * here so that a program including both sees one definition.
 */
typedef long clock_t;

/*
 * The name of a System V IPC object, as ftok() makes it.  <sys/ipc.h> takes
 * it from here.
 */
typedef int key_t;

/*
 * The scheduling parameters of a thread or a process.  <sched.h> is where a
 * program looks for it; it is defined here because a thread attribute object
 * below carries one by value, and <sched.h> includes this header.
 */
struct sched_param {
	int sched_priority;
};

/*
 * The thread types.  <pthread.h> declares the functions that work on them
 * and the initializers that fill them; the objects themselves are defined
 * here so that a program can hold one, or name its size, with only
 * <sys/types.h>.  A thread is named by the kernel's thread identifier, and
 * every lock and condition is a plain structure the C library waits on with
 * a futex, so none of them owns memory outside itself.
 */
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

#endif
