/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Synchronous primary-plane flips within the boot display's established mode. */
#ifndef KERN_DRIVERS_GPU_BCM2711_DISPLAY_FLIP_H
#define KERN_DRIVERS_GPU_BCM2711_DISPLAY_FLIP_H

#include <stdbool.h>
#include <stdint.h>
#include <kern/lock.h>

#include "drivers/gpu/bcm2711/bcm2711-gpu.h"

/* Two private SRAM lists alternate while the boot console list stays intact. */
#define BCM2711_FLIP_SLOTS 2U

/* One display owns this state until shutdown; guard protects IRQ and callers. */
struct bcm2711_flip_state {
	struct spinlock guard;
	bool initialized;
	bool attached;
	bool busy;
	bool uncertain;
	bool completed;
	uint32_t active_list;
	uint32_t pending_list;
	uint32_t retained_mask;
	uint64_t frame_sequence;
	struct drv_bcm2711_boot_screen frames[BCM2711_FLIP_SLOTS];
};

/* A caller-owned snapshot names borrowed buffers that must remain DMA-readable. */
struct bcm2711_flip_status {
	bool attached;
	bool busy;
	bool uncertain;
	uint32_t active_list;
	uint32_t pending_list;
	uint32_t retained_mask;
	uint64_t frame_sequence;
	struct drv_bcm2711_boot_screen frames[BCM2711_FLIP_SLOTS];
};

/* The driver keeps this owner and its mapped registers alive throughout use. */
struct bcm2711_display;

/* Initialization precedes IRQ publication; it must never reset a live owner. */
void bcm2711_display_flip_init(struct bcm2711_display *display);

/* Attachment requires a successful R0 and sole ownership of HVS channel zero. */
int bcm2711_display_flip_attach(struct bcm2711_display *display);

/*
 * Caller serializes buffer ownership, freezes bytes before submission and keeps
 * storage contiguous and mapped. Never free or modify a retained buffer, even
 * after an error. Snapshot after each call determines which buffers are held.
 * Calls may busy-wait up to 100 ms; never call from IRQ or while holding a lock.
 */
int bcm2711_display_flip_present(struct bcm2711_display *display, const struct drv_bcm2711_boot_screen *frame);

/* Only confirmed boot-console adoption releases buffers after uncertain DMA. */
int bcm2711_display_flip_restore(struct bcm2711_display *display);
void bcm2711_display_flip_snapshot(struct bcm2711_display *display, struct bcm2711_flip_status *status);

/* Only the selected PV service calls this, with flip.guard held after W1C. */
void bcm2711_display_flip_vblank_locked(struct bcm2711_display *display, uint32_t current);

#endif
