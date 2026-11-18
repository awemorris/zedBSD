/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS HAL's processor operations for C: reading the coprocessor 0
 * registers it reports, and the stops.
 */

#ifndef KERN_HAL_MIPS_CPU_H
#define KERN_HAL_MIPS_CPU_H

#include <hal/types.h>

/*
 * Reads Status (coprocessor 0 register 12).
 */
static __inline uint32_t
mips_read_status(
	void)
{
	uint32_t value;

	/* Moves the register from coprocessor 0. */
	__asm__ volatile("mfc0 %0, $12" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads the processor's identity, PRId (coprocessor 0 register 15,
 * select 0).
 */
static __inline uint32_t
mips_read_processor_id(
	void)
{
	uint32_t value;

	/* Moves the register from coprocessor 0. */
	__asm__ volatile("mfc0 %0, $15, 0" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads EBase (coprocessor 0 register 15, select 1), where the
 * exception vectors are.
 */
static __inline uint32_t
mips_read_exception_base(
	void)
{
	uint32_t value;

	/* Moves the register from coprocessor 0. */
	__asm__ volatile("mfc0 %0, $15, 1" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads Config (coprocessor 0 register 16, select 0).
 */
static __inline uint32_t
mips_read_config(
	void)
{
	uint32_t value;

	/* Moves the register from coprocessor 0. */
	__asm__ volatile("mfc0 %0, $16, 0" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads Config1 (coprocessor 0 register 16, select 1), which describes
 * the TLB, the caches and the floating-point unit.
 */
static __inline uint32_t
mips_read_config1(
	void)
{
	uint32_t value;

	/* Moves the register from coprocessor 0. */
	__asm__ volatile("mfc0 %0, $16, 1" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads Config2 (coprocessor 0 register 16, select 2), which describes
 * the secondary cache.  It exists when Config1.M is set.
 */
static __inline uint32_t
mips_read_config2(
	void)
{
	uint32_t value;

	/* Moves the register from coprocessor 0. */
	__asm__ volatile("mfc0 %0, $16, 2" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

void mips_halt_forever(void) __attribute__((noreturn));

void
mips_cmain(
	uint32_t argument_count,
	uint32_t argument_table,
	uint32_t environment_table,
	uint32_t low_memory_size);

void
mips_early_exception(
	uint32_t cause,
	uint32_t exception_pc,
	uint32_t bad_address,
	uint32_t status) __attribute__((noreturn));

#endif
