/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The UltraSPARC address space identifiers the HAL uses, and the single
 * instructions that reach the MMU, the physical bypass and the processor
 * state through them.
 *
 * Values referenced: UltraSPARC-IIi User's Manual (Sun Microsystems,
 * 805-0087), the ASI assignments (physical bypass, the IMMU and DMMU
 * registers, the TLB data access and tag read ASIs) and the MMU register
 * offsets (tag target, primary context, fault address, tag access).
 */

#ifndef KERN_HAL_SPARCV9_ASI_H
#define KERN_HAL_SPARCV9_ASI_H

#include <hal/types.h>

/* Physical addresses past the caches, with side effects, in each byte order. */
#define SPARCV9_ASI_PHYS_BYPASS		0x15
#define SPARCV9_ASI_PHYS_BYPASS_LE	0x1d

/* The instruction MMU's registers, its TLB's data and its tags. */
#define SPARCV9_ASI_IMMU		0x50
#define SPARCV9_ASI_ITLB_DATA_ACCESS	0x55
#define SPARCV9_ASI_ITLB_TAG_READ	0x56

/* The data MMU's registers, its TLB's data and its tags. */
#define SPARCV9_ASI_DMMU		0x58
#define SPARCV9_ASI_DTLB_DATA_ACCESS	0x5d
#define SPARCV9_ASI_DTLB_TAG_READ	0x5e

/* The MMU registers, by their offset in the IMMU and DMMU ASIs. */
#define SPARCV9_MMU_TAG_TARGET		0x00UL
#define SPARCV9_MMU_PRIMARY_CONTEXT	0x08UL
#define SPARCV9_MMU_FAULT_ADDRESS	0x20UL
#define SPARCV9_MMU_TAG_ACCESS		0x30UL

/* The number of entries in each fully associative TLB. */
#define SPARCV9_TLB_ENTRIES		64U

/* The processor state bit that enables interrupts. */
#define SPARCV9_PSTATE_IE		0x02UL

/*
 * Reads one byte at a physical address, bypassing the caches.
 */
static inline uint8_t
sparcv9_phys_read8(
	uint64_t address)
{
	uint8_t value;

	/* Loads the byte through the bypass ASI. */
	__asm__ volatile("lduba [%1] %2, %0" : "=r"(value) : "r"(address), "i"(SPARCV9_ASI_PHYS_BYPASS));

	/* Succeeded: the byte. */
	return value;
}

/*
 * Writes one byte at a physical address, bypassing the caches.
 */
static inline void
sparcv9_phys_write8(
	uint64_t address,
	uint8_t value)
{
	/* Stores the byte through the bypass ASI. */
	__asm__ volatile("stba %0, [%1] %2" : : "r"(value), "r"(address), "i"(SPARCV9_ASI_PHYS_BYPASS) : "memory");
}

/*
 * Writes a little-endian 16-bit word at a physical address, bypassing the
 * caches.
 */
static inline void
sparcv9_phys_write16_le(
	uint64_t address,
	uint16_t value)
{
	/* Stores the word through the little-endian bypass ASI. */
	__asm__ volatile("stha %0, [%1] %2" : : "r"(value), "r"(address), "i"(SPARCV9_ASI_PHYS_BYPASS_LE) : "memory");
}

/*
 * Reads a little-endian 16-bit word at a physical address, bypassing the
 * caches.
 */
static inline uint16_t
sparcv9_phys_read16_le(
	uint64_t address)
{
	uint16_t value;

	/* Loads the word through the little-endian bypass ASI. */
	__asm__ volatile("lduha [%1] %2, %0" : "=r"(value) : "r"(address), "i"(SPARCV9_ASI_PHYS_BYPASS_LE));

	/* Succeeded: the word. */
	return value;
}

/*
 * Reads a little-endian 32-bit word at a physical address, bypassing the
 * caches.
 */
static inline uint32_t
sparcv9_phys_read32_le(
	uint64_t address)
{
	uint32_t value;

	/* Loads the word through the little-endian bypass ASI. */
	__asm__ volatile("lduwa [%1] %2, %0" : "=r"(value) : "r"(address), "i"(SPARCV9_ASI_PHYS_BYPASS_LE));

	/* Succeeded: the word. */
	return value;
}

/*
 * Writes a little-endian 32-bit word at a physical address, bypassing the
 * caches.
 */
static inline void
sparcv9_phys_write32_le(
	uint64_t address,
	uint32_t value)
{
	/* Stores the word through the little-endian bypass ASI. */
	__asm__ volatile("stwa %0, [%1] %2" : : "r"(value), "r"(address), "i"(SPARCV9_ASI_PHYS_BYPASS_LE) : "memory");
}

/*
 * Reads one register of the data MMU.
 */
static inline uint64_t
sparcv9_dmmu_read(
	uint64_t offset)
{
	uint64_t value;

	/* Loads the register through the DMMU ASI. */
	__asm__ volatile("ldxa [%1] %2, %0" : "=r"(value) : "r"(offset), "i"(SPARCV9_ASI_DMMU));

	/* Succeeded: the register. */
	return value;
}

/*
 * Reads one register of the instruction MMU.
 */
static inline uint64_t
sparcv9_immu_read(
	uint64_t offset)
{
	uint64_t value;

	/* Loads the register through the IMMU ASI. */
	__asm__ volatile("ldxa [%1] %2, %0" : "=r"(value) : "r"(offset), "i"(SPARCV9_ASI_IMMU));

	/* Succeeded: the register. */
	return value;
}

/*
 * Reads the data of one data TLB entry.
 */
