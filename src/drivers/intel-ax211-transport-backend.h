/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 transport backend adapter
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_TRANSPORT_BACKEND_H
#define ZEDBSD_DRIVERS_INTEL_AX211_TRANSPORT_BACKEND_H

#include "intel-ax211-dma.h"
#include "intel-ax211-pci-mmio.h"
#include "intel-ax211-transport.h"

#include <stdint.h>

enum intel_ax211_transport_backend_result {
	INTEL_AX211_TRANSPORT_BACKEND_OK = 0,
	INTEL_AX211_TRANSPORT_BACKEND_INVALID = 1,
	INTEL_AX211_TRANSPORT_BACKEND_NOT_READY = 2,
	INTEL_AX211_TRANSPORT_BACKEND_NOT_COHERENT = 3
};

/*
 * The owning controller must serialize ISR, runtime, reset, and MMIO access
 * to this object.  The adapter deliberately contains no independent lock:
 * both its monotonic-clock latch and the referenced MMIO ownership depth are
 * part of the controller's single hardware state machine.  All three
 * referenced objects and their BAR/DMA allocations must outlive the adapter.
 * Only coherent DMA is admitted; the current generic DMA API has no fallible
 * coherent-buffer subrange synchronization operation for a noncoherent port.
 */
struct intel_ax211_transport_backend {
	struct intel_ax211_mmio *mmio;
	struct intel_ax211_pci_mmio_backend *pci_mmio;
	struct intel_ax211_dma_resources *dma;
	uint64_t last_clock_us;
	uint8_t initialized;
	uint8_t clock_observed;
	uint8_t failed;
};

int intel_ax211_transport_backend_init(
	struct intel_ax211_transport_backend *backend,
	struct intel_ax211_mmio *mmio,
	struct intel_ax211_pci_mmio_backend *pci_mmio,
	struct intel_ax211_dma_resources *dma);
const struct intel_ax211_transport_ops *
intel_ax211_transport_backend_ops(void);

/*
 * Builds a borrowed ring view transactionally.  On failure, memory is left
 * unchanged.  The DMA resources retain allocation and lifetime ownership.
 */
int intel_ax211_transport_backend_ring_memory(
	const struct intel_ax211_transport_backend *backend,
	struct intel_ax211_transport_ring_memory *memory);

#endif
