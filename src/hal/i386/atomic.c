/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 atomic-operation implementation.
 */

#include <hal/hal.h>

#include "defs.h"

/*
 * The serialization region's entry points, declared as the macros of
 * <hal/atomic.h> declare them, so that a definition which drifts from the
 * macros does not compile.
 */
HAL_ATOMIC_ENTER_DECLARE();

#if !defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
/*
 * The compiler's atomic runtime, for a kernel built for the i386 itself.
 *
 * The i386 has no CMPXCHG or XADD, so the compiler turns every atomic
 * operation of the kernel -- the loads and the stores included -- into a call
 * to one of these functions (the libatomic interface).  Each one does its
 * work inside the serialization region that the HAL's own atomic macros use,
 * so the two kinds of operation exclude each other, and the region's IRQ mask
 * keeps an interrupt handler out of an operation it interrupted.  The memory
 * order arguments need nothing more: entering and leaving the region are full
 * barriers.
 *
 * Only the functions the kernel calls are here; a new one is added when the
 * kernel's link names it as undefined.  The C names differ from the names the
 * compiler reserves, and assembler labels give the interface names.
 */
uint32_t hal_atomic_runtime_load_4(const volatile void *object, int order) __asm__("__atomic_load_4");
void hal_atomic_runtime_store_4(volatile void *object, uint32_t value, int order) __asm__("__atomic_store_4");
uint32_t hal_atomic_runtime_exchange_4(volatile void *object, uint32_t value, int order) __asm__("__atomic_exchange_4");
bool hal_atomic_runtime_compare_exchange_4(volatile void *object, void *expected, uint32_t desired, int success_order, int failure_order) __asm__("__atomic_compare_exchange_4");
uint32_t hal_atomic_runtime_fetch_add_4(volatile void *object, uint32_t operand, int order) __asm__("__atomic_fetch_add_4");
uint32_t hal_atomic_runtime_fetch_sub_4(volatile void *object, uint32_t operand, int order) __asm__("__atomic_fetch_sub_4");
uint32_t hal_atomic_runtime_fetch_or_4(volatile void *object, uint32_t operand, int order) __asm__("__atomic_fetch_or_4");
void hal_atomic_runtime_load(size_t size, const volatile void *object, void *result, int order) __asm__("__atomic_load");
void hal_atomic_runtime_store(size_t size, volatile void *object, const void *value, int order) __asm__("__atomic_store");
void hal_atomic_runtime_exchange(size_t size, volatile void *object, const void *value, void *result, int order) __asm__("__atomic_exchange");
bool hal_atomic_runtime_compare_exchange(size_t size, volatile void *object, void *expected, const void *desired, int success_order, int failure_order) __asm__("__atomic_compare_exchange");
uint8_t hal_atomic_runtime_load_1(const volatile void *object, int order) __asm__("__atomic_load_1");
void hal_atomic_runtime_store_1(volatile void *object, uint8_t value, int order) __asm__("__atomic_store_1");
uint16_t hal_atomic_runtime_load_2(const volatile void *object, int order) __asm__("__atomic_load_2");
void hal_atomic_runtime_store_2(volatile void *object, uint16_t value, int order) __asm__("__atomic_store_2");
uint64_t hal_atomic_runtime_load_8(const volatile void *object, int order) __asm__("__atomic_load_8");
void hal_atomic_runtime_store_8(volatile void *object, uint64_t value, int order) __asm__("__atomic_store_8");
uint64_t hal_atomic_runtime_fetch_add_8(volatile void *object, uint64_t operand, int order) __asm__("__atomic_fetch_add_8");

static void runtime_bytes_copy(volatile uint8_t *destination, const volatile uint8_t *source, size_t size);
static bool runtime_bytes_equal(const volatile uint8_t *left, const volatile uint8_t *right, size_t size);
#endif

/*
 * An i386 has XCHG but no CMPXCHG.  This guard serializes the uncommon
 * read-modify-write and 64-bit operations across CPUs.  IRQs are disabled
 * before acquisition so an interrupt cannot recursively wait on the guard
 * held by the interrupted context.
 */
static volatile unsigned atomic_guard;

/*
 * Acquires the i386 atomic-operation guard.
 */
uintptr_t
hal_atomic_enter(
	void)
{
	uintptr_t cookie;
	bool enabled;
	bool acquired;

	/* Preserves the caller's interrupt state while acquiring the guard. */
	enabled = hal_irq_disable();
	cookie = 0U;
	if (enabled)
		cookie = 1U;

	/* Waits until this CPU owns the atomic-operation guard. */
	for (;;) {
		acquired = hal_atomic_uint_try_acquire(&atomic_guard);
		if (acquired)
			break;
		hal_atomic_relax();
	}

	/* Returns the saved interrupt-state cookie. */
	return cookie;
}

/*
 * Releases the i386 atomic-operation guard.
 */
