/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_STDATOMIC_H
#define ZEDBSD_STDATOMIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zedbsd/atomic-compiler.h>

#define ATOMIC_BOOL_LOCK_FREE	__ZEDBSD_ATOMIC_BOOL_LOCK_FREE
#define ATOMIC_CHAR_LOCK_FREE	__ZEDBSD_ATOMIC_CHAR_LOCK_FREE
#define ATOMIC_CHAR16_T_LOCK_FREE	__ZEDBSD_ATOMIC_CHAR16_T_LOCK_FREE
#define ATOMIC_CHAR32_T_LOCK_FREE	__ZEDBSD_ATOMIC_CHAR32_T_LOCK_FREE
#define ATOMIC_WCHAR_T_LOCK_FREE	__ZEDBSD_ATOMIC_WCHAR_T_LOCK_FREE
#define ATOMIC_SHORT_LOCK_FREE	__ZEDBSD_ATOMIC_SHORT_LOCK_FREE
#define ATOMIC_INT_LOCK_FREE	__ZEDBSD_ATOMIC_INT_LOCK_FREE
#define ATOMIC_LONG_LOCK_FREE	__ZEDBSD_ATOMIC_LONG_LOCK_FREE
#define ATOMIC_LLONG_LOCK_FREE	__ZEDBSD_ATOMIC_LLONG_LOCK_FREE
#define ATOMIC_POINTER_LOCK_FREE	__ZEDBSD_ATOMIC_POINTER_LOCK_FREE

typedef enum memory_order {
	memory_order_relaxed = __ZEDBSD_MEMORY_ORDER_RELAXED,
	memory_order_consume = __ZEDBSD_MEMORY_ORDER_CONSUME,
	memory_order_acquire = __ZEDBSD_MEMORY_ORDER_ACQUIRE,
	memory_order_release = __ZEDBSD_MEMORY_ORDER_RELEASE,
	memory_order_acq_rel = __ZEDBSD_MEMORY_ORDER_ACQ_REL,
	memory_order_seq_cst = __ZEDBSD_MEMORY_ORDER_SEQ_CST
} memory_order;

#define ATOMIC_VAR_INIT(value)	(value)

#define atomic_init(object, desired) \
	__zedbsd_atomic_store((object), (desired), memory_order_relaxed)
#define kill_dependency(value)	(value)
#define atomic_thread_fence(order)	__zedbsd_atomic_thread_fence(order)
#define atomic_signal_fence(order)	__zedbsd_atomic_signal_fence(order)
#define atomic_is_lock_free(object) \
	__zedbsd_atomic_is_lock_free(object)

#define atomic_store_explicit(object, desired, order) \
	__zedbsd_atomic_store((object), (desired), (order))
#define atomic_store(object, desired) \
	atomic_store_explicit((object), (desired), memory_order_seq_cst)
#define atomic_load_explicit(object, order) \
	__zedbsd_atomic_load((object), (order))
#define atomic_load(object) \
	atomic_load_explicit((object), memory_order_seq_cst)
#define atomic_exchange_explicit(object, desired, order) \
	__zedbsd_atomic_exchange((object), (desired), (order))
#define atomic_exchange(object, desired) \
	atomic_exchange_explicit((object), (desired), memory_order_seq_cst)
#define atomic_compare_exchange_strong_explicit(object, expected, desired, \
	    success, failure) \
	__zedbsd_atomic_compare_exchange((object), (expected), (desired), false, \
	    (success), (failure))
#define atomic_compare_exchange_weak_explicit(object, expected, desired, \
	    success, failure) \
	__zedbsd_atomic_compare_exchange((object), (expected), (desired), true, \
	    (success), (failure))
#define atomic_compare_exchange_strong(object, expected, desired) \
	atomic_compare_exchange_strong_explicit((object), (expected), (desired), \
	    memory_order_seq_cst, memory_order_seq_cst)
#define atomic_compare_exchange_weak(object, expected, desired) \
	atomic_compare_exchange_weak_explicit((object), (expected), (desired), \
	    memory_order_seq_cst, memory_order_seq_cst)

#define atomic_fetch_add_explicit(object, operand, order) \
	__zedbsd_atomic_fetch_add((object), (operand), (order))
#define atomic_fetch_sub_explicit(object, operand, order) \
	__zedbsd_atomic_fetch_sub((object), (operand), (order))
#define atomic_fetch_or_explicit(object, operand, order) \
	__zedbsd_atomic_fetch_or((object), (operand), (order))
#define atomic_fetch_xor_explicit(object, operand, order) \
	__zedbsd_atomic_fetch_xor((object), (operand), (order))
#define atomic_fetch_and_explicit(object, operand, order) \
	__zedbsd_atomic_fetch_and((object), (operand), (order))
