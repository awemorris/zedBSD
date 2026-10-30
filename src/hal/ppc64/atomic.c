/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 64-bit PowerPC atomic-operation guard.
 */

#include <hal/hal.h>

/*
 * The serialization region's entry points, declared as the macros of
 * <hal/atomic.h> declare them, so that a definition which drifts from the
 * macros does not compile.
 */
HAL_ATOMIC_ENTER_DECLARE();

/*
 * Takes the guard of the read-modify-write and 64-bit operations.  The
 * port is uniprocessor, so turning interrupts off excludes every other
 * context.
 */
uintptr_t
hal_atomic_enter(
	void)
{
	uintptr_t cookie;
	bool enabled;

	/* Keeps the caller's interrupt state as the cookie. */
	enabled = hal_irq_disable();
	cookie = 0U;
	if (enabled)
		cookie = 1U;
	__asm__ volatile("" ::: "memory");

	/* Succeeded: the saved interrupt state. */
	return cookie;
}

/*
 * Releases the guard and turns interrupts back on when they were on.
 */
void
hal_atomic_leave(
	uintptr_t cookie)
{
	/* Orders the guarded accesses before interrupts can come in. */
	__asm__ volatile("" ::: "memory");
	if (cookie != 0U)
		hal_irq_enable();
}
