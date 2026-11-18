/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Intel AX211 PCI BAR backend.
 */

#ifndef KERN_DRIVERS_INTEL_AX211_PCI_MMIO_H
#define KERN_DRIVERS_INTEL_AX211_PCI_MMIO_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-mmio.h"

/*
 * The mapped BAR0 of one device, and the counter that times the busy waits of
 * the MMIO layer.
 *
 * The counter's frequency and origin are fixed when the backend is bound; the
 * last counter reading and the last reported time only move forward, so the
 * clock the MMIO layer sees never goes back.
 */
struct intel_ax211_pci_mmio_backend {
	volatile uint8_t *registers;
	size_t mapping_size;
	uint64_t counter_frequency_hz;
	uint64_t counter_origin;
	uint64_t last_counter;
	uint64_t last_microseconds;
	uint8_t counter_ready;
};

int
drv_intel_ax211_pci_mmio_backend_init(
	struct intel_ax211_pci_mmio_backend *backend,
	void *registers,
	size_t mapping_size);

const struct intel_ax211_mmio_ops *
drv_intel_ax211_pci_mmio_ops(
	void);

#ifdef INTEL_AX211_PCI_MMIO_HOST_TEST
int
drv_intel_ax211_pci_mmio_host_ticks_for_us(
	uint64_t frequency_hz,
	uint64_t microseconds,
	uint64_t *ticks);

int
drv_intel_ax211_pci_mmio_host_ticks_to_us(
	uint64_t frequency_hz,
	uint64_t ticks,
	uint64_t *microseconds);
#endif

#endif
