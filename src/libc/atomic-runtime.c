/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library atomic runtime support.
 */

#include "syscall.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <uapi/atomic.h>
#include <uapi/syscall.h>

#if defined(HAL_ARCH_I386)
/*
 * The number of lock words that serialize the atomic objects of a process.
 * A power of two, so that an address is reduced to a word by a mask.
 */
#define ATOMIC_LOCK_COUNT	64U

/*
 * The lock word of an object is chosen by the address of its first byte
 * shifted by this many bits, so that the objects in one 16-byte block share a
 * word and an access to a part of a wider object meets the same word.
 */
#define ATOMIC_LOCK_SHIFT	4U

/*
 * How many times a waiter looks at a held word before it gives the processor
 * away.  On a uniprocessor the holder can only finish once it runs again.
 */
#define ATOMIC_SPIN_LIMIT	64U

/*
 * One lock word, alone on its cache line so that two words do not bounce
 * one line between processors.
 */
struct atomic_lock {
	volatile unsigned word;
	unsigned char padding[60];
};

/*
 * The lock words of this process.
 *
 * An i386 has XCHG but neither CMPXCHG nor XADD nor CMPXCHG8B, so every
 * operation this runtime is asked for -- a load, a store, an exchange, a
 * comparison and exchange, and the arithmetic built on them -- takes the word
 * its object's address selects, works on plain memory, and gives the word
 * back.  A word is zero while free and one while held.  The words live as
 * long as the process; fork() holds all of them across the copy, so the child
 * starts with every word free.
 *
 * The words are the process's own, so an object reached through memory that
 * another process maps is not serialized against that process.  Such an
 * object is not lock-free here, and C leaves it to the program to share only
 * lock-free atomics between processes.  The same holds for a signal handler:
 * one which uses this runtime while the thread it interrupted holds the same
 * word waits forever.
 */
static struct atomic_lock atomic_locks[ATOMIC_LOCK_COUNT] __attribute__((aligned(64)));
#endif

/*
 * GCC and Clang use the libatomic ABI when an atomic object is wider than the
 * target can update lock-free.  The C identifiers deliberately differ from
 * the reserved compiler built-in names; assembler labels provide the ABI.
 */

void zed_atomic_load(size_t size, const volatile void *object, void *result,
		     int order) __asm__("__atomic_load");

