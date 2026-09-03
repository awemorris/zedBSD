/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

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
hal_atomic_cookie_t
hal_atomic_enter(
	void)
{
	hal_atomic_cookie_t cookie;

	/* Preserves the caller's interrupt state while acquiring the guard. */
	cookie = hal_irq_disable() ? 1U : 0U;

	/* Waits until this CPU owns the atomic-operation guard. */
	while (!hal_atomic_uint_try_acquire(&atomic_guard))
		hal_atomic_relax();

	/* Returns the saved interrupt-state cookie. */
	return cookie;
}

/*
 * Releases the i386 atomic-operation guard.
 */
void
hal_atomic_leave(
	hal_atomic_cookie_t cookie)
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
