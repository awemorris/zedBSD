/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The values the MIPS HAL writes back to the processor after an
 * exception: the Status an eret returns with, and a floating-point
 * control and status register that is safe to load.
 *
 * Both are pure functions of their arguments, so that they can be checked
 * apart from the processor.
 */

#ifndef KERN_HAL_MIPS_STATUS_H
#define KERN_HAL_MIPS_STATUS_H

#include <hal/types.h>
#include "asm.h"

/*
 * Builds the Status a return to user mode erets with.
 *
 * Nothing of the saved Status is taken over: a frame that sigreturn or a
 * debugger wrote may hold any bit, and CU0, the kernel mode or a cleared
 * exception level would let user code run privileged.  The value is user
 * mode at the exception level with interrupts on, the interrupt mask the
 * kernel runs with now, the 64-bit processor's addressing bits, CU1 only
 * for the task that owns the floating-point unit and FR whenever the
 * unit is used.
 */
static __inline uint32_t
mips_user_status(
	uint32_t current,
	bool float_owner,
	bool float_usable)
{
	uint32_t status;

	/* User mode at the exception level, interrupts on, under the kernel's mask. */
	status = MIPS_STATUS_MODE_USER | MIPS_STATUS_EXCEPTION_LEVEL | MIPS_STATUS_INTERRUPTS;
	status |= current & MIPS_STATUS_INTERRUPT_MASK;

	/*
	 * The 64-bit processor's addressing (KX and UX): the user's segment
	 * is xuseg, and the next exception enters the kernel with xkphys in
	 * reach.  Zero on the 32-bit processor.
	 */
	status |= MIPS_STATUS_KERNEL_ADDRESSING;

	/* The unit's register model is the same for every task that may use it. */
	if (float_usable)
		status |= MIPS_STATUS_FLOAT_64;

	/* Only the unit's owner reaches it without a coprocessor unusable exception. */
	if (float_owner && float_usable)
		status |= MIPS_STATUS_COPROCESSOR_1;

	/* Succeeded: a Status with no privilege the task may choose. */
	return status;
}

/*
 * Builds the Status a return to the kernel erets with.
 *
 * The saved value was the kernel's own, so it goes back as it was, but
 * with the interrupt mask the kernel runs with now: a line masked or
 * unmasked by the handler stays so.
 */
static __inline uint32_t
mips_kernel_status(
	uint32_t saved,
	uint32_t current)
{
	uint32_t status;

	/* The saved Status with today's interrupt mask, at the exception level. */
	status = saved & ~(uint32_t)MIPS_STATUS_INTERRUPT_MASK;
	status |= current & MIPS_STATUS_INTERRUPT_MASK;
	status |= MIPS_STATUS_EXCEPTION_LEVEL;

	/* Succeeded: the interrupted kernel's Status. */
	return status;
}

/*
 * Makes a floating-point control and status register safe to load.
 *
 * A write that sets a Cause bit together with its Enable bit raises the
 * floating-point exception at once, in the kernel; the Cause bits only
 * describe an operation that already happened, so they are cleared.
 */
static __inline uint32_t
mips_float_control_sanitize(
	uint32_t value)
{
	/* Succeeded: the register without its Cause field. */
	return value & ~(uint32_t)MIPS_FCSR_CAUSE_MASK;
}

#endif
