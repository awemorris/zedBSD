/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library atomic runtime support.
 */

#include "userland/base/libc/syscall.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <zedbsd/atomic.h>
#include <zedbsd/syscall.h>

/*
 * GCC and Clang use the libatomic ABI when an atomic object is wider than the
 * target can update lock-free.  The C identifiers deliberately differ from
 * the reserved compiler built-in names; assembler labels provide the ABI.
 */

void zed_atomic_load(size_t size, const volatile void *object, void *result,
		     int order) __asm__("__atomic_load");

static intptr_t atomic_call(unsigned operation, const volatile void *object, void *first, void *second, size_t size);

/*
 * Implements the zed atomic load operation.
 */
void
zed_atomic_load(
	size_t size,
	const volatile void *object,
	void *result,
	int order)
{
	(void)order;
	(void)atomic_call(ZEDBSD_ATOMIC_LOAD, object, result, NULL, size);
}

void zed_atomic_store(size_t size, volatile void *object, const void *desired,
		      int order) __asm__("__atomic_store");

/*
 * Implements the zed atomic store operation.
 */
void
zed_atomic_store(
	size_t size,
	volatile void *object,
	const void *desired,
	int order)
{
	(void)order;
	(void)atomic_call(ZEDBSD_ATOMIC_STORE, object,
			  (void *)(uintptr_t)desired, NULL, size);
}

void zed_atomic_exchange(size_t size, volatile void *object,
			 const void *desired, void *result,
			 int order) __asm__("__atomic_exchange");

/*
 * Implements the zed atomic exchange operation.
 */
void
zed_atomic_exchange(
	size_t size,
	volatile void *object,
	const void *desired,
	void *result,
	int order)
{
	(void)order;
	(void)atomic_call(ZEDBSD_ATOMIC_EXCHANGE, object,
			  (void *)(uintptr_t)desired, result, size);
}

bool zed_atomic_compare_exchange(
    size_t size, volatile void *object, void *expected, const void *desired,
    int success_order, int failure_order) __asm__("__atomic_compare_exchange");

/*
 * Implements the zed atomic compare exchange operation.
 */
bool
zed_atomic_compare_exchange(
	size_t size,
	volatile void *object,
	void *expected,
	const void *desired,
	int success_order,
	int failure_order)
{
	bool function_result;

	(void)success_order;
	(void)failure_order;

	/* Computes the function result. */
	function_result = atomic_call(ZEDBSD_ATOMIC_COMPARE_EXCHANGE, object, expected,
			   (void *)(uintptr_t)desired, size) != 0;

	/* Returns the computed result. */
	return function_result;
}

bool zed_atomic_is_lock_free(size_t size, const volatile void *object) __asm__(
    "__atomic_is_lock_free");

/*
 * Implements the zed atomic is lock free operation.
 */
bool
zed_atomic_is_lock_free(
	size_t size,
	const volatile void *object)
{
	uintptr_t address;

	address = (uintptr_t)object;

	/* Dispatch the selected operation case. */
	switch (size) {
	case 1:
		/* Returns the computed result. */
		return __GCC_ATOMIC_CHAR_LOCK_FREE == 2;
	case 2:
		/* Returns the computed result. */
		return __GCC_ATOMIC_SHORT_LOCK_FREE == 2 && (address & 1U) == 0;
	case 4:
		/* Returns the computed result. */
		return __GCC_ATOMIC_INT_LOCK_FREE == 2 && (address & 3U) == 0;
	case 8:
		/* Returns the computed result. */
		return __GCC_ATOMIC_LLONG_LOCK_FREE == 2 && (address & 7U) == 0;
#if defined(__SIZEOF_INT128__) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)
	case 16:
		/* Returns the computed result. */
		return (address & 15U) == 0;
#endif
	default:
		/* Reports operation failure. */
		return false;
	}
}

