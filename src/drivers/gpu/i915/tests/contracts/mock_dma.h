/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A mock address producer behind struct i915_dma_ops.
 *
 * It exists only for the host tests: it fabricates device addresses so the
 * mapping bookkeeping can run without hardware.  The production driver never
 * makes up a DMA address this way.
 *
 *   - The translation is neither the identity nor linear: each physical
 *     page maps to a device page through a 24-bit reversal of the page
 *     number, so a device address never equals the CPU physical one and
 *     physically adjacent pages are not adjacent on the device side (a
 *     "base + i * PAGE" consumer is caught).
 *   - Physically contiguous entries may be coalesced into one segment, so
 *     the mapped segment count falls below the entry count.
 *   - The next mapping may be made to fail, to drive the failure path.
 */

#ifndef DRIVERS_GPU_I915_TESTS_CONTRACTS_MOCK_DMA_H
#define DRIVERS_GPU_I915_TESTS_CONTRACTS_MOCK_DMA_H

#include "../../dma.h"

#include <stdint.h>

/*
 * The state and call record of one mock address producer.
 *
 * A test owns it for the whole program; mock_dma_reset() clears it.
 */
struct mock_dma {
	/* Nonzero merges physically contiguous entries into one segment. */
	int coalesce;

	/* Nonzero makes the next mapping fail; the failure clears it. */
	int fail;

	/* The last mask and segment size set_info received, and how often it ran. */
	int set_info_calls;
	unsigned last_mask_bits;
	uint64_t last_max_segment;

	/* How many syncs and unmaps reached the producer. */
	int sync_device_calls;
	int sync_cpu_calls;
	int unmap_sg_calls;
	int unmap_page_calls;

	/* The entry count the last scatter unmap was given. */
	unsigned last_unmap_orig_nents;
};

const struct i915_dma_ops *mock_dma_ops(void);
void mock_dma_reset(struct mock_dma *mock);
uint64_t mock_dma_translate(uint64_t phys);
uint64_t mock_dma_untranslate(uint64_t address);

#endif
