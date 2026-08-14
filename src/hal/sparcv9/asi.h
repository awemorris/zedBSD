/* SPARC V9 ASI helpers used by the sun4u BSP. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_SPARCV9_ASI_H
#define ZEDBSD_HAL_SPARCV9_ASI_H

#define SPARCV9_ASI_PHYS_BYPASS 0x15
#define SPARCV9_ASI_PHYS_BYPASS_LE 0x1d
#define SPARCV9_ASI_IMMU 0x50
#define SPARCV9_ASI_ITLB_DATA_IN 0x54
#define SPARCV9_ASI_IMMU_DEMAP 0x57
#define SPARCV9_ASI_DMMU 0x58
#define SPARCV9_ASI_DTLB_DATA_IN 0x5c
#define SPARCV9_ASI_DMMU_DEMAP 0x5f

#define SPARCV9_MMU_TAG_TARGET 0x00UL
#define SPARCV9_MMU_PRIMARY_CONTEXT 0x08UL
#define SPARCV9_MMU_TAG_ACCESS 0x30UL

static inline unsigned char
sparcv9_phys_read8(unsigned long long address)
{
	unsigned char value;

	__asm__ volatile("lduba [%1] %2, %0" : "=r"(value) :
	    "r"(address), "i"(SPARCV9_ASI_PHYS_BYPASS));
	return value;
}

static inline void
sparcv9_phys_write8(unsigned long long address, unsigned char value)
{
	__asm__ volatile("stba %0, [%1] %2" : : "r"(value), "r"(address),
	    "i"(SPARCV9_ASI_PHYS_BYPASS) : "memory");
}

static inline unsigned short
sparcv9_phys_read16_le(unsigned long long address)
{
	unsigned short value;

	__asm__ volatile("lduha [%1] %2, %0" : "=r"(value) :
	    "r"(address), "i"(SPARCV9_ASI_PHYS_BYPASS_LE));
	return value;
}

static inline void
sparcv9_phys_write16_le(unsigned long long address, unsigned short value)
{
	__asm__ volatile("stha %0, [%1] %2" : : "r"(value), "r"(address),
	    "i"(SPARCV9_ASI_PHYS_BYPASS_LE) : "memory");
}

static inline unsigned int
sparcv9_phys_read32_le(unsigned long long address)
{
	unsigned int value;

	__asm__ volatile("lduwa [%1] %2, %0" : "=r"(value) :
	    "r"(address), "i"(SPARCV9_ASI_PHYS_BYPASS_LE));
	return value;
}

static inline void
sparcv9_phys_write32_le(unsigned long long address, unsigned int value)
{
	__asm__ volatile("stwa %0, [%1] %2" : : "r"(value), "r"(address),
	    "i"(SPARCV9_ASI_PHYS_BYPASS_LE) : "memory");
}

static inline unsigned long long
sparcv9_mmu_read(unsigned int asi, unsigned long address)
{
	unsigned long long value;

	if (asi == SPARCV9_ASI_IMMU)
		__asm__ volatile("ldxa [%1] %2, %0" : "=r"(value) :
		    "r"(address), "i"(SPARCV9_ASI_IMMU));
	else
		__asm__ volatile("ldxa [%1] %2, %0" : "=r"(value) :
		    "r"(address), "i"(SPARCV9_ASI_DMMU));
	return value;
}

static inline void
sparcv9_set_primary_context(unsigned long context)
{
	__asm__ volatile("stxa %0, [%1] %2\n\tmembar #Sync" : :
	    "r"(context), "r"(SPARCV9_MMU_PRIMARY_CONTEXT),
	    "i"(SPARCV9_ASI_DMMU) : "memory");
}

static inline unsigned long
sparcv9_pstate(void)
{
	unsigned long value;
	__asm__ volatile("rdpr %%pstate, %0" : "=r"(value));
	return value;
}

static inline void
sparcv9_write_pstate(unsigned long value)
{
	__asm__ volatile("wrpr %0, 0, %%pstate" : : "r"(value) : "memory");
}

static inline unsigned long long
sparcv9_tick(void)
{
	unsigned long long value;
	__asm__ volatile("rd %%tick, %0" : "=r"(value));
	return value & ~(1ULL << 63);
}

static inline void
sparcv9_tick_compare(unsigned long long value)
{
	__asm__ volatile("wr %0, 0, %%tick_cmpr" : : "r"(value));
}

static inline void
sparcv9_clear_tick_interrupt(void)
{
	unsigned long value = 1;
	__asm__ volatile("wr %0, 0, %%clear_softint" : : "r"(value));
}

#endif