#define ZEDBSD_ATOMIC_WIDTH(suffix, type)                                      \
	type zed_atomic_load_##suffix(const volatile void *,                   \
				      int) __asm__("__atomic_load_" #suffix);  \
	type zed_atomic_load_##suffix(const volatile void *object, int order)  \
	{                                                                      \
		type result;                                                   \
		zed_atomic_load(sizeof(result), object, &result, order);       \
		return result;                                                 \
	}                                                                      \
	void zed_atomic_store_##suffix(volatile void *, type, int) __asm__(    \
	    "__atomic_store_" #suffix);                                        \
	void zed_atomic_store_##suffix(volatile void *object, type desired,    \
				       int order)                              \
	{                                                                      \
		zed_atomic_store(sizeof(desired), object, &desired, order);    \
	}                                                                      \
	type zed_atomic_exchange_##suffix(volatile void *, type, int) __asm__( \
	    "__atomic_exchange_" #suffix);                                     \
	type zed_atomic_exchange_##suffix(volatile void *object, type desired, \
					  int order)                           \
	{                                                                      \
		type result;                                                   \
		zed_atomic_exchange(sizeof(desired), object, &desired,         \
				    &result, order);                           \
		return result;                                                 \
	}                                                                      \
	bool zed_atomic_compare_exchange_##suffix(                             \
	    volatile void *, type *, type, int,                                \
	    int) __asm__("__atomic_compare_exchange_" #suffix);                \
	bool zed_atomic_compare_exchange_##suffix(                             \
	    volatile void *object, type *expected, type desired,               \
	    int success_order, int failure_order)                              \
	{                                                                      \
		bool result;                                                   \
		                                                               \
		/* Performs the requested atomic comparison. */                 \
		result = zed_atomic_compare_exchange(                           \
		    sizeof(desired), object, expected, &desired,               \
		    success_order, failure_order);                             \
		                                                               \
		/* Returns the comparison result. */                            \
		return result;                                                 \
	}                                                                      \
	type zed_atomic_fetch_add_##suffix(                                    \
	    volatile void *, type,                                             \
	    int) __asm__("__atomic_fetch_add_" #suffix);                       \
	type zed_atomic_fetch_add_##suffix(volatile void *object,              \
					   type operand, int order)            \
	{                                                                      \
		type expected = zed_atomic_load_##suffix(object, order);       \
		type original;                                                 \
		do {                                                           \
			original = expected;                                   \
		} while (!zed_atomic_compare_exchange_##suffix(                \
		    object, &expected, (type)(original + operand), order,      \
		    order));                                                   \
		return original;                                               \
	}                                                                      \
	type zed_atomic_add_fetch_##suffix(                                    \
	    volatile void *, type,                                             \
	    int) __asm__("__atomic_add_fetch_" #suffix);                       \
	type zed_atomic_add_fetch_##suffix(volatile void *object,              \
					   type operand, int order)            \
	{                                                                      \
		return (type)(zed_atomic_fetch_add_##suffix(object, operand,   \
							    order) +           \
			      operand);                                        \
	}                                                                      \
	type zed_atomic_fetch_sub_##suffix(                                    \
	    volatile void *, type,                                             \
	    int) __asm__("__atomic_fetch_sub_" #suffix);                       \
	type zed_atomic_fetch_sub_##suffix(volatile void *object,              \
					   type operand, int order)            \
	{                                                                      \
		return zed_atomic_fetch_add_##suffix(                          \
		    object, (type)(0 - operand), order);                       \
	}                                                                      \
	type zed_atomic_sub_fetch_##suffix(                                    \
	    volatile void *, type,                                             \
	    int) __asm__("__atomic_sub_fetch_" #suffix);                       \
	type zed_atomic_sub_fetch_##suffix(volatile void *object,              \
					   type operand, int order)            \
	{                                                                      \
		return (type)(zed_atomic_fetch_sub_##suffix(object, operand,   \
							    order) -           \
			      operand);                                        \
	}                                                                      \
	type zed_atomic_fetch_and_##suffix(                                    \
	    volatile void *, type,                                             \
	    int) __asm__("__atomic_fetch_and_" #suffix);                       \
	type zed_atomic_fetch_and_##suffix(volatile void *object,              \
					   type operand, int order)            \
	{                                                                      \
		type expected = zed_atomic_load_##suffix(object, order);       \
		type original;                                                 \
		do {                                                           \
			original = expected;                                   \
		} while (!zed_atomic_compare_exchange_##suffix(                \
		    object, &expected, (type)(original & operand), order,      \
		    order));                                                   \
		return original;                                               \
	}                                                                      \
	type zed_atomic_and_fetch_##suffix(                                    \
	    volatile void *, type,                                             \
	    int) __asm__("__atomic_and_fetch_" #suffix);                       \
	type zed_atomic_and_fetch_##suffix(volatile void *object,              \
					   type operand, int order)            \
	{                                                                      \
		return (type)(zed_atomic_fetch_and_##suffix(object, operand,   \
							    order) &           \
			      operand);                                        \
	}                                                                      \
	type zed_atomic_fetch_or_##suffix(volatile void *, type, int) __asm__( \
	    "__atomic_fetch_or_" #suffix);                                     \
	type zed_atomic_fetch_or_##suffix(volatile void *object, type operand, \
					  int order)                           \
	{                                                                      \
		type expected = zed_atomic_load_##suffix(object, order);       \
		type original;                                                 \
		do {                                                           \
			original = expected;                                   \
		} while (!zed_atomic_compare_exchange_##suffix(                \
		    object, &expected, (type)(original | operand), order,      \
		    order));                                                   \
		return original;                                               \
	}                                                                      \
	type zed_atomic_or_fetch_##suffix(volatile void *, type, int) __asm__( \
	    "__atomic_or_fetch_" #suffix);                                     \
	type zed_atomic_or_fetch_##suffix(volatile void *object, type operand, \
					  int order)                           \
	{                                                                      \
		return (type)(zed_atomic_fetch_or_##suffix(object, operand,    \
							   order) |            \
			      operand);                                        \
	}                                                                      \
	type zed_atomic_fetch_xor_##suffix(                                    \
	    volatile void *, type,                                             \
	    int) __asm__("__atomic_fetch_xor_" #suffix);                       \
	type zed_atomic_fetch_xor_##suffix(volatile void *object,              \
					   type operand, int order)            \
	{                                                                      \
		type expected = zed_atomic_load_##suffix(object, order);       \
		type original;                                                 \
		do {                                                           \
			original = expected;                                   \
		} while (!zed_atomic_compare_exchange_##suffix(                \
		    object, &expected, (type)(original ^ operand), order,      \
		    order));                                                   \
		return original;                                               \
	}                                                                      \
	type zed_atomic_xor_fetch_##suffix(                                    \
	    volatile void *, type,                                             \
	    int) __asm__("__atomic_xor_fetch_" #suffix);                       \
	type zed_atomic_xor_fetch_##suffix(volatile void *object,              \
					   type operand, int order)            \
	{                                                                      \
		return (type)(zed_atomic_fetch_xor_##suffix(object, operand,   \
							    order) ^           \
			      operand);                                        \
	}                                                                      \
	type zed_atomic_fetch_nand_##suffix(                                   \
	    volatile void *, type,                                             \
	    int) __asm__("__atomic_fetch_nand_" #suffix);                      \
	type zed_atomic_fetch_nand_##suffix(volatile void *object,             \
					    type operand, int order)           \
	{                                                                      \
		type expected = zed_atomic_load_##suffix(object, order);       \
		type original;                                                 \
		do {                                                           \
			original = expected;                                   \
		} while (!zed_atomic_compare_exchange_##suffix(                \
		    object, &expected, (type) ~(original & operand), order,    \
		    order));                                                   \
		return original;                                               \
	}                                                                      \
	type zed_atomic_nand_fetch_##suffix(                                   \
	    volatile void *, type,                                             \
	    int) __asm__("__atomic_nand_fetch_" #suffix);                      \
	type zed_atomic_nand_fetch_##suffix(volatile void *object,             \
					    type operand, int order)           \
	{                                                                      \
		return (type) ~(                                               \
		    zed_atomic_fetch_nand_##suffix(object, operand, order) &   \
		    operand);                                                  \
	}

