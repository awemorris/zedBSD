/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decision whether a device can reach a physical range directly.
 */

#ifndef KERN_DRIVERS_DMA_RANGE_H
#define KERN_DRIVERS_DMA_RANGE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Translates a physical range to the bus address a device would use for it.
 *
 * The range [paddr, paddr + size) must lie at or below physical_limit (an
 * inclusive limit, zero for none), its bus alias paddr + bus_offset must lie
 * inside the device's address_bits, the range must not cross a multiple of
 * segment_boundary (zero for none), and it must be no longer than
 * max_segment_size.  The caller checks coherence itself.  Reports 1 and the
 * bus address when every rule holds, and 0 otherwise.
 */
static __inline int
drv_dma_range_bus_address(
	unsigned address_bits,
	size_t max_segment_size,
	uint64_t segment_boundary,
	uint64_t bus_offset,
	uint64_t physical_limit,
	uint64_t paddr,
	size_t size,
	uint64_t *bus_address)
{
	uint64_t last;
	uint64_t bus_last;
	uint64_t bus_limit;

	/* An empty range, or one longer than one segment, is not mapped. */
	if (size == 0)
		return 0;
	if (size > max_segment_size)
		return 0;

	/* The last byte of the range, refusing a range that wraps. */
	if ((uint64_t)size - 1U > UINT64_MAX - paddr)
		return 0;
	last = paddr + ((uint64_t)size - 1U);

	/* The platform's reachable physical memory ends at physical_limit. */
	if (physical_limit != 0 && last > physical_limit)
		return 0;

	/* The bus alias of the last byte, refusing one that wraps. */
	if (bus_offset > UINT64_MAX - last)
		return 0;
	bus_last = last + bus_offset;

	/* The device addresses only address_bits of the bus. */
	if (address_bits == 0 || address_bits > 64U)
		return 0;
	if (address_bits < 64U) {
		bus_limit = (UINT64_C(1) << address_bits) - 1U;
		if (bus_last > bus_limit)
			return 0;
	}

	/* A segment may not cross a boundary the device's counter cannot carry over. */
	if (segment_boundary != 0) {
		if (((paddr + bus_offset) / segment_boundary) != (bus_last / segment_boundary))
			return 0;
	}

	/* Succeeded: reports where the device sees the range start. */
	*bus_address = paddr + bus_offset;
	return 1;
}

#endif
