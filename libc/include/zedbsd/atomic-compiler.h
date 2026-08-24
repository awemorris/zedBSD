/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_ATOMIC_COMPILER_H
#define ZEDBSD_ATOMIC_COMPILER_H

/* Compiler-specific implementation details for the ISO C atomic facade. */
#define __ZEDBSD_ATOMIC_BOOL_LOCK_FREE	__GCC_ATOMIC_BOOL_LOCK_FREE
#define __ZEDBSD_ATOMIC_CHAR_LOCK_FREE	__GCC_ATOMIC_CHAR_LOCK_FREE
#define __ZEDBSD_ATOMIC_CHAR16_T_LOCK_FREE	__GCC_ATOMIC_CHAR16_T_LOCK_FREE
#define __ZEDBSD_ATOMIC_CHAR32_T_LOCK_FREE	__GCC_ATOMIC_CHAR32_T_LOCK_FREE
#define __ZEDBSD_ATOMIC_WCHAR_T_LOCK_FREE	__GCC_ATOMIC_WCHAR_T_LOCK_FREE
#define __ZEDBSD_ATOMIC_SHORT_LOCK_FREE	__GCC_ATOMIC_SHORT_LOCK_FREE
#define __ZEDBSD_ATOMIC_INT_LOCK_FREE	__GCC_ATOMIC_INT_LOCK_FREE
#define __ZEDBSD_ATOMIC_LONG_LOCK_FREE	__GCC_ATOMIC_LONG_LOCK_FREE
#define __ZEDBSD_ATOMIC_LLONG_LOCK_FREE	__GCC_ATOMIC_LLONG_LOCK_FREE
#define __ZEDBSD_ATOMIC_POINTER_LOCK_FREE	__GCC_ATOMIC_POINTER_LOCK_FREE

#define __ZEDBSD_MEMORY_ORDER_RELAXED	__ATOMIC_RELAXED
#define __ZEDBSD_MEMORY_ORDER_CONSUME	__ATOMIC_CONSUME
#define __ZEDBSD_MEMORY_ORDER_ACQUIRE	__ATOMIC_ACQUIRE
#define __ZEDBSD_MEMORY_ORDER_RELEASE	__ATOMIC_RELEASE
#define __ZEDBSD_MEMORY_ORDER_ACQ_REL	__ATOMIC_ACQ_REL
#define __ZEDBSD_MEMORY_ORDER_SEQ_CST	__ATOMIC_SEQ_CST

#define __zedbsd_atomic_store(object, desired, order) \
	do { \
		__typeof__(*(object)) __zedbsd_atomic_desired = (desired); \
		__atomic_store((object), &__zedbsd_atomic_desired, (order)); \
	} while (0)

#define __zedbsd_atomic_load(object, order) \
	({ \
		__typeof__(*(object)) __zedbsd_atomic_result; \
		__atomic_load((object), &__zedbsd_atomic_result, (order)); \
		__zedbsd_atomic_result; \
	})

#define __zedbsd_atomic_exchange(object, desired, order) \
	({ \
		__typeof__(*(object)) __zedbsd_atomic_desired = (desired); \
		__typeof__(*(object)) __zedbsd_atomic_result; \
		__atomic_exchange((object), &__zedbsd_atomic_desired, \
		    &__zedbsd_atomic_result, (order)); \
		__zedbsd_atomic_result; \
	})

#define __zedbsd_atomic_compare_exchange(object, expected, desired, weak, \
	success, failure) \
	({ \
		__typeof__(*(object)) __zedbsd_atomic_desired = (desired); \
		__atomic_compare_exchange((object), (expected), \
		    &__zedbsd_atomic_desired, (weak), (success), (failure)); \
	})

#define __zedbsd_atomic_is_lock_free(object) \
	__atomic_is_lock_free(sizeof(*(object)), (object))
#define __zedbsd_atomic_thread_fence(order)	__atomic_thread_fence(order)
#define __zedbsd_atomic_signal_fence(order)	__atomic_signal_fence(order)
#define __zedbsd_atomic_fetch_add(object, operand, order) \
	__atomic_fetch_add((object), (operand), (order))
#define __zedbsd_atomic_fetch_sub(object, operand, order) \
	__atomic_fetch_sub((object), (operand), (order))
#define __zedbsd_atomic_fetch_or(object, operand, order) \
	__atomic_fetch_or((object), (operand), (order))
#define __zedbsd_atomic_fetch_xor(object, operand, order) \
	__atomic_fetch_xor((object), (operand), (order))
#define __zedbsd_atomic_fetch_and(object, operand, order) \
	__atomic_fetch_and((object), (operand), (order))
#define __zedbsd_atomic_flag_test_and_set(object, order) \
	__atomic_test_and_set((object), (order))
#define __zedbsd_atomic_flag_clear(object, order)	__atomic_clear((object), (order))

#endif