ZEDBSD_ATOMIC_WIDTH(1, uint8_t)
ZEDBSD_ATOMIC_WIDTH(2, uint16_t)
ZEDBSD_ATOMIC_WIDTH(4, uint32_t)
ZEDBSD_ATOMIC_WIDTH(8, uint64_t)
#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 zedbsd_uint128_t;
ZEDBSD_ATOMIC_WIDTH(16, zedbsd_uint128_t)
#endif

#undef ZEDBSD_ATOMIC_WIDTH

bool zed_atomic_test_and_set_1(volatile void *object,
			       int order) __asm__("__atomic_test_and_set_1");

/*
 * Implements the zed atomic test and set 1 operation.
 */
bool
zed_atomic_test_and_set_1(
	volatile void *object,
	int order)
{
	bool function_result;

	/* Computes the function result. */
	function_result = zed_atomic_exchange_1(object, 1U, order) != 0;

	/* Returns the computed result. */
	return function_result;
}

/* Invokes the kernel atomic operation. */
static intptr_t
atomic_call(
	unsigned operation,
	const volatile void *object,
	void *first,
	void *second,
	size_t size)
{
	intptr_t result;

	result =
	    __syscall6(ZEDBSD_SYS_atomic, (uintptr_t)object, (uintptr_t)first,
		       (uintptr_t)second, (uintptr_t)size, operation, 0);

	/* Checks the operation result. */
	if (result < 0)
		_exit(127);

	/* Returns the computed result. */
	return result;
}
