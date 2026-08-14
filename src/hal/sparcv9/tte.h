/* UltraSPARC-I/II TTE encoding used by the software MMU. */
#ifndef ZEDBSD_HAL_SPARCV9_TTE_H
#define ZEDBSD_HAL_SPARCV9_TTE_H

#include <hal/types.h>

#define SPARCV9_TTE_VALID (1ULL << 63)
#define SPARCV9_TTE_SIZE_4M (3ULL << 61)
#define SPARCV9_TTE_NFO (1ULL << 60)
#define SPARCV9_TTE_IE (1ULL << 59)
#define SPARCV9_TTE_LOCKED (1ULL << 6)
#define SPARCV9_TTE_CP (1ULL << 5)
#define SPARCV9_TTE_CV (1ULL << 4)
#define SPARCV9_TTE_SIDE_EFFECT (1ULL << 3)
#define SPARCV9_TTE_PRIVILEGED (1ULL << 2)
#define SPARCV9_TTE_WRITE (1ULL << 1)
#define SPARCV9_TTE_GLOBAL (1ULL << 0)
#define SPARCV9_TTE_PA_MASK 0x000001ffffffe000ULL

static inline uint64
sparcv9_tte(uintptr_t physical, uint64 flags)
{
	return SPARCV9_TTE_VALID | (physical & SPARCV9_TTE_PA_MASK) | flags;
}

#endif
