/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The register builders of the host unit tests of the x86 processor
 * decoders: they place fields into CPUID registers and model-specific
 * register values from the bit positions the specification gives, without
 * the decoders' code, so a test can make an input from the values it
 * expects back.  A field wider than its width is a mistake in the test,
 * which the builder reports by aborting.
 */

#ifndef CPUX86_TEST_REGBUILD_H
#define CPUX86_TEST_REGBUILD_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "cpux86.h"

/*
 * Places a field into a 32-bit register: bits low..low+width-1 become
 * field.  The test's own input must fit; a field that does not is a test
 * bug and stops the test.
 */
static __inline void
regbuild_put32(
	uint32_t *reg,
	uint32_t low,
	uint32_t width,
	uint32_t field)
{
	uint32_t mask;
	uint32_t index;

	/* Refuses a field position no register has. */
	if (width == 0U || width > 32U || low >= 32U || low + width > 32U) {
		printf("regbuild_put32: bad field %u+%u\n", low, width);
		abort();
	}

	/* Builds the mask one bit at a time, so no shift reaches the width. */
	mask = 0U;
	for (index = 0U; index < width; index++)
		mask |= (uint32_t)1U << (low + index);

	/* Refuses a value wider than the field. */
	if (width < 32U && (field >> width) != 0U) {
		printf("regbuild_put32: value %u wider than %u bits\n", field, width);
		abort();
	}

	/* Replaces the field and keeps every other bit. */
	*reg = (*reg & ~mask) | ((field << low) & mask);
}

/*
 * Places a field into a 64-bit register value, as regbuild_put32() does.
 */
static __inline void
regbuild_put64(
	uint64_t *reg,
	uint32_t low,
	uint32_t width,
	uint64_t field)
{
	uint64_t mask;
	uint32_t index;

	/* Refuses a field position no register has. */
	if (width == 0U || width > 64U || low >= 64U || low + width > 64U) {
		printf("regbuild_put64: bad field %u+%u\n", low, width);
		abort();
	}

	/* Builds the mask one bit at a time, so no shift reaches the width. */
	mask = 0U;
	for (index = 0U; index < width; index++)
		mask |= (uint64_t)1U << (low + index);

	/* Refuses a value wider than the field. */
	if (width < 64U && (field >> width) != 0U) {
		printf("regbuild_put64: value wider than %u bits\n", width);
		abort();
	}

	/* Replaces the field and keeps every other bit. */
	*reg = (*reg & ~mask) | ((field << low) & mask);
}

/*
 * Builds the leaf 00H registers of an Intel processor with the given
 * MAX_LEAF (Intel SDM Vol. 1 rev. 093, Table 21-4: "Genu" in EBX, "ineI" in
 * EDX, "ntel" in ECX, first character in the low byte).
 */
static __inline void
regbuild_leaf0_intel(
	struct cpux86_cpuid_regs *regs,
	uint32_t max_leaf)
{
	/* The string is spelled out character by character, low byte first. */
	regs->eax = max_leaf;
	regs->ebx = (uint32_t)'G' | ((uint32_t)'e' << 8) | ((uint32_t)'n' << 16) | ((uint32_t)'u' << 24);
	regs->edx = (uint32_t)'i' | ((uint32_t)'n' << 8) | ((uint32_t)'e' << 16) | ((uint32_t)'I' << 24);
	regs->ecx = (uint32_t)'n' | ((uint32_t)'t' << 8) | ((uint32_t)'e' << 16) | ((uint32_t)'l' << 24);
}

/*
 * Builds leaf 01H EAX from its raw fields (Intel SDM Vol. 1 rev. 093,
 * Table 21-6): stepping 3:0, model 7:4, family 11:8, type 13:12, extended
 * model 19:16, extended family 27:20.
 */
static __inline uint32_t
regbuild_leaf1_eax(
	uint32_t family,
	uint32_t model,
	uint32_t stepping,
	uint32_t type,
	uint32_t extended_family,
	uint32_t extended_model)
{
	uint32_t eax;

	/* Places each field at its bits. */
	eax = 0U;
	regbuild_put32(&eax, 0U, 4U, stepping);
	regbuild_put32(&eax, 4U, 4U, model);
	regbuild_put32(&eax, 8U, 4U, family);
	regbuild_put32(&eax, 12U, 2U, type);
	regbuild_put32(&eax, 16U, 4U, extended_model);
	regbuild_put32(&eax, 20U, 8U, extended_family);

	/* Reports the register. */
	return eax;
}

/*
 * Builds one sub-leaf of leaf 0BH or 1FH (Intel SDM Vol. 1 rev. 093,
 * Table 21-70): shift EAX[4:0], count EBX[15:0], level ECX[7:0], type
 * ECX[15:8], x2APIC ID EDX.
 */
static __inline void
regbuild_topology(
	struct cpux86_cpuid_regs *regs,
	uint32_t level,
	uint32_t type,
	uint32_t shift,
	uint32_t count,
	uint32_t x2apic_id)
{
	/* Starts from a clean set of registers. */
	regs->eax = 0U;
	regs->ebx = 0U;
	regs->ecx = 0U;
	regs->edx = x2apic_id;

	/* Places each field at its bits. */
	regbuild_put32(&regs->eax, 0U, 5U, shift);
	regbuild_put32(&regs->ebx, 0U, 16U, count);
	regbuild_put32(&regs->ecx, 0U, 8U, level);
	regbuild_put32(&regs->ecx, 8U, 8U, type);
}

#endif
