/*
 * zedBSD Intel AX211 PCI BAR backend
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_PCI_MMIO_H
#define ZEDBSD_DRIVERS_INTEL_AX211_PCI_MMIO_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-mmio.h"

struct intel_ax211_pci_mmio_backend {
	volatile uint8_t *registers;
	size_t mapping_size;
	uint64_t counter_frequency_hz;
	uint64_t counter_origin;
	uint64_t last_counter;
	uint64_t last_microseconds;
	uint8_t counter_ready;
};

int intel_ax211_pci_mmio_backend_init(
	struct intel_ax211_pci_mmio_backend *backend, void *registers,
	size_t mapping_size);
const struct intel_ax211_mmio_ops *intel_ax211_pci_mmio_ops(void);

#ifdef INTEL_AX211_PCI_MMIO_HOST_TEST
int intel_ax211_pci_mmio_host_ticks_for_us(uint64_t frequency_hz,
	uint64_t microseconds, uint64_t *ticks);
int intel_ax211_pci_mmio_host_ticks_to_us(uint64_t frequency_hz,
	uint64_t ticks, uint64_t *microseconds);
#endif

#endif
