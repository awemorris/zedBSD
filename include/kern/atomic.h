/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_ATOMIC_H
#define ZEDBSD_KERN_ATOMIC_H

#include <hal/hal.h>
#include <limits.h>
#include <stdint.h>

typedef struct {
	unsigned value;
} atomic_uint_t;

typedef struct {
	unsigned value;
} refcount_t;


/*
 * The kernel must not acquire an implicit libatomic dependency.  In
 * particular, some ports need a HAL-provided serialization region for
 * operations which are not natively lock-free.  Compiler and architecture
 * details are confined to the HAL; this header retains only kernel types and
 * reference-counting policy.
 */
static inline int
atomic_try_acquire_zero(
	atomic_uint_t *value)
{
	return hal_atomic_uint_try_acquire(&value->value);
}

static inline unsigned
atomic_raw_load_acquire(
	const volatile unsigned *value)
{
	return hal_atomic_load_acquire(value);
}

static inline unsigned
atomic_raw_load_relaxed(
	const volatile unsigned *value)
{
	return hal_atomic_load_relaxed(value);
}

static inline void
atomic_raw_store_release(
	volatile unsigned *value,
	unsigned next)
{
	hal_atomic_store_release(value, next);
}

static inline unsigned
atomic_raw_fetch_add_relaxed(
	volatile unsigned *value,
	unsigned add)
{
	return hal_atomic_fetch_add_relaxed(value, add);
}

static inline unsigned
atomic_raw_fetch_or_release(
	volatile unsigned *value,
	unsigned bits)
{
	return hal_atomic_fetch_or_release(value, bits);
}

static inline int
atomic_raw_compare_exchange(
	volatile unsigned *value,
	unsigned *expected,
	unsigned desired)
{
	return hal_atomic_compare_exchange_acq_rel(value, expected, desired);
}

static inline unsigned
atomic_load_acquire(
	const atomic_uint_t *value)
{
	return atomic_raw_load_acquire(&value->value);
}

static inline void
atomic_store_release(
	atomic_uint_t *value,
	unsigned next)
{
	atomic_raw_store_release(&value->value, next);
}

static inline unsigned
atomic_fetch_add_relaxed(
	atomic_uint_t *value,
	unsigned add)
{
	return atomic_raw_fetch_add_relaxed(&value->value, add);
}

static inline int
atomic_compare_exchange(
	atomic_uint_t *value,
	unsigned *expected,
	unsigned desired)
{
	return atomic_raw_compare_exchange(&value->value, expected, desired);
}

static inline uint64_t
atomic_u64_load_acquire(
	const volatile uint64_t *value)
{
	return hal_atomic_load_acquire(value);
}

static inline void
atomic_u64_store_release(
	volatile uint64_t *value,
	uint64_t next)
{
	hal_atomic_store_release(value, next);
}

static inline uint64_t
atomic_u64_fetch_add_relaxed(
	volatile uint64_t *value,
	uint64_t add)
{
	return hal_atomic_fetch_add_relaxed(value, add);
}

static inline uint64_t
atomic_u64_fetch_add_release(
	volatile uint64_t *value,
	uint64_t add)
{
	return hal_atomic_fetch_add_release(value, add);
}

static inline uint64_t
atomic_u64_fetch_or_release(
	volatile uint64_t *value,
	uint64_t bits)
{
	return hal_atomic_fetch_or_release(value, bits);
}

static inline int
atomic_u64_compare_exchange(
	volatile uint64_t *value,
	uint64_t *expected,
	uint64_t desired)
{
	return hal_atomic_compare_exchange_acq_rel(value, expected, desired);
}

static inline int
atomic_int_load_acquire(
	const volatile int *value)
{
	return hal_atomic_load_acquire(value);
}

static inline int
atomic_int_load_relaxed(
	const volatile int *value)
{
	return hal_atomic_load_relaxed(value);
}

static inline void
atomic_int_store_release(
	volatile int *value,
	int next)
{
	hal_atomic_store_release(value, next);
}

static inline void
atomic_int_store_relaxed(
	volatile int *value,
	int next)
{
	hal_atomic_store_relaxed(value, next);
}

static inline void
refcount_init(
	refcount_t *count,
	unsigned value)
{
	atomic_raw_store_release(&count->value, value);
}

static inline unsigned
refcount_load(
	const refcount_t *count)
{
	return atomic_raw_load_acquire(&count->value);
}

static inline int
refcount_tryget(
	refcount_t *count)
{
	unsigned value = hal_atomic_load_relaxed(&count->value);
	for (;;) {
		if (value == 0 || value == UINT_MAX)
			return 0;
		if (hal_atomic_compare_exchange_acquire(&count->value,
		    &value, value + 1U))
			return 1;
	}
}

static inline void
refcount_get(
	refcount_t *count)
{
	if (!refcount_tryget(count))
		__builtin_trap();
}

static inline int
refcount_put(
	refcount_t *count)
{
	unsigned value = hal_atomic_load_relaxed(&count->value);
	for (;;) {
		if (value == 0)
			__builtin_trap();
		if (hal_atomic_compare_exchange_release(&count->value,
		    &value, value - 1U)) {
			if (value == 1U) {
				hal_atomic_fence_acquire();
				return 1;
			}
			return 0;
		}
	}
}

/*
 * Drop a caller reference while preserving an object's permanent registry or
 * cache reference.  Returns the new count, or zero if no reference was
 * dropped because only the permanent reference remains.
 */
static inline unsigned
refcount_put_not_last(
	refcount_t *count)
{
	unsigned value = hal_atomic_load_relaxed(&count->value);
	for (;;) {
		if (value <= 1U)
			return 0;
		if (hal_atomic_compare_exchange_release(&count->value,
		    &value, value - 1U))
			return value - 1U;
	}
}

#endif
