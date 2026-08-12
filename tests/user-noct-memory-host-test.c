/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "user/noct/memory.h"

#include <limits.h>

#define KIB 1024U
#define MIB (1024U * KIB)
#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (0)

static int
check(uint64_t capacity, uint64_t available, uint32_t expected_mib)
{
	struct zedbsd_user_noct_memory_profile profile;
	size_t reserved;

	CHECK(zedbsd_user_noct_select_memory(capacity, available, &profile));
	CHECK(profile.capacity_bytes == capacity);
	CHECK(profile.available_bytes == available);
	CHECK(profile.profile_mib == expected_mib);
	CHECK(profile.arena_size == (size_t)expected_mib * MIB);
	CHECK(profile.jit_code_size != 0);
	CHECK(profile.gc_nursery_size != 0);
	CHECK(profile.gc_graduate_size != 0);
	CHECK(profile.gc_tenure_size != 0);
	reserved = profile.jit_code_size + profile.gc_nursery_size +
		profile.gc_graduate_size + profile.gc_tenure_size;
	CHECK(reserved < profile.arena_size / 2U);
	return 0;
}

int
main(void)
{
	int result;

	CHECK(!zedbsd_user_noct_select_memory(8U * MIB - 1U,
		UINT64_MAX, &(struct zedbsd_user_noct_memory_profile){0}));
	result = check(8U * MIB, 1, 5); CHECK(result == 0);
	result = check(16U * MIB - 1U, UINT64_MAX, 5); CHECK(result == 0);
	result = check(16U * MIB, UINT64_MAX, 10); CHECK(result == 0);
	result = check(32U * MIB, UINT64_MAX, 20); CHECK(result == 0);
	result = check(64U * MIB - 1U, UINT64_MAX, 20); CHECK(result == 0);
	result = check(64U * MIB, UINT64_MAX, 32); CHECK(result == 0);
	result = check(96U * MIB, UINT64_MAX, 48); CHECK(result == 0);
	result = check(128U * MIB, UINT64_MAX, 64); CHECK(result == 0);
	result = check(2048ULL * MIB, UINT64_MAX, 1024); CHECK(result == 0);
	result = check(UINT64_MAX, UINT64_MAX, 1024); CHECK(result == 0);
	CHECK(!zedbsd_user_noct_select_memory(8U * MIB, 0, NULL));
	return 0;
}