void
hal_atomic_leave(
	uintptr_t cookie)
{
	unsigned previous;

	/* Releases the guard and records its prior ownership state. */
	previous = 0U;
	__asm__ volatile("xchgl %0, %1"
	    : "+r"(previous), "+m"(atomic_guard)
	    :
	    : "memory");

	/* Rejects a release by a CPU which did not own the guard. */
	if (previous != 1U)
		__builtin_trap();

	/* Restores interrupts only when they were enabled on entry. */
	if (cookie != 0U)
		hal_irq_enable();
}

#if !defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_4)
/*
 * Loads a 4-byte atomic object.
 */
uint32_t
hal_atomic_runtime_load_4(
	const volatile void *object,
	int order)
{
	const volatile uint32_t *word;
	uintptr_t cookie;
	uint32_t value;

	UNUSED_PARAMETER(order);

	word = object;

	/* Reads the object while no other atomic operation can touch it. */
	cookie = hal_atomic_enter();
	value = *word;
	hal_atomic_leave(cookie);

	/* Reports the value the object held. */
	return value;
}

/*
 * Stores a 4-byte atomic object.
 */
void
hal_atomic_runtime_store_4(
	volatile void *object,
	uint32_t value,
	int order)
{
	volatile uint32_t *word;
	uintptr_t cookie;

	UNUSED_PARAMETER(order);

	word = object;

	/* Writes the object while no other atomic operation can touch it. */
	cookie = hal_atomic_enter();
	*word = value;
	hal_atomic_leave(cookie);
}

/*
 * Exchanges a 4-byte atomic object with a value.
 */
uint32_t
hal_atomic_runtime_exchange_4(
	volatile void *object,
	uint32_t value,
	int order)
{
	volatile uint32_t *word;
	uintptr_t cookie;
	uint32_t previous;

	UNUSED_PARAMETER(order);

	word = object;

	/* Replaces the object and keeps what it held, as one operation. */
	cookie = hal_atomic_enter();
	previous = *word;
	*word = value;
	hal_atomic_leave(cookie);

	/* Reports the value the object held before. */
	return previous;
}

/*
 * Stores a value into a 4-byte atomic object that holds an expected value.
 *
 * When the object holds another value, that value is written back to the
 * expected value instead.
 */
bool
hal_atomic_runtime_compare_exchange_4(
	volatile void *object,
	void *expected,
	uint32_t desired,
	int success_order,
	int failure_order)
{
	volatile uint32_t *word;
	uint32_t *expected_word;
	uintptr_t cookie;
	uint32_t current;
	bool exchanged;

	UNUSED_PARAMETER(success_order);
	UNUSED_PARAMETER(failure_order);

	word = object;
	expected_word = expected;

	/* Compares and, on a match, stores as one operation. */
	cookie = hal_atomic_enter();
	current = *word;
	exchanged = false;
	if (current == *expected_word) {
		*word = desired;
		exchanged = true;
	}
	hal_atomic_leave(cookie);

	/* Hands the value found back to a caller whose expectation failed. */
	if (!exchanged) {
		*expected_word = current;
		return false;
	}

	/* Succeeded: the object now holds the desired value. */
	return true;
}

/*
 * Adds to a 4-byte atomic object.
 */
uint32_t
hal_atomic_runtime_fetch_add_4(
	volatile void *object,
	uint32_t operand,
	int order)
{
	volatile uint32_t *word;
	uintptr_t cookie;
	uint32_t previous;

	UNUSED_PARAMETER(order);

	word = object;

	/* Reads and updates the object as one operation. */
	cookie = hal_atomic_enter();
	previous = *word;
	*word = previous + operand;
	hal_atomic_leave(cookie);

	/* Reports the value the object held before. */
	return previous;
}

/*
 * Subtracts from a 4-byte atomic object.
 */
uint32_t
hal_atomic_runtime_fetch_sub_4(
	volatile void *object,
	uint32_t operand,
	int order)
{
	volatile uint32_t *word;
	uintptr_t cookie;
	uint32_t previous;

	UNUSED_PARAMETER(order);

	word = object;

	/* Reads and updates the object as one operation. */
	cookie = hal_atomic_enter();
	previous = *word;
	*word = previous - operand;
	hal_atomic_leave(cookie);

	/* Reports the value the object held before. */
	return previous;
}

/*
 * Sets bits in a 4-byte atomic object.
 */
uint32_t
hal_atomic_runtime_fetch_or_4(
	volatile void *object,
	uint32_t operand,
	int order)
{
	volatile uint32_t *word;
	uintptr_t cookie;
	uint32_t previous;

	UNUSED_PARAMETER(order);

	word = object;

	/* Reads and updates the object as one operation. */
	cookie = hal_atomic_enter();
	previous = *word;
	*word = previous | operand;
	hal_atomic_leave(cookie);

	/* Reports the value the object held before. */
	return previous;
}

/*
 * Loads an atomic object of any size.
 */
void
hal_atomic_runtime_load(
	size_t size,
	const volatile void *object,
	void *result,
	int order)
{
	uintptr_t cookie;

	UNUSED_PARAMETER(order);

	/* Copies the object out while no other atomic operation can touch it. */
	cookie = hal_atomic_enter();
	runtime_bytes_copy(result, object, size);
	hal_atomic_leave(cookie);
}

