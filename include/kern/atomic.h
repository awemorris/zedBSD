/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_ATOMIC_H
#define ZEDBSD_KERN_ATOMIC_H

#include <hal/hal.h>
#include <limits.h>
#include <stdint.h>

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

static inline unsigned atomic_raw_load_acquire(const volatile unsigned *value)
{
#if defined(__i386__)
	bool enabled = hal_irq_disable();
	unsigned result = *value;
	if (enabled) hal_irq_enable();
	return result;
#else
	return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}
static inline void atomic_raw_store_release(volatile unsigned *value,
	unsigned next)
{
#if defined(__i386__)
	bool enabled = hal_irq_disable();
	*value = next;
	if (enabled) hal_irq_enable();
#else
	__atomic_store_n(value, next, __ATOMIC_RELEASE);
#endif
}
static inline unsigned atomic_raw_fetch_add_relaxed(volatile unsigned *value,
	unsigned add)
{
#if defined(__i386__)
	bool enabled = hal_irq_disable();
	unsigned previous = *value;
	*value = previous + add;
	if (enabled) hal_irq_enable();
	return previous;
#else
	return __atomic_fetch_add(value, add, __ATOMIC_RELAXED);
#endif
}
static inline unsigned atomic_raw_fetch_or_release(volatile unsigned *value,
	unsigned bits)
{
#if defined(__i386__)
	bool enabled = hal_irq_disable();
	unsigned previous = *value;
	*value = previous | bits;
	if (enabled) hal_irq_enable();
	return previous;
#else
	return __atomic_fetch_or(value, bits, __ATOMIC_RELEASE);
#endif
}
static inline int atomic_raw_compare_exchange(volatile unsigned *value,
	unsigned *expected, unsigned desired)
{
#if defined(__i386__)
	bool enabled = hal_irq_disable();
	unsigned current = *value;
	int exchanged = current == *expected;
	if (exchanged) *value = desired; else *expected = current;
	if (enabled) hal_irq_enable();
	return exchanged;
#else
	return __atomic_compare_exchange_n(value, expected, desired, 0,
	    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#endif
}
static inline unsigned atomic_load_acquire(const atomic_uint_t *value)
{ return atomic_raw_load_acquire(&value->value); }
static inline void atomic_store_release(atomic_uint_t *value, unsigned next)
{ atomic_raw_store_release(&value->value, next); }
static inline unsigned atomic_fetch_add_relaxed(atomic_uint_t *value,
	unsigned add)
{ return atomic_raw_fetch_add_relaxed(&value->value, add); }
static inline int atomic_compare_exchange(atomic_uint_t *value,
	unsigned *expected, unsigned desired)
{ return atomic_raw_compare_exchange(&value->value, expected, desired); }

static inline uint64_t atomic_u64_load_acquire(const volatile uint64_t *value)
{
#if defined(__i386__)
	bool enabled = hal_irq_disable();
	uint64_t result = *value;
	if (enabled) hal_irq_enable();
	return result;
#else
	return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}
static inline void atomic_u64_store_release(volatile uint64_t *value,
	uint64_t next)
{
#if defined(__i386__)
	bool enabled = hal_irq_disable();
	*value = next;
	if (enabled) hal_irq_enable();
#else
	__atomic_store_n(value, next, __ATOMIC_RELEASE);
#endif
}
static inline uint64_t atomic_u64_fetch_add_relaxed(volatile uint64_t *value,
	uint64_t add)
{
#if defined(__i386__)
	bool enabled = hal_irq_disable();
	uint64_t previous = *value;
	*value = previous + add;
	if (enabled) hal_irq_enable();
	return previous;
#else
	return __atomic_fetch_add(value, add, __ATOMIC_RELAXED);
#endif
}
static inline uint64_t atomic_u64_fetch_or_release(volatile uint64_t *value,
	uint64_t bits)
{
#if defined(__i386__)
	bool enabled = hal_irq_disable();
	uint64_t previous = *value;
	*value = previous | bits;
	if (enabled) hal_irq_enable();
	return previous;
#else
	return __atomic_fetch_or(value, bits, __ATOMIC_RELEASE);
#endif
}

static inline void refcount_init(refcount_t *count, unsigned value)
{ atomic_raw_store_release(&count->value, value); }
static inline unsigned refcount_load(const refcount_t *count)
{ return atomic_raw_load_acquire(&count->value); }
static inline int refcount_tryget(refcount_t *count)
{
#if defined(__i386__)
	/* zedBSD i386 targets pre-CMPXCHG CPUs and is strictly uniprocessor. */
	bool enabled = hal_irq_disable();
	unsigned value = count->value;
	if (value == 0 || value == UINT_MAX) {
		if (enabled) hal_irq_enable();
		return 0;
	}
	count->value = value + 1U;
	if (enabled) hal_irq_enable();
	return 1;
#else
	unsigned value = __atomic_load_n(&count->value, __ATOMIC_RELAXED);
	for (;;) {
		if (value == 0 || value == UINT_MAX) return 0;
		if (__atomic_compare_exchange_n(&count->value, &value, value + 1U,
		    0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
	}
#endif
}
static inline void refcount_get(refcount_t *count)
{ if (!refcount_tryget(count)) __builtin_trap(); }
static inline int refcount_put(refcount_t *count)
{
#if defined(__i386__)
	bool enabled = hal_irq_disable();
	unsigned value = count->value;
	if (value == 0) __builtin_trap();
	count->value = value - 1U;
	if (enabled) hal_irq_enable();
	return value == 1U;
#else
	unsigned value = __atomic_load_n(&count->value, __ATOMIC_RELAXED);
	for (;;) {
		if (value == 0) __builtin_trap();
		if (__atomic_compare_exchange_n(&count->value, &value, value - 1U,
		    0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
			if (value == 1U) { __atomic_thread_fence(__ATOMIC_ACQUIRE); return 1; }
			return 0;
		}
	}
#endif
}

/* Drop a caller reference while preserving an object's permanent registry or
 * cache reference.  Returns the new count, or zero if no reference was
 * dropped because only the permanent reference remains. */
static inline unsigned refcount_put_not_last(refcount_t *count)
{
#if defined(__i386__)
	bool enabled = hal_irq_disable();
	unsigned value = count->value;
	if (value <= 1U) { if (enabled) hal_irq_enable(); return 0; }
	count->value = value - 1U;
	if (enabled) hal_irq_enable();
	return value - 1U;
#else
	unsigned value = __atomic_load_n(&count->value, __ATOMIC_RELAXED);
	for (;;) {
		if (value <= 1U) return 0;
		if (__atomic_compare_exchange_n(&count->value, &value, value - 1U,
		    0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return value - 1U;
	}
#endif
}

#endif