static inline uint64_t
sparcv9_dtlb_read_data(
	unsigned entry)
{
	uint64_t value;

	/* Loads the entry's data; the entry is addressed by its index times 8. */
	__asm__ volatile("ldxa [%1] %2, %0" : "=r"(value) : "r"((uint64_t)entry * 8U), "i"(SPARCV9_ASI_DTLB_DATA_ACCESS));

	/* Succeeded: the entry's data. */
	return value;
}

/*
 * Reads the tag of one data TLB entry.
 */
static inline uint64_t
sparcv9_dtlb_read_tag(
	unsigned entry)
{
	uint64_t value;

	/* Loads the entry's tag. */
	__asm__ volatile("ldxa [%1] %2, %0" : "=r"(value) : "r"((uint64_t)entry * 8U), "i"(SPARCV9_ASI_DTLB_TAG_READ));

	/* Succeeded: the entry's tag. */
	return value;
}

/*
 * Writes one data TLB entry with its tag.
 *
 * The tag access register and the entry are written back to back, so no
 * miss taken in between can change the tag the entry receives.
 */
static inline void
sparcv9_dtlb_write_entry(
	unsigned entry,
	uint64_t tag,
	uint64_t data)
{
	/* Sets the tag, stores the entry's data and waits for both. */
	__asm__ volatile("stxa %0, [%1] %2\n\tstxa %3, [%4] %5\n\tmembar #Sync" : : "r"(tag), "r"(SPARCV9_MMU_TAG_ACCESS), "i"(SPARCV9_ASI_DMMU), "r"(data), "r"((uint64_t)entry * 8U), "i"(SPARCV9_ASI_DTLB_DATA_ACCESS) : "memory");
}

/*
 * Writes one instruction TLB entry with its tag.
 *
 * The tag access register and the entry are written back to back, so no
 * miss taken in between can change the tag the entry receives.
 */
static inline void
sparcv9_itlb_write_entry(
	unsigned entry,
	uint64_t tag,
	uint64_t data)
{
	/* Sets the tag, stores the entry's data and waits for both. */
	__asm__ volatile("stxa %0, [%1] %2\n\tstxa %3, [%4] %5\n\tmembar #Sync" : : "r"(tag), "r"(SPARCV9_MMU_TAG_ACCESS), "i"(SPARCV9_ASI_IMMU), "r"(data), "r"((uint64_t)entry * 8U), "i"(SPARCV9_ASI_ITLB_DATA_ACCESS) : "memory");
}

/*
 * Reads the data of one instruction TLB entry.
 */
static inline uint64_t
sparcv9_itlb_read_data(
	unsigned entry)
{
	uint64_t value;

	/* Loads the entry's data. */
	__asm__ volatile("ldxa [%1] %2, %0" : "=r"(value) : "r"((uint64_t)entry * 8U), "i"(SPARCV9_ASI_ITLB_DATA_ACCESS));

	/* Succeeded: the entry's data. */
	return value;
}

/*
 * Reads the tag of one instruction TLB entry.
 */
static inline uint64_t
sparcv9_itlb_read_tag(
	unsigned entry)
{
	uint64_t value;

	/* Loads the entry's tag. */
	__asm__ volatile("ldxa [%1] %2, %0" : "=r"(value) : "r"((uint64_t)entry * 8U), "i"(SPARCV9_ASI_ITLB_TAG_READ));

	/* Succeeded: the entry's tag. */
	return value;
}

/*
 * Sets the context the MMU matches user addresses against.
 */
static inline void
sparcv9_set_primary_context(
	uint64_t context)
{
	/* Writes the primary context register and waits for it. */
	__asm__ volatile("stxa %0, [%1] %2\n\tmembar #Sync" : : "r"(context), "r"(SPARCV9_MMU_PRIMARY_CONTEXT), "i"(SPARCV9_ASI_DMMU) : "memory");
}

/*
 * Reads the processor state register.
 */
static inline uint64_t
sparcv9_read_pstate(
	void)
{
	uint64_t value;

	/* Reads the privileged register. */
	__asm__ volatile("rdpr %%pstate, %0" : "=r"(value));

	/* Succeeded: the register. */
	return value;
}

/*
 * Writes the processor state register.
 */
static inline void
sparcv9_write_pstate(
	uint64_t value)
{
	/* Writes the privileged register. */
	__asm__ volatile("wrpr %0, 0, %%pstate" : : "r"(value) : "memory");
}

/*
 * Reads the trap level the processor runs at.
 */
static inline uint64_t
sparcv9_read_trap_level(
	void)
{
	uint64_t value;

	/* Reads the privileged register. */
	__asm__ volatile("rdpr %%tl, %0" : "=r"(value));

	/* Succeeded: the trap level. */
	return value;
}

/*
 * Reads the cycle counter, without its non-privileged-trap bit.
 */
static inline uint64_t
sparcv9_read_tick(
	void)
{
	uint64_t value;

	/* Reads the tick register. */
	__asm__ volatile("rd %%tick, %0" : "=r"(value));

	/* Succeeded: the count; the top bit is a control bit, not a count. */
	return value & ~(1ULL << 63);
}

/*
 * Sets the cycle count at which the next tick interrupt is raised.
 */
static inline void
sparcv9_write_tick_compare(
	uint64_t value)
{
	/* Writes the tick compare register. */
	__asm__ volatile("wr %0, 0, %%tick_cmpr" : : "r"(value));
}

/*
 * Clears the pending tick interrupt.
 */
static inline void
sparcv9_clear_tick_interrupt(
	void)
{
	uint64_t tick_bit;

	/* The tick interrupt is bit 0 of the soft interrupt register. */
	tick_bit = 1;
	__asm__ volatile("wr %0, 0, %%clear_softint" : : "r"(tick_bit));
}

#endif
