/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef HAL_HAL_ATOMIC_H
#define HAL_HAL_ATOMIC_H

#include <hal/arch.h>
#include <hal/types.h>

/*
 * Atomic fallback serialization
 * -----------------------------
 * HAL_ATOMIC_STYLE_ENTER ports serialize the short plain-memory sequences
 * used to emulate atomic operations.  The region is non-recursive, cannot
 * sleep, and must exclude every CPU.  A local IRQ mask is sufficient only
 * for a HAL which guarantees uniprocessor execution.
 *
 * Both enter and leave are full compiler barriers.  Native ports instead use
 * compiler primitives hidden behind the HAL interface below.
 */
typedef uintptr_t hal_atomic_cookie_t;

#if HAL_ATOMIC_STYLE == HAL_ATOMIC_STYLE_ENTER

hal_atomic_cookie_t
hal_atomic_enter(void);

void
hal_atomic_leave(
	hal_atomic_cookie_t cookie);

#define hal_atomic_load_acquire(pointer)					\
	({									\
		__auto_type hal_atomic_pointer_ = (pointer);			\
		hal_atomic_cookie_t hal_atomic_cookie_ = hal_atomic_enter();	\
		__auto_type hal_atomic_result_ = *hal_atomic_pointer_;		\
		hal_atomic_leave(hal_atomic_cookie_);				\
		hal_atomic_result_;						\
	})

#define hal_atomic_load_relaxed(pointer)					\
	hal_atomic_load_acquire(pointer)

#define hal_atomic_store_release(pointer, value)				\
	do {									\
		__auto_type hal_atomic_pointer_ = (pointer);			\
		__auto_type hal_atomic_value_ = (value);			\
		hal_atomic_cookie_t hal_atomic_cookie_ = hal_atomic_enter();	\
		*hal_atomic_pointer_ = hal_atomic_value_;			\
		hal_atomic_leave(hal_atomic_cookie_);				\
	} while (0)

#define hal_atomic_store_relaxed(pointer, value)				\
	hal_atomic_store_release((pointer), (value))

#define hal_atomic_fetch_add_relaxed(pointer, value)				\
	({									\
		__auto_type hal_atomic_pointer_ = (pointer);			\
		__auto_type hal_atomic_value_ = (value);			\
		hal_atomic_cookie_t hal_atomic_cookie_ = hal_atomic_enter();	\
		__auto_type hal_atomic_previous_ = *hal_atomic_pointer_;	\
		*hal_atomic_pointer_ = hal_atomic_previous_ +			\
		    hal_atomic_value_;						\
		hal_atomic_leave(hal_atomic_cookie_);				\
		hal_atomic_previous_;						\
	})

#define hal_atomic_fetch_add_release(pointer, value)				\
	hal_atomic_fetch_add_relaxed((pointer), (value))

#define hal_atomic_fetch_or_release(pointer, value)				\
	({									\
		__auto_type hal_atomic_pointer_ = (pointer);			\
		__auto_type hal_atomic_value_ = (value);			\
		hal_atomic_cookie_t hal_atomic_cookie_ = hal_atomic_enter();	\
		__auto_type hal_atomic_previous_ = *hal_atomic_pointer_;	\
		*hal_atomic_pointer_ = hal_atomic_previous_ |			\
		    hal_atomic_value_;						\
		hal_atomic_leave(hal_atomic_cookie_);				\
		hal_atomic_previous_;						\
	})

#define hal_atomic_compare_exchange_serialized(pointer, expected, desired)	\
	({									\
		__auto_type hal_atomic_pointer_ = (pointer);			\
		__auto_type hal_atomic_expected_ = (expected);			\
		__auto_type hal_atomic_desired_ = (desired);			\
		hal_atomic_cookie_t hal_atomic_cookie_ = hal_atomic_enter();	\
		__auto_type hal_atomic_current_ = *hal_atomic_pointer_;		\
		int hal_atomic_exchanged_ =					\
		    hal_atomic_current_ == *hal_atomic_expected_;		\
		if (hal_atomic_exchanged_)					\
			*hal_atomic_pointer_ = hal_atomic_desired_;		\
		else								\
			*hal_atomic_expected_ = hal_atomic_current_;		\
		hal_atomic_leave(hal_atomic_cookie_);				\
		hal_atomic_exchanged_;						\
	})

#define hal_atomic_compare_exchange_acq_rel(pointer, expected, desired)	\
	hal_atomic_compare_exchange_serialized((pointer), (expected),	\
		 (desired))

#define hal_atomic_compare_exchange_acquire(pointer, expected, desired)	\
	hal_atomic_compare_exchange_serialized((pointer), (expected),	\
		 (desired))

#define hal_atomic_compare_exchange_release(pointer, expected, desired)	\
	hal_atomic_compare_exchange_serialized((pointer), (expected),	\
		(desired))

#define hal_atomic_fence_acquire()					\
	__asm__ volatile("" ::: "memory")

#else

/*
 * GCC and Clang compiler primitives are an implementation detail of the HAL.
 * Memory-order constants do not cross this interface into the kernel.
 */
#define hal_atomic_load_acquire(pointer)				\
	__atomic_load_n((pointer), __ATOMIC_ACQUIRE)

#define hal_atomic_load_relaxed(pointer)				\
	__atomic_load_n((pointer), __ATOMIC_RELAXED)

#define hal_atomic_store_release(pointer, value)			\
	__atomic_store_n((pointer), (value), __ATOMIC_RELEASE)

#define hal_atomic_store_relaxed(pointer, value)			\
	__atomic_store_n((pointer), (value), __ATOMIC_RELAXED)

#define hal_atomic_fetch_add_relaxed(pointer, value)			\
	__atomic_fetch_add((pointer), (value), __ATOMIC_RELAXED)

#define hal_atomic_fetch_add_release(pointer, value)			\
	__atomic_fetch_add((pointer), (value), __ATOMIC_RELEASE)

#define hal_atomic_fetch_or_release(pointer, value)			\
	__atomic_fetch_or((pointer), (value), __ATOMIC_RELEASE)

#define hal_atomic_compare_exchange_acq_rel(pointer, expected, desired)	\
	__atomic_compare_exchange_n((pointer), (expected), (desired),	\
		0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)

#define hal_atomic_compare_exchange_acquire(pointer, expected, desired) \
	__atomic_compare_exchange_n((pointer), (expected), (desired),	\
		0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)

#define hal_atomic_compare_exchange_release(pointer, expected, desired) \
	__atomic_compare_exchange_n((pointer), (expected), (desired),	\
		 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)

#define hal_atomic_fence_acquire()					\
	__atomic_thread_fence(__ATOMIC_ACQUIRE)

#endif

#endif
