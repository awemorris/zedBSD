/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Device access for the MIPS HAL: loads and stores of device registers by
 * physical address, through the uncached segment.
 *
 * The devices of the boards this port runs on are in the low 512 MiB of
 * physical space, which KSEG1 maps uncached without the TLB.  The
 * processor is little-endian, as are the PCI devices, so a register is
 * read and written as it is.
 */

#ifndef KERN_HAL_MIPS_IO_H
#define KERN_HAL_MIPS_IO_H

#include <hal/types.h>
#include "asm.h"

/*
 * Reports the pointer of an address in KSEG0 or KSEG1 given as its 32
 * bits.  The 64-bit processor reaches the segments at the sign-extended
 * address (CKSEG0 and CKSEG1); on the 32-bit one the value is the same.
 */
static __inline uintptr_t
mips_segment_pointer(
	uint32_t address)
{
	/* Extends bit 31 through the upper half of a 64-bit pointer. */
	return (uintptr_t)(intptr_t)(int32_t)address;
}

/*
 * Reports the kernel's cached address of a physical RAM address: KSEG0
 * on the 32-bit processor, which reaches the low 512 MiB, and the
 * cached window of xkphys on the 64-bit one, which reaches all of it.
 */
static __inline uintptr_t
mips_cached_address(
	uint64_t physical)
{
#if defined(HAL_ARCH_MIPS64)
	/* xkphys holds every physical address at a fixed offset. */
	return (uintptr_t)(MIPS_XKPHYS_CACHED + physical);
#else
	/* KSEG0 holds the low 512 MiB at a fixed offset. */
	return mips_segment_pointer((uint32_t)MIPS_KSEG0_BASE + (uint32_t)physical);
#endif
}

/*
 * Reports the kernel's uncached address of a physical address: KSEG1 on
 * the 32-bit processor and the uncached window of xkphys on the 64-bit
 * one.
 */
static __inline uintptr_t
mips_uncached_address(
	uint64_t physical)
{
#if defined(HAL_ARCH_MIPS64)
	/* xkphys holds every physical address at a fixed offset. */
	return (uintptr_t)(MIPS_XKPHYS_UNCACHED + physical);
#else
	/* KSEG1 holds the low 512 MiB at a fixed offset. */
	return mips_segment_pointer((uint32_t)MIPS_KSEG1_BASE + (uint32_t)physical);
#endif
}

/*
 * Reports the physical address of an address of the image, which runs in
 * KSEG0 (on the 64-bit processor CKSEG0, whose low 32 bits are the same):
 * the address less the segment's base, in the low RAM alias.
 */
static __inline uint64_t
mips_image_physical(
	uintptr_t address)
{
	/* The segment maps the low 512 MiB from its base. */
	return (uint64_t)((uint32_t)address - (uint32_t)MIPS_KSEG0_BASE);
}

/*
 * Reports the uncached address of a device's physical address.
 */
static __inline uintptr_t
mips_device_address(
	uint32_t physical)
{
	/* KSEG1 holds the low 512 MiB of physical space at a fixed offset. */
	return mips_segment_pointer((uint32_t)MIPS_KSEG1_BASE + physical);
}

/*
 * Reads a 32-bit device register.
 */
static __inline uint32_t
mips_io_read32(
	uint32_t physical)
{
	const volatile uint32_t *word;

	/* The register through the uncached segment. */
	word = (const volatile uint32_t *)mips_device_address(physical);

	/* Succeeded: the register's value. */
	return *word;
}

/*
 * Writes a 32-bit device register.
 */
static __inline void
mips_io_write32(
	uint32_t physical,
	uint32_t value)
{
	volatile uint32_t *word;

	/* The register through the uncached segment. */
	word = (volatile uint32_t *)mips_device_address(physical);
	*word = value;
}

/*
 * Writes a 16-bit device register.
 */
static __inline void
mips_io_write16(
	uint32_t physical,
	uint16_t value)
{
	volatile uint16_t *halfword;

	/* The register through the uncached segment. */
	halfword = (volatile uint16_t *)mips_device_address(physical);
	*halfword = value;
}

/*
 * Reads an 8-bit device register.
 */
static __inline uint8_t
mips_io_read8(
	uint32_t physical)
{
	const volatile uint8_t *byte;

	/* The register through the uncached segment. */
	byte = (const volatile uint8_t *)mips_device_address(physical);

	/* Succeeded: the register's value. */
	return *byte;
}

/*
 * Writes an 8-bit device register.
 */
static __inline void
mips_io_write8(
	uint32_t physical,
	uint8_t value)
{
	volatile uint8_t *byte;

	/* The register through the uncached segment. */
	byte = (volatile uint8_t *)mips_device_address(physical);
	*byte = value;
}

#endif
