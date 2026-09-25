/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The hardware state of one device.
 *
 * The device start builds it step by step and the device stop takes it apart
 * in reverse; the flags at the end record which steps completed, so a start
 * that stops half-way gives back exactly what it took.
 */

#ifndef DRIVERS_GPU_I915_GT_H
#define DRIVERS_GPU_I915_GT_H

#include <drivers/generic/dma.h>
#include <drivers/pci/pci.h>
#include <kern/lock.h>
#include <stdint.h>

#include "defaults.h"
#include "device-info.h"
#include "dma.h"
#include "engine.h"
#include "irq.h"
#include "memory.h"
#include "migrate.h"
#include "mmio.h"
#include "pci.h"
#include "ppgtt.h"
#include "pxp.h"
#include "runtime-pm.h"
#include "trace.h"
#include "verify-workarounds.h"
#include "workarounds.h"

/*
 * Everything the device start acquires on the hardware.
 *
 * It lives inside the device from attach to detach.  The start worker fills
 * it; after the node is published, the request worker is its only user.
 */
struct i915_gt {
	/* What the start did, for the log dump at the end of the start. */
	struct i915_trace trace;

	/* PCI configuration access and the MSI vector. */
	struct i915_pci_context pci_context;
	struct i915_pci pci;

	/* The PCI-core runtime PM hold taken for the duration of the start and the device's life. */
	struct i915_rpm probe_pm;

	/* The device's own runtime PM state (initialized, never enabled). */
	struct i915_rpm rpm;

	/* The register half of BAR0 and the access that guards it. */
	struct drv_pci_mapping regs;
	struct i915_mmio_window regs_window;
	struct i915_mmio mmio;

	/* Serializes the engine reset against other uncore users. */
	struct spinlock uncore_lock;

	/* Serializes the PCODE mailbox. */
	struct mutex sb_lock;

	/* The GGTT table half of BAR0. */
	struct drv_pci_mapping gtt;

	/* How many GGTT entries the table window holds. */
	unsigned ggtt_entries;

	/* The DMA device the GPU's 39-bit addressing needs, and its bookkeeping. */
	struct drv_dma_device *dma_device;
	struct i915_dma dma;

	/* The zeroed page every unused GGTT entry points at, and that entry. */
	struct drv_dma_vector *scratch;
	uint64_t scratch_pte;

	/* The CPU's write-combining view of the GMADR aperture. */
	uint64_t gmadr_base;
	uint64_t gmadr_size;
	void *aperture;

	/* The display version and platform, told from the PCI device id. */
	unsigned display_ver;
	int is_alderlake_p;

	/* The GT and engine information read from the fuses. */
	struct i915_gt_info info;

	/* The interrupt device. */
	struct i915_irq_dev irq;

	/* The workaround, MOCS and power tables and what applying them did. */
	struct i915_gt_init init;

	/* The GT objects, the GGTT windows and the kernel address space. */
	struct i915_gt_mem mem;
	struct i915_gt_object *gt_scratch;
	struct i915_gt_ppgtt ppgtt;

	/* The engines, their status pages and kernel contexts. */
	struct i915_gt_engines engines;

	/* The default context images, recorded once from the GPU. */
	struct i915_gt_defaults defaults;

	/* The workaround check the engines ran. */
	struct i915_gt_verify_wa verify;

	/* The migration context on the copy engine. */
	struct i915_gt_migrate migrate;

	/* The protected-content context. */
	struct i915_pxp pxp;

	/* Nonzero for each step that completed and must be undone by the stop. */
	unsigned probe_pm_held;
	unsigned pci_enabled;
	unsigned bar_claimed;
	unsigned regs_mapped;
	unsigned gtt_mapped;
	unsigned dma_created;
	unsigned scratch_created;
	unsigned msi_kept;
	unsigned irq_installed;
	unsigned mem_inited;
	unsigned engines_inited;
	unsigned engines_resumed;
	unsigned defaults_inited;
	unsigned verify_inited;
	unsigned migrate_inited;
	unsigned pxp_inited;

	/* Nonzero while the start holds every forcewake domain for the running device. */
	unsigned forcewake_held;
};

#endif
