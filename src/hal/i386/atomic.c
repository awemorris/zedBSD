/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <hal/hal.h>

/*
 * An i386 has XCHG but no CMPXCHG.  This guard serializes the uncommon
 * read-modify-write and 64-bit operations across CPUs.  IRQs are disabled
 * before acquisition so an interrupt cannot recursively wait on the guard
 * held by the interrupted context.
 */
static volatile unsigned atomic_guard;

hal_atomic_cookie_t
hal_atomic_enter(void)
{
	hal_atomic_cookie_t cookie;

	cookie = hal_irq_disable() ? 1U : 0U;
	while (!hal_atomic_uint_try_acquire(&atomic_guard))
		hal_atomic_relax();
	return cookie;
}

void
hal_atomic_leave(
	hal_atomic_cookie_t cookie)
{
	unsigned previous = 0U;

	__asm__ volatile("xchgl %0, %1"
			 : "+r"(previous), "+m"(atomic_guard)
			 :
			 : "memory");
	if (previous != 1U)
		__builtin_trap();
	if (cookie != 0U)
		hal_irq_enable();
}
