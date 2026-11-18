/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS page table entries: the software bits a mapping's attributes
 * give, and the hardware bits the TLB refill loads, derived from them.
 *
 * Both are pure functions of their arguments, so that they can be checked
 * apart from the processor.
 */

#ifndef KERN_HAL_MIPS_PTE_H
#define KERN_HAL_MIPS_PTE_H

#include <hal/hal.h>
#include "asm.h"

/* The cache attribute of a user page: cacheable, noncoherent, write back. */
#define MIPS_PTE_CACHED		3U

/*
 * A table entry: a word on the 32-bit processor, a doubleword on the
 * 64-bit one, whose physical addresses go past 32 bits.
 */
typedef uintptr_t mips_pte_t;

/*
 * Reports the software bits of a mapped page with the given access:
 * present, and what may be done with it.  Execution is only recorded:
 * the processor reads and runs the same pages.
 */
static __inline mips_pte_t
mips_pte_software_bits(
	uint32_t attr)
{
	mips_pte_t bits;

	/* Mapped, then each access the attributes give. */
	bits = MIPS_PTE_PRESENT;
	if ((attr & HAL_SPACE_READ) != 0)
		bits |= MIPS_PTE_READABLE;
	if ((attr & HAL_SPACE_WRITE) != 0)
		bits |= MIPS_PTE_WRITABLE;
	if ((attr & HAL_SPACE_EXEC) != 0)
		bits |= MIPS_PTE_EXEC;

	/* Succeeded: the bits. */
	return bits;
}

/*
 * Derives a table entry's hardware bits from its software bits.
 *
 * The page is valid once it is used and allows some access, and
 * writable to the TLB once it is also written and may be.  The cache
 * attribute is always the cached one.
 */
static __inline mips_pte_t
mips_pte_compose(
	mips_pte_t entry)
{
	mips_pte_t composed;
	bool usable;

	/* Keeps the frame and the software bits, with the cache attribute. */
	composed = entry & (MIPS_PTE_FRAME_MASK | MIPS_PTE_SOFTWARE_MASK);
	composed |= (mips_pte_t)MIPS_PTE_CACHED << MIPS_PTE_CACHE_SHIFT;

	/* Valid: mapped, used, and with some access. */
	usable = false;
	if ((entry & MIPS_PTE_PRESENT) != 0 &&
	    (entry & MIPS_PTE_ACCESSED) != 0 &&
	    (entry & (MIPS_PTE_READABLE | MIPS_PTE_WRITABLE | MIPS_PTE_EXEC)) != 0)
		usable = true;
	if (usable)
		composed |= MIPS_PTE_VALID;

	/* Dirty to the TLB: valid, writable and already written. */
	if (usable &&
	    (entry & MIPS_PTE_WRITABLE) != 0 &&
	    (entry & MIPS_PTE_DIRTY) != 0)
		composed |= MIPS_PTE_WRITE_ENABLE;

	/* Succeeded: the entry the refill loads. */
	return composed;
}

#endif