#define atomic_fetch_add(object, operand) \
	atomic_fetch_add_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_sub(object, operand) \
	atomic_fetch_sub_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_or(object, operand) \
	atomic_fetch_or_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_xor(object, operand) \
	atomic_fetch_xor_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_and(object, operand) \
	atomic_fetch_and_explicit((object), (operand), memory_order_seq_cst)

#define ZEDBSD_ATOMIC_TYPE(name, type)	typedef _Atomic(type) name
ZEDBSD_ATOMIC_TYPE(atomic_bool, _Bool);
ZEDBSD_ATOMIC_TYPE(atomic_char, char);
ZEDBSD_ATOMIC_TYPE(atomic_schar, signed char);
ZEDBSD_ATOMIC_TYPE(atomic_uchar, unsigned char);
ZEDBSD_ATOMIC_TYPE(atomic_short, short);
ZEDBSD_ATOMIC_TYPE(atomic_ushort, unsigned short);
ZEDBSD_ATOMIC_TYPE(atomic_int, int);
ZEDBSD_ATOMIC_TYPE(atomic_uint, unsigned int);
ZEDBSD_ATOMIC_TYPE(atomic_long, long);
ZEDBSD_ATOMIC_TYPE(atomic_ulong, unsigned long);
ZEDBSD_ATOMIC_TYPE(atomic_llong, long long);
ZEDBSD_ATOMIC_TYPE(atomic_ullong, unsigned long long);
ZEDBSD_ATOMIC_TYPE(atomic_char16_t, __CHAR16_TYPE__);
ZEDBSD_ATOMIC_TYPE(atomic_char32_t, __CHAR32_TYPE__);
ZEDBSD_ATOMIC_TYPE(atomic_wchar_t, __WCHAR_TYPE__);
ZEDBSD_ATOMIC_TYPE(atomic_int_least8_t, int_least8_t);
ZEDBSD_ATOMIC_TYPE(atomic_uint_least8_t, uint_least8_t);
ZEDBSD_ATOMIC_TYPE(atomic_int_least16_t, int_least16_t);
ZEDBSD_ATOMIC_TYPE(atomic_uint_least16_t, uint_least16_t);
ZEDBSD_ATOMIC_TYPE(atomic_int_least32_t, int_least32_t);
ZEDBSD_ATOMIC_TYPE(atomic_uint_least32_t, uint_least32_t);
ZEDBSD_ATOMIC_TYPE(atomic_int_least64_t, int_least64_t);
ZEDBSD_ATOMIC_TYPE(atomic_uint_least64_t, uint_least64_t);
ZEDBSD_ATOMIC_TYPE(atomic_int_fast8_t, int_fast8_t);
ZEDBSD_ATOMIC_TYPE(atomic_uint_fast8_t, uint_fast8_t);
ZEDBSD_ATOMIC_TYPE(atomic_int_fast16_t, int_fast16_t);
ZEDBSD_ATOMIC_TYPE(atomic_uint_fast16_t, uint_fast16_t);
ZEDBSD_ATOMIC_TYPE(atomic_int_fast32_t, int_fast32_t);
ZEDBSD_ATOMIC_TYPE(atomic_uint_fast32_t, uint_fast32_t);
ZEDBSD_ATOMIC_TYPE(atomic_int_fast64_t, int_fast64_t);
ZEDBSD_ATOMIC_TYPE(atomic_uint_fast64_t, uint_fast64_t);
ZEDBSD_ATOMIC_TYPE(atomic_intptr_t, intptr_t);
ZEDBSD_ATOMIC_TYPE(atomic_uintptr_t, uintptr_t);
ZEDBSD_ATOMIC_TYPE(atomic_size_t, size_t);
ZEDBSD_ATOMIC_TYPE(atomic_ptrdiff_t, ptrdiff_t);
ZEDBSD_ATOMIC_TYPE(atomic_intmax_t, intmax_t);
ZEDBSD_ATOMIC_TYPE(atomic_uintmax_t, uintmax_t);
#undef ZEDBSD_ATOMIC_TYPE

typedef struct atomic_flag {
	_Atomic _Bool value;
} atomic_flag;

#define ATOMIC_FLAG_INIT	{ false }
#define atomic_flag_test_and_set_explicit(object, order) \
	__zedbsd_atomic_flag_test_and_set(&(object)->value, (order))
#define atomic_flag_test_and_set(object) \
	atomic_flag_test_and_set_explicit((object), memory_order_seq_cst)
#define atomic_flag_clear_explicit(object, order) \
	__zedbsd_atomic_flag_clear(&(object)->value, (order))
#define atomic_flag_clear(object) \
	atomic_flag_clear_explicit((object), memory_order_seq_cst)

#endif