static intptr_t atomic_call(unsigned operation, const volatile void *object, void *first, void *second, size_t size);
#if defined(HAL_ARCH_I386)
static volatile unsigned *atomic_lock_word(const volatile void *object);
static void atomic_lock_take(volatile unsigned *word);
static void atomic_lock_give(volatile unsigned *word);
static void atomic_bytes_copy(volatile unsigned char *destination, const volatile unsigned char *source, size_t size);
static bool atomic_bytes_equal(const volatile unsigned char *left, const volatile unsigned char *right, size_t size);
#endif

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
	(void)atomic_call(KERN_ATOMIC_LOAD, object, result, NULL, size);
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
	(void)atomic_call(KERN_ATOMIC_STORE, object,
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
	(void)atomic_call(KERN_ATOMIC_EXCHANGE, object,
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
	function_result = atomic_call(KERN_ATOMIC_COMPARE_EXCHANGE, object, expected,
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

#define KERN_ATOMIC_WIDTH(suffix, type)                                      \
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

KERN_ATOMIC_WIDTH(1, uint8_t)
KERN_ATOMIC_WIDTH(2, uint16_t)
KERN_ATOMIC_WIDTH(4, uint32_t)
KERN_ATOMIC_WIDTH(8, uint64_t)
#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 kern_uint128_t;
KERN_ATOMIC_WIDTH(16, kern_uint128_t)
#endif

#undef KERN_ATOMIC_WIDTH

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

#if defined(HAL_ARCH_I386)
/*
 * Holds every lock word across fork().
 *
 * Another thread may hold a word in the middle of an operation when the
 * process forks.  The child has no copy of that thread, so a word copied held
 * would never be given back.  Holding all of them in the forking thread makes
 * the copy a moment at which no operation is in flight.
 */
void
__libc_atomic_fork_prepare(
	void)
{
	unsigned index;

	/* Takes the words in one order, as no operation holds two of them. */
	for (index = 0; index < ATOMIC_LOCK_COUNT; index++)
		atomic_lock_take(&atomic_locks[index].word);
}

/*
 * Gives back the lock words fork() held, in the parent.
 */
void
__libc_atomic_fork_parent(
	void)
{
	unsigned index;

	/* Lets the parent's other threads continue their operations. */
	for (index = 0; index < ATOMIC_LOCK_COUNT; index++)
		atomic_lock_give(&atomic_locks[index].word);
}

/*
 * Gives back the lock words fork() held, in the child.
 */
void
__libc_atomic_fork_child(
	void)
{
	unsigned index;

	/* Frees the child's copies, which only this thread holds. */
	for (index = 0; index < ATOMIC_LOCK_COUNT; index++)
		atomic_lock_give(&atomic_locks[index].word);
}

/*
 * Performs an atomic operation on an object under its lock word.
 *
 * first and second are the operation's operands as the kernel's interface
 * names them: the result of a load, the value of a store, the value and the
 * result of an exchange, and the expected and the desired value of a
 * comparison.  Reports whether a comparison found the expected value, and 0
 * for the other operations.
 */
static intptr_t
atomic_call(
	unsigned operation,
	const volatile void *object,
	void *first,
	void *second,
	size_t size)
{
	volatile unsigned char *bytes;
	volatile unsigned *word;
	bool equal;

	bytes = (volatile unsigned char *)(uintptr_t)object;
	equal = false;

	/* Selects the word that serializes the object. */
	word = atomic_lock_word(object);

	/* Performs the operation while no other thread of the process can touch the object. */
	atomic_lock_take(word);

	/* Applies the operation the caller named. */
	switch (operation) {
	case KERN_ATOMIC_LOAD:
		/* Copies the object out to the result. */
		atomic_bytes_copy(first, bytes, size);
		break;
	case KERN_ATOMIC_STORE:
		/* Copies the value into the object. */
		atomic_bytes_copy(bytes, first, size);
		break;
	case KERN_ATOMIC_EXCHANGE:
		/* Copies the old object out, and then the value in. */
		atomic_bytes_copy(second, bytes, size);
		atomic_bytes_copy(bytes, first, size);
		break;
	case KERN_ATOMIC_COMPARE_EXCHANGE:
		/* Stores the desired value over an expected object, or reports the object found. */
		equal = atomic_bytes_equal(bytes, first, size);
		if (equal) {
			atomic_bytes_copy(bytes, second, size);
		} else {
			atomic_bytes_copy(first, bytes, size);
		}
		break;
	default:
		/* The public entries name only the four operations above. */
		__builtin_trap();
	}

	atomic_lock_give(word);

	/* Reports a comparison that stored the desired value. */
	if (equal)
		return 1;

	/* Succeeded: a load, a store, an exchange, or a comparison that did not store. */
	return 0;
}

/* Selects the lock word that serializes an object. */
static volatile unsigned *
atomic_lock_word(
	const volatile void *object)
{
	uintptr_t index;

	/* Reduces the object's 16-byte block to a word. */
	index = ((uintptr_t)object >> ATOMIC_LOCK_SHIFT) & (ATOMIC_LOCK_COUNT - 1U);

	/* Succeeded: the object's word. */
	return &atomic_locks[index].word;
}

/* Takes a lock word, waiting while another thread holds it. */
static void
atomic_lock_take(
	volatile unsigned *word)
{
	unsigned previous;
	unsigned spins;

	/*
	 * Exchanges one into the word until the word was zero.  XCHG with a
	 * memory operand locks the bus by itself on every x86, the 386
	 * included, and orders every access around it.
	 */
	spins = 0;
	for (;;) {
		previous = 1U;
		__asm__ volatile("xchgl %0, %1"
		    : "+r"(previous), "+m"(*word)
		    :
		    : "memory");
		if (previous == 0U)
			break;

		/* Gives the processor to the holder after a short spin, so that it can finish. */
		spins++;
		if (spins < ATOMIC_SPIN_LIMIT) {
			__asm__ volatile("pause" ::: "memory");
		} else {
			(void)__syscall6(KERN_SYS_sched_yield, 0, 0, 0, 0, 0, 0);
			spins = 0;
		}
	}
}

/* Gives back a lock word this thread holds. */
static void
atomic_lock_give(
	volatile unsigned *word)
{
	unsigned previous;

	/* Clears the word with an exchange, which publishes the operation's stores before it. */
	previous = 0U;
	__asm__ volatile("xchgl %0, %1"
	    : "+r"(previous), "+m"(*word)
	    :
	    : "memory");

	/* Refuses to give back a word that was not held: the runtime's own error. */
	if (previous != 1U)
		__builtin_trap();
}

/* Copies an object's bytes while its lock word is held. */
static void
atomic_bytes_copy(
	volatile unsigned char *destination,
	const volatile unsigned char *source,
	size_t size)
{
	size_t index;

	/* Copies byte by byte, as the operands have any size and alignment. */
	for (index = 0; index < size; index++)
		destination[index] = source[index];
}

/* Compares an object with an expected value while its lock word is held. */
static bool
atomic_bytes_equal(
	const volatile unsigned char *left,
	const volatile unsigned char *right,
	size_t size)
{
	size_t index;

	/* Finds the first byte that differs. */
	for (index = 0; index < size; index++) {
		if (left[index] != right[index])
			return false;
	}

	/* Succeeded: every byte is equal. */
	return true;
}
#else
/*
 * Prepares the atomic runtime for fork().
 *
 * The kernel serializes this port's fallback, so the process holds no lock
 * word a fork could copy held.
 */
void
__libc_atomic_fork_prepare(
	void)
{
}

/*
 * Restores the atomic runtime in the parent after fork().
 */
void
__libc_atomic_fork_parent(
	void)
{
}

/*
 * Restores the atomic runtime in the child after fork().
 */
void
__libc_atomic_fork_child(
	void)
{
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
	    __syscall6(KERN_SYS_atomic, (uintptr_t)object, (uintptr_t)first,
		       (uintptr_t)second, (uintptr_t)size, operation, 0);

	/* Checks the operation result. */
	if (result < 0)
		_exit(127);

	/* Returns the computed result. */
	return result;
}
#endif
