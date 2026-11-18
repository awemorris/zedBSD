/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's runtime helpers used across its files.
 */

#ifndef KERN_HAL_SPARCV9_RUNTIME_H
#define KERN_HAL_SPARCV9_RUNTIME_H

/*
 * Waits forever with interrupts off.
 */
void
sparcv9_halt_forever(
	void) __attribute__((noreturn));

/*
 * Counts the trailing zero bits of a 64-bit word, for the compiler.
 */
int
__ctzdi2(
	unsigned long value);

#endif
