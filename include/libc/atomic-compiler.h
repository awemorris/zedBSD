/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * XXX:
 *  - Describe what this is for.
 *  - Move this file under include/libc/
 */

#ifndef LIBC_ATOMIC_COMPILER_H
#define LIBC_ATOMIC_COMPILER_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * XXX: Rename these __ZEDBSD_* to __LIBC_*
 */
/* Compiler-specific implementation details for the ISO C atomic facade. */
#define __ZEDBSD_ATOMIC_BOOL_LOCK_FREE		__GCC_ATOMIC_BOOL_LOCK_FREE
#define __ZEDBSD_ATOMIC_CHAR_LOCK_FREE		__GCC_ATOMIC_CHAR_LOCK_FREE
#define __ZEDBSD_ATOMIC_CHAR16_T_LOCK_FREE	__GCC_ATOMIC_CHAR16_T_LOCK_FREE
#define __ZEDBSD_ATOMIC_CHAR32_T_LOCK_FREE	__GCC_ATOMIC_CHAR32_T_LOCK_FREE
#define __ZEDBSD_ATOMIC_WCHAR_T_LOCK_FREE	__GCC_ATOMIC_WCHAR_T_LOCK_FREE
#define __ZEDBSD_ATOMIC_SHORT_LOCK_FREE		__GCC_ATOMIC_SHORT_LOCK_FREE
#define __ZEDBSD_ATOMIC_INT_LOCK_FREE		__GCC_ATOMIC_INT_LOCK_FREE
#define __ZEDBSD_ATOMIC_LONG_LOCK_FREE		__GCC_ATOMIC_LONG_LOCK_FREE
#define __ZEDBSD_ATOMIC_LLONG_LOCK_FREE		__GCC_ATOMIC_LLONG_LOCK_FREE
#define __ZEDBSD_ATOMIC_POINTER_LOCK_FREE	__GCC_ATOMIC_POINTER_LOCK_FREE

/*
 * XXX: Rename these __ZEDBSD_* to __LIBC_*
 */
#define __ZEDBSD_MEMORY_ORDER_RELAXED		__ATOMIC_RELAXED
#define __ZEDBSD_MEMORY_ORDER_CONSUME		__ATOMIC_CONSUME
#define __ZEDBSD_MEMORY_ORDER_ACQUIRE		__ATOMIC_ACQUIRE
#define __ZEDBSD_MEMORY_ORDER_RELEASE		__ATOMIC_RELEASE
#define __ZEDBSD_MEMORY_ORDER_ACQ_REL		__ATOMIC_ACQ_REL
#define __ZEDBSD_MEMORY_ORDER_SEQ_CST		__ATOMIC_SEQ_CST

/*
 * XXX: Rename these __ZEDBSD_* to __LIBC_*
 */

/*
 * The ISO C atomic operations act on _Atomic-qualified objects.  Clang refuses
 * such an object to the __atomic_* builtins, which describe a plain object
 * accessed atomically, and offers __c11_atomic_* for the qualified form
 * instead.  The two families are not interchangeable: using the former here
 * made every standard use of <stdatomic.h> fail to compile.
 */

#define __zedbsd_atomic_store(object, desired, order) \
	__c11_atomic_store((object), (desired), (order))

#define __zedbsd_atomic_load(object, order) \
	__c11_atomic_load((object), (order))

#define __zedbsd_atomic_exchange(object, desired, order) \
	__c11_atomic_exchange((object), (desired), (order))

#define __zedbsd_atomic_compare_exchange(object, expected, desired, weak, \
	success, failure) \
	((weak) \
	    ? __c11_atomic_compare_exchange_weak((object), (expected), \
		  (desired), (success), (failure)) \
	    : __c11_atomic_compare_exchange_strong((object), (expected), \
		  (desired), (success), (failure)))

#define __zedbsd_atomic_is_lock_free(object) \
	__c11_atomic_is_lock_free(sizeof(*(object)))
#define __zedbsd_atomic_thread_fence(order)			__c11_atomic_thread_fence(order)
#define __zedbsd_atomic_signal_fence(order)			__c11_atomic_signal_fence(order)
#define __zedbsd_atomic_fetch_add(object, operand, order)	__c11_atomic_fetch_add((object), (operand), (order))
#define __zedbsd_atomic_fetch_sub(object, operand, order) 	__c11_atomic_fetch_sub((object), (operand), (order))
#define __zedbsd_atomic_fetch_or(object, operand, order)	__c11_atomic_fetch_or((object), (operand), (order))
#define __zedbsd_atomic_fetch_xor(object, operand, order)	__c11_atomic_fetch_xor((object), (operand), (order))
#define __zedbsd_atomic_fetch_and(object, operand, order) 	__c11_atomic_fetch_and((object), (operand), (order))
#define __zedbsd_atomic_flag_test_and_set(object, order) 	__c11_atomic_exchange((object), 1, (order))
#define __zedbsd_atomic_flag_clear(object, order)		__c11_atomic_store((object), 0, (order))

#ifdef __cplusplus
}
#endif

#endif
