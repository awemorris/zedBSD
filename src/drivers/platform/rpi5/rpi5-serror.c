/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Pi 5 drivers' check for an SError a start-up step left pending.
 *
 * The check masks SErrors, waits for every earlier access to complete
 * (DSB SY, then ISB), and reads ISR_EL1, whose A bit says an SError is
 * pending.  A pending one is logged with the step's name and then taken:
 * SErrors are unmasked for a moment, the HAL's handler records it as one
 * taken in the kernel, and the caller's mask is restored.
 */

#include <drivers/platform/rpi5/rpi5-serror.h>
#include <kern/klog.h>

/* ISR_EL1's bit of a pending SError. */
#define ISR_SERROR_PENDING	(1ULL << 8)

/*
 * Reports whether an SError is pending after a step.
 */
int
drv_rpi5_serror_check(
	const char *step)
{
	unsigned long long mask;
	unsigned long long pending;

	/* Masks SErrors, waits for the step's accesses, and reads the pending bits. */
	__asm__ volatile(
		"mrs %0, daif\n\t"
		"msr daifset, #4\n\t"
		"dsb sy\n\t"
		"isb\n\t"
		"mrs %1, isr_el1"
		: "=&r"(mask), "=r"(pending)
		:
		: "memory");

	/* Restores the caller's mask when nothing is pending. */
	if ((pending & ISR_SERROR_PENDING) == 0) {
		__asm__ volatile("msr daif, %0\n\tisb" : : "r"(mask) : "memory");

		/* Succeeded: the step left no SError. */
		return 0;
	}

	/* Names the step whose accesses raised it. */
	kern_logf("rpi5: SError pending after %s\n", step);

	/* Takes it now, then restores the caller's mask. */
	__asm__ volatile(
		"msr daifclr, #4\n\t"
		"isb\n\t"
		"msr daif, %0\n\t"
		"isb"
		:
		: "r"(mask)
		: "memory");

	/* Succeeded: the step left an SError, now taken. */
	return 1;
}
