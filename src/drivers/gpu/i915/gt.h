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

	/*
	 * The graphics (GT) version, told from the PCI device id: 9 for Skylake
	 * and its Gen9.5 successors, 12 for Tiger Lake and Alder Lake.  Every
	 * GT table, context layout and submission path is chosen by it.
	 */
	unsigned graphics_ver;

	/* The display version, told from the PCI device id. */
	unsigned display_ver;

	/* The engines the platform is built with before the fuses take some away, from its product row. */
	uint32_t platform_engine_mask;

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

	/*
	 * How many forcewake domains the running device holds now: every
	 * domain while it has a holder (awake_refs), none while it rests.
	 * Changed only under awake_lock.
	 */
	unsigned forcewake_held;

	/* Nonzero while the request worker is parked for a suspend, between drv_i915_device_park_gt() and the unpark. */
	unsigned forcewake_parked;

	/*
	 * The holders of the running GT: the request worker while it has work
	 * or has just had it (worker_awake), and the RPS while it writes a
	 * frequency.  The first holder wakes every forcewake domain and the
	 * last gives them back, so the GT can drop into RC6 between them.
	 * awake_lock guards awake_refs and forcewake_held; worker_awake is the
	 * worker thread's own.
	 */
	struct spinlock awake_lock;
	unsigned awake_refs;
	unsigned worker_awake;

	/* Nonzero from the node's publication to the stop: only then may a holder wake the GT (under awake_lock). */
	unsigned awake_open;

	/* How many held-accessor refusals of the register access the worker last reported. */
	uint32_t refusals_reported;

	/* The time the GT spent in RC6, sampled while the running device holds its domains. */
	struct i915_rc6_residency rc6_residency;

	/* When the last park gave the domains back and the RC6 time then, in nanoseconds, for the unpark's log. */
	uint64_t parked_at_ns;
	uint64_t parked_sleep_ns;

	/* How many times the RC6 counter had been given up for a loss of power when the last park began. */
	unsigned parked_forgets;
};

#endif