/*
 * Stores an atomic object of any size.
 */
void
hal_atomic_runtime_store(
	size_t size,
	volatile void *object,
	const void *value,
	int order)
{
	uintptr_t cookie;

	UNUSED_PARAMETER(order);

	/* Copies the value in while no other atomic operation can touch it. */
	cookie = hal_atomic_enter();
	runtime_bytes_copy(object, value, size);
	hal_atomic_leave(cookie);
}

/*
 * Exchanges an atomic object of any size with a value.
 */
void
hal_atomic_runtime_exchange(
	size_t size,
	volatile void *object,
	const void *value,
	void *result,
	int order)
{
	uintptr_t cookie;

	UNUSED_PARAMETER(order);

	/* Copies the old object out and the value in, as one operation. */
	cookie = hal_atomic_enter();
	runtime_bytes_copy(result, object, size);
	runtime_bytes_copy(object, value, size);
	hal_atomic_leave(cookie);
}

/*
 * Stores a value into an atomic object of any size that holds an expected
 * value.
 *
 * When the object holds another value, that value is copied to the expected
 * value instead.
 */
bool
hal_atomic_runtime_compare_exchange(
	size_t size,
	volatile void *object,
	void *expected,
	const void *desired,
	int success_order,
	int failure_order)
{
	uintptr_t cookie;
	bool exchanged;

	UNUSED_PARAMETER(success_order);
	UNUSED_PARAMETER(failure_order);

	/* Compares and then stores or reports, as one operation. */
	cookie = hal_atomic_enter();
	exchanged = runtime_bytes_equal(object, expected, size);
	if (exchanged)
		runtime_bytes_copy(object, desired, size);
	else
		runtime_bytes_copy(expected, object, size);
	hal_atomic_leave(cookie);

	/* Reports a caller whose expectation failed. */
	if (!exchanged)
		return false;

	/* Succeeded: the object now holds the desired value. */
	return true;
}

/*
 * Loads a 1-byte atomic object.
 */
uint8_t
hal_atomic_runtime_load_1(
	const volatile void *object,
	int order)
{
	uint8_t value;

	/* Copies the object out under the serialization region. */
	hal_atomic_runtime_load(sizeof(value), object, &value, order);

	/* Reports the value the object held. */
	return value;
}

/*
 * Stores a 1-byte atomic object.
 */
void
hal_atomic_runtime_store_1(
	volatile void *object,
	uint8_t value,
	int order)
{
	/* Copies the value in under the serialization region. */
	hal_atomic_runtime_store(sizeof(value), object, &value, order);
}

/*
 * Loads a 2-byte atomic object.
 */
uint16_t
hal_atomic_runtime_load_2(
	const volatile void *object,
	int order)
{
	uint16_t value;

	/* Copies the object out under the serialization region. */
	hal_atomic_runtime_load(sizeof(value), object, &value, order);

	/* Reports the value the object held. */
	return value;
}

/*
 * Stores a 2-byte atomic object.
 */
void
hal_atomic_runtime_store_2(
	volatile void *object,
	uint16_t value,
	int order)
{
	/* Copies the value in under the serialization region. */
	hal_atomic_runtime_store(sizeof(value), object, &value, order);
}

/*
 * Loads an 8-byte atomic object.
 */
uint64_t
hal_atomic_runtime_load_8(
	const volatile void *object,
	int order)
{
	uint64_t value;

	/* Copies the object out under the serialization region. */
	hal_atomic_runtime_load(sizeof(value), object, &value, order);

	/* Reports the value the object held. */
	return value;
}

/*
 * Stores an 8-byte atomic object.
 */
void
hal_atomic_runtime_store_8(
	volatile void *object,
	uint64_t value,
	int order)
{
	/* Copies the value in under the serialization region. */
	hal_atomic_runtime_store(sizeof(value), object, &value, order);
}

/*
 * Adds to an 8-byte atomic object.
 */
uint64_t
hal_atomic_runtime_fetch_add_8(
	volatile void *object,
	uint64_t operand,
	int order)
{
	volatile uint64_t *word;
	uintptr_t cookie;
	uint64_t previous;

	UNUSED_PARAMETER(order);

	word = object;

	/* Reads and updates the object as one operation. */
	cookie = hal_atomic_enter();
	previous = *word;
	*word = previous + operand;
	hal_atomic_leave(cookie);

	/* Reports the value the object held before. */
	return previous;
}

/* Copies an object's bytes inside the serialization region. */
static void
runtime_bytes_copy(
	volatile uint8_t *destination,
	const volatile uint8_t *source,
	size_t size)
{
	size_t index;

	/* Copies byte by byte, as the operands have any size and alignment. */
	for (index = 0; index < size; index++)
		destination[index] = source[index];
}

/* Compares an object with a value inside the serialization region. */
static bool
runtime_bytes_equal(
	const volatile uint8_t *left,
	const volatile uint8_t *right,
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
#endif
