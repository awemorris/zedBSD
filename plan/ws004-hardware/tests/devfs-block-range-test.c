/*
 * WS004 p023 devfs raw-block 64-bit range fixture.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include "../../../src/kern/devfs-block-range.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

static unsigned failures;

#define CHECK(expression) do { \
	if (!(expression)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
		failures++; \
	} \
} while (0)

static void
test_ranges(void)
{
	struct devfs_block_io_range range;
	const uint64_t large_blocks = UINT64_C(0x02000010);
	const int64_t above_4g = INT64_C(0x100001000);

	CHECK(devfs_block_io_range_prepare(large_blocks, 512U, above_4g,
	    4096U, DEVFS_BLOCK_IO_WRITE, &range) == 0);
	CHECK(range.position == (uint64_t)above_4g && range.length == 4096U);
	CHECK(range.device_bytes == large_blocks * UINT64_C(512));
	CHECK(devfs_block_io_range_prepare(8U, 512U, 3584, 1024U,
	    DEVFS_BLOCK_IO_READ, &range) == 0);
	CHECK(range.position == 3584U && range.length == 512U);
	CHECK(devfs_block_io_range_prepare(8U, 512U, 3584, 1024U,
	    DEVFS_BLOCK_IO_WRITE, &range) == ENOSPC);
	CHECK(devfs_block_io_range_prepare(8U, 512U, 4096, 1U,
	    DEVFS_BLOCK_IO_READ, &range) == 0 && range.length == 0U);
	CHECK(devfs_block_io_range_prepare(8U, 512U, 4096, 1U,
	    DEVFS_BLOCK_IO_WRITE, &range) == ENOSPC);
	CHECK(devfs_block_io_range_prepare(8U, 512U, -1, 1U,
	    DEVFS_BLOCK_IO_READ, &range) == EINVAL);
	CHECK(devfs_block_io_range_prepare(UINT64_MAX, 512U, 0, 1U,
	    DEVFS_BLOCK_IO_READ, &range) == EOVERFLOW);
#if SIZE_MAX == UINT64_MAX
	CHECK(devfs_block_io_range_prepare(UINT64_MAX / 512U, 512U,
	    INT64_MAX, SIZE_MAX, DEVFS_BLOCK_IO_READ, &range) == EOVERFLOW);
#endif
	CHECK(devfs_block_io_range_prepare(8U, 512U, INT64_MAX, 0U,
	    DEVFS_BLOCK_IO_WRITE, &range) == 0 && range.length == 0U);
	CHECK(devfs_block_io_range_prepare(0U, 512U, 0, 1U,
	    DEVFS_BLOCK_IO_READ, &range) == EINVAL);
	CHECK(devfs_block_io_range_prepare(8U, 0U, 0, 1U,
	    DEVFS_BLOCK_IO_READ, &range) == EINVAL);
	CHECK(devfs_block_io_range_prepare(8U, 512U, 0, 1U,
	    (enum devfs_block_io_direction)2, &range) == EINVAL);
}

static void
test_unaligned_pieces(void)
{
	uint64_t block;
	size_t within, count;

	CHECK(devfs_block_io_piece(511U, 2U, 512U, &block, &within,
	    &count) == 0);
	CHECK(block == 0U && within == 511U && count == 1U);
	CHECK(devfs_block_io_piece(512U, 1U, 512U, &block, &within,
	    &count) == 0);
	CHECK(block == 1U && within == 0U && count == 1U);
	CHECK(devfs_block_io_piece(513U, 1024U, 512U, &block, &within,
	    &count) == 0);
	CHECK(block == 1U && within == 1U && count == 511U);
	CHECK(devfs_block_io_piece(1024U, 513U, 512U, &block, &within,
	    &count) == 0);
	CHECK(block == 2U && within == 0U && count == 512U);
	CHECK(devfs_block_io_piece(1536U, 1U, 512U, &block, &within,
	    &count) == 0);
	CHECK(block == 3U && within == 0U && count == 1U);
	CHECK(devfs_block_io_piece(0U, 0U, 512U, &block, &within,
	    &count) == EINVAL);
	CHECK(devfs_block_io_piece(0U, 1U, 0U, &block, &within,
	    &count) == EINVAL);
}

int
main(void)
{
	test_ranges();
	test_unaligned_pieces();
	if (failures != 0U) {
		printf("HW-T20 devfs block range: %u failure(s)\n", failures);
		return 1;
	}
	puts("HW-T20 devfs block range: PASS");
	return 0;
}
