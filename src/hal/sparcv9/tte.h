/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The UltraSPARC-I/II translation table entry, as the HAL writes it into
 * the TLB.
 */

#ifndef KERN_HAL_SPARCV9_TTE_H
#define KERN_HAL_SPARCV9_TTE_H

#include <hal/types.h>

/* The entry is valid. */
#define SPARCV9_TTE_VALID	(1ULL << 63)

/* The page size field: 4 MiB; zero is the 8 KiB base page. */
#define SPARCV9_TTE_SIZE_4M	(3ULL << 61)

/* Faults on any access but a non-faulting load. */
#define SPARCV9_TTE_NFO		(1ULL << 60)

/* Inverts the byte order of every access, for little-endian devices. */
#define SPARCV9_TTE_IE		(1ULL << 59)

/* Keeps the entry in the TLB when others are replaced. */
#define SPARCV9_TTE_LOCKED	(1ULL << 6)

/* Cacheable in the physically and the virtually indexed caches. */
#define SPARCV9_TTE_CP		(1ULL << 5)
#define SPARCV9_TTE_CV		(1ULL << 4)

/* Accesses have side effects: the processor neither merges nor repeats them. */
#define SPARCV9_TTE_SIDE_EFFECT	(1ULL << 3)

/* Only privileged code may use the page. */
#define SPARCV9_TTE_PRIVILEGED	(1ULL << 2)

/* The page may be written. */
#define SPARCV9_TTE_WRITE	(1ULL << 1)

/* The entry matches in every context. */
#define SPARCV9_TTE_GLOBAL	(1ULL << 0)

/* The physical page number's bits. */
#define SPARCV9_TTE_PA_MASK	0x000001ffffffe000ULL

/*
 * Builds a valid entry for a physical page with the given attributes.
 */
static inline uint64_t
sparcv9_tte(
	uintptr_t physical,
	uint64_t flags)
{
	/* Reports the entry: valid, the page's address and the attributes. */
	return SPARCV9_TTE_VALID | (physical & SPARCV9_TTE_PA_MASK) | flags;
}

#endif
