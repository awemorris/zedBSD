/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A mock PCI function behind struct i915_pci_ops.
 *
 * It holds a 256-byte configuration space and models the interrupt vector
 * allocator: a vector is handed out in sequence, or the allocation fails,
 * or it hands out a vector whose low byte is zero so the MSI message cannot
 * be formed.  Every configuration write and every vector call is counted.
 */

#ifndef DRIVERS_GPU_I915_TESTS_CONTRACTS_MOCK_PCI_H
#define DRIVERS_GPU_I915_TESTS_CONTRACTS_MOCK_PCI_H

#include "../../pci.h"

#include <stdint.h>

/* The size of the configuration space the mock holds. */
#define MOCK_PCI_CONFIG_BYTES	256U

/*
 * The configuration space and vector allocator of one mock PCI function.
 *
 * A test owns it for the whole program; each mock_pci_setup_*() rebuilds
 * it from scratch.
 */
struct mock_pci {
	/* The configuration space, little endian as on the bus. */
	uint8_t config[MOCK_PCI_CONFIG_BYTES];

	/* How many configuration writes of each width arrived. */
	int write8_calls;
	int write16_calls;
	int write32_calls;

	/* Nonzero makes the vector allocation fail with EIO. */
	int msi_alloc_fail;

	/* Nonzero makes the allocation hand out a vector whose low byte is zero. */
	int msi_alloc_bad;

	/* The vector the next allocation hands out. */
	int msi_vector_next;

	/* How many vectors were allocated and freed, and the last of each. */
	int alloc_calls;
	int free_calls;
	int last_alloc_vector;
	int last_free_vector;
};

const struct i915_pci_ops *mock_pci_ops(void);

void mock_pci_setup_full(struct mock_pci *mock);
void mock_pci_setup_no_msi(struct mock_pci *mock);
void mock_pci_setup_no_pm(struct mock_pci *mock);
void mock_pci_setup_io_and_mem(struct mock_pci *mock);

#endif
