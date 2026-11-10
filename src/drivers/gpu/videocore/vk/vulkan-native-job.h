/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native submission ownership survives worker callback retirement when DMA has no retirement proof. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_JOB_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_NATIVE_JOB_H

#include "drivers/gpu/videocore/render-device.h"
#include "drivers/gpu/videocore/vk/vulkan-native-pass.h"

/* One prepared primary and its currently executing FIFO pass remain rooted in the controller quarantine after an uncertain callback. */
struct bcm2711_vulkan_native_job {
	struct bcm2711_vulkan_native_job *next;
	struct bcm2711_render_session *session;
	struct bcm2711_vulkan_prepared *prepared;
	struct bcm2711_vulkan_native_pass *pass;
	bool executed;
	bool retired;
	bool quarantined;
	/*
	 * A job runs in steps of at most one native pass: stepping is true from
	 * the first step until the last, and cursor, region and remaining say
	 * where the next step starts.
	 */
	bool stepping;
	const struct bcm2711_vulkan_prepared_event *cursor;
	uint32_t region;
	uint64_t remaining;
	/* The last step failed while preparing, before it launched anything or left a translation unpublished. */
	bool prepare_failed;
};

/* The renderer must own the same Vulkan session before a primary can become independently pending native work. */
int bcm2711_vulkan_native_job_create(struct bcm2711_vulkan_object *command, struct bcm2711_vulkan_native_job **job);

/* Runs the job's next step (at most one native pass); done tells that the job has finished. */
int bcm2711_vulkan_native_job_step(struct bcm2711_render_device *controller, struct bcm2711_render_session *session, void *payload, bool *done, bool *retired);
int bcm2711_vulkan_native_job_dispose(struct bcm2711_render_device *controller, void *payload, bool retired);

/* Only checked global native reset and zero external sessions authorize whole quarantine/closed-session retirement. */
int bcm2711_vulkan_native_jobs_recover(struct bcm2711_render_device *controller);

#endif
