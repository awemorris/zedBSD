/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <hal/hal.h>

/*
 * The lock-word macro calls the function defined below.  The name is freed
 * here, before the function is declared and defined, so that neither is
 * taken for a use of the macro.
 */
#undef hal_atomic_uint_try_acquire

/*
 * The serialization region's entry points and the lock-word function,
 * declared as the macros of <hal/atomic.h> declare them, so that a
 * definition which drifts from the macros does not compile.
 */
HAL_ATOMIC_ENTER_DECLARE();
HAL_ATOMIC_TRY_ACQUIRE_DECLARE();

/*
 * Enters the serialization region of the atomic fallback.
 *
 * The MC68030 HAL is uniprocessor.  Excluding interrupts therefore
 * serializes atomic fallback regions against every execution context.
 */
uintptr_t
hal_atomic_enter(void)
{
	uintptr_t cookie;
	bool enabled;

	/* Keeps the caller's interrupt state as the cookie. */
	enabled = hal_irq_disable();
	cookie = 0U;
	if (enabled)
		cookie = 1U;

	/* Orders the guarded accesses after the interrupts went off. */
	__asm__ volatile("" ::: "memory");

	/* Succeeded: the saved interrupt state. */
	return cookie;
}

/*
 * Leaves the serialization region of the atomic fallback.
 */
void
hal_atomic_leave(
	uintptr_t cookie)
{
	/* Orders the guarded accesses before interrupts can come in. */
	__asm__ volatile("" ::: "memory");

	/* Turns interrupts back on only when they were on at the enter. */
	if (cookie != 0U)
		hal_irq_enable();
}

/*
 * Takes a lock word if it is free.
 */
bool
hal_atomic_uint_try_acquire(
	volatile unsigned *value)
{
	uintptr_t cookie;
	unsigned previous;
	bool acquired;

	/* Opens the serialization region around the test and the store. */
	cookie = hal_atomic_enter();

	/* Takes the word when it held zero. */
	previous = *value;
	if (previous == 0U)
		*value = 1U;

	/* Closes the serialization region. */
	hal_atomic_leave(cookie);

	/* The lock was taken when the word held zero. */
	acquired = false;
	if (previous == 0U)
		acquired = true;

	/* Reports whether the lock was taken. */
	return acquired;
}
