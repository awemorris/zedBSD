/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/packages/lang/noct/runtime/memory.h"

#include <limits.h>
#include <string.h>

#define KIB (1024U)
#define MIB (1024U * KIB)

struct memory_limits {
	uint32_t profile_mib;
	size_t jit_code_size;
	size_t gc_nursery_size;
	size_t gc_graduate_size;
	size_t gc_tenure_size;
};

/*
 * The arena is Noct's general allocator.  GC and JIT reservations grow more
 * conservatively so that each class retains room for parser, VM, BeUI, and
 * application allocations.  These are policy, not ABI.
 */
static const struct memory_limits limits[] = {
	{    5U,  128U * KIB,  192U * KIB,  48U * KIB,  640U * KIB },
	{   10U,  192U * KIB,  320U * KIB,  80U * KIB, 1280U * KIB },
	{   20U,  384U * KIB,  640U * KIB, 160U * KIB, 2560U * KIB },
	{   32U,  512U * KIB,    1U * MIB, 256U * KIB,    4U * MIB },
};

int
user_noct_select_memory(
	uint64_t capacity_bytes,
	uint64_t available_bytes,
	struct user_noct_memory_profile *profile)
{
	struct memory_limits selected;
	uint64_t arena_bytes;

	if (profile == NULL)
		return 0;
	/* VM heap policy is based on installed physical memory plus swap.  The
	 * current strict-commit availability is deliberately not used to shrink
	 * the requested heap: mmap must fail atomically if the selected Noct
	 * profile cannot be guaranteed alongside the other live processes. */
	if (capacity_bytes < 8ULL * MIB)
		return 0;
	if (capacity_bytes > 64ULL * MIB) {
		arena_bytes = capacity_bytes / 2U;
		if (arena_bytes > 1024ULL * MIB)
			arena_bytes = 1024ULL * MIB;
		selected = limits[3];
		selected.profile_mib = (uint32_t)(arena_bytes / MIB);
		selected.jit_code_size = (size_t)(arena_bytes / 64U);
		selected.gc_nursery_size = (size_t)(arena_bytes / 32U);
		selected.gc_graduate_size = (size_t)(arena_bytes / 128U);
		selected.gc_tenure_size = (size_t)(arena_bytes / 8U);
	} else if (capacity_bytes >= 64ULL * MIB) {
		selected = limits[3];
		arena_bytes = 32ULL * MIB;
	} else if (capacity_bytes >= 32ULL * MIB) {
		selected = limits[2];
		arena_bytes = 20ULL * MIB;
	} else if (capacity_bytes >= 16ULL * MIB) {
		selected = limits[1];
		arena_bytes = 10ULL * MIB;
	} else {
		selected = limits[0];
		arena_bytes = 5ULL * MIB;
	}

	memset(profile, 0, sizeof(*profile));
	profile->capacity_bytes = capacity_bytes;
	profile->available_bytes = available_bytes;
	profile->profile_mib = selected.profile_mib;
	profile->arena_size = (size_t)arena_bytes;
	profile->jit_code_size = selected.jit_code_size;
	profile->gc_nursery_size = selected.gc_nursery_size;
	profile->gc_graduate_size = selected.gc_graduate_size;
	profile->gc_tenure_size = selected.gc_tenure_size;
	return 1;
}
