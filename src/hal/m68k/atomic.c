/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <hal/hal.h>

/*
 * The serialization region's entry points, declared as the macros of
 * <hal/atomic.h> declare them, so that a definition which drifts from the
 * macros does not compile.
 */
HAL_ATOMIC_ENTER_DECLARE();

/*
 * The MC68030 HAL is uniprocessor.  Excluding interrupts therefore
 * serializes atomic fallback regions against every execution context.
 */
uintptr_t
hal_atomic_enter(void)
{
	uintptr_t cookie;

	cookie = hal_irq_disable() ? 1U : 0U;
	__asm__ volatile("" ::: "memory");
	return cookie;
}

void
hal_atomic_leave(
	uintptr_t cookie)
{
	__asm__ volatile("" ::: "memory");
	if (cookie != 0U)
		hal_irq_enable();
}

bool
hal_atomic_uint_try_acquire(
	volatile unsigned *value)
{
	uintptr_t cookie;
	unsigned previous;

	cookie = hal_atomic_enter();
	previous = *value;
	if (previous == 0U)
		*value = 1U;
	hal_atomic_leave(cookie);
	return previous == 0U;
}
