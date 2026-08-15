/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_ATOMIC_H
#define ZEDBSD_KERN_ATOMIC_H

#include <limits.h>

typedef struct { unsigned value; } atomic_uint_t;
typedef struct { unsigned value; } refcount_t;

/*
 * The kernel must not acquire an implicit libatomic dependency.  In
 * particular, the i386 build still targets CPUs which predate CMPXCHG, while
 * XCHG itself has the required locked-memory semantics.  AArch64 uses the
 * baseline ARMv8 exclusive monitor rather than LSE/out-of-line helpers.
 */
static inline int atomic_try_acquire_zero(atomic_uint_t *value)
{
#if defined(__i386__) || defined(__x86_64__)
	unsigned previous = 1U;
	__asm__ volatile("xchgl %0, %1"
	    : "+r" (previous), "+m" (value->value) : : "memory");
	return previous == 0U;
#elif defined(__aarch64__)
	unsigned previous, status;
	__asm__ volatile(
	    "1: ldaxr %w0, [%2]\n"
	    "   cbnz %w0, 2f\n"
	    "   stxr %w1, %w3, [%2]\n"
	    "   cbnz %w1, 1b\n"
	    "   b 3f\n"
	    "2: clrex\n"
	    "3:\n"
	    : "=&r" (previous), "=&r" (status)
	    : "r" (&value->value), "r" (1U) : "memory");
	return previous == 0U;
#else
	return __atomic_exchange_n(&value->value, 1U,
	    __ATOMIC_ACQUIRE) == 0U;
#endif
}

static inline unsigned atomic_load_acquire(const atomic_uint_t *value)
{ return __atomic_load_n(&value->value, __ATOMIC_ACQUIRE); }
static inline void atomic_store_release(atomic_uint_t *value, unsigned next)
{ __atomic_store_n(&value->value, next, __ATOMIC_RELEASE); }
static inline unsigned atomic_fetch_add_relaxed(atomic_uint_t *value,
	unsigned add)
{ return __atomic_fetch_add(&value->value, add, __ATOMIC_RELAXED); }
static inline int atomic_compare_exchange(atomic_uint_t *value,
	unsigned *expected, unsigned desired)
{ return __atomic_compare_exchange_n(&value->value, expected, desired, 0,
	__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }

static inline void refcount_init(refcount_t *count, unsigned value)
{ __atomic_store_n(&count->value, value, __ATOMIC_RELAXED); }
static inline int refcount_tryget(refcount_t *count)
{
	unsigned value = __atomic_load_n(&count->value, __ATOMIC_RELAXED);
	for (;;) {
		if (value == 0 || value == UINT_MAX) return 0;
		if (__atomic_compare_exchange_n(&count->value, &value, value + 1U,
		    0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
	}
}
static inline void refcount_get(refcount_t *count)
{ if (!refcount_tryget(count)) __builtin_trap(); }
static inline int refcount_put(refcount_t *count)
{
	unsigned value = __atomic_load_n(&count->value, __ATOMIC_RELAXED);
	for (;;) {
		if (value == 0) __builtin_trap();
		if (__atomic_compare_exchange_n(&count->value, &value, value - 1U,
		    0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
			if (value == 1U) { __atomic_thread_fence(__ATOMIC_ACQUIRE); return 1; }
			return 0;
		}
	}
}

#endif
