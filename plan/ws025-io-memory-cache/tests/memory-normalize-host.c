/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "bootloader/common/memory-map.h"

static struct zbl6_memory_range_v6 input[257], output[257];
static unsigned checks;

static enum zbl_memory_result decode(const void *context, uint32_t index,
    struct zbl6_memory_range_v6 *range)
{
	const struct zbl6_memory_range_v6 *raw = context;
	*range = raw[index];
	return ZBL_MEMORY_OK;
}

static void expect(unsigned entries, unsigned capacity, unsigned options,
    enum zbl_memory_result expected, unsigned expected_count)
{
	uint32_t count = 777;
	enum zbl_memory_result result;
	result = zbl_memory_normalize(input, entries, decode, options,
	    output, capacity, &count);
	assert(result == expected && count == expected_count);
	checks++;
}

int main(void)
{
	struct zbl6_memory_range_v6 original[3];
	uint64_t high = UINT64_C(0x100000000);
	unsigned i, j, k;

	input[0] = (struct zbl6_memory_range_v6){high, 0x300000, ZBL6_MEMORY_USABLE, 0, 1};
	input[1] = (struct zbl6_memory_range_v6){high + 0x100001, 1, ZBL6_MEMORY_RESERVED, 0, 1};
	input[2] = (struct zbl6_memory_range_v6){0x1000, 0x3000, ZBL6_MEMORY_USABLE, 0, 1};
	expect(3, 257, 0, ZBL_MEMORY_OK, 4);
	assert(output[0].base == 0x1000 && output[0].size == 0x3000);
	assert(output[1].base == high && output[1].size == 0x100000);
	assert(output[2].base == high + 0x100000 && output[2].size == 0x1000);
	assert(output[2].type == ZBL6_MEMORY_RESERVED);
	assert(output[3].base == high + 0x101000 && output[3].size == 0x1ff000);
	expect(3, 3, 0, ZBL_MEMORY_CAPACITY, 0);
	expect(3, 257, ZBL_MEMORY_REJECT_OVERLAP, ZBL_MEMORY_OVERLAP, 0);

	input[0] = (struct zbl6_memory_range_v6){0x1001, 0x1fff, ZBL6_MEMORY_USABLE, 0, 1};
	expect(1, 257, 0, ZBL_MEMORY_OK, 1);
	assert(output[0].base == 0x2000 && output[0].size == 0x1000);
	input[1] = (struct zbl6_memory_range_v6){0x2500, 1, 99, 0, 1};
	expect(2, 257, 0, ZBL_MEMORY_OK, 1);
	assert(output[0].base == 0x2000 && output[0].size == 0x1000 &&
	    output[0].type == ZBL6_MEMORY_RESERVED);

	/* A stronger reservation must win regardless of input permutation. */
	original[0] = (struct zbl6_memory_range_v6){high, 0x1000, ZBL6_MEMORY_USABLE, 0, 1};
	original[1] = (struct zbl6_memory_range_v6){high, 0x1000, ZBL6_MEMORY_USABLE, 0, 2};
	original[2] = (struct zbl6_memory_range_v6){high, 0x1000, ZBL6_MEMORY_RESERVED, 0, UINT64_C(1) << 63};
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			for (k = 0; k < 3; k++) {
				if (i == j || i == k || j == k)
					continue;
				input[0] = original[i]; input[1] = original[j]; input[2] = original[k];
				expect(3, 257, 0, ZBL_MEMORY_OK, 1);
				assert(output[0].type == ZBL6_MEMORY_RESERVED &&
				    output[0].attributes == (UINT64_C(1) << 63) && output[0].flags == 0);
			}
	input[0] = original[0]; input[1] = original[1];
	expect(2, 257, 0, ZBL_MEMORY_OK, 1);
	assert(output[0].type == ZBL6_MEMORY_RESERVED &&
	    output[0].flags == ZBL6_RANGE_MIXED_ATTRIBUTES);

	for (i = 0; i < 257; i++)
		input[i] = (struct zbl6_memory_range_v6){
		    high + (uint64_t)i * 0x2000, 0x1000, ZBL6_MEMORY_USABLE, 0, 1};
	expect(256, 256, 0, ZBL_MEMORY_OK, 256);
	expect(257, 256, 0, ZBL_MEMORY_CAPACITY, 0);
	for (i = 0; i < 257; i++)
		input[i].base = high + (uint64_t)i * 0x1000;
	expect(257, 256, 0, ZBL_MEMORY_OK, 1);
	assert(output[0].size == (uint64_t)257 * 0x1000);
	input[0].base = UINT64_MAX - 1; input[0].size = 2;
	expect(1, 257, 0, ZBL_MEMORY_OVERFLOW, 0);
	input[0].size = 1;
	expect(1, 257, 0, ZBL_MEMORY_EMPTY, 0);
	input[0].type = ZBL6_MEMORY_RESERVED;
	expect(1, 257, 0, ZBL_MEMORY_OVERFLOW, 0);
	input[0].base = 0x2000; input[0].size = 0x1000;
	input[0].type = ZBL6_MEMORY_BOOT_RECLAIM;
	input[0].attributes = UINT64_C(1) << 63;
	expect(1, 257, ZBL_MEMORY_REJECT_OVERLAP, ZBL_MEMORY_OK, 1);
	assert(output[0].type == ZBL6_MEMORY_BOOT_RECLAIM &&
	    output[0].attributes == (UINT64_C(1) << 63));
	expect(0, 257, 0, ZBL_MEMORY_EMPTY, 0);
	expect(1, 0, 0, ZBL_MEMORY_INVALID, 0);
	expect(1, 257, 2, ZBL_MEMORY_INVALID, 0);
	printf("MEM-NORMALIZE PASS: %u scenarios, sparse/high/overlap/capacity/overflow\n", checks);
	return 0;
}
