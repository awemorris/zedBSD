/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 atomic-operation guard.
 */

#include <hal/hal.h>

/*
 * The serialization region's entry points, declared as the macros of
 * <hal/atomic.h> declare them, so that a definition which drifts from the
 * macros does not compile.
 */
HAL_ATOMIC_ENTER_DECLARE();

/*
 * A 32-bit hart has no 64-bit atomic instruction.  This guard serializes
 * the read-modify-write and 64-bit operations across harts.  Interrupts
 * are disabled before it is taken so that an interrupt cannot wait on the
 * guard the interrupted context holds.
 */
static volatile unsigned atomic_guard;

/*
 * Takes the atomic-operation guard.
 */
uintptr_t
hal_atomic_enter(
	void)
{
	uintptr_t cookie;
	bool enabled;
	bool acquired;

	/* Keeps the caller's interrupt state while the guard is taken. */
	enabled = hal_irq_disable();
	cookie = 0U;
	if (enabled)
		cookie = 1U;

	/* Waits until this hart holds the guard. */
	for (;;) {
		acquired = hal_atomic_uint_try_acquire(&atomic_guard);
		if (acquired)
			break;
		hal_atomic_relax();
	}

	/* Succeeded: the saved interrupt state. */
	return cookie;
}

/*
 * Releases the atomic-operation guard.
 */
void
hal_atomic_leave(
	uintptr_t cookie)
{
	unsigned previous;

	/* Releases the guard with release ordering and reads who held it. */
	__asm__ volatile("amoswap.w.rl %0, zero, (%1)"
			 : "=&r"(previous)
			 : "r"(&atomic_guard)
			 : "memory");

	/* A release by a hart that did not hold the guard is a bug. */
	if (previous != 1U)
		__builtin_trap();

	/* Turns interrupts back on only when they were on at the enter. */
	if (cookie != 0U)
		hal_irq_enable();
}
