/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* One FIFO pass owns every mapped draw input, command list and framebuffer hold through native DMA uncertainty. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_PASS_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_PASS_H

#include "drivers/gpu/bcm2711/native-pass.h"
#include "drivers/gpu/bcm2711/v3d-job.h"
#include "drivers/gpu/bcm2711/vulkan-native-draw.h"

#define BCM2711_VULKAN_PASS_STORAGE 9U

/* The enclosing submission keeps its prepared primary pending; this root retains independent GPU owners and copied numerical target/clear state. */
struct bcm2711_vulkan_native_pass {
	struct bcm2711_v3d_space *space;
	struct bcm2711_v3d_view *output;
	void *cpu;
	/* The enclosing pending primary retains this logical image through implicit pass layout transitions. */
	struct bcm2711_vulkan_resource *target;
	VkImageLayout initial_layout;
	VkImageLayout final_layout;
	struct bcm2711_vulkan_native_draw *first;
	struct bcm2711_vulkan_native_draw *last;
	struct bcm2711_native_storage *storage[BCM2711_VULKAN_PASS_STORAGE];
	uint64_t bytes;
	uint32_t count;
	uint32_t draws;
	struct bcm2711_native_pass state;
	struct bcm2711_v3d_job job;
	/* Clear is performed only at execution after the whole pass has prepared successfully, never during allocation or OOM rollback. */
	VkRect2D area;
	VkFormat format;
	VkAttachmentLoadOp load;
	uint32_t clear[4];
	/* Single-use execution refuses replay and preserves the last native DMA-retirement result. */
	bool executed;
	bool retired;
};

/* Earlier queue writes must have completed and become CPU-visible; next borrows the event after this pass's END from the enclosing prepared primary. */
int bcm2711_vulkan_native_pass_create(struct bcm2711_v3d_space *space, const struct bcm2711_vulkan_prepared_event *begin, uint64_t *available, struct bcm2711_vulkan_native_pass **pass, const struct bcm2711_vulkan_prepared_event **next);

/* A full-image clear uses the native tile clear colour and store without CPU target writes or user graphics state. */
int bcm2711_vulkan_native_clear_create(struct bcm2711_v3d_space *space, const struct bcm2711_vulkan_prepared_event *event, uint64_t *available, struct bcm2711_vulkan_native_pass **pass);

/* An unpublished owner with complete output/draw state gains all independently owned lists; failure leaves its partial mappings attached for release. */
int bcm2711_vulkan_native_pass_build_lists(struct bcm2711_vulkan_native_pass *pass, uint64_t *available);

/* Run once under the controller mutex, then retain the whole parent payload when the returned native retirement proof is false. */
int bcm2711_vulkan_native_pass_run(struct bcm2711_vulkan_native_pass *pass, struct bcm2711_v3d_job_result *result);

/* false preserves the complete root, including output and every draw; a controller quarantine transfer is still required before worker payload disposal. */
int bcm2711_vulkan_native_pass_release(struct bcm2711_vulkan_native_pass **pass, bool retired);

#endif
