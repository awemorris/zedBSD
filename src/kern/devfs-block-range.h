/*
 * devfs block-descriptor byte-range arithmetic
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_KERN_DEVFS_BLOCK_RANGE_H
#define ZEDBSD_KERN_DEVFS_BLOCK_RANGE_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

enum devfs_block_io_direction {
	DEVFS_BLOCK_IO_READ,
	DEVFS_BLOCK_IO_WRITE
};

struct devfs_block_io_range {
	uint64_t device_bytes;
	uint64_t position;
	size_t length;
};

static inline int
devfs_block_io_range_prepare(uint64_t block_count, uint32_t block_size,
	int64_t offset, size_t requested, enum devfs_block_io_direction direction,
	struct devfs_block_io_range *range)
{
	uint64_t device_bytes, position, available;

	if (range == NULL || block_count == 0U || block_size == 0U ||
	    offset < 0 || (direction != DEVFS_BLOCK_IO_READ &&
	    direction != DEVFS_BLOCK_IO_WRITE))
		return EINVAL;
	if (block_count > UINT64_MAX / block_size)
		return EOVERFLOW;
	device_bytes = block_count * block_size;
	position = (uint64_t)offset;
	if (requested != 0U && (uint64_t)requested > UINT64_MAX - position)
		return EOVERFLOW;
	range->device_bytes = device_bytes;
	range->position = position;
	range->length = requested;
	if (requested == 0U)
		return 0;
	if (position >= device_bytes) {
		range->length = 0U;
		return direction == DEVFS_BLOCK_IO_READ ? 0 : ENOSPC;
	}
	available = device_bytes - position;
	if ((uint64_t)requested > available) {
		if (direction == DEVFS_BLOCK_IO_WRITE)
			return ENOSPC;
		range->length = (size_t)available;
	}
	return 0;
}

static inline int
devfs_block_io_piece(uint64_t position, size_t remaining,
	uint32_t block_size, uint64_t *block, size_t *within,
	size_t *count)
{
	size_t offset, amount;

	if (remaining == 0U || block_size == 0U || block == NULL ||
	    within == NULL || count == NULL)
		return EINVAL;
	offset = (size_t)(position % block_size);
	amount = (size_t)block_size - offset;
	if (amount > remaining)
		amount = remaining;
	*block = position / block_size;
	*within = offset;
	*count = amount;
	return 0;
}

#endif
